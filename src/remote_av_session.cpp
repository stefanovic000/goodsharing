#include "remote_av_session.h"
#include "wasapi_playout_sink.h"
#include <windows.h>
#include <objbase.h>
#include <avrt.h>
#include <algorithm>

namespace goodsharing {
namespace {struct AudioPriority{
    HANDLE handle=nullptr;
    AudioPriority(){DWORD task=0;
        // Pro Audio with critical priority is what a device-rendering thread needs;
        // the plain Audio category caps at HIGH and starves under video load.
        handle=AvSetMmThreadCharacteristicsW(L"Pro Audio",&task);
        if(!handle)handle=AvSetMmThreadCharacteristicsW(L"Audio",&task);
        if(!handle)throw std::runtime_error("Audio worker scheduling registration failed");
        if(!AvSetMmThreadPriority(handle,AVRT_PRIORITY_CRITICAL)){AvRevertMmThreadCharacteristics(handle);handle=nullptr;throw std::runtime_error("Audio worker priority registration failed");}}
    ~AudioPriority(){if(handle)AvRevertMmThreadCharacteristics(handle);}
};}
RemoteAvSession::RemoteAvSession():sessionId_([]{static std::atomic_uint64_t next=0;return ++next;}()){}
RemoteAvSession::~RemoteAvSession(){stop();}
void RemoteAvSession::configure(const std::shared_ptr<RemoteVideo>& frames,const std::shared_ptr<RemoteShare>& video){
    if(!frames||!video||video->manifest().videoSid.isEmpty()||!video->manifest().audioSid.isEmpty())throw std::invalid_argument("A/V video requires a separate video watch");
    const auto weak=weak_from_this();const std::weak_ptr<RemoteShare> watch=video;
    frames->setTiming({[weak,watch](livekit::VideoFrameEvent& event)->std::optional<PresentationTicket>{if(auto self=weak.lock())return self->video(watch.lock(),event);return {};},
        [weak,watch](PresentationTicket ticket){auto self=weak.lock();return self&&self->valid(watch.lock(),ticket);},
        [weak,watch](PresentationTicket ticket){if(auto self=weak.lock())self->presented(watch.lock(),ticket);},true,permission_});
}
void RemoteAvSession::setAudioRestart(std::function<void()> callback){
    std::lock_guard lock(mutex_);if(started_||closed_||!callback)throw std::logic_error("Invalid audio restart callback");
    audioRestart_=std::move(callback);
}
void RemoteAvSession::start(const std::shared_ptr<RemoteShare>& audio,const std::shared_ptr<RemoteShare>& video){
    std::lock_guard lock(mutex_);if(started_||closed_||!audio||!video)throw std::logic_error("A/V session requires separate audio/video watches");
    started_=true;audioWatch_=audio;worker_=std::jthread([this,audio,video](std::stop_token stop)mutable{run(stop,std::move(audio),std::move(video));});
}
void RemoteAvSession::setGain(float gain) {
    std::lock_guard lock(mutex_);
    gain_=std::clamp(gain,0.0f,1.0f);
    if(playout_)playout_->setGain(gain_);
}
void RemoteAvSession::setLogger(std::function<void(const QString&)> logger){std::lock_guard lock(mutex_);if(started_)return;logger_=std::move(logger);}
void RemoteAvSession::configureTiming(std::int64_t leadUs,std::int64_t videoWaitLimitUs) {
    std::lock_guard lock(mutex_);
    if(started_||closed_)throw std::logic_error("Session timing must precede start");
    leadUs_=leadUs;videoWaitLimitUs_=videoWaitLimitUs;
}
bool RemoteAvSession::stageVideo(const std::shared_ptr<RemoteShare>& video){std::lock_guard lock(mutex_);return !closed_&&playout_&&playout_->stageVideo(video);}
bool RemoteAvSession::cancelVideo(const std::shared_ptr<RemoteShare>& video){std::lock_guard lock(mutex_);return !closed_&&playout_&&playout_->cancelVideo(video);}
std::shared_ptr<RemoteShare> RemoteAvSession::selectedVideo()const{std::lock_guard lock(mutex_);return permission_?permission_->selected():nullptr;}
bool RemoteAvSession::workerReady()const{std::lock_guard lock(mutex_);return !closed_&&playout_;}
std::optional<PresentationTicket> RemoteAvSession::video(const std::shared_ptr<RemoteShare>& video,livekit::VideoFrameEvent& event){
    const auto playout=playoutAtomic_.load(std::memory_order_acquire);
    if(!playout||closedAtomic_.load(std::memory_order_relaxed))return {};
    auto ticket=playout->scheduleVideo(video,event);sequence_++;wake_.notify_one();return ticket;
}
void RemoteAvSession::audio(livekit::AudioFrameEvent& event){
    const auto playout=playoutAtomic_.load(std::memory_order_acquire);
    if(!playout||closedAtomic_.load(std::memory_order_relaxed))return;
    playout->audio(event);sequence_++;wake_.notify_one();
}
bool RemoteAvSession::valid(const std::shared_ptr<RemoteShare>& video,PresentationTicket ticket)const{
    const auto playout=playoutAtomic_.load(std::memory_order_acquire);
    return playout&&!closedAtomic_.load(std::memory_order_relaxed)&&playout->ticketValid(video,ticket);
}
void RemoteAvSession::presented(const std::shared_ptr<RemoteShare>& video,PresentationTicket ticket){
    const auto playout=playoutAtomic_.load(std::memory_order_acquire);
    if(!playout||closedAtomic_.load(std::memory_order_relaxed))return;
    playout->presented(video,ticket);sequence_++;wake_.notify_one();
}
void RemoteAvSession::run(std::stop_token stop,std::shared_ptr<RemoteShare> audio,std::shared_ptr<RemoteShare> video){
    if(!audio||!video)return;

    const auto com=CoInitializeEx(nullptr,COINIT_MULTITHREADED);
    try{
        if(FAILED(com))throw std::runtime_error("Playback worker COM initialization failed");
        AudioPriority priority;
        WasapiPlayoutSink sink;RemoteAudioPlayout output(sink,{},permission_);
        if(logger_)output.setLogger(logger_);output.configureTiming(leadUs_,videoWaitLimitUs_);output.setGain(gain_);output.requirePresentation();output.enableAudioContinuity();
        {std::lock_guard lock(mutex_);workerStarts_++;}
        auto snapshot=[&](bool history=false){
            const auto began=std::chrono::steady_clock::now();const auto a=output.report(history),endpoint=sink.report();
            const auto elapsed=std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now()-began).count();
            std::lock_guard lock(mutex_);if(!history)maximumLiveSnapshotUs_=std::max(maximumLiveSnapshotUs_,elapsed);
            last_={{"audio",a},{"endpoint",endpoint},{"audio_task_registered",true},{"audio_starvation_restarts",audioStarvationRestarts_}};return a;
        };
        try{
            bool bound=false;
            try{output.bindMedia(audio,video);bound=true;}
            catch(const std::invalid_argument&){if(!audio->closed()&&!video->closed())throw;}
            video.reset();
            {std::lock_guard lock(mutex_);if(!closed_&&bound){playout_=&output;playoutAtomic_.store(&output,std::memory_order_release);}}
            auto nextReport=std::chrono::steady_clock::now();
        std::uint64_t lastAdmittedAudio=0,lastPresentedVideo=0;auto lastAudioProgress=std::chrono::steady_clock::now();int starvationRestarts=0;const auto sessionStart=std::chrono::steady_clock::now();
            bool audioRestartSent=false;
            while(bound&&!stop.stop_requested()){
                std::uint64_t observed=0;{std::lock_guard lock(mutex_);observed=sequence_;}
                const auto pumped=output.pump();
                if(pumped==RemoteAudioPlayout::Pump::Failed||pumped==RemoteAudioPlayout::Pump::Closed){audio->revoke();break;}
                if(std::chrono::steady_clock::now()>=nextReport){
                    const auto audioStats=snapshot();
            // Delivery watchdog: audio can stop arriving while the track still reports
            // ready and error-free, which leaves the viewer silently mute. If no block
            // has been admitted for 750 ms while pictures are still being presented, ask
            // for a fresh subscription instead of staying silent.
            {const auto admitted=static_cast<std::uint64_t>(audioStats["admitted_audio_blocks"].toDouble());
             const auto presented=static_cast<std::uint64_t>(audioStats["presented_video_frames"].toDouble());
             const auto now=std::chrono::steady_clock::now();
             if(admitted!=lastAdmittedAudio){lastAdmittedAudio=admitted;lastAudioProgress=now;}
             const auto sessionAge=std::chrono::duration_cast<std::chrono::milliseconds>(now-sessionStart).count();
             const bool neverStarted=admitted==0&&presented>100&&sessionAge>2500;
             const bool stopped=admitted>0&&now-lastAudioProgress>std::chrono::milliseconds(750);
             if(starvationRestarts<2&&presented>100&&(neverStarted||stopped)){
                 ++starvationRestarts;lastAudioProgress=now;std::function<void()> callback;
                 {std::lock_guard lock(mutex_);callback=audioRestart_;audioStarvationRestarts_=++starvationRestarts;}
                 if(callback)callback();}}
                    if(!audioRestartSent&&audioStats["committed_media_frames"].toDouble()>=48000&&
                       audioStats["decoded_channel_difference_ratio"].toDouble()==0){
                        audioRestartSent=true;std::function<void()> callback;
                        {std::lock_guard lock(mutex_);callback=audioRestart_;}
                        if(callback)callback();
                    }
                    nextReport=std::chrono::steady_clock::now()+std::chrono::milliseconds(100);
                }
                if(output.needsDevicePolling())sink.wait(2);
                else{std::unique_lock lock(mutex_);wake_.wait_for(lock,std::chrono::milliseconds(20),[&]{return stop.stop_requested()||sequence_!=observed||closed_;});}
            }
        }catch(...){audio->revoke();{std::lock_guard lock(mutex_);playout_=nullptr;}output.stop();snapshot(true);throw;}
        {std::lock_guard lock(mutex_);playout_=nullptr;}
        output.stop();snapshot(true);
    }catch(const std::exception& e){audio->revoke();std::lock_guard lock(mutex_);playout_=nullptr;last_["error"]=QString::fromUtf8(e.what());}
    if(SUCCEEDED(com))CoUninitialize();
}
void RemoteAvSession::stop(){
    playoutAtomic_.store(nullptr,std::memory_order_release);closedAtomic_.store(true,std::memory_order_release);
    {std::lock_guard lock(mutex_);closed_=true;if(auto audio=audioWatch_.lock())audio->revoke();}
    if(worker_.joinable()){worker_.request_stop();wake_.notify_all();worker_.join();}
}
QJsonObject RemoteAvSession::report()const{
    std::lock_guard lock(mutex_);auto result=last_;if(playout_)result["audio"]=playout_->report(false);
    result["session_id"]=static_cast<double>(sessionId_);result["worker_starts"]=static_cast<int>(workerStarts_);
    if(auto audio=audioWatch_.lock())result["audio_sid"]=audio->manifest().audioSid;
    if(permission_)if(auto video=permission_->selected())result["selected_video_sid"]=video->manifest().videoSid;
    result["started"]=started_;result["closed"]=closed_;result["worker_ready"]=playout_!=nullptr;
    result["maximum_live_snapshot_us"]=static_cast<double>(maximumLiveSnapshotUs_);
    result["physical_speaker_display_sync_measured"]=false;return result;
}
bool RemoteAvSession::readyForTest()const{std::lock_guard lock(mutex_);const auto audio=last_["audio"].toObject();
    return !closed_&&playout_&&audio["committed_media_frames"].toDouble()>=96000&&audio["presented_video_frames"].toDouble()>=120;
}
}
