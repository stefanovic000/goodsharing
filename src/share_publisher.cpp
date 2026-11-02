#include "share_publisher.h"
#include "publication_failure.h"
#include <livekit/local_audio_track.h>
#include <livekit/local_participant.h>
#include <livekit/local_track_publication.h>
#include <QJsonDocument>
#include <algorithm>
#include <limits>
#include <windows.h>
namespace {
// A minimized Windows 11 process is moved to EcoQoS and throttled, which starves a
// share of CPU exactly when the user hides the app. Share publishing opts out for
// the lifetime of the process.
void keepFullSpeedWhileSharing(){
    static const bool once=[]{
#ifdef _WIN32
        PROCESS_POWER_THROTTLING_STATE state{};
        state.Version=PROCESS_POWER_THROTTLING_CURRENT_VERSION;
        state.ControlMask=PROCESS_POWER_THROTTLING_EXECUTION_SPEED;
        state.StateMask=0; // 0 disables the execution-speed throttle
        return SetProcessInformation(GetCurrentProcess(),ProcessPowerThrottling,&state,sizeof(state))!=0;
#else
        return false;
#endif
    }();
    (void)once;
}
}
#include <stdexcept>

namespace goodsharing {
namespace {
using Clock=std::chrono::steady_clock;
struct PublicationCancelled {};
constexpr std::array<int,2> widths{1920,1280},heights{1080,720},bitrates{10000000,5000000};
std::uint64_t nowUs(){return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(Clock::now().time_since_epoch()).count());}
bool fresh(std::uint64_t timestamp,std::uint64_t maximumAge) {
    const auto now=nowUs();return timestamp>0&&timestamp<=now+50000&&(timestamp>=now||now-timestamp<=maximumAge);
}
}
ShareInput::ShareInput(QString id,bool audio,std::shared_ptr<EncryptionGate> gate,int fps):id_(std::move(id)),audio_(audio),gate_(std::move(gate)),fps_(fps==30?30:60) {
    if(!gate_)throw std::invalid_argument("Missing publication gate");
    shareTrackName({id_,gate_->generation(),ShareTrack::Video1080});
}
bool ShareInput::video(ShareVideoInput frame) {
    std::lock_guard lock(mutex_);
    if(closed_||!gate_->ready()||frame.identity.priming||frame.identity.captureTimestampUs<=lastVideo_||!fresh(frame.identity.captureTimestampUs,150000)) {rejectedVideo_++;return false;}
    for(std::size_t i=0;i<2;i++)if(frame.nv12[i].size()!=static_cast<std::size_t>(widths[i])*heights[i]*3/2){rejectedVideo_++;return false;}
    lastVideo_=frame.identity.captureTimestampUs;if(latest_)replaced_++;
    latest_=std::move(frame);return true;
}
bool ShareInput::audio(ShareAudioInput block) {
    std::unique_lock lock(mutex_);
    // WASAPI loopback delivers packets of varying size (480, 960, 441 ...). The
    // publisher needs exactly 10 ms, so accumulate and re-chunk instead of
    // discarding everything that is not already the right size - that is what
    // silenced most of the audio.
    if(closed_||!audio_||!gate_->ready()||block.stereo.empty()||block.timestampUs<=lastAudio_||!fresh(block.timestampUs,100000)){rejectedAudio_++;return false;}
    lastAudio_=block.timestampUs;
    if(pendingSamples_.empty())pendingStartUs_=block.timestampUs;
    pendingSamples_.insert(pendingSamples_.end(),block.stereo.begin(),block.stereo.end());
    const auto now=nowUs();
    bool queued=false;
    while(pendingSamples_.size()>=960){
        ShareAudioInput chunk;
        chunk.stereo.assign(pendingSamples_.begin(),pendingSamples_.begin()+960);
        chunk.timestampUs=pendingStartUs_;
        pendingSamples_.erase(pendingSamples_.begin(),pendingSamples_.begin()+960);
        pendingStartUs_+=10000; // 480 frames at 48 kHz
        if(audioQueue_.size()>=audioCapacityBlocks){audioQueue_.pop_front();overflowAudioDrops_++;if(!firstAudioPressureUs_)firstAudioPressureUs_=now;}
        audioQueue_.push_back(std::move(chunk));
        audioHighWater_=std::max(audioHighWater_,audioQueue_.size());
        queued=true;
    }
    discardExpiredAudio(now);
    if(!queued)return true;
    lock.unlock();audioReady_.notify_one();return true;
}
void ShareInput::discardExpiredAudio(std::uint64_t now){
    while(!audioQueue_.empty()&&audioQueue_.front().timestampUs<now&&now-audioQueue_.front().timestampUs>100000){
        audioQueue_.pop_front();expiredAudioDrops_++;if(!firstAudioPressureUs_)firstAudioPressureUs_=now;
    }
}
void ShareInput::close(){gate_->close();{std::lock_guard lock(mutex_);closed_=true;latest_.reset();audioQueue_.clear();}audioReady_.notify_all();}
std::optional<ShareVideoInput> ShareInput::takeVideo(){std::lock_guard lock(mutex_);if(closed_||!gate_->ready())return {};auto next=std::move(latest_);latest_.reset();return next;}
std::optional<ShareAudioInput> ShareInput::waitAudio(std::stop_token stop){
    std::unique_lock lock(mutex_);
    for(;;){
        audioReady_.wait(lock,stop,[this]{return closed_||gate_->closed()||!audioQueue_.empty();});
        if(stop.stop_requested()||closed_||!gate_->ready())return {};
        discardExpiredAudio(nowUs());
        if(audioQueue_.empty())continue;
        auto block=std::move(audioQueue_.front());audioQueue_.pop_front();return block;
    }
}
QJsonObject ShareInput::report()const {
    std::lock_guard lock(mutex_);
    return {{"closed",closed_},{"pending_video_pairs",latest_?1:0},{"pending_audio_ms",static_cast<int>(audioQueue_.size()*10)},
            {"audio_high_water_ms",static_cast<int>(audioHighWater_*10)},{"replaced_raw_frames",static_cast<double>(replaced_)},
            {"rejected_video",static_cast<double>(rejectedVideo_)},{"rejected_audio",static_cast<double>(rejectedAudio_)},
            {"overflow_audio_drops",static_cast<double>(overflowAudioDrops_)},{"expired_audio_drops",static_cast<double>(expiredAudioDrops_)},
            {"first_audio_pressure_us",static_cast<double>(firstAudioPressureUs_)},{"audio_capacity_ms",static_cast<int>(audioCapacityBlocks*10)},{"maximum_audio_residence_ms",100}};
}
SharePublisher::SharePublisher(livekit::Room& room,Config config,std::shared_ptr<ShareInput> input,Observers observers)
    :room_(room),config_(std::move(config)),input_(std::move(input)),gate_(input_?input_->gate():nullptr),observers_(std::move(observers)) {
    if(!gate_||gate_->generation()!=config_.generation||input_->id()!=config_.id||input_->audio_!=config_.audio||config_.readinessTimeout.count()<100||config_.readinessTimeout.count()>10000)
        throw std::invalid_argument("Invalid publisher generation/configuration");
    shareTrackName({config_.id,config_.generation,ShareTrack::Video1080});
}
SharePublisher::~SharePublisher(){stop();}
bool SharePublisher::allowed(bool priming)const {
    const auto manager=room_.e2eeManager().lock();
    return !gate_->closed()&&room_.connectionState()==livekit::ConnectionState::Connected&&manager&&manager->enabled()&&(priming||gate_->ready());
}
void SharePublisher::start() {
    keepFullSpeedWhileSharing(); // a minimized window must not throttle the share
    if(started_)throw std::logic_error("A publisher is one-shot");started_=true;state_=State::Starting;
    try {
        if(gate_->closed()){stop();return;}
        if(!allowed(true))throw std::runtime_error("Encrypted room unavailable");
        const auto manager=room_.e2eeManager().lock();const auto provider=manager->keyProvider().lock();
        if(!provider||!provider->options().shared_key||provider->options().shared_key->size()!=32)throw std::runtime_error("Client key unavailable");
        const auto participant=room_.localParticipant().lock();if(!participant)throw std::runtime_error("Local participant unavailable");
        std::vector<std::string> sids;
        auto published=[&](std::shared_ptr<livekit::Track> track) {
            tracks_.push_back(track);sids.push_back(track->sid());
            const auto publications=participant->trackPublications();const auto p=publications.find(track->sid());
            if(track->sid().empty()||p==publications.end()||p->second->encryptionType()!=livekit::EncryptionType::GCM)throw std::runtime_error("Publication is not GCM");
            if(observers_.published)observers_.published(tracks_.size());
            if(gate_->closed())throw PublicationCancelled{};
        };
        for(std::size_t i=0;i<2;i++) {
            auto observed=[this,i](const EncodedFrame& frame,FrameIdentity identity){if(observers_.video)observers_.video(heights[i],frame,identity);};
            auto before=[this,i](FrameIdentity identity){if(observers_.beforeVideoHandoff)observers_.beforeVideoHandoff(heights[i],identity);};
            outputs_[i]=std::make_unique<EncodedVideoOutput>(widths[i],heights[i],i?config_.bitrate720:config_.bitrate1080,shareTrackName({config_.id,config_.generation,i?ShareTrack::Video720:ShareTrack::Video1080}).toStdString(),EncodedVideoOutput::MetadataTransform{},std::move(observed),true,[this](bool priming){return allowed(priming);},std::chrono::milliseconds(config_.maximumCaptureAgeUs/1000),std::move(before),config_.fps);
            participant->publishTrack(outputs_[i]->track(),outputs_[i]->publishOptions());published(outputs_[i]->track());
            black_[i].assign(static_cast<std::size_t>(widths[i])*heights[i]*3/2,128);std::fill_n(black_[i].begin(),static_cast<std::size_t>(widths[i])*heights[i],16);
        }
        if(config_.audio) {
            audio_=std::make_shared<livekit::AudioSource>(48000,2);
            const auto track=livekit::LocalAudioTrack::createLocalAudioTrack(shareTrackName({config_.id,config_.generation,ShareTrack::Audio}).toStdString(),audio_);
            livekit::TrackPublishOptions options;options.source=livekit::TrackSource::SOURCE_SCREENSHARE_AUDIO;options.dtx=false;options.red=true;options.audio_encoding=livekit::AudioEncodingOptions{128000};
            participant->publishTrack(track,options);published(track);
        }
        if(!gate_->configure(participant->identity(),std::move(sids)))throw PublicationCancelled{};
        deadline_=Clock::now()+config_.readinessTimeout;
    }catch(const PublicationCancelled&){stop();}
    catch(const std::exception&){fail("Encrypted publication setup failed");}
}
void SharePublisher::fail(QString message){error_=std::move(message);gate_->close(true);stop();state_=State::Failed;}
void SharePublisher::recordFailure(const char* operation,const char* code,std::uint64_t began,std::uint64_t capture,int height) {
    const auto now=nowUs();std::lock_guard lock(failureMutex_);if(!firstFailure_.isEmpty())return;
    firstFailure_={{"operation",operation},{"code",code},{"local_us",static_cast<double>(now)},
        {"operation_begin_us",static_cast<double>(began)},{"elapsed_us",static_cast<double>(now>=began?now-began:0)},
        {"capture_us",static_cast<double>(capture)},{"capture_age_us",static_cast<double>(capture&&now>=capture?now-capture:0)},
        {"height",height},{"submitted_audio_blocks",static_cast<double>(submittedAudio_.load())},
        {"retry_attempted",false}};
}
void SharePublisher::runAudio(std::stop_token stop) {
    std::uint64_t began=0,capture=0;
    try {
        while(!stop.stop_requested()&&!gate_->closed()) {
            auto block=input_->waitAudio(stop);
            if(!block)break;
            if(stop.stop_requested()||gate_->closed())break;
            began=nowUs();capture=block->timestampUs;
            if(observers_.beforeAudioHandoff)observers_.beforeAudioHandoff(block->timestampUs);
            if(!fresh(block->timestampUs,100000)){expiredAudio_++;continue;}
            if(observers_.beforeAudioCapture)observers_.beforeAudioCapture(block->timestampUs);
            if(!allowed(false)){if(gate_->closed())break;throw std::runtime_error("Audio permission lost");}
            began=nowUs();capture=block->timestampUs;
            try{audio_->captureFrameAt(livekit::AudioFrame(std::move(block->stereo),48000,2,480),static_cast<std::int64_t>(block->timestampUs),100);}
            catch(const std::exception& e){
                if(expiredAudioRejected(e.what(),block->timestampUs,nowUs())){expiredAudio_++;nativeExpiredAudio_++;continue;}
                throw;
            }
            submittedAudio_++;
            if(observers_.audioHandoff)observers_.audioHandoff(block->timestampUs,began);
        }
    }catch(const std::exception& e){if(!stop.stop_requested()&&!gate_->closed()){recordFailure("audio_capture",publicationFailureCode(e.what()),began,capture);audioFailed_=true;gate_->close(true);}}
    catch(...){if(!stop.stop_requested()&&!gate_->closed()){recordFailure("audio_capture","nonstandard_exception",began,capture);audioFailed_=true;gate_->close(true);}}
}
void SharePublisher::tick(std::array<bool,3> primeTracks) {
    if(state_!=State::Starting&&state_!=State::Sharing)return;
    if(gate_->closed()){if(gate_->failed())fail(audioFailed_?"Audio publication failed or exceeded its input age":"Publication permission revoked");else stop();return;}
    const char* operation="video_feedback";int height=0;std::uint64_t began=nowUs(),capture=0;
    try {
        for(std::size_t i=0;i<outputs_.size();i++)if(auto& output=outputs_[i];output){height=heights[i];operation="video_feedback";began=nowUs();output->updateFeedback();operation="video_poll";began=nowUs();output->poll();}
        if(state_==State::Starting) {
            if(!allowed(true)){fail("Encrypted connection unavailable");return;}
            if(gate_->ready()){
                for(auto& output:outputs_)output->requestKeyframe();state_=State::Sharing;
                // Synthetic startup canvases are never needed by this one-shot
                // generation again. clear() alone would retain ~4.3 MiB.
                for(auto& black:black_)std::vector<std::uint8_t>().swap(black);
                if(audio_)audioWorker_=std::jthread([this](std::stop_token stop){runAudio(stop);});
                return;
            }
            const auto now=Clock::now();if(now>=deadline_){fail("Per-track encryption readiness timed out; captured content was blocked");return;}
            if(now>=nextPrimeVideo_) {
                const auto timestamp=nowUs();
                for(std::size_t i=0;i<2;i++)if(primeTracks[i]){operation="video_prime";height=heights[i];began=nowUs();capture=timestamp;outputs_[i]->submit(black_[i],{timestamp,std::numeric_limits<std::uint32_t>::max(),true});}
                lastVideo_=timestamp;nextPrimeVideo_=now+std::chrono::microseconds(16667);
            }
            if(audio_&&primeTracks[2]&&now>=nextPrimeAudio_){operation="audio_prime";height=0;began=nowUs();capture=0;if(!allowed(true))throw std::runtime_error("Priming revoked");audio_->captureFrame(livekit::AudioFrame::create(48000,2,480),100);nextPrimeAudio_=now+std::chrono::milliseconds(10);}
            return;
        }
        if(!allowed(false)){fail("Outgoing encryption readiness lost; restart the share");return;}
        // The owned audio worker cannot be starved by video MFT/pacer waits.
        // Its input still has the same 100 ms bound and per-handoff gate.
        if(auto video=input_->takeVideo()) {
            if(video->identity.captureTimestampUs<=lastVideo_||!fresh(video->identity.captureTimestampUs,static_cast<std::uint64_t>(config_.maximumCaptureAgeUs))){expiredVideo_++;return;}
            for(std::size_t i=0;i<2;i++){operation="video_submit";height=heights[i];capture=video->identity.captureTimestampUs;began=nowUs();outputs_[i]->submit(video->nv12[i],video->identity);}
            lastVideo_=video->identity.captureTimestampUs;submittedVideo_++;
        }
    }catch(const std::exception& e){if(gate_->closed()&&!gate_->failed())stop();else {recordFailure(operation,publicationFailureCode(e.what()),began,capture,height);fail("Media publication failed; share stopped");}}
}
void SharePublisher::stop()noexcept {
    if(state_==State::Stopped&&!started_)return;
    if(state_==State::Stopped||state_==State::Failed)return;
    state_=State::Stopping;audioWorker_.request_stop();gate_->close();input_->close();
    if(audioWorker_.joinable())audioWorker_.join();
    try {
        finalOutputs_={};for(const auto& output:outputs_)if(output)finalOutputs_.append(output->report(true));
        if(const auto participant=room_.localParticipant().lock();participant&&room_.connectionState()==livekit::ConnectionState::Connected) {
            for(const auto& track:tracks_)try{participant->unpublishTrack(track->sid());}catch(...){error_="One or more publications did not unpublish cleanly";gate_->close(true);}
        }
    }catch(...){error_="Publication cleanup failed";gate_->close(true);}
    for(auto& output:outputs_)output.reset();audio_.reset();tracks_.clear();for(auto& black:black_)std::vector<std::uint8_t>().swap(black);
    state_=gate_->failed()?State::Failed:State::Stopped;
}
QJsonObject SharePublisher::report()const {
    QJsonObject failure;{std::lock_guard lock(failureMutex_);failure=firstFailure_;}
    QJsonArray outputs=finalOutputs_,tracks;
    if(outputs.isEmpty())for(const auto& output:outputs_)if(output)outputs.append(output->report());
    for(const auto& track:tracks_)tracks.append(QJsonObject{{"sid",QString::fromStdString(track->sid())},{"name",QString::fromStdString(track->name())}});
    const auto primingBytes=black_[0].capacity()+black_[1].capacity();
    return {{"state",static_cast<int>(state_)},{"error",error_},{"generation",static_cast<double>(config_.generation)},{"audio",config_.audio},
            {"priming_buffer_capacity_bytes",static_cast<double>(primingBytes)},
            {"tracks",tracks},{"gate",gate_->report()},{"input",input_->report()},{"outputs",outputs},{"first_publication_failure",failure},
            {"submitted_video_pairs",static_cast<double>(submittedVideo_)},{"submitted_audio_blocks",static_cast<double>(submittedAudio_.load())},
            {"expired_video",static_cast<double>(expiredVideo_)},{"expired_audio",static_cast<double>(expiredAudio_.load())},
            {"native_expired_audio_rejections",static_cast<double>(nativeExpiredAudio_.load())}};
}
void SharePublisher::publishSourceState(bool minimized) {
    // Advisory state for viewers: the share keeps running either way, so a
    // failed data packet is ignored instead of failing the publication.
    try {
        const auto local=room_.localParticipant().lock();
        if(!local)return;
        const auto payload=QJsonDocument(QJsonObject{{"share_id",config_.id},{"source_minimized",minimized}}).toJson(QJsonDocument::Compact);
        local->publishData(std::vector<std::uint8_t>(payload.begin(),payload.end()),true,{},"gs-source");
    } catch(...) {}
}}

