#include "audio_playout.h"
#include <algorithm>
#include <stdexcept>
#include <cstdlib>

namespace goodsharing {
namespace {
constexpr std::int64_t maximumLocalUs=1000000000000000ll;
std::int64_t ticks(std::uint32_t now,std::uint32_t before) {
    const auto value=static_cast<std::uint32_t>(now-before);
    return value<0x80000000u?value:static_cast<std::int64_t>(value)-0x100000000ll;
}
std::int64_t sampleUs(std::size_t frames){return static_cast<std::int64_t>(frames)*1000000/48000;}
}
void AudioPlayout::configureTiming(std::int64_t leadUs,std::int64_t videoWaitLimitUs,std::int64_t bufferUs) {
    if(leadUs<5000||leadUs>600000||videoWaitLimitUs<20000||videoWaitLimitUs>700000||bufferUs<0||bufferUs>600000)
        throw std::invalid_argument("Invalid playout timing");
    leadUs_=leadUs;videoWaitLimitUs_=videoWaitLimitUs;
    // A block must survive long enough to reach a deadline one buffer away, plus a
    // 15 ms margin. The 60 ms constant stays the floor, so the shipped 45 ms profile
    // keeps its tuned residence and only larger buffers widen it.
    residenceUs_=std::max(maximumResidenceUs,bufferUs+15000);
    videoLifetimeValueUs_=std::max(videoLifetimeUs,bufferUs+50000);
    presentationLeadLimitUs_=std::max(maximumPresentationLeadUs,bufferUs+25000);
}
bool PlayoutClock::valid()const{return hasRtp&&senderUs>0&&senderUs<=5000000000000000ll&&epoch>0&&reports>=2;}
void AudioPlayout::resetDevice(){stats_.resetPending=true;if(++stats_.resetSerial==0)++stats_.resetSerial;stats_.resets++;lastOutputEnd_=0;}
void AudioPlayout::pop(){blocks_[head_]={};head_=(head_+1)%capacity;stats_.queued--;}
void AudioPlayout::invalidate(){
    const bool changed=stats_.anchored||stats_.queued;
    while(stats_.queued)pop();stats_.anchored=false;stats_.alignmentLocked=false;consecutiveLate_=0;consecutiveAlignmentRejected_=0;audioTimelineOffsetUs_=0;clearCadence();
    if(changed)resetDevice();
}
bool AudioPlayout::localTime(std::int64_t now){
    if(now<=0||now>maximumLocalUs||now<lastNow_){stats_.invalid++;invalidate();return false;}
    lastNow_=now;return true;
}
bool AudioPlayout::video(PlayoutClock clock,std::int64_t now,bool allowBoundedAdvance){
    if(stats_.closed||!localTime(now))return false;
    if(!clock.valid()){stats_.unclocked++;invalidate();return false;}
    if(clock.epoch<lastVideo_.epoch||(clock.epoch==lastVideo_.epoch&&clock.senderUs<=lastVideo_.senderUs)){stats_.invalid++;return false;}
    const auto delta=clock.senderUs-anchorSender_;
    const auto projected=anchorLocal_+delta;
    if(alignAudio_&&stats_.anchored&&clock.epoch==lastVideo_.epoch&&delta>=-2000000&&delta<=2000000&&
       now-lastVideoLocal_<=videoLifetimeValueUs_&&projected-now>videoWaitLimitUs_&&projected-now<=videoWaitLimitUs_+20000){
        if(allowBoundedAdvance&&stats_.alignmentLocked&&!stats_.resetPending){
            // Running video can be presented at the existing 80 ms wait bound,
            // up to 20 ms before its common-clock target. Keep source stamps,
            // PCM deadlines and queued tickets unchanged. The adapter validates
            // this separate presentation target and the real source A/V fence.
            stats_.boundedVideoAdvances++;
            stats_.maximumVideoAdvanceUs=std::max(stats_.maximumVideoAdvanceUs,projected-now-videoWaitLimitUs_);
            stats_.maximumAlignedVideoLeadUs=std::max(stats_.maximumAlignedVideoLeadUs,projected-now);
            stats_.maximumPresentationLeadUs=std::max(stats_.maximumPresentationLeadUs,videoWaitLimitUs_);
            if(delta>1000000){anchorSender_=clock.senderUs;anchorLocal_+=delta;}
            lastVideo_=clock;lastVideoLocal_=now;return true;
        }
        // Startup and callers without coordinated bounded presentation retain
        // the original drop policy; no freshness or PCM clock is changed.
        stats_.earlyVideoDrops++;return false;
    }
    // A picture may legitimately be scheduled as early as the wait bound; a narrower
    // tolerance re-anchored (and so flushed video and audio) frames the presentation
    // path would have accepted within its own bound.
    const auto earlyLimit=std::max<std::int64_t>(leadUs_+40000,videoWaitLimitUs_);
    const bool stableAudioTimeline=audioContinuity_&&stats_.anchored&&clock.epoch==lastVideo_.epoch&&
        delta>=-2000000&&delta<=2000000;
    if(stableAudioTimeline&&((anchorLocal_+delta)-now>earlyLimit||now-(anchorLocal_+delta)>40000)){
        if((anchorLocal_+delta)>now)stats_.earlyVideoDrops++;else stats_.lateInput++;
        return false;
    }
    const bool reanchor=!stats_.anchored||clock.epoch!=lastVideo_.epoch||delta < -2000000||delta > 2000000||
        now-(anchorLocal_+delta)>40000||(anchorLocal_+delta)-now>earlyLimit||
        (alignAudio_&&(anchorLocal_+delta)-now>videoWaitLimitUs_);
    if(reanchor){invalidate();anchorSender_=clock.senderUs;anchorLocal_=now+leadUs_;stats_.anchored=true;resetDevice();}
    else if(delta>1000000){
        // Move the origin without changing deadlines; keep bounded arithmetic.
        anchorSender_=clock.senderUs;anchorLocal_+=delta;
    }
    lastVideo_=clock;lastVideoLocal_=now;
    if(alignAudio_){stats_.maximumAlignedVideoLeadUs=std::max(stats_.maximumAlignedVideoLeadUs,*videoDeadline()-now);
        stats_.maximumPresentationLeadUs=std::max(stats_.maximumPresentationLeadUs,*videoPresentationDeadline()-now);}
    return true;
}
std::optional<std::int64_t> AudioPlayout::projectVideo(PlayoutClock clock,std::int64_t received)const{
    if(stats_.closed||!stats_.anchored||stats_.resetPending||!stats_.alignmentLocked||!clock.valid()||
       received<=0||received>maximumLocalUs||received<lastNow_||received-lastVideoLocal_>videoLifetimeValueUs_)return {};
    const auto deadline=localFor(clock.senderUs);
    if(!deadline||*deadline-received<10000||*deadline-received>80000)return {};
    return deadline;
}
bool AudioPlayout::adoptVideo(PlayoutClock clock,std::int64_t received,std::int64_t now,std::int64_t deadline,std::uint64_t serial){
    if(stats_.closed||!stats_.anchored||stats_.resetPending||!stats_.alignmentLocked||serial!=stats_.resetSerial||!clock.valid()||
       received<=0||now<received||now>maximumLocalUs||now<lastNow_||now-received>videoLifetimeValueUs_||
       now-lastVideoLocal_>videoLifetimeValueUs_||localFor(clock.senderUs)!=deadline||now<deadline||now-deadline>videoWaitLimitUs_)return false;
    // This is a different video SSRC: its epoch is independent of the retired
    // track. Preserve the common clock, audio ordering, queued PCM and device.
    lastVideo_=clock;lastVideoLocal_=received;lastNow_=now;return true;
}
bool AudioPlayout::audio(PlayoutClock clock,std::span<const std::int16_t> pcm,std::int64_t now){
    if(stats_.closed||!localTime(now))return false;
    if(!clock.valid()){
        stats_.unclocked++;
        if(alignAudio_&&!stats_.alignmentLocked&&stats_.anchored&&now-lastVideoLocal_<=videoLifetimeValueUs_){
            // Audio and video may train at different times. Missing startup
            // audio reports cannot invalidate an independently valid video
            // clock. Reject all such PCM and wait for trained audio instead.
            while(stats_.queued)pop();clearCadence();return false;
        }
        invalidate();return false;
    }
    if(pcm.size()!=blockFrames*channels){stats_.invalid++;invalidate();return false;}
    if(clock.epoch<lastAudio_.epoch||(clock.epoch==lastAudio_.epoch&&
        (clock.senderUs<=lastAudio_.senderUs||ticks(clock.rtp,lastAudio_.rtp)<=0))){stats_.invalid++;return false;}
    if(lastAudio_.epoch&&clock.epoch!=lastAudio_.epoch){lastAudio_=clock;invalidate();return false;}
    if(!stats_.anchored||(!audioContinuity_&&now-lastVideoLocal_>videoLifetimeValueUs_)){invalidate();return false;}
    auto delta=clock.senderUs-anchorSender_;
    // Audio continuation: when video is stalled (or the shared source is
    // minimized) only video() used to advance the common origin, so a still-
    // contiguous audio timeline eventually tripped the bounded delta below and
    // rebuilt itself, costing one silent device gap. Move the origin forward with
    // the audio clock exactly as video() does; the local<->sender mapping and
    // every queued deadline are unchanged by this re-basing.
    if(audioContinuity_&&stats_.anchored&&delta>1000000&&delta<=2000000){
        anchorSender_=clock.senderUs;anchorLocal_+=delta;delta=0;
        stats_.audioAnchorAdvances++;
    }
    if(delta < -2000000||delta > 2000000){stats_.invalid++;invalidate();return false;}
    auto deadline=anchorLocal_+delta+audioTimelineOffsetUs_;
    const auto nominalDeadline=deadline;
    const auto stride=ticks(clock.rtp,audioTailClock_.rtp),sourceStep=clock.senderUs-audioTailClock_.senderUs;
    const bool freshTail=alignAudio_&&audioTailClock_.valid()&&clock.epoch==audioTailClock_.epoch&&now-audioTailArrival_<=residenceUs_;
    // A later valid SR can refine the NTP origin while retaining the epoch.
    // SenderReportClock already validates that update's slope/quantization.
    // Require close RTP/NTP agreement only within an unchanged reference;
    // all refinements still face the same absolute correction/source bounds.
    const bool consistentSource=clock.reports>audioTailClock_.reports||
        (clock.reports==audioTailClock_.reports&&std::abs(sourceStep-stride*1000000/48000)<=1000);
    const bool contiguous=freshTail&&stride>0&&stride<960&&sourceStep>0&&
        consistentSource;
    if(contiguous){
        // NetEq has already adapted this decoded PCM. Consecutive 480-frame
        // outputs occupy 10 ms of device time even if their source positions
        // advance by a different amount. Never re-time queued video tickets.
        deadline=audioTailDeadline_+10000;
        const auto correction=std::abs(deadline-nominalDeadline);
        if(correction>maximumCadenceCorrectionUs){
            if(stats_.alignmentLocked&&!stats_.resetPending&&stats_.queued&&deadline>nominalDeadline&&
               correction<=maximumCadenceCorrectionUs+10000&&audioTailDeadline_>now+pcmWaitGuardUs){
                // A stretched decoded block has fallen just behind the running
                // source phase. Discard this new block, preserving valid queued
                // PCM, device output, the common mapping and all video tickets.
                // Do not stitch a retained block's source span across this gap.
                auto& previous=blocks_[(head_+stats_.queued-1)%capacity];
                if(previous.senderUs==audioTailClock_.senderUs&&!previous.used)previous.sourceDurationUs=sourceStep;
                lastAudio_=audioTailClock_=clock;
                // Keep the last admitted arrival/deadline: drops cannot extend
                // queue residence or postpone the next device window.
                stats_.cadenceLateDrops++;
                stats_.maximumCadenceDropUs=std::max(stats_.maximumCadenceDropUs,correction);
                return false;
            }
            if(stats_.alignmentLocked&&!stats_.resetPending&&stats_.queued&&nominalDeadline>deadline&&
               correction<=maximumCadenceCorrectionUs+10000&&audioTailDeadline_>now+pcmWaitGuardUs&&
               nominalDeadline-now<=residenceUs_-10000){
                // Compression can advance source time faster than output PCM.
                // Preserve the queued tail and use this input's original common
                // deadline. The bounded intervening gap renders as silence;
                // no existing sample, ticket or clock is moved or replayed.
                deadline=nominalDeadline;stats_.cadencePhaseGaps++;
                stats_.maximumCadenceGapUs=std::max(stats_.maximumCadenceGapUs,correction);
            }else{lastAudio_=clock;stats_.cadenceLimitResets++;invalidate();return false;}
        }
    }else if(freshTail){
        // Missing whole decoded blocks or incoherent clocks must not be
        // hidden by stitching together callbacks separated in source time.
        stats_.cadenceDiscontinuities++;
        if(deadline<audioTailDeadline_+10000){lastAudio_=clock;stats_.cadenceLimitResets++;invalidate();return false;}
    }
    if(alignAudio_&&stats_.alignmentLocked){
        if(deadline<now+10000){
            stats_.lowHeadroomBlocks++;
            if(++consecutiveLate_>=3){
                const auto desired=now+15000;
                const auto available=40000-audioTimelineOffsetUs_;
                const auto shift=std::clamp<std::int64_t>(desired-deadline,0,std::max<std::int64_t>(0,available));
                if(shift>=1000){
                    for(std::size_t n=0;n<stats_.queued;n++)blocks_[(head_+n)%capacity].deadlineUs+=shift;
                    if(audioTailDeadline_)audioTailDeadline_+=shift;
                    deadline+=shift;audioTimelineOffsetUs_+=shift;consecutiveLate_=0;
                    stats_.headroomRebases++;stats_.maximumHeadroomOffsetUs=std::max(stats_.maximumHeadroomOffsetUs,audioTimelineOffsetUs_);
                }else{
                    lastAudio_=clock;stats_.sustainedHeadroomResets++;invalidate();return false;
                }
            }
        }else consecutiveLate_=0;
    }
    // Startup may align the two decoded streams. Once media is committed,
    // isolated callback jitter must not flush both outputs and their tickets.
    if(alignAudio_&&!stats_.alignmentLocked&&deadline<now+10000){
        const auto shift=now+20000-deadline;
        const auto videoLead=anchorLocal_+(lastVideo_.senderUs-anchorSender_)+shift-lastVideoLocal_;
        if(videoLead>80000){
            // This PCM cannot fit the common latency bound. Before the first
            // media commit, drop it and queued startup PCM without discarding
            // an otherwise valid video clock. Reanchoring every video/audio
            // pair merely repeats the same impossible alignment and flushes
            // the device continuously. Fresh PCM may fit this same timeline.
            stats_.alignmentRejected++;lastAudio_=clock;
            if(++consecutiveAlignmentRejected_>=64){
                stats_.alignmentRejectResets++;
                while(stats_.queued)pop();clearCadence();
                anchorSender_=clock.senderUs;anchorLocal_=now+leadUs_;stats_.anchored=true;
                stats_.alignmentLocked=false;consecutiveAlignmentRejected_=0;audioTimelineOffsetUs_=0;resetDevice();
            }
            while(stats_.queued)pop();clearCadence();return false;
        }
        while(stats_.queued)pop();clearCadence();anchorLocal_+=shift;deadline+=shift;resetDevice();
        stats_.alignments++;consecutiveAlignmentRejected_=0;stats_.maximumAlignedVideoLeadUs=std::max(stats_.maximumAlignedVideoLeadUs,videoLead);
        stats_.maximumPresentationLeadUs=std::max(stats_.maximumPresentationLeadUs,videoLead);
    }
    if(deadline>now+100000){stats_.invalid++;invalidate();return false;}
    lastAudio_=clock;
    if(deadline+10000<now-residenceUs_){stats_.lateInput++;return false;}
    consecutiveAlignmentRejected_=0;
    if(contiguous&&audioTailClock_.valid()){
        const auto correction=std::abs(deadline-(anchorLocal_+delta));
        if(correction)stats_.cadenceAdjustedBlocks++;
        stats_.maximumCadenceCorrectionUs=std::max(stats_.maximumCadenceCorrectionUs,correction);
        if(stats_.queued){auto& previous=blocks_[(head_+stats_.queued-1)%capacity];
            if(previous.senderUs==audioTailClock_.senderUs&&!previous.used)previous.sourceDurationUs=sourceStep;}
    }
    if(stats_.queued==capacity){pop();stats_.overflowBlocks++;}
    auto& block=blocks_[(head_+stats_.queued)%capacity];
    std::copy(pcm.begin(),pcm.end(),block.pcm.begin());block.senderUs=clock.senderUs;block.receivedUs=now;block.used=0;block.deadlineUs=deadline;block.sourceDurationUs=10000;
    audioTailClock_=clock;audioTailDeadline_=deadline;audioTailArrival_=now;
    stats_.queued++;stats_.highWater=std::max(stats_.highWater,stats_.queued);stats_.admitted++;return true;
}
bool AudioPlayout::acknowledgeReset(std::uint64_t serial){
    if(serial!=stats_.resetSerial)return false;
    stats_.resetPending=false;return true;
}
void AudioPlayout::revoke(){if(stats_.closed)return;invalidate();stats_.closed=true;resetDevice();}
AudioPlayout::Render AudioPlayout::render(std::int64_t now,std::int64_t audible,std::span<std::int16_t> output,std::optional<SourceWindow> sourceWindow){
    std::fill(output.begin(),output.end(),0);
    std::size_t frames=output.size()/channels;
    std::int64_t firstSource=0,lastSource=0;bool sourceLeadWait=false,sourceLagReset=false;
    const auto result=[&](bool ready=false,std::size_t media=0){return Render{ready,stats_.resetPending,stats_.resetSerial,media,ready?frames:0,firstSource,lastSource,sourceLeadWait,sourceLagReset};};
    if(stats_.closed)return result();
    if(!localTime(now))return result();
    if(output.empty()||output.size()%channels||output.size()>blockFrames*channels||audible<now||audible>now+30000||
       (sourceWindow&&(sourceWindow->minimum<=0||sourceWindow->maximum<sourceWindow->minimum))||
       (lastOutputEnd_&&audible<lastOutputEnd_-1000)){stats_.invalid++;invalidate();return result();}
    if(stats_.anchored&&!audioContinuity_&&now-lastVideoLocal_>videoLifetimeValueUs_)invalidate();
    if(stats_.resetPending||!stats_.anchored)return result();
    while(stats_.queued&&now-blocks_[head_].receivedUs>residenceUs_){pop();stats_.expiredBlocks++;}
    if(alignAudio_&&stats_.alignmentLocked&&audible-now>pcmWaitGuardUs){
        const bool unknownTail=!stats_.queued||blocks_[(head_+stats_.queued-1)%capacity].deadlineUs+10000<=audible;
        if(unknownTail){
            // A still-future device window is not evidence of missing PCM.
            // Leave its original deadline available for ingress. At the write
            // guard, commit silence if still missing; never delay the timeline.
            stats_.pcmArrivalWaits++;return result();
        }
    }
    if(stats_.queued){
        const auto& last=blocks_[(head_+stats_.queued-1)%capacity];
        const auto knownEnd=last.deadlineUs+10000;
        if(knownEnd>audible&&knownEnd<audible+sampleUs(frames)){
            // Do not commit synthetic silence over the next timely capture
            // block. The endpoint may request more than is yet available.
            frames=std::min(frames,static_cast<std::size_t>((knownEnd-audible)*48000/1000000));
            if(!frames)return result();
        }
    }
    std::size_t media=0;
    for(std::size_t frame=0;frame<frames;frame++){
        const auto at=audible+sampleUs(frame);
        while(stats_.queued){
            auto& block=blocks_[head_];
            if(now-block.receivedUs>residenceUs_){pop();stats_.expiredBlocks++;continue;}
            const auto due=block.deadlineUs+sampleUs(block.used);
            if(at-due>2000){
                const auto skip=std::min(blockFrames-block.used,static_cast<std::size_t>(((at-due)*48000+999999)/1000000));
                block.used+=skip;stats_.lateFrames+=skip;
                if(block.used==blockFrames)pop();continue;
            }
            // Both positions truncate 48 kHz frame offsets to microseconds.
            // A split packet can differ by one microsecond solely because
            // floor(a+b) != floor(a)+floor(b). Do not insert a whole silent
            // sample for this sub-sample representation error.
            if(due>at+1)break;
            const auto source=block.senderUs+static_cast<std::int64_t>(block.used)*block.sourceDurationUs/static_cast<std::int64_t>(blockFrames);
            if(sourceWindow&&source<sourceWindow->minimum){sourceLagReset=true;invalidate();std::fill(output.begin(),output.end(),0);return result();}
            if(sourceWindow&&source>sourceWindow->maximum){
                sourceLeadWait=true;
                if(!frame){
                    // Keep the future window open for a fresh presentation,
                    // but do not starve a due device merely because the real
                    // PCM source is ahead of that picture. At the write guard
                    // this window contains only silence; PCM stays queued and
                    // is trimmed by its original deadline on a later render.
                    // The adapter still restricts the window's nominal A/V
                    // horizon and holds final permission at every commit.
                    if(!alignAudio_||!stats_.alignmentLocked||audible-now>pcmWaitGuardUs)return result();
                    stats_.sourceHorizonSilentFrames+=frames;
                }else frames=frame;
                break;
            }
            if(!firstSource)firstSource=source;lastSource=source;
            output[frame*2]=block.pcm[block.used*2];output[frame*2+1]=block.pcm[block.used*2+1];
            block.used++;media++;if(block.used==blockFrames)pop();break;
        }
        if(sourceLeadWait)break;
    }
    if(gain_<1.0f)for(auto& sample:output)sample=static_cast<std::int16_t>(static_cast<float>(sample)*gain_);
    lastOutputEnd_=audible+sampleUs(frames);stats_.renderedFrames+=media;
    stats_.silentFrames+=frames-media;return result(true,media);
}
}

