#include "encoded_video_output.h"
#include "frame_pacer.h"
#include "encoded_delivery.h"
#include "raw_frame_budget.h"
#include "periodic_keyframe.h"
#include <chrono>
#include <stdexcept>
#include <QJsonArray>
#include <QByteArray>

namespace goodsharing {
namespace {std::uint64_t nowUs(){return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();}}
struct EncodedVideoOutput::Impl {
    struct Handoff {std::uint64_t capture,begin,end;std::uint32_t id;bool priming;};
    std::unique_ptr<std::vector<Handoff>> testHandoffs;
    std::uint64_t testHandoffsOmitted=0;
    int width,height,initialBitrate,handoffs=0,keyframes=0;
    std::uint64_t targetBitrate=0;
    std::uint64_t lastCaptureTimestamp=0;
    bool paced;
    MetadataTransform transform;
    Observer observer;
    ContentCheck check;
    BeforeHandoff beforeHandoff;
    std::shared_ptr<livekit::EncodedVideoSource> source;
    std::shared_ptr<livekit::LocalVideoTrack> track;
    std::map<std::int64_t,FrameIdentity> pending;
    FramePacer pacer{60};
    EncodedDelivery delivery;
    RawFrameBudget rawBudget;
    PeriodicKeyframe periodicKeyframe;
    std::uint64_t configuredBitrate,lastRateKeyframeRequestUs=0,rateKeyframeRequests=0,lastSdkKeyframeRequestUs=0,sdkKeyframeRequests=0,sdkKeyframeSuppressed=0;
    std::uint64_t bitrateFloorHits=0,bitrateFloorRelaxed=0;
    std::uint64_t belowFloorSinceUs=0;
    std::uint64_t lateWindowStartUs=0,windowFrames=0,lateFrames=0,strideCounter=0,fpsFallbacks=0;
    int inputStride=1;
    std::uint64_t recoveryKeyframeRequests=0;
    std::uint64_t revokedEncodedDrops=0;
    std::uint64_t maximumSdkHandoffUs=0;
    void recoverKeyframe(){if(delivery.takeKeyframeRequest()){encoder->requestKeyframe();recoveryKeyframeRequests++;}}
    void checkPendingAge(){if(!pending.empty()&&delivery.expired(pending.begin()->second.captureTimestampUs,nowUs()))throw std::runtime_error("Hardware encoder output exceeded its age budget");}
    // Destroy encoder first: its output callback refers to the storage above.
    std::unique_ptr<HardwareEncoder> encoder;
    int fps=60;
    Impl(int w,int h,int bitrate,std::string name,MetadataTransform t,Observer o,bool pace,ContentCheck c,std::chrono::milliseconds age,BeforeHandoff before,int framesPerSecond=60)
        :fps(framesPerSecond),width(w),height(h),initialBitrate(bitrate),paced(pace),transform(std::move(t)),observer(std::move(o)),check(std::move(c)),beforeHandoff(std::move(before)),delivery(age.count()<0||age.count()>1000?1000001:static_cast<std::uint64_t>(age.count())*1000),rawBudget(bitrate),configuredBitrate(bitrate) {
        if(qEnvironmentVariableIsSet("GOODSHARING_TEST_AV_TRACE")){testHandoffs=std::make_unique<std::vector<Handoff>>();testHandoffs->reserve(8192);}
        source=std::make_shared<livekit::EncodedVideoSource>(livekit::VideoCodec::H264,w,h);
        track=livekit::LocalVideoTrack::createLocalVideoTrack(name,source);
        encoder=std::make_unique<HardwareEncoder>(w,h,bitrate,[this](EncodedFrame frame) {
            const auto entry=pending.find(frame.timestampUs);
            if(entry==pending.end()) throw std::runtime_error("Encoder output has no admitted frame identity");
            const auto identity=entry->second; pending.erase(entry);
            if(beforeHandoff)beforeHandoff(identity);
            if(delivery.inspect(identity.captureTimestampUs,nowUs(),frame.keyframe)!=EncodedDelivery::Decision::Send)return;
            livekit::VideoFrameMetadata metadata;
            metadata.frame_id=identity.frameId; metadata.user_timestamp_us=identity.captureTimestampUs;
            if(transform) metadata=transform(std::move(metadata),identity.priming);
            else { metadata={};metadata.user_data=encodeFrameIdentity(identity); }
            livekit::EncodedVideoSource::Frame packet;
            packet.width=width;packet.height=height;packet.is_keyframe=frame.keyframe;
            packet.timestamp_us=frame.timestampUs;packet.data=std::move(frame.bytes);packet.metadata=std::move(metadata);
            if(paced) pacer.wait();
            if(check&&!check(identity.priming)){revokedEncodedDrops++;throw std::runtime_error("Outgoing video permission revoked");}
            const auto handoffStart=nowUs();
            if(delivery.inspect(identity.captureTimestampUs,handoffStart,frame.keyframe)!=EncodedDelivery::Decision::Send)return;
            if(!source->captureFrame(packet)) throw std::runtime_error("Encoded video handoff rejected; restart from a fresh keyframe");
            const auto handoffEnd=nowUs(),sdkTime=handoffEnd-handoffStart;if(sdkTime>maximumSdkHandoffUs)maximumSdkHandoffUs=sdkTime;
            if(testHandoffs){if(testHandoffs->size()<8192)testHandoffs->push_back({identity.captureTimestampUs,handoffStart,handoffEnd,identity.frameId,identity.priming});else testHandoffsOmitted++;}
            delivery.handedOff(identity.captureTimestampUs,handoffStart,frame.keyframe);
            if(delivery.budgetUs()&&!identity.priming)rawBudget.sent(packet.data.size(),nowUs());
            if(frame.keyframe)periodicKeyframe.accepted(identity.captureTimestampUs,packet.data.size());
            handoffs++;
            if(frame.keyframe)keyframes++;
            if(observer) {frame.bytes=std::move(packet.data);observer(frame,identity);}
        },fps);
    }
};
EncodedVideoOutput::EncodedVideoOutput(int w,int h,int bitrate,std::string name,MetadataTransform t,Observer o,bool pace,ContentCheck c,std::chrono::milliseconds age,BeforeHandoff before,int fps)
    :impl_(std::make_unique<Impl>(w,h,bitrate,std::move(name),std::move(t),std::move(o),pace,std::move(c),age,std::move(before),fps)){}
EncodedVideoOutput::~EncodedVideoOutput()=default;
bool EncodedVideoOutput::submit(const std::vector<std::uint8_t>& nv12,FrameIdentity identity) {
    auto& p=*impl_;
    if(p.check&&!p.check(identity.priming))throw std::runtime_error("Outgoing video permission revoked");
    if(identity.captureTimestampUs<=p.lastCaptureTimestamp||identity.captureTimestampUs>static_cast<std::uint64_t>(INT64_MAX/10))
        throw std::invalid_argument("Invalid capture timestamp");
    p.lastCaptureTimestamp=identity.captureTimestampUs;
    const auto timestamp=static_cast<std::int64_t>(identity.captureTimestampUs);
    // Polling first makes room for completions; never evict an identity for an
    // encoded reference frame. A stuck encoder is a terminal bounded failure.
    p.encoder->poll();
    p.recoverKeyframe();p.checkPendingAge();
    // At 60 fps a machine or encoder that cannot keep up turns into a picture that
    // stalls and catches up. Measure the handoff lag instead of guessing: when a
    // sustained share of frames misses its age budget, halve the input cadence and
    // retry twice as fast later; a steady 30 fps reads better than a stuttering 60.
    if(p.fps>=50){
        const auto now=nowUs();
        if(p.delivery.expired(identity.captureTimestampUs,now)){
            p.lateFrames++;
            if(!p.lateWindowStartUs)p.lateWindowStartUs=now;
            if(now-p.lateWindowStartUs>=2000000){
                if(p.lateFrames*100>=p.windowFrames*20&&p.inputStride<2){p.inputStride=2;p.fpsFallbacks++;}
                else if(p.lateFrames*100<=p.windowFrames*5&&p.inputStride==1&&p.fpsFallbacks){p.fpsFallbacks++;}
                p.lateFrames=0;p.windowFrames=0;p.lateWindowStartUs=now;
            }
            return false; // raw input, before encoding
        }
        p.windowFrames++;
        if(p.inputStride>1&&(++p.strideCounter%p.inputStride)!=0)return false;
    }else if(p.delivery.expired(identity.captureTimestampUs,nowUs()))return false;
    if(p.delivery.budgetUs()&&!identity.priming&&!p.rawBudget.admit(nowUs()))return false;
    // Refresh by capture time, while accounting for the observed IDR cost.
    // Explicit receiver/recovery requests bypass this optional refresh policy.
    if(p.delivery.budgetUs()&&!identity.priming&&p.periodicKeyframe.request(identity.captureTimestampUs,p.targetBitrate?p.targetBitrate:p.configuredBitrate))p.encoder->requestKeyframe();
    if(p.pending.size()>=16) throw std::runtime_error("Hardware encoder backlog exceeded 16 admitted frames");
    if(!p.pending.emplace(timestamp,identity).second) throw std::invalid_argument("Duplicate capture timestamp");
    if(!p.encoder->submit(nv12,timestamp)) {p.pending.erase(timestamp);p.recoverKeyframe();return false;}
    p.recoverKeyframe();
    return true;
}
void EncodedVideoOutput::poll(){impl_->encoder->poll();impl_->recoverKeyframe();impl_->checkPendingAge();}
void EncodedVideoOutput::requestKeyframe(){impl_->encoder->requestKeyframe();}
livekit::EncodedVideoSource::Feedback EncodedVideoOutput::updateFeedback(std::uint64_t overrideBitrate) {
    auto& p=*impl_;auto feedback=p.source->takeFeedback();
    if(overrideBitrate) {if(!feedback.rate_control)feedback.rate_control.emplace();feedback.rate_control->target_bitrate_bps=overrideBitrate;}
    // A receiver asks for a keyframe whenever it loses a packet. Honouring every
    // request turns loss into a keyframe storm: each IDR is a burst that causes
    // more loss, and the viewer sees a freeze every couple of seconds. Cap the
    // rate; the periodic refresh still repairs a stale picture.
    if(feedback.keyframe_requested){
        const auto now=nowUs();
        if(now-p.lastSdkKeyframeRequestUs>=1000000){p.encoder->requestKeyframe();p.lastSdkKeyframeRequestUs=now;p.sdkKeyframeRequests++;}
        else p.sdkKeyframeSuppressed++;
    }
    if(feedback.rate_control&&feedback.rate_control->target_bitrate_bps) {
        // The transport estimate starts far below a screen-share budget (measured
        // 3.6 Mbps against a configured 10 Mbps) and would keep text soft. Keep a
        // floor relative to the configured rate, adapt above it, and count the
        // times the floor had to engage.
        // Keep the screen-share floor on healthy links, but follow a transport
        // estimate that stays below it: pinning 6 Mbps on a 2 Mbps uplink loses
        // frames instead of protecting quality.
        const auto floor=static_cast<std::uint64_t>(p.initialBitrate)*3/5;
        auto target=feedback.rate_control->target_bitrate_bps;
        const auto feedbackNowUs=nowUs();
        if(target<floor){
            if(!p.belowFloorSinceUs)p.belowFloorSinceUs=feedbackNowUs;
            if(feedbackNowUs-p.belowFloorSinceUs>2500000){p.bitrateFloorRelaxed++;}
            else target=floor;
        }else{p.belowFloorSinceUs=0;}
        const auto requested=std::max<std::uint64_t>(target,100000);
        if(requested>feedback.rate_control->target_bitrate_bps)p.bitrateFloorHits++;
        const auto now=nowUs();
        const bool cut=p.targetBitrate&&requested<p.targetBitrate*3/4;
        p.targetBitrate=requested;
        const auto configured=std::clamp<std::uint64_t>(requested,100000,20000000);
        if(configured!=p.configuredBitrate){p.encoder->setBitrate(configured);p.configuredBitrate=configured;}
        if(p.delivery.budgetUs()) {
            p.rawBudget.rate(requested,now);
            if(cut&&now-p.lastRateKeyframeRequestUs>=500000){p.encoder->requestKeyframe();p.lastRateKeyframeRequestUs=now;p.rateKeyframeRequests++;}
        }
    }
    return feedback;
}
std::shared_ptr<livekit::LocalVideoTrack> EncodedVideoOutput::track()const{return impl_->track;}
livekit::TrackPublishOptions EncodedVideoOutput::publishOptions()const {
    livekit::TrackPublishOptions options;
    options.video_codec=livekit::VideoCodec::H264;options.simulcast=false;
    options.video_encoding=livekit::VideoEncodingOptions{static_cast<std::uint64_t>(impl_->initialBitrate),60};
    options.video_encoder=livekit::VideoEncoderBackend::PreEncoded;
    options.source=livekit::TrackSource::SOURCE_SCREENSHARE;
    // Sharp text beats motion smoothness when the link is short: keep resolution
    // and let the encoder drop frames instead of blurring the picture.
    options.degradation_preference=livekit::DegradationPreference::MaintainResolution;
    livekit::FrameMetadataFeatures metadata;metadata.user_data=true;options.frame_metadata_features=metadata;
    return options;
}
QJsonObject EncodedVideoOutput::report(bool includeTestTrace)const {
    const auto& p=*impl_;
    QJsonArray trace;if(includeTestTrace&&p.testHandoffs)for(const auto& e:*p.testHandoffs)trace.append(QJsonObject{
        {"capture_us",static_cast<double>(e.capture)},{"handoff_begin_us",static_cast<double>(e.begin)},{"handoff_end_us",static_cast<double>(e.end)},
        {"frame_id",static_cast<double>(e.id)},{"priming",e.priming}});
    QJsonObject result{{"name",p.encoder->name()},{"width",p.width},{"height",p.height},{"frames_encoded",p.encoder->framesEncoded()},
            {"last_capture_us",static_cast<double>(p.lastCaptureTimestamp)},
            {"oldest_pending_capture_us",static_cast<double>(p.pending.empty()?0:p.pending.begin()->second.captureTimestampUs)},
            {"frames_handed_off",p.handoffs},{"pending_frames",static_cast<int>(p.pending.size())},
            {"keyframes_handed_off",p.keyframes},
            {"age_budget_ms",static_cast<double>(p.delivery.budgetUs())/1000},{"stale_encoded_drops",static_cast<double>(p.delivery.stale())},
            {"dependent_recovery_drops",static_cast<double>(p.delivery.dependent())},{"keyframe_recoveries",static_cast<double>(p.delivery.recoveries())},
            {"revoked_encoded_drops",static_cast<double>(p.revokedEncodedDrops)},
            {"recovery_keyframe_requests",static_cast<double>(p.recoveryKeyframeRequests)},{"recovering",p.delivery.recovering()},
            {"maximum_handoff_start_age_ms",static_cast<double>(p.delivery.maximumHandoffAgeUs())/1000},
            {"maximum_sdk_handoff_call_ms",static_cast<double>(p.maximumSdkHandoffUs)/1000},
            {"configured_encoder_bitrate_bps",static_cast<double>(p.configuredBitrate)},
            {"rate_limited_raw_frames",static_cast<double>(p.rawBudget.limited())},{"encoded_payload_bytes_handed_off",static_cast<double>(p.rawBudget.payloadBytes())},
            {"raw_rate_debt_bytes",p.rawBudget.debtBytes()},{"bitrate_cut_keyframe_requests",static_cast<double>(p.rateKeyframeRequests)},{"sdk_keyframe_requests",static_cast<double>(p.sdkKeyframeRequests)},{"sdk_keyframe_suppressed",static_cast<double>(p.sdkKeyframeSuppressed)},
            {"periodic_keyframe_requests",static_cast<double>(p.periodicKeyframe.requests())},
            {"target_bitrate_bps",static_cast<double>(p.targetBitrate)},
            {"bitrate_floor_hits",static_cast<double>(p.bitrateFloorHits)},
            {"bitrate_floor_relaxed",static_cast<double>(p.bitrateFloorRelaxed)},
            {"input_stride",static_cast<double>(p.inputStride)},
            {"fps_fallbacks",static_cast<double>(p.fpsFallbacks)}};
    if(includeTestTrace&&p.testHandoffs){result["test_handoffs"]=trace;result["test_handoffs_omitted"]=static_cast<double>(p.testHandoffsOmitted);result["test_trace_capacity"]=8192;}
    return result;
}
}
