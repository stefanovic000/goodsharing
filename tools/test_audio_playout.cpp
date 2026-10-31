#include "audio_playout.h"
#include <algorithm>
#include <iostream>
#include <limits>
#include <memory>
#include <stdexcept>
using namespace goodsharing;
constexpr std::int64_t sender=4000000000000000ll,local=1000000;
using Pcm=std::array<std::int16_t,960>;
void require(bool value,const char* message){if(!value)throw std::runtime_error(message);}
PlayoutClock clockAt(std::int64_t elapsed=0,std::uint32_t epoch=1){return {sender+elapsed,static_cast<std::uint32_t>(0xfffff000ull+elapsed*48/1000),epoch,2,true};}
Pcm pcm(int value){Pcm p;for(int i=0;i<480;i++){p[i*2]=static_cast<std::int16_t>(value);p[i*2+1]=static_cast<std::int16_t>(-value);}return p;}
bool zero(const Pcm& p){return std::all_of(p.begin(),p.end(),[](auto v){return v==0;});}
PlayoutClock decodedClock(std::uint64_t ticks){return {sender+static_cast<std::int64_t>(ticks*1000000/48000),static_cast<std::uint32_t>(0xfffff000ull+ticks),1,2,true};}
Pcm numbered(int block){Pcm p;for(int n=0;n<480;n++){p[n*2]=static_cast<std::int16_t>(block*500+n+1);p[n*2+1]=-p[n*2];}return p;}
void anchor(AudioPlayout& p){require(p.video(clockAt(),local),"Valid sender clock rejected");require(p.acknowledgeReset(p.stats().resetSerial),"Reset acknowledgement failed");}
int main(){try{
    {
        AudioPlayout startup;startup.enableAudioAlignment();anchor(startup);Pcm blocked;
        startup.audio(clockAt(),pcm(333),local);
        require(!startup.render(local+30000,local+30000,blocked,AudioPlayout::SourceWindow{sender-80000,sender-1}).ready&&
            zero(blocked)&&startup.stats().sourceHorizonSilentFrames==0&&!startup.lastOutputEnd(),"Startup source wait bypassed initial media admission");
        AudioPlayout p;p.enableAudioAlignment();anchor(p);Pcm output;
        require(p.audio(clockAt(),pcm(111),local),"Source horizon guard control did not start");
        require(p.render(local+30000,local+30000,output).mediaFrames==480,"Source horizon guard first PCM missing");p.mediaCommitted();
        require(p.audio(clockAt(10000),pcm(121),local+30000),"Source horizon guard queued PCM missing");
        const auto serial=p.stats().resetSerial;const auto deadline=p.videoDeadline();
        const AudioPlayout::SourceWindow horizon{sender-80000,sender+9999};
        const auto waiting=p.render(local+35000,local+40000,output,horizon);
        require(!waiting.ready&&waiting.sourceLeadWait&&zero(output)&&p.stats().queued==1,"Future source horizon did not wait before write guard");
        const auto filler=p.render(local+36000,local+40000,output,horizon);
        require(filler.ready&&filler.sourceLeadWait&&filler.mediaFrames==0&&filler.outputFrames==480&&zero(output)&&p.stats().queued==1,
            "Source horizon wait starved a due endpoint instead of bounded silence");
        require(p.stats().resetSerial==serial&&p.videoDeadline()==deadline&&p.lastOutputEnd()==local+50000,"Source wait retimed video or invalidated clock");
        require(p.audio(clockAt(20000),pcm(222),local+40000),"Fresh post-horizon PCM missing");
        const auto fresh=p.render(local+46000,local+50000,output,AudioPlayout::SourceWindow{sender-80000,sender+40000});
        require(fresh.mediaFrames==480&&output==pcm(222)&&p.stats().lateFrames==480&&p.stats().silentFrames==480&&p.stats().sourceHorizonSilentFrames==480,
            "Source wait replayed expired PCM or altered fresh samples");
    }
    {
        AudioPlayout refined;refined.enableAudioAlignment();anchor(refined);
        require(refined.audio(decodedClock(0),numbered(1),local),"Report-refinement control did not start");
        const auto serial=refined.stats().resetSerial;auto next=decodedClock(480);next.senderUs=sender+8501;next.reports=3;
        require(refined.audio(next,numbered(2),local+10000),"Valid sender-report refinement reset continuous decoded PCM");
        Pcm output;require(refined.render(local+30000,local+30000,output).mediaFrames==480&&output==numbered(1),"Report refinement changed first PCM");
        refined.mediaCommitted();require(refined.render(local+40000,local+40000,output).mediaFrames==480&&output==numbered(2)&&
            refined.stats().resetSerial==serial&&refined.stats().maximumCadenceCorrectionUs==1499,"Report refinement changed PCM order, video serial or correction bounds");
    }
    {
        // NetEq can stretch decoded duration while source RTP advances less.
        // Once the running phase reaches the correction fence, discard only
        // the new late-source block. Previously that flushed valid queued PCM
        // and the paired video clock, extending a small correction into a gap.
        AudioPlayout p;p.enableAudioAlignment();Pcm output;
        std::uint64_t serial=0;
        for(unsigned n=0;n<=24;n++){
            const auto now=local+n*10000;
            require(p.video(clockAt(n*10000),now),"Late-source control video rejected");
            if(p.stats().resetPending)p.acknowledgeReset(p.stats().resetSerial);
            const auto tail=p.audioDeadline(),video=p.videoDeadline();
            const auto sourceTicks=n<=18?n*425:18*425+(n-18)*480;
            const auto accepted=p.audio(decodedClock(sourceTicks),numbered(n+1),now);
            if(n==18){
                require(!accepted&&p.stats().anchored&&p.stats().alignmentLocked&&p.stats().resetSerial==serial&&
                    p.audioDeadline()==tail&&p.videoDeadline()==video&&p.stats().queued==3,
                    "Late decoded block flushed valid PCM and the paired video clock");
            }else require(accepted,"Fresh PCM did not continue after one late decoded block");
            if(n<3)continue;
            const auto expected=n<21?n-2:n-1; // Block 19 alone is discarded.
            const auto rendered=p.render(now,now,output,AudioPlayout::SourceWindow{sender+n*10000-80000,sender+n*10000+80000});
            require(rendered.mediaFrames==480&&rendered.outputFrames==480&&output==numbered(expected),
                "Late-source recovery inserted silence, replayed or altered retained PCM");
            if(n==20)require(rendered.lastSourceUs<decodedClock(18*425).senderUs,
                "Retained PCM source span was stretched across the discarded block");
            p.mediaCommitted();serial=p.stats().resetSerial;
        }
        require(p.stats().highWater<=4&&!p.stats().silentFrames&&!p.stats().lateFrames&&!p.stats().cadenceLimitResets&&
            p.stats().maximumCadenceCorrectionUs<=AudioPlayout::maximumCadenceCorrectionUs&&p.stats().cadenceLateDrops==1&&p.stats().maximumCadenceDropUs==20625,
            "Late-source recovery enlarged storage or its timing correction fence");
        p.revoke();require(!p.render(local+250000,local+250000,output).ready&&zero(output),"Late-source recovery bypassed revocation");
    }
    for(const bool committed:{false,true}){
        AudioPlayout p;p.enableAudioAlignment();Pcm output;std::uint64_t serial=0;
        for(unsigned n=0;n<=18;n++){
            // The second control has running playback, but its retained tail
            // is already due. Neither case may use late-block preservation.
            const auto now=local+n*10000+(committed&&n==18?20000:0);
            require(p.video(clockAt(n*10000),now),"Ineligible cadence control video rejected");
            if(p.stats().resetPending)p.acknowledgeReset(p.stats().resetSerial);
            const auto accepted=p.audio(decodedClock(n*425),numbered(n+1),now);
            if(n==18){require(!accepted&&!p.stats().anchored&&p.stats().resetSerial>serial&&p.stats().queued==0&&
                p.stats().cadenceLimitResets==1,"Unstarted or expired cadence bypassed its reset fence");break;}
            require(accepted,"Ineligible cadence control failed early");
            if(n>=3){require(p.render(now,now,output).mediaFrames==480,"Ineligible cadence control did not render");if(committed)p.mediaCommitted();}
            serial=p.stats().resetSerial;
        }
    }
    {
        AudioPlayout p;p.enableAudioAlignment();Pcm output;std::uint64_t serial=0;
        const auto sourceClock=[](unsigned ticks){auto c=decodedClock(ticks);c.senderUs-=10000;c.rtp-=480;return c;};
        for(unsigned n=0;n<=5;n++){
            const auto now=local+n*10000;require(p.video(clockAt(n*10000),now),"Ahead-source control video rejected");
            if(p.stats().resetPending)p.acknowledgeReset(p.stats().resetSerial);
            const auto video=p.videoDeadline();
            require(p.audio(sourceClock(n*698),numbered(n+1),now),"Ahead decoded block flushed valid PCM instead of keeping its original deadline");
            if(n==5)require(p.stats().resetSerial==serial&&p.stats().alignmentLocked&&p.videoDeadline()==video&&
                p.audioDeadline()==local+92708,"Ahead-source recovery moved an existing clock or replaced the input deadline");
            if(n<2)continue;
            require(p.render(now,now,output).mediaFrames==480&&output==numbered(n-1),"Ahead-source recovery corrupted retained PCM");
            p.mediaCommitted();serial=p.stats().resetSerial;
        }
        require(p.audio(sourceClock(5*698+480),numbered(7),local+60000),"Fresh audio did not follow the ahead-source deadline");
        require(p.render(local+60000,local+60000,output).mediaFrames==480&&output==numbered(5),"Ahead-source recovery discarded previously queued audio");
        for(const auto t:{70000,80000})require(p.render(local+t,local+t,output).mediaFrames==0&&zero(output),"Ahead-source gap emitted audio before its source deadline");
        const auto prefix=p.render(local+90000,local+90000,output);
        require(prefix.mediaFrames==350&&prefix.outputFrames==480,"Ahead-source gap exceeded its original 22.708 ms interval");
        for(unsigned n=0;n<130*2;n++)require(output[n]==0,"Ahead-source gap prefix was not silent");
        for(unsigned n=0;n<350*2;n++)require(output[130*2+n]==numbered(6)[n],"Ahead-source gap changed fresh audio samples");
        const auto tail=p.render(local+100000,local+100000,std::span{output}.first(130*2));
        require(tail.mediaFrames==130&&tail.outputFrames==130,"Ahead-source split lost its remaining PCM");
        for(unsigned n=0;n<130*2;n++)require(output[n]==numbered(6)[350*2+n],"Ahead-source gap replayed fresh audio samples");
        require(p.render(local+102708,local+102708,output).mediaFrames==480&&output==numbered(7)&&
            p.stats().resetSerial==serial&&!p.stats().cadenceLimitResets&&!p.stats().lateFrames&&p.stats().highWater<=3&&p.stats().silentFrames==1090&&
            p.stats().maximumCadenceCorrectionUs<=AudioPlayout::maximumCadenceCorrectionUs&&p.stats().cadencePhaseGaps==1&&p.stats().maximumCadenceGapUs==22708,
            "Ahead-source recovery enlarged timing correction, storage or silence");
    }
    {
        AudioPlayout p;p.enableAudioAlignment();Pcm output;std::uint64_t serial=0;
        for(unsigned n=0;n<=5;n++){
            const auto now=local+n*10000;require(p.video(clockAt(n*10000),now),"Residence-bound control video rejected");
            if(p.stats().resetPending)p.acknowledgeReset(p.stats().resetSerial);
            const auto accepted=p.audio(decodedClock(n*698),numbered(n+1),now);
            if(n==5){require(!accepted&&p.stats().resetSerial>serial&&!p.stats().anchored&&p.stats().queued==0&&p.stats().cadenceLimitResets==1&&
                !p.stats().cadencePhaseGaps,"Ahead-source recovery exceeded the original PCM residence budget");break;}
            require(accepted,"Residence-bound control failed early");
            if(n>=3){require(p.render(now,now,output).mediaFrames==480,"Residence-bound control did not render");p.mediaCommitted();}
            serial=p.stats().resetSerial;
        }
    }
    for(const auto stride:{36u,44u,262u,350u,480u,698u}){
        AudioPlayout p;p.enableAudioAlignment();anchor(p);const auto serial=p.stats().resetSerial;
        require(p.audio(decodedClock(0),numbered(1),local)&&p.audio(decodedClock(stride),numbered(2),local+10000),"Decoded cadence ingress failed");
        const auto videoDeadline=p.videoDeadline();Pcm output;
        require(p.render(local+30000,local+30000,output).mediaFrames==480&&output==numbered(1),"Variable stride altered first decoded PCM");p.mediaCommitted();
        require(p.frontDeadline()==local+40000&&p.audioDeadline()==local+40000,"Decoded duration was replaced by source duration");
        require(p.render(local+40000,local+40000,output).mediaFrames==480&&output==numbered(2),"Variable stride inserted silence, skipped or replayed decoded samples");
        require(p.stats().resetSerial==serial&&p.videoDeadline()==videoDeadline&&p.localFor(sender)==local+30000&&
            !p.stats().silentFrames&&!p.stats().lateFrames&&p.stats().queued==0,"Decoded cadence moved video, reset output or distorted PCM");
    }
    {
        AudioPlayout p;p.enableAudioAlignment();Pcm output;std::uint64_t sourceTicks=0,checked=0;std::int64_t nextVideo=0;
        for(std::int64_t t=0;t<10000000;t+=1000){
            if(t>=nextVideo){require(p.video(clockAt(t),local+t),"Cadence soak video rejected");nextVideo=t+16667;}
            if(t%10000)continue;
            const auto index=t/10000;
            if(index)sourceTicks+=index%2?262:698;
            require(p.audio(decodedClock(sourceTicks),numbered(static_cast<int>(index%30+1)),local+t),"Alternating decoded cadence rejected");
            if(p.stats().resetPending)p.acknowledgeReset(p.stats().resetSerial);
            if(t<30000)continue;
            const auto r=p.render(local+t,local+t,output,AudioPlayout::SourceWindow{sender+t-110000,sender+t+50000});
            require(r.mediaFrames==480&&r.outputFrames==480&&output==numbered(static_cast<int>((index-3)%30+1)),"Alternating cadence lost exact decoded PCM order");checked++;
        }
        require(checked==997&&p.stats().cadenceAdjustedBlocks>400&&p.stats().maximumCadenceCorrectionUs<=5000&&
            p.stats().highWater<=6&&!p.stats().lateFrames&&!p.stats().silentFrames&&!p.stats().cadenceLimitResets,"Cadence soak enlarged queues or accumulated timing error");
    }
    {
        AudioPlayout p;p.enableAudioAlignment();anchor(p);p.audio(decodedClock(0),numbered(1),local);p.audio(decodedClock(698),numbered(2),local+10000);Pcm output;
        p.render(local+30000,local+30000,output);p.mediaCommitted();
        const auto prefix=p.render(local+40000,local+40000,output,AudioPlayout::SourceWindow{sender-80000,sender+20000});
        require(prefix.ready&&prefix.sourceLeadWait&&prefix.outputFrames>0&&prefix.outputFrames<480&&prefix.mediaFrames==prefix.outputFrames&&
            prefix.firstSourceUs==decodedClock(698).senderUs&&prefix.lastSourceUs<=sender+20000,"Corrected PCM bypassed its real source-time horizon");
        for(std::size_t n=0;n<prefix.outputFrames*2;n++)require(output[n]==numbered(2)[n],"Permitted source prefix corrupted PCM");
        const auto remainingAt=local+40000+static_cast<std::int64_t>(prefix.outputFrames)*1000000/48000;
        const auto rest=p.render(remainingAt,remainingAt,output,AudioPlayout::SourceWindow{sender-80000,sender+80000});
        require(rest.mediaFrames+prefix.mediaFrames==480&&p.stats().queued==0,"Fresh source horizon did not release exact remaining PCM");
        for(std::size_t n=0;n<rest.mediaFrames*2;n++)require(output[n]==numbered(2)[prefix.outputFrames*2+n],"Source prefix replayed or skipped retained samples");
        AudioPlayout stale;stale.enableAudioAlignment();anchor(stale);stale.audio(decodedClock(0),numbered(1),local);stale.audio(decodedClock(698),numbered(2),local+10000);
        stale.render(local+30000,local+30000,output);const auto serial=stale.stats().resetSerial;
        const auto rejected=stale.render(local+40000,local+40000,output,AudioPlayout::SourceWindow{sender+15000,sender+80000});
        require(rejected.sourceLagReset&&rejected.resetRequired&&!rejected.ready&&zero(output)&&!stale.stats().anchored&&
            stale.stats().queued==0&&stale.stats().resetSerial>serial,"Stale source PCM escaped the output fence");
    }
    {
        AudioPlayout p;p.enableAudioAlignment();anchor(p);const auto serial=p.stats().resetSerial;
        for(unsigned n=0;n<5;n++)require(p.audio(decodedClock(n*698),numbered(n+1),local+n*10000),"Within-bound cadence rejected");
        require(!p.audio(decodedClock(5*698),numbered(6),local+50000)&&p.stats().cadenceLimitResets==1&&
            p.stats().queued==0&&!p.stats().anchored&&p.stats().resetSerial>serial&&
            p.stats().maximumCadenceCorrectionUs<=AudioPlayout::maximumCadenceCorrectionUs,"Cadence correction grew without a bound");
        require(p.video(clockAt(60000),local+60000)&&p.audio(decodedClock(6*698),numbered(7),local+60000)&&
            p.audioDeadline()==p.localFor(decodedClock(6*698).senderUs),"A reset reused the retired decoded timeline");
        p.revoke();Pcm output;p.render(local+70000,local+70000,output);require(zero(output)&&!p.audio(decodedClock(7*698),numbered(8),local+70000),"Cadence recovery bypassed revocation");
    }
    {
        AudioPlayout p;p.enableAudioAlignment();anchor(p);p.audio(decodedClock(0),numbered(1),local);p.audio(decodedClock(960),numbered(2),local+20000);Pcm output;
        require(p.render(local+30000,local+30000,output).mediaFrames==480,"Missing-block control did not start");p.mediaCommitted();
        require(p.render(local+40000,local+40000,output).mediaFrames==0&&zero(output)&&p.frontDeadline()==local+50000,
            "A missing whole decoded block was concealed by cadence stitching");
        require(p.render(local+50000,local+50000,output).mediaFrames==480&&output==numbered(2)&&p.stats().cadenceDiscontinuities==1,"Missing-block source order changed");
        AudioPlayout incoherent;incoherent.enableAudioAlignment();anchor(incoherent);incoherent.audio(decodedClock(0),numbered(1),local);
        auto changed=decodedClock(698);changed.rtp=decodedClock(480).rtp;
        require(incoherent.audio(changed,numbered(2),local+10000)&&incoherent.audioDeadline()==incoherent.localFor(changed.senderUs)&&
            incoherent.stats().cadenceDiscontinuities==1,"Inconsistent source/RTP clocks were stitched as decoded cadence");
        AudioPlayout overlap;overlap.enableAudioAlignment();anchor(overlap);overlap.audio(decodedClock(0),numbered(1),local);
        const auto serial=overlap.stats().resetSerial;auto backwardPhase=decodedClock(262);backwardPhase.rtp=decodedClock(480).rtp;
        require(!overlap.audio(backwardPhase,numbered(2),local+10000)&&overlap.stats().cadenceDiscontinuities==1&&
            overlap.stats().cadenceLimitResets==1&&overlap.stats().queued==0&&!overlap.stats().anchored&&overlap.stats().resetSerial>serial,
            "Incoherent source phase overlapped retained decoded PCM");
    }
    {
        AudioPlayout waiting;waiting.enableAudioAlignment();anchor(waiting);const auto serial=waiting.stats().resetSerial;
        for(std::int64_t t=0;t<500000;t+=10000){
            if(t)require(waiting.video(clockAt(t),local+t),"Cold video clock rejected while audio trained");
            auto incomplete=clockAt(t);incomplete.reports=1;
            require(!waiting.audio(incomplete,pcm(71),local+t)&&waiting.stats().anchored&&waiting.stats().resetSerial==serial&&waiting.stats().queued==0,
                "Missing startup audio reports reset a valid video clock");
        }
        require(waiting.audio(clockAt(500000),pcm(72),local+500000),"Trained audio did not enter the retained video timeline");
        Pcm output;require(waiting.render(local+530000,local+530000,output).mediaFrames==480&&output==pcm(72),"Untimed startup PCM survived training");
        waiting.mediaCommitted();require(!waiting.audio({},pcm(73),local+540000)&&!waiting.stats().anchored,
            "Loss of trained audio timing during playback did not fence output");
    }
    {
        AudioPlayout early;early.enableAudioAlignment();anchor(early);
        require(early.audio(clockAt(),pcm(81),local+59000),"Early-frame control failed initial alignment");
        early.acknowledgeReset(early.stats().resetSerial);Pcm output;
        require(early.render(local+79000,local+79000,output).mediaFrames==480,"Early-frame control did not play initial PCM");early.mediaCommitted();
        early.audio(clockAt(10000),pcm(82),local+79000);require(early.video(clockAt(80000),local+79000),"Exact 80 ms video lead rejected");
        const auto serial=early.stats().resetSerial,queued=early.stats().queued;const auto deadline=early.videoDeadline();
        require(!early.video(clockAt(96667),local+95500)&&early.stats().resetSerial==serial&&early.stats().queued==queued&&
            early.videoDeadline()==deadline&&early.lastVideoArrival()==local+79000,"Slightly early video reset the common clock, refreshed freshness or discarded valid PCM");
        require(early.audio(clockAt(20000),pcm(83),local+95500),"Fresh PCM rejected after an early video drop");
        require(early.render(local+99000,local+99000,output).mediaFrames==480&&output==pcm(83)&&early.stats().resetSerial==serial,
            "Early-frame drop moved or replayed the PCM deadline");
        require(early.video(clockAt(113334),local+113500),"Next in-budget video did not recover after an early drop");
        require(early.video(clockAt(130000,2),local+128000)&&early.stats().resetSerial>serial&&
            early.videoDeadline()==local+158000,"Early-frame rejection hid a changed video clock epoch");
        AudioPlayout jump;jump.enableAudioAlignment();anchor(jump);const auto jumpSerial=jump.stats().resetSerial;
        require(jump.video(clockAt(80000),local+1)&&jump.stats().resetSerial>jumpSerial&&jump.videoDeadline()==local+30001,
            "Large early clock discontinuity bypassed its reset fence");
        AudioPlayout stale;stale.enableAudioAlignment();anchor(stale);const auto staleSerial=stale.stats().resetSerial;
        require(stale.video(clockAt(220000),local+160001)&&stale.stats().resetSerial>staleSerial&&stale.videoDeadline()==local+190001,
            "Early-frame dropping extended an expired video timeline");
    }
    {
        AudioPlayout early;early.enableAudioAlignment();anchor(early);
        require(early.audio(clockAt(),pcm(81),local+59000),"Bounded advance startup alignment failed");
        early.acknowledgeReset(early.stats().resetSerial);Pcm output;
        require(early.render(local+79000,local+79000,output).mediaFrames==480,"Bounded advance initial PCM missing");early.mediaCommitted();
        require(early.audio(clockAt(10000),pcm(82),local+80000),"Bounded advance fresh PCM rejected");
        const auto serial=early.stats().resetSerial;const auto audioDeadline=early.frontDeadline();const auto queued=early.stats().queued;
        require(early.video(clockAt(101000),local+80000,true)&&early.videoDeadline()==local+180000&&
            early.videoPresentationDeadline()==local+160000&&early.stats().maximumVideoAdvanceUs==20000&&
            early.stats().maximumPresentationLeadUs==80000&&early.stats().maximumAlignedVideoLeadUs==100000,
            "Exact bounded picture advance changed its source target or expanded the presentation wait");
        require(early.stats().resetSerial==serial&&early.frontDeadline()==audioDeadline&&early.stats().queued==queued&&
            early.render(local+89000,local+89000,output).mediaFrames==480&&output==pcm(82),"Earlier picture retimed, replayed or discarded exact PCM");
        auto sustained=std::make_unique<AudioPlayout>(early);
        for(std::int64_t t=16000;t<=10000000;t+=16000){
            require(sustained->video(clockAt(101000+t),local+80000+t,true)&&sustained->stats().resetSerial==serial&&
                sustained->videoDeadline()==local+180000+t&&sustained->videoPresentationDeadline()==local+160000+t,
                "Sustained earlier pictures failed to rebase the unchanged common timeline");
        }
        require(early.video(clockAt(125000),local+90000,true)&&early.stats().resetSerial>serial&&early.videoDeadline()==local+120000,
            "Over-budget video jump bypassed the full timing fence");
        AudioPlayout startup;startup.enableAudioAlignment();anchor(startup);
        require(!startup.video(clockAt(80000),local+10000,true)&&startup.stats().boundedVideoAdvances==0,
            "Bounded earlier presentation bypassed startup alignment");
    }
    {
        AudioPlayout recovering;recovering.enableAudioAlignment();anchor(recovering);
        const auto serial=recovering.stats().resetSerial;
        for(std::int64_t t=0;t<=630000;t+=10000){
            require(!t||recovering.video(clockAt(t),local+t),"Fresh recovery video was rejected");
            require(!recovering.audio(clockAt(t-70000),pcm(91),local+t),"Over-budget recovery audio was admitted");
            if(t<630000)require(recovering.stats().anchored&&!recovering.stats().alignmentLocked&&recovering.stats().resetSerial==serial&&recovering.stats().queued==0,
                "Unalignable startup PCM reset before its bounded rejection limit");
        }
        require(recovering.stats().alignmentRejected==64&&recovering.stats().alignmentRejectResets==1&&recovering.stats().anchored&&
            recovering.stats().resetSerial>serial,
            "Repeated alignment rejection did not force one bounded audio reanchor");
        AudioPlayout queued;queued.enableAudioAlignment();anchor(queued);queued.audio(clockAt(),pcm(93),local);
        queued.video(clockAt(80000),local+80000);const auto queuedSerial=queued.stats().resetSerial;
        require(!queued.audio(clockAt(10000),pcm(94),local+90000)&&queued.stats().queued==0&&queued.stats().anchored&&
            queued.stats().resetSerial==queuedSerial,"Unalignable startup PCM retained older queued samples or reset video");
        AudioPlayout shift;shift.enableAudioAlignment();anchor(shift);const auto shiftSerial=shift.stats().resetSerial;
        require(!shift.audio(clockAt(-70000),pcm(95),local),"Unalignable control unexpectedly fit");shift.video(clockAt(10000),local+10000);
        require(shift.audio(clockAt(-10000),pcm(96),local+20000)&&shift.stats().alignments==1&&shift.stats().resetSerial==shiftSerial+1&&
            shift.videoDeadline()==local+60000,"Fresh recovery alignment did not fence exactly one legal timeline shift");
        shift.acknowledgeReset(shift.stats().resetSerial);
        Pcm output;
        require(shift.render(local+40000,local+40000,output).mediaFrames==480&&output==pcm(96),"Legally realigned recovery played rejected PCM or lost fresh PCM");
        AudioPlayout expire;expire.enableAudioAlignment();anchor(expire);expire.audio(clockAt(-70000),pcm(97),local);
        require(!expire.audio(clockAt(90000),pcm(98),local+150001)&&!expire.stats().anchored,"Dropping late startup PCM extended stale video clock lifetime");
    }
    {
        AudioPlayout aligned;aligned.enableAudioAlignment();anchor(aligned);
        const auto old=aligned.stats().resetSerial;
        require(aligned.audio(clockAt(),pcm(6),local+45000),"Late decoded audio could not align the common clock");
        require(aligned.videoDeadline()==local+65000&&aligned.stats().alignments==1&&aligned.stats().resetSerial!=old,"Alignment did not move both outputs and fence the old timeline");
        aligned.acknowledgeReset(aligned.stats().resetSerial);Pcm output;
        require(aligned.render(local+65000,local+65000,output).mediaFrames==480&&output==pcm(6),"Aligned original PCM did not reach its new deadline");
        AudioPlayout capped;capped.enableAudioAlignment();anchor(capped);
        const auto cappedSerial=capped.stats().resetSerial;
        require(!capped.audio(clockAt(),pcm(7),local+90000)&&capped.stats().anchored&&capped.stats().queued==0&&
            capped.stats().resetSerial==cappedSerial&&capped.stats().alignmentRejected==1,"Common clock exceeded its 80 ms cap or rejected PCM reset valid video");
        AudioPlayout early;early.enableAudioAlignment();anchor(early);early.audio(clockAt(),pcm(1),local+45000);
        const auto earlySerial=early.stats().resetSerial;
        require(!early.video(clockAt(80000),local+47000)&&early.videoDeadline()==local+65000&&early.stats().earlyVideoDrops==1&&
            early.stats().resetSerial==earlySerial&&early.stats().maximumAlignedVideoLeadUs<=80000,"An early frame escaped the cap or reset an otherwise valid common clock");
        require(!early.localFor(INT64_MAX)&&!early.senderAt(INT64_MAX),"Clock conversion accepted an overflowing domain");
    }
    {
        AudioPlayout p;Pcm output;output.fill(7);
        require(!p.audio(clockAt(),pcm(1),local),"Audio invented its own video timeline");
        auto r=p.render(local,local,output);require(!r.ready&&zero(output),"Unanchored output leaked samples");
        anchor(p);require(p.audio(clockAt(),pcm(1234),local),"Valid PCM rejected");
        r=p.render(local,local,output);require(r.ready&&!r.mediaFrames&&zero(output),"Early audio was played");
        r=p.render(local+30000,local+30000,output);
        require(r.mediaFrames==480&&output==pcm(1234),"Timestamped stereo did not land on its exact deadline");
        require(!p.audio(clockAt(),pcm(7),local+30000),"Duplicate RTP/PCM accepted");
    }
    {
        AudioPlayout p;anchor(p);Pcm output;
        for(int i=0;i<7;i++)require(p.audio(clockAt(i*10000),pcm(i+1),local),"Bounded future PCM rejected");
        require(p.stats().queued==6&&p.stats().highWater==6&&p.stats().overflowBlocks==1,"FIFO overflow was not bounded/latest");
        p.render(local+30000,local+30000,output);require(zero(output),"Dropped oldest block replayed");
        p.render(local+40000,local+40000,output);require(output==pcm(2),"Fresh block order changed");
        p.render(local+61001,local+61001,output);require(zero(output)&&p.stats().expiredBlocks==5,"Old decoded PCM survived residence limit");
    }
    {
        AudioPlayout p;anchor(p);Pcm output;
        p.audio(clockAt(),pcm(9),local);
        const auto shortened=p.render(local+35000,local+35000,output);
        require(shortened.outputFrames==240&&shortened.mediaFrames==240&&p.stats().lateFrames==240&&output[0]==9&&output[480]==0,"Late PCM was delayed or the endpoint extended beyond known PCM");
        auto serial=p.stats().resetSerial;
        require(!p.audio(clockAt(40000,2),pcm(8),local+40000),"Audio epoch change played before a fresh video anchor");
        require(!p.acknowledgeReset(serial)&&p.stats().queued==0,"Old device reset acknowledged a new timeline");
        require(p.video(clockAt(50000),local+50000),"Fresh video did not reanchor");
        p.acknowledgeReset(p.stats().resetSerial);
        require(!p.audio(clockAt(60000),pcm(1),local+60000),"Retired audio epoch revived");
        require(p.audio(clockAt(60000,2),pcm(2),local+60000),"Current audio epoch rejected");
        p.revoke();p.acknowledgeReset(p.stats().resetSerial);
        require(!p.video(clockAt(70000,2),local+70000)&&!p.audio(clockAt(70000,2),pcm(1),local+70000),"Terminal playout reopened");
        output.fill(123);p.render(local+70000,local+70000,output);require(zero(output),"Revoked PCM retained in output");
    }
    {
        AudioPlayout p;anchor(p);Pcm output;
        require(!p.audio(clockAt(3000000),pcm(1),local),"Unrelated sender time accepted");
        require(!p.video({std::numeric_limits<std::int64_t>::max(),0,1,2,true},local),"Overflowing NTP accepted");
        require(!p.video(clockAt(),std::numeric_limits<std::int64_t>::max()),"Overflowing local time accepted");
        AudioPlayout valid;anchor(valid);valid.audio(clockAt(),pcm(4),local);
        valid.render(local,local+30001,output);require(zero(output)&&!valid.stats().anchored,"Excess endpoint lead accepted");
        AudioPlayout stall;anchor(stall);stall.audio(clockAt(),pcm(4),local);
        stall.render(local+150001,local+150001,output);require(zero(output)&&!stall.stats().anchored&&stall.stats().queued==0,"Video stall kept audio running as synchronized");
    }
    // Completed capture blocks and differently phased endpoint packets. A
    // request spanning the known PCM tail must shrink rather than commit a
    // silent suffix that steals the next block's scheduled sample positions.
    for(std::int64_t phase=0;phase<10000;phase+=1000){
        AudioPlayout timed;Pcm output;std::int64_t nextVideo=0;std::uint64_t deviceFrames=0,media=0;
        for(std::int64_t t=0;t<2000000;t+=1000){
            if(t>=nextVideo){require(timed.video(clockAt(t),local+t),"Phased video rejected");nextVideo=t+16667;}
            if(t>=10000&&t%10000==0)require(timed.audio(clockAt(t-10000),pcm(4),local+t),"Completed capture block rejected");
            if(timed.stats().resetPending)timed.acknowledgeReset(timed.stats().resetSerial);
            const auto audible=local+22000+phase+static_cast<std::int64_t>(deviceFrames)*1000000/48000;
            if(audible>local+t+15000)continue;
            require(audible>=local+t,"Phased endpoint underrun");
            const auto rendered=timed.render(local+t,audible,output);
            require(rendered.ready&&rendered.outputFrames>0,"Phased output failed");
            deviceFrames+=rendered.outputFrames;media+=rendered.mediaFrames;
        }
        require(media>deviceFrames*.99,"Packet/capture phase caused repeated artificial silence");
    }
    // Ten minutes of virtual time, generated PCM only. This is NOT a device,
    // real-time soak, acoustic sync or measured ten-minute clock-drift test.
    AudioPlayout p;Pcm output;std::int64_t nextVideo=0;
    std::uint64_t checked=0;
    for(std::int64_t t=0;t<600000000;t+=1000){
        const int rates[]{60,55,45,30};const int fps=rates[(t/2500000)%4];
        if(t>=nextVideo){require(p.video(clockAt(t),local+t),"Variable FPS video rejected");nextVideo=t+1000000/fps;}
        if(t%10000)continue;
        const auto index=t/10000;
        require(p.audio(clockAt(t),pcm(static_cast<int>(index%30000+1)),local+t),"Ordered sample clock rejected");
        if(p.stats().resetPending)p.acknowledgeReset(p.stats().resetSerial);
        const bool clogged=t>=2500000&&t<2600000;
        if(clogged)continue;
        const auto r=p.render(local+t,local+t,output);
        if(t>=30000){require(r.ready&&r.mediaFrames==480&&output==pcm(static_cast<int>((index-3)%30000+1)),"Virtual PCM content drifted or replayed after congestion");checked++;}
    }
    require(p.stats().highWater<=6&&p.stats().overflowBlocks>0,"Virtual congestion did not exercise bounded eviction");
    std::cout<<"Bounded 60 ms PCM, stereo deadlines, RTP wrap, epoch/reset fencing, variable 60/55/45/30 FPS and 600 virtual seconds passed; checked_blocks="<<checked<<" scheduler_bytes="<<sizeof(AudioPlayout)<<"; no physical playback\n";
    return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
