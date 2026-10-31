#include "track_reader.h"
#include "frame_metadata_codec.h"
#include <QJsonArray>
#include <QByteArray>
#include <chrono>
#include <mutex>
#include <thread>
#include <stdexcept>

namespace goodsharing {
namespace {
constexpr std::size_t videoQueueCapacity=4;
std::int64_t nowUs(){return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();}
}
struct TrackReader::Impl {
    struct Timing {std::int64_t begin=0,read=0,callbackEnd=0,sender=0;std::uint32_t rtp=0,epoch=0,reports=0;bool hasRtp=false;
        std::uint64_t capture=0;std::uint32_t frameId=0;bool identity=false,priming=false;int samples=0;};
    std::unique_ptr<std::vector<Timing>> testTiming;
    std::uint64_t omitted=0;
    Impl(){if(qEnvironmentVariableIsSet("GOODSHARING_TEST_AV_TRACE")){testTiming=std::make_unique<std::vector<Timing>>();testTiming->reserve(8192);}}
    template<class T> Timing timing(const T& event,std::int64_t begin)const{
        Timing e;e.begin=begin;e.read=nowUs();e.sender=event.sender_ntp_us.value_or(0);e.rtp=event.rtp_timestamp.value_or(0);
        e.hasRtp=event.rtp_timestamp.has_value();e.epoch=event.sender_clock_epoch.value_or(0);e.reports=event.rtcp_reports.value_or(0);return e;
    }
    void remember(Timing e){if(!testTiming)return;e.callbackEnd=nowUs();if(testTiming->size()<8192)testTiming->push_back(e);else omitted++;}
    std::shared_ptr<livekit::VideoStream> video;
    std::shared_ptr<livekit::AudioStream> audio;
    std::jthread reader;
    mutable std::mutex mutex;
    std::string error;
    QJsonObject completed;
    bool started=false;
    void failed(const char* message){std::lock_guard lock(mutex);error=message;}
};
TrackReader::TrackReader():impl_(std::make_unique<Impl>()){}
TrackReader::~TrackReader(){stop();}
void TrackReader::startVideo(const std::shared_ptr<livekit::Track>& track,VideoCallback callback) {
    auto& p=*impl_;
    if(p.started)throw std::logic_error("A track reader is one-shot");
    if(!track||track->kind()!=livekit::TrackKind::KIND_VIDEO||!callback)throw std::invalid_argument("Invalid video reader");
    p.started=true;
    livekit::VideoStream::Options options;options.capacity=videoQueueCapacity;options.format=livekit::VideoBufferType::RGBA;
    p.video=livekit::VideoStream::fromTrack(track,options);
    p.reader=std::jthread([&p,callback=std::move(callback)](std::stop_token stop) {
        try {livekit::VideoFrameEvent event;while(!stop.stop_requested()) {
            const auto begin=p.testTiming?nowUs():0;if(!p.video->read(event)||stop.stop_requested())break;
            Impl::Timing timing;if(p.testTiming){timing=p.timing(event,begin);
                if(event.metadata&&event.metadata->user_data)if(const auto id=decodeFrameIdentity(*event.metadata->user_data)){
                    timing.identity=true;timing.capture=id->captureTimestampUs;timing.frameId=id->frameId;timing.priming=id->priming;}}
            callback(event);p.remember(timing);
        }}
        catch(const std::exception& e){p.failed(e.what());}catch(...){p.failed("Unknown video reader failure");}
    });
}
void TrackReader::startAudio(const std::shared_ptr<livekit::Track>& track,AudioCallback callback,unsigned channels) {
    auto& p=*impl_;
    if(p.started)throw std::logic_error("A track reader is one-shot");
    if(!track||track->kind()!=livekit::TrackKind::KIND_AUDIO||!callback||(channels!=1&&channels!=2))throw std::invalid_argument("Invalid audio reader");
    p.started=true;
    livekit::AudioStream::Options options;options.capacity=8;options.sample_rate=48000;options.num_channels=channels;
    p.audio=livekit::AudioStream::fromTrack(track,options);
    p.reader=std::jthread([&p,callback=std::move(callback)](std::stop_token stop) {
        try {livekit::AudioFrameEvent event;while(!stop.stop_requested()) {
            const auto begin=p.testTiming?nowUs():0;if(!p.audio->read(event)||stop.stop_requested())break;
            Impl::Timing timing;if(p.testTiming){timing=p.timing(event,begin);timing.samples=event.frame.samplesPerChannel();}
            callback(event);p.remember(timing);
        }}
        catch(const std::exception& e){p.failed(e.what());}catch(...){p.failed("Unknown audio reader failure");}
    });
}
void TrackReader::stop() {
    auto& p=*impl_;
    if(p.reader.joinable()) {
        p.reader.request_stop();
        if(p.video)p.video->close();if(p.audio)p.audio->close();
        p.reader.join();
    }
    if(p.video) {
        const auto d=p.video->diagnostics();
        std::lock_guard lock(p.mutex);
        p.completed={{"frames_received",static_cast<double>(d.frames_received)},{"frames_read",static_cast<double>(d.frames_read)},
            {"frames_dropped",static_cast<double>(d.frames_dropped)},{"queue_high_water",static_cast<double>(d.queue_high_water)},{"queue_capacity",static_cast<int>(videoQueueCapacity)}};
    }
    if(p.testTiming){
        QJsonArray rows;for(const auto& e:*p.testTiming)rows.append(QJsonObject{{"read_begin_us",static_cast<double>(e.begin)},
            {"read_end_us",static_cast<double>(e.read)},{"callback_end_us",static_cast<double>(e.callbackEnd)},
            {"sender_us",static_cast<double>(e.sender)},{"rtp",static_cast<double>(e.rtp)},{"has_rtp",e.hasRtp},
            {"epoch",static_cast<double>(e.epoch)},{"reports",static_cast<double>(e.reports)},
            {"capture_us",static_cast<double>(e.capture)},{"frame_id",static_cast<double>(e.frameId)},
            {"has_identity",e.identity},{"priming",e.priming},{"samples",e.samples}});
        std::lock_guard lock(p.mutex);p.completed["kind"]=p.video?"video":"audio";p.completed["test_read_timing"]=rows;
        p.completed["test_trace_capacity"]=8192;p.completed["test_trace_omitted"]=static_cast<double>(p.omitted);p.testTiming.reset();
    }
    p.video.reset();p.audio.reset();
}
std::string TrackReader::error()const{std::lock_guard lock(impl_->mutex);return impl_->error;}
QJsonObject TrackReader::report()const{std::lock_guard lock(impl_->mutex);return impl_->completed;}
}
