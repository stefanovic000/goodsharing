#include "remote_audio_playout.h"
#include "frame_metadata_codec.h"
#include <chrono>
#include <cmath>
#include <stdexcept>
#include <algorithm>
#include <QJsonArray>

namespace goodsharing {
namespace {
std::int64_t localNow(){return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();}
template<class T> PlayoutClock stamp(const T& event){return {event.sender_ntp_us.value_or(0),event.rtp_timestamp.value_or(0),event.sender_clock_epoch.value_or(0),event.rtcp_reports.value_or(0),event.rtp_timestamp.has_value()};}
}
RemoteAudioPlayout::RemoteAudioPlayout(AudioPlayoutSink& sink,LocalClock clock,std::shared_ptr<RemoteMediaSelection> selection):sink_(sink),clock_(clock?std::move(clock):LocalClock{localNow}),selection_(selection?std::move(selection):std::make_shared<RemoteMediaSelection>()){}
RemoteAudioPlayout::~RemoteAudioPlayout(){stop();}
RemoteAudioPlayout::ResetTrace RemoteAudioPlayout::resetContext(int kind,PlayoutClock clock,std::int64_t now,const std::shared_ptr<RemoteShare>& video)const{
    if(!traceEnabled_)return {};
    const auto stats=core_.stats();
    ResetTrace event{kind,video?video->manifest().videoSid:QString{},now,clock.senderUs,core_.localFor(clock.senderUs).value_or(0),core_.lastVideoArrival(),
        clock.epoch,kind==1?core_.lastAudioClock().epoch:core_.lastVideoClock().epoch,stats.resetSerial};
    event.videoSender=core_.lastVideoClock().senderUs;event.videoDeadline=core_.videoDeadline().value_or(0);event.presented=presentedLocal_;
    event.reports=clock.reports;event.hasRtp=clock.hasRtp;event.anchored=stats.anchored;event.locked=stats.alignmentLocked;
    event.previousOutputEnd=core_.lastOutputEnd();
    event.rejectedBefore=stats.alignmentRejected;event.headroomBefore=stats.sustainedHeadroomResets;event.earlyBefore=stats.earlyVideoDrops;
    event.pumpGapUs=pumpGapUs_;event.sinceCommitUs=lastCommitUs_?now-lastCommitUs_:0;return event;
}
void RemoteAudioPlayout::rememberReset(ResetTrace event){
    if(!traceEnabled_)return;
    const bool unavailable=!event.hasRtp||event.reports<2||!event.epoch||event.sender<=0;
    const bool sampleUnavailable=unavailable&&core_.stats().unclocked&&core_.stats().unclocked%128==0;
    if(event.kind!=4&&!sampleUnavailable&&event.before==core_.stats().resetSerial&&event.rejectedBefore==core_.stats().alignmentRejected&&event.earlyBefore==core_.stats().earlyVideoDrops)return;
    if(!resetTrace_)resetTrace_=std::make_unique<std::array<ResetTrace,16>>();
    const auto stats=core_.stats();event.alignmentRejected=stats.alignmentRejected>event.rejectedBefore;
    event.headroomReset=stats.sustainedHeadroomResets>event.headroomBefore;
    event.earlyDropped=stats.earlyVideoDrops>event.earlyBefore;
    event.after=stats.resetSerial;event.media=committedMedia_;
    if(!firstFault_&&event.media&&(event.before!=event.after||event.kind==4)){
        firstFault_=std::make_unique<FirstFault>();firstFault_->event=event;firstFault_->count=traceCount_;
        for(std::size_t n=0;n<traceCount_;n++)firstFault_->preceding[n]=trace_[(traceCursor_+trace_.size()-traceCount_+n)%trace_.size()];
    }
    (*resetTrace_)[resetCursor_++%16]=std::move(event);resetCount_=std::min(resetCount_+1,std::size_t{16});
}
void RemoteAudioPlayout::trace(Trace event){if(!traceEnabled_)return;event.serial=core_.stats().resetSerial;trace_[traceCursor_++%trace_.size()]=event;traceCount_=std::min(traceCount_+1,trace_.size());}
void RemoteAudioPlayout::bind(const std::shared_ptr<RemoteShare>& watch){
    std::lock_guard lock(mutex_);
    if(bound_||closed_||!watch||watch->manifest().audioSid.isEmpty()||watch->manifest().videoSid.isEmpty())throw std::invalid_argument("Audio playout requires one fixed audio/video watch");
    watch_=watch;bound_=true;
}
void RemoteAudioPlayout::bindMedia(const std::shared_ptr<RemoteShare>& audio,const std::shared_ptr<RemoteShare>& video){
    std::lock_guard lock(mutex_);
    if(bound_||closed_||!presentationRequired_)throw std::invalid_argument("Split media requires coordinated presentation before binding");
    selection_->bind(audio,video);watch_=audio;split_=bound_=true;
}
bool RemoteAudioPlayout::admit(bool audio,const std::function<void()>& operation)const{
    if(split_)return selection_->presentActive(operation);
    const auto watch=watch_.lock();return watch&&(audio?watch->presentAudio(operation):watch->presentVideo(operation));
}
bool RemoteAudioPlayout::presentVideo(const std::shared_ptr<RemoteShare>& video,const std::function<void()>& operation)const{
    return selection_->presentVideo(video,operation);
}
bool RemoteAudioPlayout::stageVideo(const std::shared_ptr<RemoteShare>& video){
    std::lock_guard lock(mutex_);
    if(!split_||closed_||failed_||!selection_->stage(video))return false;
    if(++candidateAttempt_==0)++candidateAttempt_;return true;
}
bool RemoteAudioPlayout::cancelVideo(const std::shared_ptr<RemoteShare>& video){
    std::lock_guard lock(mutex_);return split_&&selection_->cancel(video);
}
PresentationTicket RemoteAudioPlayout::activeTicket(PlayoutClock clock)const{
    PresentationTicket ticket{*core_.videoDeadline(),clock.senderUs,core_.stats().resetSerial};
    if(const auto deadline=core_.videoPresentationDeadline();deadline&&*deadline<ticket.deadlineUs){
        ticket.timelineDeadlineUs=ticket.deadlineUs;ticket.deadlineUs=*deadline;
        ticket.receivedUs=core_.lastVideoArrival();ticket.candidateClock=clock;
    }
    return ticket;
}
std::optional<PresentationTicket> RemoteAudioPlayout::scheduleVideo(const std::shared_ptr<RemoteShare>& video,PlayoutClock clock){
    std::lock_guard lock(mutex_);
    if(!split_||!video||selection_->selected()!=video||!videoLocked(clock))return {};
    return activeTicket(clock);
}
std::optional<PresentationTicket> RemoteAudioPlayout::scheduleVideo(const std::shared_ptr<RemoteShare>& video,livekit::VideoFrameEvent& frame){
    if(!video)return {};
    const auto identity=frame.metadata&&frame.metadata->user_data?decodeFrameIdentity(*frame.metadata->user_data):std::nullopt;
    if(identity&&identity->priming)return {};
    std::lock_guard lock(mutex_);if(!split_||closed_)return {};
    const bool active=selection_->selected()==video,candidate=selection_->candidate()==video;
    if(!active&&!candidate)return {};
    const int height=video->manifest().quality==ShareTrack::Video1080?1080:720,width=height==1080?1920:1280;
    if(!identity||frame.rotation!=livekit::VideoRotation::VIDEO_ROTATION_0||frame.frame.type()!=livekit::VideoBufferType::RGBA||
       frame.frame.width()!=width||frame.frame.height()!=height||frame.frame.dataSize()!=static_cast<std::size_t>(width)*height*4){if(active)videoLocked({});return {};}
    if(active){const auto clock=stamp(frame);if(!videoLocked(clock))return {};return activeTicket(clock);}
    const auto projected=projectLocked(video,stamp(frame));if(!projected)return {};
    auto ticket=projected->presentation;ticket.candidateClock=projected->clock;ticket.receivedUs=projected->receivedUs;ticket.attempt=projected->attempt;return ticket;
}
bool RemoteAudioPlayout::ticketValid(const std::shared_ptr<RemoteShare>& video,PresentationTicket ticket)const{
    std::lock_guard lock(mutex_);if(!split_||!video)return false;
    if(selection_->selected()==video)return validLocked(ticket)&&selection_->presentVideo(video,[]{});
    return ticket.attempt&&candidateValidLocked(video,{ticket,ticket.candidateClock,ticket.receivedUs,ticket.attempt});
}
std::optional<RemoteAudioPlayout::CandidateTicket> RemoteAudioPlayout::projectLocked(const std::shared_ptr<RemoteShare>& video,PlayoutClock clock){
    if(!split_||closed_||failed_||!video||selection_->candidate()!=video)return {};
    std::optional<CandidateTicket> ticket;
    selection_->presentVideo(video,[&]{const auto now=clock_();if(const auto deadline=core_.projectVideo(clock,now))
        ticket=CandidateTicket{{*deadline,clock.senderUs,core_.stats().resetSerial},clock,now,candidateAttempt_};});
    return ticket;
}
std::optional<RemoteAudioPlayout::CandidateTicket> RemoteAudioPlayout::projectVideo(const std::shared_ptr<RemoteShare>& video,PlayoutClock clock){
    std::lock_guard lock(mutex_);return projectLocked(video,clock);
}
std::optional<RemoteAudioPlayout::CandidateTicket> RemoteAudioPlayout::projectVideo(const std::shared_ptr<RemoteShare>& video,livekit::VideoFrameEvent& frame){
    const auto identity=frame.metadata&&frame.metadata->user_data?decodeFrameIdentity(*frame.metadata->user_data):std::nullopt;
    if(!video||!identity||identity->priming)return {};
    const int height=video->manifest().quality==ShareTrack::Video1080?1080:720,width=height==1080?1920:1280;
    if(frame.rotation!=livekit::VideoRotation::VIDEO_ROTATION_0||frame.frame.type()!=livekit::VideoBufferType::RGBA||
       frame.frame.width()!=width||frame.frame.height()!=height||frame.frame.dataSize()!=static_cast<std::size_t>(width)*height*4)return {};
    std::lock_guard lock(mutex_);return projectLocked(video,stamp(frame));
}
bool RemoteAudioPlayout::candidateValidLocked(const std::shared_ptr<RemoteShare>& video,const CandidateTicket& ticket)const{
    const auto now=clock_();
    return split_&&video&&selection_->candidate()==video&&ticket.attempt==candidateAttempt_&&validLocked(ticket.presentation)&&
        ticket.clock.valid()&&ticket.clock.senderUs==ticket.presentation.senderUs&&ticket.receivedUs>0&&
        now>=ticket.receivedUs&&now-ticket.receivedUs<=AudioPlayout::videoLifetimeUs&&selection_->presentVideo(video,[]{});
}
bool RemoteAudioPlayout::candidateValid(const std::shared_ptr<RemoteShare>& video,const CandidateTicket& ticket)const{
    std::lock_guard lock(mutex_);return candidateValidLocked(video,ticket);
}
bool RemoteAudioPlayout::adoptVideo(const std::shared_ptr<RemoteShare>& video,const CandidateTicket& ticket){
    std::lock_guard lock(mutex_);return adoptLocked(video,ticket);
}
bool RemoteAudioPlayout::adoptLocked(const std::shared_ptr<RemoteShare>& video,const CandidateTicket& ticket){
    if(!candidateValidLocked(video,ticket))return false;
    const auto now=clock_();
    if(!selection_->adopt(video,[&]{return core_.adoptVideo(ticket.clock,ticket.receivedUs,now,ticket.presentation.deadlineUs,ticket.presentation.serial);}))return false;
    presentedSerial_=ticket.presentation.serial;presentedSender_=ticket.clock.senderUs;presentedLocal_=now;presentedFrames_++;videoHandovers_++;return true;
}
void RemoteAudioPlayout::denied(const std::shared_ptr<RemoteShare>& watch){
    denied_++;core_.resetTiming();
    if(!watch||watch->closed()||(split_&&selection_->closed())){closed_=true;core_.revoke();}
}
bool RemoteAudioPlayout::video(PlayoutClock clock){
    std::lock_guard lock(mutex_);return !split_&&videoLocked(clock);
}
bool RemoteAudioPlayout::videoLocked(PlayoutClock clock){
    if(closed_||!bound_)return false;
    const auto watch=watch_.lock();bool accepted=false;
    const auto selected=split_?selection_->selected():watch;
    if(!admit(false,[&]{const auto now=clock_();auto event=resetContext(2,clock,now,selected);
        accepted=core_.video(clock,now,presentationRequired_);rememberReset(std::move(event));
        if(split_&&accepted)trace({2,now,clock.senderUs,*core_.videoDeadline(),core_.frontDeadline().value_or(0),core_.stats().queued});}))denied(watch);
    return accepted;
}
bool RemoteAudioPlayout::audio(PlayoutClock clock,std::span<const std::int16_t> pcm){
    std::lock_guard lock(mutex_);if(closed_||!bound_)return false;
    for(std::size_t i=0;i+1<pcm.size();i+=2){
        const auto left=static_cast<double>(pcm[i]),right=static_cast<double>(pcm[i+1]);
        decodedAudioSignalAbs_+=(std::abs(left)+std::abs(right))/2;
        decodedAudioDifferenceAbs_+=std::abs(left-right);
    }
    const auto watch=watch_.lock();bool accepted=false;
    const auto selected=split_?selection_->selected():watch;
    if(!admit(true,[&]{const auto now=clock_();auto event=resetContext(1,clock,now,selected);
        accepted=core_.audio(clock,pcm,now);rememberReset(std::move(event));
        if(accepted)if(const auto deadline=core_.audioDeadline()){audioLeads_[audioLeadCursor_++%audioLeads_.size()]=*deadline-now;audioLeadCount_=std::min(audioLeadCount_+1,audioLeads_.size());}
        trace({1,now,clock.senderUs,core_.audioDeadline().value_or(0),core_.frontDeadline().value_or(0),core_.stats().queued});
    }))denied(watch);
    return accepted;
}
bool RemoteAudioPlayout::video(livekit::VideoFrameEvent& frame){
    return scheduleVideo(frame).has_value();
}
std::optional<PresentationTicket> RemoteAudioPlayout::scheduleVideo(livekit::VideoFrameEvent& frame){
    const auto identity=frame.metadata&&frame.metadata->user_data?decodeFrameIdentity(*frame.metadata->user_data):std::nullopt;
    if(identity&&identity->priming)return {};
    std::lock_guard lock(mutex_);if(split_)return {};const auto watch=watch_.lock();
    const int height=watch&&watch->manifest().quality==ShareTrack::Video1080?1080:720,width=height==1080?1920:1280;
    if(!identity||frame.rotation!=livekit::VideoRotation::VIDEO_ROTATION_0||frame.frame.type()!=livekit::VideoBufferType::RGBA||
       frame.frame.width()!=width||frame.frame.height()!=height||frame.frame.dataSize()!=static_cast<std::size_t>(width)*height*4){videoLocked({});return {};}
    const auto clock=stamp(frame);if(!videoLocked(clock))return {};
    trace({2,clock_(),clock.senderUs,*core_.videoDeadline(),core_.frontDeadline().value_or(0),core_.stats().queued});
    return activeTicket(clock);
}
std::optional<PresentationTicket> RemoteAudioPlayout::scheduleVideo(PlayoutClock clock){
    std::lock_guard lock(mutex_);if(split_||!videoLocked(clock))return {};
    return activeTicket(clock);
}
void RemoteAudioPlayout::setLogger(std::function<void(const QString&)> logger){std::lock_guard lock(mutex_);logger_=std::move(logger);}
void RemoteAudioPlayout::logGap(const char* reason,std::int64_t detailMs){
    const auto now=clock_();
    if(lastGapLogUs_&&now-lastGapLogUs_<1000000)return;
    lastGapLogUs_=now;
    if(logger_)logger_(QString("audio gap: %1 (%2 ms, leadWaits=%3 resets=%4 endGaps=%5)")
        .arg(reason).arg(detailMs).arg(presentationLeadWaits_).arg(presentationResets_).arg(endpointGapResets_));
}
void RemoteAudioPlayout::setGain(float gain){
    std::lock_guard lock(mutex_);
    core_.setGain(gain);
}
void RemoteAudioPlayout::configureTiming(std::int64_t leadUs,std::int64_t videoWaitLimitUs){
    std::lock_guard lock(mutex_);
    horizonUs_=std::max<std::int64_t>(80000,leadUs+40000);
    if(bound_)throw std::logic_error("Playout timing must precede binding");
    // The lead is the presentation delay (stream_profile.h derives one from the
    // other), so it doubles as the delayed-timeline buffer. The test hook keeps
    // the old source for an A/B control on the same binary.
    bufferUs_=qEnvironmentVariableIntValue("GOODSHARING_TEST_OLD_TIMELINE")?0:leadUs;
    core_.configureTiming(leadUs,videoWaitLimitUs,bufferUs_);
}
std::optional<std::int64_t> RemoteAudioPlayout::delayedSender(std::int64_t now)const{
    if(!bufferUs_)return {};
    return core_.senderAt(now);
}
void RemoteAudioPlayout::requirePresentation(){std::lock_guard lock(mutex_);if(bound_)throw std::logic_error("Presentation mode must precede binding");presentationRequired_=true;core_.enableAudioAlignment();}
void RemoteAudioPlayout::enableAudioContinuity(){std::lock_guard lock(mutex_);if(bound_)throw std::logic_error("Audio continuity must precede binding");audioContinuity_=true;core_.enableAudioContinuity();}
bool RemoteAudioPlayout::validLocked(PresentationTicket ticket)const{
    if(closed_||failed_||!core_.stats().anchored||ticket.serial!=core_.stats().resetSerial||ticket.senderUs<=0)return false;
    if(!ticket.timelineDeadlineUs)return core_.senderAt(ticket.deadlineUs)==ticket.senderUs;
    if(ticket.timelineDeadlineUs<=0||ticket.timelineDeadlineUs>1000000000000000ll||
       ticket.deadlineUs<=0||ticket.deadlineUs>1000000000000000ll||
       ticket.receivedUs<=0||ticket.receivedUs>1000000000000000ll)return false;
    const auto now=clock_(),advance=ticket.timelineDeadlineUs-ticket.deadlineUs;
    return presentationRequired_&&core_.stats().alignmentLocked&&!ticket.attempt&&
        ticket.candidateClock.valid()&&ticket.candidateClock.senderUs==ticket.senderUs&&
        ticket.candidateClock.epoch==core_.lastVideoClock().epoch&&
        core_.senderAt(ticket.timelineDeadlineUs)==ticket.senderUs&&
        ticket.receivedUs>0&&now>=ticket.receivedUs&&now-ticket.receivedUs<=AudioPlayout::videoLifetimeUs&&
        ticket.deadlineUs==ticket.receivedUs+AudioPlayout::maximumPresentationLeadUs&&
        advance>0&&advance<=AudioPlayout::maximumVideoAdvanceUs;
}
bool RemoteAudioPlayout::ticketValid(PresentationTicket ticket)const{std::lock_guard lock(mutex_);return validLocked(ticket);}
bool RemoteAudioPlayout::hasTimeline()const{std::lock_guard lock(mutex_);return !closed_&&!failed_&&core_.stats().anchored;}
bool RemoteAudioPlayout::needsDevicePolling()const{
    std::lock_guard lock(mutex_);const auto s=core_.stats();
    const bool audioPriority=presentationRequired_&&audioContinuity_&&presentedFrames_>0;
    return !closed_&&!failed_&&s.anchored&&(!presentationRequired_||audioPriority||(presentedSerial_==s.resetSerial&&(s.alignmentLocked||s.queued)));
}
bool RemoteAudioPlayout::presented(PresentationTicket ticket){
    std::lock_guard lock(mutex_);return !split_&&presentedLocked(ticket);
}
bool RemoteAudioPlayout::presented(const std::shared_ptr<RemoteShare>& video,PresentationTicket ticket){
    std::lock_guard lock(mutex_);if(!split_||!video)return false;
    if(selection_->selected()==video)return presentedLocked(ticket);
    return ticket.attempt&&adoptLocked(video,{ticket,ticket.candidateClock,ticket.receivedUs,ticket.attempt});
}
bool RemoteAudioPlayout::presentedLocked(PresentationTicket ticket){
    const auto now=clock_();
    if(!presentationRequired_||!validLocked(ticket)||now<ticket.deadlineUs||now-ticket.deadlineUs>80000||!admit(false,[]{}))return false;
    if(ticket.timelineDeadlineUs)advancedVideoPresentations_++;
    presentedSerial_=ticket.serial;presentedSender_=ticket.senderUs;presentedLocal_=now;presentedFrames_++;return true;
}
bool RemoteAudioPlayout::audio(livekit::AudioFrameEvent& frame){
    if(frame.frame.sampleRate()!=48000||frame.frame.numChannels()!=2||frame.frame.samplesPerChannel()!=480)return audio({},{});
    return audio(stamp(frame),frame.frame.data());
}
bool RemoteAudioPlayout::flush(){
    output_.fill(0);const auto serial=core_.stats().resetSerial;
    if(!sink_.reset()){failed_=closed_=true;selection_->stop();core_.revoke();return false;}
    deviceResets_++;return core_.acknowledgeReset(serial);
}
RemoteAudioPlayout::Pump RemoteAudioPlayout::fail(){failed_=closed_=true;selection_->stop();core_.revoke();flush();return Pump::Failed;}
RemoteAudioPlayout::Pump RemoteAudioPlayout::pump(){
    std::lock_guard lock(mutex_);if(failed_)return Pump::Failed;if(stopped_)return Pump::Closed;
    if(traceEnabled_){const auto now=clock_();pumpGapUs_=previousPumpUs_?now-previousPumpUs_:0;previousPumpUs_=now;}
    const auto watch=watch_.lock();
    if(!bound_||!admit(true,[]{}))denied(watch);
    if(core_.stats().resetPending){if(!flush())return Pump::Failed;return closed_?Pump::Closed:Pump::Reset;}
    if(closed_)return Pump::Closed;
    if(!core_.stats().anchored)return Pump::Waiting;
    if(presentationRequired_&&!core_.stats().alignmentLocked&&!core_.stats().queued){presentationWaits_++;return Pump::Waiting;}
    const bool audioContinuity=presentationRequired_&&audioContinuity_&&presentedFrames_>0;
    // Delayed timeline: the audible sender position exists as soon as the common
    // clock is anchored, so neither a first-picture ticket nor the age of the last
    // paint gates the session. Each flush used to reset video as well and the two
    // then validated each other into the reset storm recorded in docs/70.
    const auto delayed=delayedSender(clock_());
    if(presentationRequired_){
        if(presentedSerial_!=core_.stats().resetSerial&&!audioContinuity&&!delayed){presentationWaits_++;return Pump::Waiting;}
        if(!audioContinuity&&presentedLocal_&&clock_()-presentedLocal_>80000){auto event=resetContext(5,core_.lastVideoClock(),clock_(),split_?selection_->selected():watch);
            core_.resetTiming();rememberReset(std::move(event));presentationResets_++;presentationAgeResets_++;if(!flush())return Pump::Failed;return Pump::Reset;}
    }
    // Reserve outside the watch lock. A revoke during preparation must win
    // the second admission check at the actual device commit below.
    std::optional<AudioPlayoutSink::Window> window;
    const auto prepareBegin=traceEnabled_?clock_():0;
    try{if(!startedWallUs_)startedWallUs_=clock_();window=sink_.prepare(clock_());}
    catch(const EndpointDiscontinuity& gap){sink_.cancel();
        if(!presentationRequired_||gap.gapUs<0||gap.gapUs>30000){logGap("device gap too long");return fail();}
        logGap("device underrun",gap.gapUs/1000);
        const auto now=clock_();auto event=resetContext(4,core_.lastVideoClock(),now,split_?selection_->selected():watch);
        event.endpointGapUs=gap.gapUs;event.prepareElapsedUs=traceEnabled_?clock_()-prepareBegin:0;
        const auto stats=core_.stats();const auto end=core_.lastOutputEnd();
        // A short device underrun does not invalidate trained source clocks or
        // already queued video. Flush the physical endpoint, retain the source
        // mapping and its output high-water mark, and let ordinary rendering
        // discard PCM whose original deadline is now late. Never retime/replay
        // it. Longer absence, invalid clocks and stale presentation still use
        // the full timeline fence above or below.
        const bool preserve=stats.anchored&&stats.alignmentLocked&&!stats.resetPending&&end>0&&
            now-end>=-30000&&now-end<=30000&&now-core_.lastVideoArrival()<=AudioPlayout::videoLifetimeUs&&
            now-presentedLocal_<=80000;
        if(preserve){endpointTimelinePreserved_++;event.deviceOnlyRecovery=true;}
        else core_.resetTiming();
        rememberReset(std::move(event));endpointGapResets_++;if(!flush())return Pump::Failed;return Pump::Reset;
    }catch(...){sink_.cancel();return fail();}
    if(!window){sink_.cancel();return Pump::Waiting;}
    if(!window->frames||window->frames>AudioPlayout::blockFrames){sink_.cancel();return fail();}
    auto permittedFrames=window->frames;
    // What should be audible now: the delayed timeline when it exists, otherwise
    // the last painted picture as before.
    const auto reference=delayed.value_or(presentedSender_);
    if(presentationRequired_&&!audioContinuity){
        // Validate the endpoint timestamp before adding the bounded packet
        // duration, including when a faulty sink returns an extreme value.
        auto source=core_.senderAt(window->firstAudibleUs);
        if(source)*source+=static_cast<std::int64_t>(window->frames-1)*1000000/48000;
        if(!source||*source-reference < -horizonUs_){
            sink_.cancel();core_.resetTiming();presentationResets_++;presentationLagResets_++;if(!flush())return Pump::Failed;return Pump::Reset;
        }
        if(*source-reference>horizonUs_){
            // Before the first picture is presented there is no anchor to wait for,
            // and waiting on one silenced the start of every share. Give video a
            // short head start, then let audio run on its own clock.
            if(!presentedLocal_){
                const auto now=clock_();
                if(!anchorWaitStartUs_)anchorWaitStartUs_=now;
                if(now-anchorWaitStartUs_<=300000){
                    sink_.cancel();presentationLeadWaits_++;return Pump::Waiting;
                }
                audioStartedWithoutVideo_++;
            }else{
                const auto first=core_.senderAt(window->firstAudibleUs);
                if(!first||*first-reference>horizonUs_){sink_.cancel();presentationLeadWaits_++;logGap("waiting for video lead");return Pump::Waiting;}
                // Commit only samples already within the unchanged A/V horizon.
                const auto margin=reference+horizonUs_-*first;
                permittedFrames=std::min(permittedFrames,static_cast<std::size_t>(margin*48000/1000000+1));
            }
        }else anchorWaitStartUs_=0;
    }
    auto output=std::span(output_).first(permittedFrames*2);
    const auto renderNow=clock_(),front=core_.frontDeadline().value_or(0);const auto queued=core_.stats().queued;
    ResetTrace rendering;
    if(traceEnabled_){rendering=resetContext(3,core_.lastVideoClock(),renderNow,split_?selection_->selected():watch);
        rendering.windowAudible=window->firstAudibleUs;rendering.previousOutputEnd=core_.lastOutputEnd();
        rendering.prepareElapsedUs=renderNow-prepareBegin;
        rendering.outputPast=window->firstAudibleUs<renderNow;
        rendering.outputOverlaps=rendering.previousOutputEnd&&window->firstAudibleUs<rendering.previousOutputEnd-1000;}
    const auto rendered=core_.render(renderNow,window->firstAudibleUs,output,presentationRequired_&&!audioContinuity?
        std::optional<AudioPlayout::SourceWindow>{{reference-horizonUs_,reference+horizonUs_}}:std::nullopt);
    if(traceEnabled_)rememberReset(std::move(rendering));
    trace({3,renderNow,0,window->firstAudibleUs,front,queued,rendered.mediaFrames,rendered.outputFrames});
    if(rendered.sourceLeadWait)presentationLeadWaits_++;
    if(rendered.sourceLagReset){presentationResets_++;presentationLagResets_++;}
    if(rendered.resetRequired){sink_.cancel();if(!flush())return Pump::Failed;return Pump::Reset;}
    if(!rendered.ready){
        // Starved cycle: repeat the last block for at most two windows so the device
        // never reads silence in the middle of speech.
        // Cover a short hiccup, never a sustained starvation: repeating content
        // inflates the endpoint's non-media share and hides the real fault.
        const auto holdNow=clock_();
        if(holdNow-holdBucketUs_>=1000000){holdBucketUs_=holdNow;holdsThisSecond_=0;}
        if(holdWindow_<1&&holdsThisSecond_<3&&sink_.repeatLast()){++holdWindow_;++holdsThisSecond_;return Pump::Committed;}
        sink_.cancel();return Pump::Waiting;
    }
    if(!rendered.outputFrames||rendered.outputFrames>permittedFrames){sink_.cancel();return fail();}
    output=output.first(rendered.outputFrames*2);
    std::int64_t avLag=0;
    if(presentationRequired_&&!audioContinuity){
        const auto source=rendered.mediaFrames?std::optional{rendered.lastSourceUs}:
            core_.senderAt(window->firstAudibleUs+static_cast<std::int64_t>(rendered.outputFrames-1)*1000000/48000);
        avLag=source?*source-reference:INT64_MAX;
        const auto firstLag=rendered.mediaFrames?rendered.firstSourceUs-reference:avLag;
        if(firstLag < -horizonUs_||firstLag>horizonUs_||avLag < -horizonUs_||avLag>horizonUs_){sink_.cancel();core_.resetTiming();presentationResets_++;presentationLagResets_++;if(!flush())return Pump::Failed;return Pump::Reset;}
        if(std::abs(firstLag)>std::abs(avLag))avLag=firstLag;
    }
    const auto gain=muted_?0.f:volume_;
    for(auto& value:output)value=static_cast<std::int16_t>(value*gain);
    bool committed=false,allowed=false;
    try{allowed=admit(true,[&]{committed=sink_.commit(output);});}
    catch(...){sink_.cancel();return fail();}
    output_.fill(0);
    if(!allowed){sink_.cancel();denied(watch);if(!flush())return Pump::Failed;return closed_?Pump::Closed:Pump::Waiting;}
    if(!committed){sink_.cancel();return fail();}
    if(rendered.mediaFrames){core_.mediaCommitted();if(!firstAudibleWallUs_)firstAudibleWallUs_=clock_();}
    if(permittedFrames<window->frames||(rendered.sourceLeadWait&&rendered.mediaFrames))presentationPrefixCommits_++;
    commits_++;committedMedia_+=rendered.mediaFrames;holdWindow_=0;
    if(traceEnabled_)lastCommitUs_=clock_();
    if(rendered.mediaFrames){if(!firstMediaAudibleUs_)firstMediaAudibleUs_=window->firstAudibleUs;
        lastMediaEndUs_=window->firstAudibleUs+static_cast<std::int64_t>(rendered.outputFrames)*1000000/48000;}
    if(presentationRequired_){maximumAvLagUs_=std::max(maximumAvLagUs_,std::abs(avLag));avLags_[avCursor_++%avLags_.size()]=std::abs(avLag);avCount_=std::min(avCount_+1,avLags_.size());}
    return Pump::Committed;
}
void RemoteAudioPlayout::setVolume(float volume){
    if(!std::isfinite(volume)||volume<0||volume>1)throw std::invalid_argument("Volume must be between zero and one");
    std::lock_guard lock(mutex_);volume_=volume;
}
void RemoteAudioPlayout::setMuted(bool muted){std::lock_guard lock(mutex_);if(muted_!=muted){muted_=muted;core_.resetTiming();}}
void RemoteAudioPlayout::stop(){std::lock_guard lock(mutex_);if(stopped_)return;stopped_=closed_=true;selection_->stop();core_.revoke();sink_.cancel();flush();}
QJsonObject RemoteAudioPlayout::report(bool includeTestHistory)const{
    std::lock_guard lock(mutex_);const auto s=core_.stats();
    const auto reportNow=clock_();const auto frontDeadline=core_.frontDeadline();const auto tailDeadline=core_.audioDeadline();
    auto lags=avLags_;std::sort(lags.begin(),lags.begin()+avCount_);const auto p95=avCount_?lags[(avCount_-1)*95/100]:0;
    auto leads=audioLeads_;std::sort(leads.begin(),leads.begin()+audioLeadCount_);
    const auto traceJson=[](const Trace& e){return QJsonObject{{"kind",e.kind},{"now",static_cast<double>(e.now)},{"sender",static_cast<double>(e.sender)},{"deadline",static_cast<double>(e.deadline)},{"front",static_cast<double>(e.front)},{"queue",static_cast<int>(e.queued)},{"media",static_cast<int>(e.media)},{"frames",static_cast<int>(e.frames)},{"serial",static_cast<double>(e.serial)}};};
    const auto resetJson=[](const ResetTrace& e){return QJsonObject{{"kind",e.kind},{"video_sid",e.videoSid},{"receiver_us",static_cast<double>(e.now)},{"sender_us",static_cast<double>(e.sender)},
        {"previous_deadline_us",static_cast<double>(e.oldDeadline)},{"last_video_arrival_us",static_cast<double>(e.videoArrival)},{"epoch",static_cast<double>(e.epoch)},{"previous_epoch",static_cast<double>(e.previousEpoch)},
        {"serial_before",static_cast<double>(e.before)},{"serial_after",static_cast<double>(e.after)},{"committed_media_frames",static_cast<double>(e.media)},
        {"previous_video_sender_us",static_cast<double>(e.videoSender)},{"previous_video_deadline_us",static_cast<double>(e.videoDeadline)},
        {"last_presented_receiver_us",static_cast<double>(e.presented)},{"reports",static_cast<int>(e.reports)},{"has_rtp",e.hasRtp},
        {"was_anchored",e.anchored},{"was_alignment_locked",e.locked},{"alignment_rejected",e.alignmentRejected},{"sustained_headroom_reset",e.headroomReset},{"early_video_dropped",e.earlyDropped},
        {"clock_unavailable",!e.hasRtp||e.reports<2||!e.epoch||e.sender<=0},
        {"window_first_audible_us",static_cast<double>(e.windowAudible)},{"previous_output_end_us",static_cast<double>(e.previousOutputEnd)},
        {"output_window_already_due",e.outputPast},{"output_window_overlaps",e.outputOverlaps},
        {"pump_gap_us",static_cast<double>(e.pumpGapUs)},{"since_commit_us",static_cast<double>(e.sinceCommitUs)},
        {"prepare_elapsed_us",static_cast<double>(e.prepareElapsedUs)},{"endpoint_gap_us",static_cast<double>(e.endpointGapUs)},{"device_only_recovery",e.deviceOnlyRecovery}};};
    QJsonArray trace;if(includeTestHistory)for(std::size_t n=0;n<traceCount_;n++)trace.append(traceJson(trace_[(traceCursor_+trace_.size()-traceCount_+n)%trace_.size()]));
    QJsonArray resets;if(includeTestHistory)for(std::size_t n=0;n<resetCount_;n++)resets.append(resetJson((*resetTrace_)[(resetCursor_+16-resetCount_+n)%16]));
    QJsonObject firstFault;if(includeTestHistory&&firstFault_){QJsonArray preceding;for(std::size_t n=0;n<firstFault_->count;n++)preceding.append(traceJson(firstFault_->preceding[n]));
        firstFault={{"event",resetJson(firstFault_->event)},{"preceding",preceding}};}
    return {{"first_audio_ms",firstAudibleWallUs_?double(firstAudibleWallUs_-startedWallUs_)/1000.0:0.0},{"audio_started_without_video",static_cast<double>(audioStartedWithoutVideo_)},{"bound",bound_},{"closed",closed_},{"failed",failed_},{"muted",muted_},{"volume",volume_},
        {"decoded_channel_difference_ratio",decodedAudioSignalAbs_>0?decodedAudioDifferenceAbs_/decodedAudioSignalAbs_:0},
        {"split_media_leases",split_},{"video_handovers",static_cast<double>(videoHandovers_)},{"candidate_staged",bool(selection_->candidate())},
        {"reset_serial",static_cast<double>(s.resetSerial)},
        {"sampled_receiver_us",static_cast<double>(clock_())},{"last_presented_receiver_us",static_cast<double>(presentedLocal_)},
        {"last_media_audible_end_us",static_cast<double>(lastMediaEndUs_)},
        {"queued_blocks",static_cast<int>(s.queued)},{"queue_high_water_blocks",static_cast<int>(s.highWater)},
        {"front_deadline_lead_ms",frontDeadline?(*frontDeadline-reportNow)/1000.0:0},
        {"tail_deadline_lead_ms",tailDeadline?(*tailDeadline-reportNow)/1000.0:0},
        {"pcm_capacity_ms",core_.configuredResidenceUs()/1000.0},{"maximum_pcm_residence_ms",core_.configuredResidenceUs()/1000.0},{"target_video_lead_ms",core_.configuredLeadUs()/1000.0},
        {"maximum_endpoint_lead_ms",30},{"video_clock_lifetime_ms",150},{"scheduler_bytes",static_cast<double>(sizeof(AudioPlayout))},
        {"admitted_audio_blocks",static_cast<double>(s.admitted)},{"unclocked",static_cast<double>(s.unclocked)},
        {"invalid",static_cast<double>(s.invalid)},{"late_input_blocks",static_cast<double>(s.lateInput)},
        {"early_video_drops",static_cast<double>(s.earlyVideoDrops)},
        {"bounded_video_advance_tickets",static_cast<double>(s.boundedVideoAdvances)},
        {"advanced_video_presentations",static_cast<double>(advancedVideoPresentations_)},
        {"maximum_video_presentation_advance_ms",s.maximumVideoAdvanceUs/1000.0},
        {"video_presentation_advance_limit_ms",AudioPlayout::maximumVideoAdvanceUs/1000.0},
        {"maximum_video_presentation_wait_ms",s.maximumPresentationLeadUs/1000.0},
        {"video_presentation_wait_limit_ms",core_.configuredVideoWaitLimitUs()/1000.0},
        {"decoded_cadence_adjusted_blocks",static_cast<double>(s.cadenceAdjustedBlocks)},
        {"decoded_cadence_limit_resets",static_cast<double>(s.cadenceLimitResets)},
        {"decoded_cadence_late_dropped_blocks",static_cast<double>(s.cadenceLateDrops)},
        {"maximum_decoded_cadence_drop_ms",s.maximumCadenceDropUs/1000.0},
        {"decoded_cadence_drop_limit_ms",(AudioPlayout::maximumCadenceCorrectionUs+10000)/1000.0},
        {"decoded_cadence_phase_gaps",static_cast<double>(s.cadencePhaseGaps)},
        {"maximum_decoded_cadence_gap_ms",s.maximumCadenceGapUs/1000.0},
        {"decoded_cadence_gap_limit_ms",(AudioPlayout::maximumCadenceCorrectionUs+10000)/1000.0},
        {"decoded_cadence_discontinuities",static_cast<double>(s.cadenceDiscontinuities)},
        {"source_horizon_silent_frames",static_cast<double>(s.sourceHorizonSilentFrames)},
        {"maximum_decoded_cadence_correction_ms",s.maximumCadenceCorrectionUs/1000.0},
        {"decoded_cadence_correction_limit_ms",AudioPlayout::maximumCadenceCorrectionUs/1000.0},
        {"overflow_blocks",static_cast<double>(s.overflowBlocks)},{"expired_blocks",static_cast<double>(s.expiredBlocks)},
        {"late_frames",static_cast<double>(s.lateFrames)},{"staged_media_frames",static_cast<double>(s.renderedFrames)},
        {"committed_media_frames",static_cast<double>(committedMedia_)},
        {"presentation_required",presentationRequired_},{"presented_video_frames",static_cast<double>(presentedFrames_)},
        {"audio_continuity_through_video_stall",audioContinuity_},
        {"endpoint_gap_resets",static_cast<double>(endpointGapResets_)},
        {"endpoint_timeline_preserved",static_cast<double>(endpointTimelinePreserved_)},
        {"estimated_media_window_coverage",lastMediaEndUs_>firstMediaAudibleUs_?std::min(1.0,committedMedia_*1000000.0/48000/(lastMediaEndUs_-firstMediaAudibleUs_)):0},
        {"common_clock_alignments",static_cast<double>(s.alignments)},{"common_clock_alignment_rejected",static_cast<double>(s.alignmentRejected)},
        {"alignment_rejection_resets",static_cast<double>(s.alignmentRejectResets)},
        {"alignment_locked_after_media_commit",s.alignmentLocked},{"running_low_headroom_audio_blocks",static_cast<double>(s.lowHeadroomBlocks)},
        {"sustained_low_headroom_resets",static_cast<double>(s.sustainedHeadroomResets)},
        {"low_headroom_audio_rebases",static_cast<double>(s.headroomRebases)},{"audio_anchor_advances",static_cast<double>(s.audioAnchorAdvances)},
        {"maximum_low_headroom_audio_offset_ms",s.maximumHeadroomOffsetUs/1000.0},
        {"maximum_allowed_low_headroom_audio_offset_ms",40},
        {"pcm_arrival_waits",static_cast<double>(s.pcmArrivalWaits)},{"pcm_wait_write_guard_us",static_cast<double>(AudioPlayout::pcmWaitGuardUs)},
        {"maximum_aligned_video_lead_ms",s.maximumAlignedVideoLeadUs/1000.0},{"common_clock_video_lead_limit_ms",80},
        {"running_video_mapping_lead_limit_ms",(AudioPlayout::maximumPresentationLeadUs+AudioPlayout::maximumVideoAdvanceUs)/1000.0},
        {"test_timing_trace",trace},
        {"test_reset_trace",resets},{"test_first_playback_fault",firstFault},{"test_history_included",includeTestHistory},
        {"recent_audio_deadline_lead_p05_ms",audioLeadCount_?leads[(audioLeadCount_-1)*5/100]/1000.0:0},
        {"recent_audio_deadline_lead_p95_ms",audioLeadCount_?leads[(audioLeadCount_-1)*95/100]/1000.0:0},
        {"presentation_waits",static_cast<double>(presentationWaits_)},{"presentation_resets",static_cast<double>(presentationResets_)},
        {"presentation_lead_waits",static_cast<double>(presentationLeadWaits_)},{"presentation_age_resets",static_cast<double>(presentationAgeResets_)},
        {"presentation_prefix_commits",static_cast<double>(presentationPrefixCommits_)},
        {"presentation_lag_resets",static_cast<double>(presentationLagResets_)},
        {"maximum_estimated_av_lag_ms",maximumAvLagUs_/1000.0},{"recent_p95_estimated_av_lag_ms",p95/1000.0},
        {"silent_frames",static_cast<double>(s.silentFrames)},{"denied",static_cast<double>(denied_)},
        {"device_commits",static_cast<double>(commits_)},{"device_resets",static_cast<double>(deviceResets_)},
        {"clock_anchored",s.anchored},{"wasapi_connected",sink_.hardwareEndpoint()},{"physical_playback_measured",false}};
}
}

