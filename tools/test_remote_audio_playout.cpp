#include "remote_audio_playout.h"
#include "frame_metadata_codec.h"
#include <QCoreApplication>
#include <QJsonDocument>
#include <QJsonArray>
#include <algorithm>
#include <atomic>
#include <future>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <thread>
using namespace goodsharing;
using Pcm=std::array<std::int16_t,960>;
using Pump=RemoteAudioPlayout::Pump;
void require(bool value,const char* message){if(!value)throw std::runtime_error(message);}
constexpr std::int64_t sender=4000000000000000ll;
PlayoutClock clockAt(std::int64_t t=0){return {sender+t,static_cast<std::uint32_t>(t*48/1000),1,2,true};}
Pcm pcm(int value){Pcm p;p.fill(static_cast<std::int16_t>(value));return p;}
struct Sink final:AudioPlayoutSink {
    Pcm hardware{};bool reserved=false,commitOk=true,resetOk=true;unsigned commits=0,resets=0,cancels=0;
    std::size_t frames=480,lastCommittedFrames=0;std::function<void()> onPrepare,onCommit;std::optional<std::int64_t> audibleOverride;
    std::optional<Window> prepare(std::int64_t now)override{reserved=true;if(onPrepare)onPrepare();return Window{audibleOverride.value_or(now),frames};}
    bool commit(std::span<const std::int16_t> data)override{require(reserved&&!data.empty()&&data.size()<=frames*2&&data.size()%2==0,"Invalid or unreserved commit");if(onCommit)onCommit();commits++;if(commitOk){hardware.fill(0);std::copy(data.begin(),data.end(),hardware.begin());lastCommittedFrames=data.size()/2;reserved=false;}return commitOk;}
    void cancel()noexcept override{reserved=false;cancels++;}
    bool reset()noexcept override{resets++;if(resetOk){hardware.fill(0);reserved=false;}return resetOk;}
};
struct Fixture {
    std::int64_t now=1000000;Sink sink;std::shared_ptr<std::atomic_bool> room=std::make_shared<std::atomic_bool>(true);
    std::shared_ptr<RemoteShare> watch=std::make_shared<RemoteShare>(RemoteShare::Manifest{"alice","0123456789abcdef0123456789abcdef",1,ShareTrack::Video720,"video","audio"},RemoteShare::Callbacks{[](livekit::VideoFrameEvent&){},[](livekit::AudioFrameEvent&){}},room);
    RemoteAudioPlayout output{sink,[this]{return now;}};
    Fixture(bool presentation=false){if(presentation)output.requirePresentation();output.bind(watch);}
    void verify(){watch->encryption("alice","video",livekit::EncryptionState::Ok);watch->encryption("alice","audio",livekit::EncryptionState::Ok);}
    void prime(){verify();require(output.video(clockAt())&&output.audio(clockAt(),pcm(1234)),"Verified audio ingress rejected");require(output.pump()==Pump::Reset,"Device was not reset before first commit");now+=30000;}
};
int main(int argc,char** argv){QCoreApplication app(argc,argv);try{
    {
        Sink sink;std::int64_t now=1000000;
        auto watch=std::make_shared<RemoteShare>(RemoteShare::Manifest{"alice","0123456789abcdef0123456789abcdef",1,ShareTrack::Video720,"video","audio"},RemoteShare::Callbacks{[](livekit::VideoFrameEvent&){},[](livekit::AudioFrameEvent&){}});
        RemoteAudioPlayout output(sink,[&]{return now;});output.requirePresentation();output.enableAudioContinuity();output.bind(watch);
        watch->encryption("alice","video",livekit::EncryptionState::Ok);watch->encryption("alice","audio",livekit::EncryptionState::Ok);
        auto first=output.scheduleVideo(clockAt());output.audio(clockAt(),pcm(1));require(output.pump()==Pump::Reset,"Audio-continuity control initial reset missing");
        now=1030000;require(first&&output.presented(*first)&&output.pump()==Pump::Committed,"Audio-continuity media did not establish");
        const auto resets=sink.resets;
        for(int n=1;n<=10;n++){
            now=1030000+(n-1)*10000;require(output.audio(clockAt(n*10000),pcm(1+n)),"Aligned audio rejected during video stall");
            now=1030000+n*10000;
            require(output.pump()==Pump::Committed&&sink.hardware[0]==1+n,"Video stall interrupted continuous audio");
        }
        require(sink.resets==resets&&output.report()["presentation_age_resets"].toInt()==0&&
            output.report()["audio_continuity_through_video_stall"].toBool(),"Audio-priority policy flushed on a video presentation stall");
        const auto serial=output.report()["reset_serial"].toDouble();
        require(!output.video(clockAt(300000))&&output.report()["reset_serial"].toDouble()==serial&&
            output.report()["early_video_drops"].toInt()>0,"Established audio lost its anchor to an early video packet");
    }
    {
        auto storage=std::make_unique<Fixture>(true);auto& f=*storage;f.verify();
        auto first=f.output.scheduleVideo(clockAt());require(first&&f.output.audio(clockAt(),pcm(111)),"Early-picture control startup failed");
        f.output.pump();f.now=1030000;require(f.output.presented(*first)&&f.output.pump()==Pump::Committed,"Early-picture control initial media missing");
        f.now=1031000;const auto before=f.output.report().value("reset_serial");
        auto normal=f.output.scheduleVideo(clockAt(80000));require(normal.has_value(),"Early-picture control normal ticket missing");
        std::array<PresentationTicket,4> tickets;
        for(int i=0;i<4;++i){
            f.now=1045000+i*16000;
            const auto ticket=f.output.scheduleVideo(clockAt(96667+i*16667));
            require(ticket&&ticket->deadlineUs==f.now+80000&&f.output.report()["reset_serial"]==before,
                QString("earlyDrops=%1 advances=%2 locked=%3").arg(f.output.report()["early_video_drops"].toInt()).arg(f.output.report()["bounded_video_advance_tickets"].toInt()).arg(f.output.report()["alignment_locked_after_media_commit"].toBool()).toUtf8().constData());
            tickets[i]=*ticket;
        }
        f.now=normal->deadlineUs;require(f.output.presented(*normal),"Earlier queued picture was invalidated by bounded advancement");
        auto previous=f.now;
        for(const auto ticket:tickets){f.now=ticket.deadlineUs;
            require(f.now-previous<=17000&&f.output.ticketValid(ticket)&&f.output.presented(ticket),"Early-picture correction left a presentation hole");previous=f.now;}
        for(const auto kind:{0,1,2,3,4,5}){
            auto forged=tickets.back();
            if(kind==0)forged.timelineDeadlineUs++;
            if(kind==1)forged.deadlineUs--;
            if(kind==2)forged.candidateClock.senderUs++;
            if(kind==3)forged.candidateClock.epoch++;
            if(kind==4)forged.timelineDeadlineUs=std::numeric_limits<std::int64_t>::min();
            if(kind==5)forged.receivedUs=std::numeric_limits<std::int64_t>::max();
            require(!f.output.ticketValid(forged)&&!f.output.presented(forged),"Forged early-picture mapping or receipt was accepted");
        }
        f.watch->encryption("alice","audio",livekit::EncryptionState::MissingKey);
        require(!f.output.presented(tickets.back())&&f.output.pump()==Pump::Closed&&f.sink.hardware==pcm(0),"Earlier presentation bypassed paired final permission");
    }
    for(const bool revoke:{false,true}){
        auto storage=std::make_unique<Fixture>(true);auto& f=*storage;f.verify();
        auto first=f.output.scheduleVideo(clockAt());require(first&&f.output.audio(clockAt(),pcm(111)),"Endpoint recovery startup failed");
        f.output.pump();f.now=1030000;require(f.output.presented(*first)&&f.output.pump()==Pump::Committed,"Endpoint recovery initial PCM missing");
        f.now=1035000;const auto next=f.output.scheduleVideo(clockAt(10000));
        require(next&&f.output.audio(clockAt(10000),pcm(121)),"Endpoint recovery old PCM missing");
        f.now=1036000;require(f.output.audio(clockAt(20000),pcm(222)),"Endpoint recovery fresh PCM missing");
        f.now=1040000;require(f.output.presented(*next),"Endpoint recovery second picture missing");
        const auto serial=f.output.report().value("reset_serial");const auto deadline=next->deadlineUs;const auto resets=f.sink.resets;
        f.sink.onPrepare=[] {throw EndpointDiscontinuity(7000);};require(f.output.pump()==Pump::Reset,"Short device gap did not reset endpoint");
        require(f.sink.resets==resets+1&&f.sink.hardware==pcm(0)&&f.output.report()["reset_serial"]==serial&&f.output.ticketValid(*next)&&
            next->deadlineUs==deadline&&f.output.report()["endpoint_timeline_preserved"].toInt()==1,"Short device gap discarded a valid common timeline");
        f.now=1045000;f.sink.audibleOverride=1050000;f.sink.onPrepare={};const auto commits=f.sink.commits;
        if(revoke)f.sink.onPrepare=[&]{f.watch->encryption("alice","audio",livekit::EncryptionState::MissingKey);};
        const auto result=f.output.pump();
        if(revoke)require(result==Pump::Closed&&f.sink.commits==commits&&f.sink.hardware==pcm(0)&&!f.output.ticketValid(*next),"Device-only recovery bypassed final revocation");
        else require(result==Pump::Committed&&f.sink.commits==commits+1&&f.sink.hardware==pcm(222)&&f.output.report()["late_frames"].toInt()==480,
            "Device-only recovery replayed expired PCM or lost fresh samples");
    }
    {
        auto storage=std::make_unique<Fixture>(true);auto& f=*storage;f.verify();
        const auto first=f.output.scheduleVideo(clockAt());f.output.audio(clockAt(),pcm(111));f.output.pump();f.now=1030000;
        require(first&&f.output.presented(*first)&&f.output.pump()==Pump::Committed,"Long device-gap control did not start");
        const auto serial=f.output.report().value("reset_serial");f.now=1070001;f.sink.onPrepare=[] {throw EndpointDiscontinuity(1);};
        require(f.output.pump()==Pump::Reset&&f.output.report().value("reset_serial").toDouble()>serial.toDouble()&&!f.output.ticketValid(*first)&&
            f.output.report()["endpoint_timeline_preserved"].toInt()==0&&f.sink.hardware==pcm(0),"Long actual output absence trusted a clipped one-microsecond device gap");
    }
    {
        auto storage=std::make_unique<Fixture>(true);auto& f=*storage;f.verify();
        const auto first=f.output.scheduleVideo(clockAt());require(first&&f.output.audio(clockAt(),pcm(111)),"Cadence adapter did not start");
        f.now+=10000;auto changed=clockAt(14541);changed.rtp=698;require(f.output.audio(changed,pcm(222)),"Adapted source clock rejected");
        require(f.output.pump()==Pump::Reset,"Cadence adapter reset missing");f.now=1030000;
        require(f.output.presented(*first)&&f.output.pump()==Pump::Committed&&f.sink.hardware==pcm(111),"Cadence adapter lost first PCM");
        const auto resets=f.sink.resets;f.now=1040000;
        require(f.output.pump()==Pump::Committed&&f.sink.hardware==pcm(222)&&f.sink.lastCommittedFrames==480&&f.sink.resets==resets&&
            f.output.ticketValid(*first)&&f.output.report()["decoded_cadence_adjusted_blocks"].toInt()==1,"Cadence adapter moved video, reset or changed decoded PCM");
        auto next=clockAt(24541);next.rtp=1178;require(f.output.audio(next,pcm(333)),"Cadence tail did not continue");
        f.now=1050000;f.sink.onPrepare=[&]{f.watch->encryption("alice","audio",livekit::EncryptionState::MissingKey);};const auto commits=f.sink.commits;
        require(f.output.pump()==Pump::Closed&&f.sink.commits==commits&&f.sink.hardware==pcm(0)&&!f.output.ticketValid(*first),
            "Corrected PCM bypassed final encryption revocation");
    }
    {
        qputenv("GOODSHARING_TEST_AV_TRACE","1");
        auto fStorage=std::make_unique<Fixture>(true);auto& f=*fStorage;f.verify();
        const auto first=f.output.scheduleVideo(clockAt());f.output.audio(clockAt(),pcm(111));f.output.pump();f.now+=30000;
        require(first&&f.output.presented(*first)&&f.output.pump()==Pump::Committed,"First-fault control did not start media");
        f.now+=20000;f.sink.onPrepare=[&]{throw EndpointDiscontinuity(7000);};
        require(f.output.pump()==Pump::Reset,"First-fault endpoint discontinuity did not flush");
        const auto saved=f.output.report()["test_first_playback_fault"].toObject();const auto event=saved["event"].toObject();
        const auto live=f.output.report(false);
        require(!live["test_history_included"].toBool()&&live["test_first_playback_fault"].toObject().isEmpty()&&
            live["test_timing_trace"].toArray().isEmpty()&&live["test_reset_trace"].toArray().isEmpty()&&
            live["committed_media_frames"].toInt()==480&&f.output.report()["test_first_playback_fault"].toObject()==saved,
            "Compact live reporting lost counters or destroyed retained stopped history");
        require(event["kind"].toInt()==4&&event["endpoint_gap_us"].toInt()==7000&&event["committed_media_frames"].toInt()==480&&
            event["pump_gap_us"].toInt()==20000&&event["since_commit_us"].toInt()==20000&&event["was_alignment_locked"].toBool()&&
            !saved["preceding"].toArray().isEmpty(),"First playback fault lacks pre-reset timing/context");
        f.sink.onPrepare={};
        for(int n=1;n<=24;n++){f.now+=10000;f.output.video(clockAt(50000+n*10000));f.output.video({});}
        require(f.output.report()["test_reset_trace"].toArray().size()==16&&f.output.report()["test_first_playback_fault"].toObject()==saved,
            "Later recovery events overwrote the first playback fault");
        qunsetenv("GOODSHARING_TEST_AV_TRACE");
        auto quiet=std::make_unique<Fixture>();quiet->prime();quiet->output.pump();quiet->output.video({});
        require(quiet->output.report()["test_first_playback_fault"].toObject().isEmpty(),"Disabled diagnostics retained first-fault history");
    }
    {
        auto fStorage=std::make_unique<Fixture>(true);auto& f=*fStorage;f.verify();auto first=f.output.scheduleVideo(clockAt());f.output.pump();
        f.now=1030000;require(first&&f.output.presented(*first),"Startup PCM-wait control lacks a picture");
        bool prepared=false;f.sink.onPrepare=[&]{prepared=true;};
        require(f.output.pump()==Pump::Waiting&&!prepared&&!f.sink.commits,"Output device was prepared before valid startup PCM existed");
        const auto serial=f.output.report().value("reset_serial");
        require(!f.output.audio({},pcm(444))&&f.output.ticketValid(*first)&&f.output.report()["reset_serial"]==serial,
            "Untrained audio invalidated an independently valid picture");
        f.now=1040000;auto fresh=f.output.scheduleVideo(clockAt(40000));f.output.audio(clockAt(40000),pcm(445));
        f.now=1070000;require(fresh&&f.output.presented(*fresh)&&f.output.pump()==Pump::Committed&&f.sink.hardware==pcm(445),
            "Fresh trained PCM did not start under original timing and permission");
    }
    {
        auto fStorage=std::make_unique<Fixture>(true);auto& f=*fStorage;f.verify();
        auto first=f.output.scheduleVideo(clockAt());f.output.audio(clockAt(),pcm(111));f.output.pump();
        const auto serial=f.output.report().value("reset_serial");const auto resets=f.sink.resets;
        f.now=1080000;auto next=f.output.scheduleVideo(clockAt(80000));f.now=1090000;
        require(!f.output.audio(clockAt(10000),pcm(222))&&next&&f.output.ticketValid(*next)&&
            f.output.report()["queued_blocks"].toInt()==0&&f.output.report()["reset_serial"]==serial,
            "Rejected startup audio invalidated the fresh picture or retained old PCM");
        f.now=1110000;require(f.output.presented(*next)&&f.output.pump()==Pump::Waiting&&!f.sink.commits&&!f.sink.reserved&&f.sink.hardware==pcm(0),
            "Unalignable startup audio prepared output or leaked samples while waiting for fresh PCM");
        f.now=1120000;auto fresh=f.output.scheduleVideo(clockAt(120000));require(f.output.audio(clockAt(120000),pcm(333)),"Fresh recovery PCM rejected");
        f.now=1150000;require(fresh&&f.output.presented(*fresh)&&f.output.pump()==Pump::Committed&&f.sink.hardware==pcm(333)&&f.sink.resets==resets,
            "Fresh audio failed to recover under existing presentation/device permission");
    }
    {
        auto fStorage=std::make_unique<Fixture>(true);auto& f=*fStorage;f.verify();auto first=f.output.scheduleVideo(clockAt());f.output.audio(clockAt(),pcm(111));f.output.pump();f.now+=30000;
        require(first&&f.output.presented(*first)&&f.output.pump()==Pump::Committed,"Permitted-prefix control did not start");
        f.now=1080000;auto next=f.output.scheduleVideo(clockAt(80000));f.output.audio(clockAt(80000),pcm(777));const auto resets=f.sink.resets;
        f.now=1101000;
        require(f.output.pump()==Pump::Committed&&f.sink.lastCommittedFrames==433&&f.sink.hardware[864]==777&&f.sink.hardware[866]==0,
            "A permitted device prefix was discarded because only its suffix exceeded the A/V limit");
        require(std::all_of(f.sink.hardware.begin(),f.sink.hardware.begin()+864,[](auto s){return s==0;})&&f.output.ticketValid(*first)&&f.sink.resets==resets,
            "Partial A/V packet changed timing, invented PCM or exceeded its admitted prefix");
        f.now=1110020;
        require(next&&f.output.presented(*next)&&f.output.pump()==Pump::Committed&&f.sink.lastCommittedFrames==479&&f.sink.hardware[0]==777&&f.sink.hardware[956]==777&&f.sink.hardware[958]==0&&f.sink.resets==resets,
            "Fresh video did not release the exact remaining PCM after a permitted prefix");
    }
    {
        auto fStorage=std::make_unique<Fixture>(true);auto& f=*fStorage;f.verify();auto first=f.output.scheduleVideo(clockAt());
        f.output.audio(clockAt(),pcm(111));f.output.pump();f.now+=30000;
        require(first&&f.output.presented(*first)&&f.output.pump()==Pump::Committed,"PCM arrival-wait control did not start");
        f.sink.audibleOverride=1040000;
        const auto commits=f.sink.commits,resets=f.sink.resets;
        require(f.output.pump()==Pump::Waiting&&f.sink.commits==commits&&f.output.ticketValid(*first),
            "Empty PCM queue committed future silence before a still-timely block could arrive");
        f.now+=2000;require(f.output.audio(clockAt(10000),pcm(222)),"Still-timely delayed block rejected");
        require(f.output.pump()==Pump::Committed&&f.sink.hardware==pcm(222)&&f.sink.resets==resets,
            "Waiting for PCM did not preserve the block's original deadline without a reset");
        f.sink.audibleOverride=1050000;f.now=1040000;
        require(f.output.pump()==Pump::Waiting,"A missing block did not wait before its write guard");
        f.now=1046000;
        require(f.output.pump()==Pump::Committed&&f.sink.hardware==pcm(0)&&f.sink.resets==resets,
            "Missing PCM waited beyond the bounded write guard instead of committing silence");
        require(f.output.report()["pcm_arrival_waits"].toInt()==2,"PCM wait diagnostics lost bounded-wait evidence");
    }
    {
        auto fStorage=std::make_unique<Fixture>(true);auto& f=*fStorage;f.verify();auto first=f.output.scheduleVideo(clockAt());f.output.audio(clockAt(),pcm(111));f.output.pump();f.now+=30000;
        require(first&&f.output.presented(*first)&&f.output.pump()==Pump::Committed,"Queued-tail wait control did not start");
        require(f.output.audio(clockAt(10000),pcm(222)),"Known old tail was rejected");f.now=1040000;f.sink.audibleOverride=1050000;
        require(f.output.pump()==Pump::Waiting&&f.output.report()["queued_blocks"].toInt()==1,"Passed known tail committed future silence");
        f.now=1042000;require(f.output.audio(clockAt(20000),pcm(333))&&f.output.pump()==Pump::Committed&&f.sink.hardware==pcm(333),
            "New PCM after a known tail was not delivered at the original deadline");
        require(f.output.report()["late_frames"].toInt()==480,"Superseded PCM was replayed or not diagnosed");
        f.sink.audibleOverride=1060000;
        require(f.output.pump()==Pump::Waiting,"Empty tail did not wait before revocation");f.watch->revoke();
        require(f.output.pump()==Pump::Closed&&f.sink.hardware==pcm(0)&&!f.output.ticketValid(*first),"PCM waiting bypassed final revoke/flush");
    }
    {
        auto fStorage=std::make_unique<Fixture>(true);auto& f=*fStorage;f.verify();auto first=f.output.scheduleVideo(clockAt());f.output.audio(clockAt(),pcm(111));f.output.pump();f.now+=30000;
        require(first&&f.output.presented(*first)&&f.output.pump()==Pump::Committed,"Wait residence control did not start");
        f.output.audio(clockAt(10000),pcm(222));f.now=1090001;f.sink.audibleOverride=1100000;
        require(f.output.pump()==Pump::Waiting&&f.output.report()["queued_blocks"].toInt()==0&&f.output.report()["expired_blocks"].toInt()==1,
            "Waiting retained PCM past its unchanged 60 ms residence bound");
        auto plainStorage=std::make_unique<Fixture>();auto& plain=*plainStorage;plain.prime();require(plain.output.pump()==Pump::Committed,"Uncoordinated endpoint control did not commit initial PCM");
        plain.sink.audibleOverride=1040000;
        require(plain.output.pump()==Pump::Committed&&plain.sink.hardware==pcm(0)&&plain.output.report()["pcm_arrival_waits"].toInt()==0,
            "Coordinated PCM wait changed the standalone endpoint policy");
    }
    for(const auto invalid:{INT64_MIN,INT64_MAX}){
        auto fStorage=std::make_unique<Fixture>(true);auto& f=*fStorage;f.verify();auto first=f.output.scheduleVideo(clockAt());
        f.output.audio(clockAt(),pcm(111));f.output.pump();f.now+=30000;
        require(first&&f.output.presented(*first)&&f.output.pump()==Pump::Committed,"Invalid endpoint timestamp control did not start");
        f.sink.audibleOverride=invalid;
        require(f.output.pump()==Pump::Reset&&f.sink.commits==1&&f.sink.hardware==pcm(0)&&!f.output.ticketValid(*first),
            "Extreme endpoint timestamp admitted audio or retained the old timeline");
    }
    {
        auto fStorage=std::make_unique<Fixture>(true);auto& f=*fStorage;f.verify();auto first=f.output.scheduleVideo(clockAt());
        f.output.audio(clockAt(),pcm(111));f.output.pump();f.now+=30000;
        require(first&&f.output.presented(*first)&&f.output.pump()==Pump::Committed,"Presentation lead control did not start");
        f.now=1080000;auto next=f.output.scheduleVideo(clockAt(80000));f.output.audio(clockAt(80000),pcm(777));
        const auto resets=f.sink.resets,commits=f.sink.commits;f.now=1101000;f.sink.audibleOverride=1110001;
        require(f.output.pump()==Pump::Waiting&&f.output.ticketValid(*first)&&f.sink.resets==resets&&f.sink.commits==commits&&f.output.report()["queued_blocks"].toInt()==1,
            "A future audio window reset or consumed PCM instead of waiting for a due video presentation");
        f.sink.audibleOverride.reset();f.now=1110000;require(next&&f.output.presented(*next)&&f.output.pump()==Pump::Committed&&f.sink.hardware==pcm(777)&&f.sink.resets==resets,
            "Fresh queued video did not release the untouched audio window without a reset");
        require(f.output.report()["presentation_lead_waits"].toInt()==1&&f.output.report()["presentation_resets"].toInt()==0,"Presentation wait/reset diagnostics are inconsistent");
    }
    {
        auto fStorage=std::make_unique<Fixture>(true);auto& f=*fStorage;f.verify();auto first=f.output.scheduleVideo(clockAt());
        f.output.audio(clockAt(),pcm(111));f.output.pump();f.now+=30000;
        require(first&&f.output.presented(*first)&&f.output.pump()==Pump::Committed,"Bounded presentation-wait control did not start");
        f.now=1080000;f.output.scheduleVideo(clockAt(80000));f.output.audio(clockAt(80000),pcm(777));f.now=1101000;f.sink.audibleOverride=1110001;
        require(f.output.pump()==Pump::Waiting,"Future audio did not wait for presentation");
        f.now=1110001;require(f.output.pump()==Pump::Reset&&f.sink.hardware==pcm(0)&&f.sink.commits==1&&!f.output.ticketValid(*first),
            "Waiting for future video bypassed the unchanged presentation-age bound");
        require(f.output.report()["presentation_age_resets"].toInt()==1,"Expired presentation was not diagnosed separately");
    }
    {
        auto fStorage=std::make_unique<Fixture>(true);auto& f=*fStorage;f.verify();auto first=f.output.scheduleVideo(clockAt());
        f.output.audio(clockAt(),pcm(111));f.output.pump();f.now+=30000;
        require(first&&f.output.presented(*first)&&f.output.pump()==Pump::Committed,"Late-jitter control did not establish playback");
        const auto resets=f.sink.resets;f.now+=15000;
        require(f.output.audio(clockAt(10000),pcm(222)),"Isolated late block rejected before trimming");
        require(f.output.ticketValid(*first)&&f.sink.resets==resets,"An isolated late audio block invalidated established video presentation");
        require(f.output.pump()==Pump::Committed&&f.sink.lastCommittedFrames==240&&f.sink.hardware[0]==222,
            "Isolated late audio was not trimmed on the established timeline");
        require(f.output.audio(clockAt(30000),pcm(333)),"On-time recovery block rejected");f.now+=15000;
        require(f.output.pump()==Pump::Committed&&f.sink.hardware==pcm(333)&&f.output.ticketValid(*first),"On-time audio failed to recover without a new presentation");
        for(int n=4;n<=5;n++){
            f.now=1000000+n*10000+35000;
            require(f.output.audio(clockAt(n*10000),pcm(444))&&f.output.pump()==Pump::Committed&&f.output.ticketValid(*first),"Short late run reset before its bound");
        }
        f.now=1095000;require(f.output.audio(clockAt(60000),pcm(555))&&f.output.pump()==Pump::Committed,"Sustained lateness did not rebase audio");
        require(f.output.ticketValid(*first)&&!f.watch->closed(),"Audio rebase flushed the existing video timeline");
        require(f.output.report()["running_low_headroom_audio_blocks"].toInt()>=3&&
            f.output.report()["low_headroom_audio_rebases"].toInt()>=1&&
            f.output.report()["sustained_low_headroom_resets"].toInt()==0,
            "Late-run diagnostics lost bounded audio-offset recovery");
    }
    {
        auto fStorage=std::make_unique<Fixture>(true);auto& f=*fStorage;f.verify();auto first=f.output.scheduleVideo(clockAt());
        f.output.audio(clockAt(),pcm(111));f.output.pump();f.now+=30000;
        require(first&&f.output.presented(*first)&&f.output.pump()==Pump::Committed,"Low-headroom control did not start");
        // These first samples are still in the future, but all have only 5 ms
        // of headroom, less than the device's normal write-ahead requirement.
        for(int n=1;n<=2;n++){
            f.now=1000000+n*10000+25000;
            require(f.output.audio(clockAt(n*10000),pcm(222))&&f.output.ticketValid(*first),"Short headroom jitter invalidated presentation");
        }
        f.now=1055000;require(f.output.audio(clockAt(30000),pcm(333))&&f.output.pump()==Pump::Committed,
            "Sustained future-but-low-headroom audio did not apply its bounded offset");
        require(f.output.ticketValid(*first)&&f.output.report()["low_headroom_audio_rebases"].toInt()>=1&&
            f.output.report()["sustained_low_headroom_resets"].toInt()==0,
            "Low-headroom rebase flushed video or exceeded its recovery bound");
    }
    {
        auto fStorage=std::make_unique<Fixture>(true);auto& f=*fStorage;f.verify();auto first=f.output.scheduleVideo(clockAt());
        f.output.audio(clockAt(),pcm(321));f.output.pump();f.now+=30000;
        require(first&&f.output.presented(*first)&&f.output.pump()==Pump::Committed,"Static transport control did not start");
        // Ongoing audio cannot stand in for fresh video transport/presentation.
        for(int n=1;n<=9;n++){f.now+=10000;f.output.audio(clockAt(n*10000+30000),pcm(321));f.output.pump();}
        require(f.output.report()["presentation_resets"].toInt()==1&&f.sink.hardware==pcm(0)&&!f.output.ticketValid(*first),"Audio-only arrivals kept a frozen picture timeline alive");
        require(!f.output.presented(*first),"Repainting the old static picture revived the expired timeline");
    }
    {
        auto fStorage=std::make_unique<Fixture>(true);auto& f=*fStorage;f.verify();auto first=f.output.scheduleVideo(clockAt());f.output.audio(clockAt(),pcm(888));f.output.pump();f.now+=30000;
        require(first&&f.output.presented(*first),"Missing initial paint in output-gap test");
        f.sink.onPrepare=[] {throw EndpointDiscontinuity(1);};
        require(f.output.pump()==Pump::Reset&&!f.watch->closed()&&!f.output.report()["failed"].toBool()&&f.sink.hardware==pcm(0),"Short output gap did not flush and hold the selected watch");
        f.sink.onPrepare={};f.now+=10000;auto next=f.output.scheduleVideo(clockAt(40000));f.output.audio(clockAt(40000),pcm(999));f.output.pump();f.now+=30000;
        require(f.output.pump()==Pump::Waiting&&f.sink.commits==0,"Output-gap recovery bypassed a fresh video presentation");
        require(next&&f.output.presented(*next)&&f.output.pump()==Pump::Committed&&f.sink.hardware==pcm(999),"Output-gap recovery replayed stale PCM");
        auto strictStorage=std::make_unique<Fixture>();auto& strict=*strictStorage;strict.prime();strict.sink.onPrepare=[] {throw EndpointDiscontinuity(1);};
        require(strict.output.pump()==Pump::Failed,"Non-coordinated endpoint silently enabled recovery");
    }
    {
        auto fStorage=std::make_unique<Fixture>(true);auto& f=*fStorage;f.verify();auto ticket=f.output.scheduleVideo(clockAt());require(ticket.has_value(),"Missing coordinated video ticket");
        f.output.audio(clockAt(),pcm(555));require(!f.output.presented(*ticket),"Early video painting admitted audio");
        f.output.pump();f.now+=30000;require(f.output.pump()==Pump::Waiting&&f.sink.commits==0,"Audio played before the first actual paint");
        auto forged=*ticket;forged.senderUs++;require(!f.output.presented(forged),"Inconsistent video timeline accepted");
        require(f.output.presented(*ticket)&&f.output.pump()==Pump::Committed&&f.sink.hardware==pcm(555),"Coordinated paint did not release timed PCM");
        f.now+=100000;require(f.output.pump()==Pump::Reset&&f.sink.hardware==pcm(0)&&!f.output.ticketValid(*ticket),"A stalled renderer kept audio running");
        auto fresh=f.output.scheduleVideo(clockAt(130000));f.output.audio(clockAt(130000),pcm(777));f.output.pump();f.now+=30000;
        require(fresh&&f.output.presented(*fresh)&&f.output.pump()==Pump::Committed&&f.sink.hardware==pcm(777),"Fresh painting failed to recover after a renderer stall");
    }
    {
        auto fStorage=std::make_unique<Fixture>();auto& f=*fStorage;
        require(!f.output.video(clockAt())&&!f.output.audio(clockAt(),pcm(1)),"RTCP metadata bypassed unverified owner/SID");
        f.output.pump();require(!f.sink.commits,"Unverified sink committed");
        f.watch->encryption("mallory","audio",livekit::EncryptionState::Ok);
        f.watch->encryption("alice","video",livekit::EncryptionState::Ok);
        require(!f.output.audio(clockAt(),pcm(2)),"Video readiness granted audio permission");
        f.watch->encryption("alice","audio",livekit::EncryptionState::Ok);
        require(f.output.video(clockAt())&&f.output.audio(clockAt(),pcm(1234)),"Ready watch could not recover from waiting");
        require(f.output.pump()==Pump::Reset,"Initial reset missing");f.now+=30000;
        f.output.setVolume(.5f);require(f.output.pump()==Pump::Committed&&f.sink.hardware==pcm(617),"Volume/stereo device commit mismatch");
        f.watch->encryption("alice","audio",livekit::EncryptionState::MissingKey);
        require(f.output.pump()==Pump::Closed&&f.sink.hardware==pcm(0),"Failed E2EE retained queued fake-device PCM");
        require(!f.watch->encryption("alice","audio",livekit::EncryptionState::Ok)&&!f.output.audio(clockAt(40000),pcm(3)),"Failed watch reopened");
    }
    {
        auto fStorage=std::make_unique<Fixture>();auto& f=*fStorage;f.prime();f.now+=5000;
        require(f.output.pump()==Pump::Committed&&f.sink.lastCommittedFrames==240&&f.output.report()["committed_media_frames"].toInt()==240,"Final commit did not honor a shortened timed packet");
        require(std::all_of(f.sink.hardware.begin(),f.sink.hardware.begin()+480,[](auto s){return s==1234;})&&
            std::all_of(f.sink.hardware.begin()+480,f.sink.hardware.end(),[](auto s){return s==0;}),"Partial packet retained an unwritten hardware suffix");
    }
    {
        auto fStorage=std::make_unique<Fixture>();auto& f=*fStorage;f.prime();
        f.sink.onPrepare=[&]{auto revoke=std::async(std::launch::async,[&]{f.watch->revoke();});revoke.get();};
        require(f.output.pump()==Pump::Closed&&f.sink.commits==0&&!f.sink.reserved&&f.sink.hardware==pcm(0),"Revoke between staging and device commit leaked PCM");
    }
    {
        auto fStorage=std::make_unique<Fixture>();auto& f=*fStorage;f.prime();f.sink.onPrepare=[&]{*f.room=false;};
        require(f.output.pump()==Pump::Closed&&f.sink.commits==0,"Room invalidation bypassed final commit check");
    }
    {
        auto fStorage=std::make_unique<Fixture>();auto& f=*fStorage;f.prime();std::atomic_bool attempting=false,finished=false;std::future<void> revoke;
        f.sink.onCommit=[&]{revoke=std::async(std::launch::async,[&]{attempting=true;f.watch->revoke();finished=true;});
            while(!attempting)std::this_thread::yield();
            require(revoke.wait_for(std::chrono::milliseconds(20))==std::future_status::timeout&&!finished,"Revoke returned while an admitted commit still held its lease");};
        require(f.output.pump()==Pump::Committed,"Earlier admitted bounded commit failed");revoke.get();
        require(f.output.pump()==Pump::Closed&&f.sink.hardware==pcm(0)&&f.sink.commits==1,"A commit occurred after revoke returned");
    }
    {
        auto fStorage=std::make_unique<Fixture>();auto& f=*fStorage;f.prime();f.sink.commitOk=false;
        require(f.output.pump()==Pump::Failed&&f.output.report()["failed"].toBool()&&!f.sink.reserved,"Device commit error did not fail closed");
        f.sink.commitOk=true;require(f.output.pump()==Pump::Failed,"Failed endpoint resumed automatically");
        auto badStorage=std::make_unique<Fixture>();auto& bad=*badStorage;bad.verify();bad.sink.resetOk=false;
        require(bad.output.pump()==Pump::Failed&&bad.sink.commits==0,"Reset failure allowed writes");
        auto oversizedStorage=std::make_unique<Fixture>();auto& oversized=*oversizedStorage;oversized.prime();oversized.sink.frames=481;
        require(oversized.output.pump()==Pump::Failed&&oversized.sink.commits==0,"Oversized device request accepted");
    }
    {
        auto fStorage=std::make_unique<Fixture>();auto& f=*fStorage;f.prime();f.output.setMuted(true);require(f.output.pump()==Pump::Reset,"Mute did not request a device flush");
        f.now=1040000;require(f.output.video(clockAt(40000))&&f.output.audio(clockAt(40000),pcm(7000)),"Muted timeline stopped accepting fresh PCM");
        require(f.output.pump()==Pump::Reset,"Muted timeline reset missing");f.now+=30000;
        require(f.output.pump()==Pump::Committed&&f.sink.hardware==pcm(0)&&f.output.report()["queued_blocks"].toInt()==0,"Mute accumulated old audio");
        f.output.setMuted(false);f.output.pump();f.now=1080000;
        f.output.video(clockAt(80000));f.output.audio(clockAt(80000),pcm(9000));f.output.pump();f.now+=30000;
        require(f.output.pump()==Pump::Committed&&f.sink.hardware==pcm(9000),"Unmute replayed obsolete PCM");
        bool invalid=false;try{f.output.setVolume(std::numeric_limits<float>::quiet_NaN());}catch(const std::invalid_argument&){invalid=true;}require(invalid,"NaN volume accepted");
        std::cout<<"Fake endpoint: "<<QJsonDocument(f.output.report()).toJson(QJsonDocument::Compact).constData()<<'\n';
    }
    {
        auto fStorage=std::make_unique<Fixture>();auto& f=*fStorage;f.verify();
        livekit::VideoFrameEvent video;video.frame=livekit::VideoFrame::create(1280,720,livekit::VideoBufferType::RGBA);
        video.rotation=livekit::VideoRotation::VIDEO_ROTATION_0;video.metadata=livekit::VideoFrameMetadata{};
        video.metadata->user_data=encodeFrameIdentity({999999999999999ull,1,false});
        video.sender_ntp_us=sender;video.rtp_timestamp=0;video.sender_clock_epoch=1;video.rtcp_reports=2;
        livekit::AudioFrameEvent audio;audio.frame=livekit::AudioFrame(std::vector<std::int16_t>(960,2345),48000,2,480);
        audio.sender_ntp_us=sender;audio.rtp_timestamp=0;audio.sender_clock_epoch=1;audio.rtcp_reports=2;
        require(f.output.video(video)&&f.output.audio(audio),"SDK events lost the clock or valid zero RTP timestamp");
        f.output.pump();f.now+=30000;
        require(f.output.pump()==Pump::Committed&&f.sink.hardware==pcm(2345),"SDK PCM did not reach the scheduled fake endpoint");
        video.metadata->user_data=encodeFrameIdentity({999999999999999ull,2,true});
        require(!f.output.video(video)&&f.output.report()["clock_anchored"].toBool(),"Priming changed the content clock");
        video.metadata.reset();require(!f.output.video(video)&&!f.output.report()["clock_anchored"].toBool(),"Missing content identity kept the clock alive");
        video.metadata=livekit::VideoFrameMetadata{};video.metadata->user_data=encodeFrameIdentity({999999999999999ull,3,false});
        video.sender_ntp_us=sender+40000;video.rtp_timestamp=3600;
        require(f.output.video(video),"Fresh valid SDK video did not reanchor");
        audio.rtp_timestamp.reset();require(!f.output.audio(audio)&&!f.output.report()["clock_anchored"].toBool(),"Missing audio clock retained a playback anchor");
        video.sender_ntp_us=sender+50000;video.frame=livekit::VideoFrame::create(640,720,livekit::VideoBufferType::RGBA);
        require(!f.output.video(video),"Wrong selected-quality dimensions established the playback clock");
        video.frame=livekit::VideoFrame::create(1280,720,livekit::VideoBufferType::RGBA);
        require(f.output.video(video),"Valid dimensions failed to recover the clock");
        audio.frame=livekit::AudioFrame(std::vector<std::int16_t>(480),48000,1,480);
        require(!f.output.audio(audio)&&!f.output.report()["clock_anchored"].toBool(),"Mono PCM retained a stereo playback anchor");
    }
    {
        auto watch=std::make_shared<RemoteShare>(RemoteShare::Manifest{"alice","0123456789abcdef0123456789abcdef",1,ShareTrack::Video720,"video",{}},RemoteShare::Callbacks{[](livekit::VideoFrameEvent&){},{}});
        watch->encryption("alice","video",livekit::EncryptionState::Ok);
        require(!watch->presentAudio([]{}),"Video-only watch granted audio output");
    }
    std::cout<<"Audio final owner/SID admission, prepare/commit revoke races, room invalidation, reset failure, terminal errors and mute passed; fake endpoint only\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
