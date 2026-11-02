#include "remote_audio_playout.h"
#include "remote_video.h"
#include "frame_metadata_codec.h"
#include <QCoreApplication>
#include <atomic>
#include <future>
#include <iostream>
#include <string>
#include <limits>
#include <stdexcept>
#include <thread>
using namespace goodsharing;
namespace {
void require(bool value,const char* message){if(!value)throw std::runtime_error(message);}
constexpr std::int64_t sender=4000000000000000ll;
const QString shareId="0123456789abcdef0123456789abcdef";
using Pcm=std::array<std::int16_t,960>;
using Pump=RemoteAudioPlayout::Pump;
Pcm pcm(int value){Pcm result;result.fill(static_cast<std::int16_t>(value));return result;}
PlayoutClock stamp(std::int64_t delta=0,std::uint32_t epoch=1){return {sender+delta,static_cast<std::uint32_t>(delta*48/1000),epoch,2,true};}
struct Sink:AudioPlayoutSink {
    Pcm hardware{};unsigned commits=0,resets=0;bool reserved=false;
    std::function<void()> onPrepare,onCommit;
    std::optional<Window> prepare(std::int64_t now)override{reserved=true;if(onPrepare)onPrepare();return Window{now,480};}
    bool commit(std::span<const std::int16_t> data)override{require(reserved&&data.size()==hardware.size(),"Invalid endpoint commit");if(onCommit)onCommit();std::copy(data.begin(),data.end(),hardware.begin());commits++;reserved=false;return true;}
    void cancel()noexcept override{reserved=false;}
    bool reset()noexcept override{hardware.fill(0);reserved=false;resets++;return true;}
};
struct Fixture {
    std::int64_t now=1000000;Sink sink;
    std::shared_ptr<std::atomic_bool> room=std::make_shared<std::atomic_bool>(true);
    std::shared_ptr<RemoteShare> sound=make(ShareTrack::Audio,"audio"),low=make(ShareTrack::Video720,"low"),high=make(ShareTrack::Video1080,"high");
    RemoteAudioPlayout output{sink,[this]{return now;}};
    std::shared_ptr<RemoteShare> make(ShareTrack role,QString sid,QString owner="alice",quint64 generation=1,std::shared_ptr<std::atomic_bool> admission={}){
        return std::make_shared<RemoteShare>(RemoteShare::Manifest{owner,shareId,generation,role,role==ShareTrack::Audio?QString{}:sid,role==ShareTrack::Audio?sid:QString{}},
            RemoteShare::Callbacks{role==ShareTrack::Audio?TrackReader::VideoCallback{}:TrackReader::VideoCallback{[](livekit::VideoFrameEvent&){}},role==ShareTrack::Audio?TrackReader::AudioCallback{[](livekit::AudioFrameEvent&){}}:TrackReader::AudioCallback{}},admission?admission:room);
    }
    static void verify(const std::shared_ptr<RemoteShare>& watch){const auto& m=watch->manifest();require(watch->encryption(m.owner,m.videoSid.isEmpty()?m.audioSid:m.videoSid,livekit::EncryptionState::Ok),"Verification failed");}
    Fixture(){output.requirePresentation();output.bindMedia(sound,low);}
    PresentationTicket prime(){
        verify(sound);verify(low);auto ticket=output.scheduleVideo(low,stamp(0,9));
        require(ticket&&output.audio(stamp(),pcm(100)),"Split ingress did not establish a timeline");
        require(output.pump()==Pump::Reset,"Initial endpoint reset missing");now+=30000;
        require(output.presented(low,*ticket)&&output.pump()==Pump::Committed&&sink.hardware==pcm(100),"Split final admission failed");return *ticket;
    }
    RemoteAudioPlayout::CandidateTicket candidate(){
        require(output.stageVideo(high),"Candidate did not stage");verify(high);now=1040000;
        auto ticket=output.projectVideo(high,stamp(40000));require(ticket.has_value(),"Verified candidate did not project onto the active clock");return *ticket;
    }
};
}
int main(int argc,char** argv){QCoreApplication app(argc,argv);try{
    for(const int admissionOffset:{0,30000,32000}){
        auto storage=std::make_unique<Fixture>();auto& f=*storage;f.prime();f.now=1040000;
        RemoteVideo frames([&]{return f.now;});
        frames.setTiming({[&](livekit::VideoFrameEvent& e){return f.output.scheduleVideo(f.low,e);},
            [&](PresentationTicket t){return f.output.ticketValid(f.low,t);},
            [&](PresentationTicket t){f.output.presented(f.low,t);},true,f.output.mediaSelection()});frames.bind(f.low);
        auto event=livekit::VideoFrameEvent{};event.frame=livekit::VideoFrame::create(1280,720,livekit::VideoBufferType::RGBA);
        event.rotation=livekit::VideoRotation::VIDEO_ROTATION_0;event.metadata=livekit::VideoFrameMetadata{};
        for(int n=0;n<8;n++){
            if(n==6)f.now+=admissionOffset;
            std::fill(event.frame.data(),event.frame.data()+event.frame.dataSize(),static_cast<std::uint8_t>(200+n));
            for(std::size_t at=3;at<event.frame.dataSize();at+=4)event.frame.data()[at]=255;
            event.metadata->user_data=encodeFrameIdentity({999999999999999ull,static_cast<std::uint64_t>(300+n),false});
            event.sender_ntp_us=sender+40000+n*1000;event.rtp_timestamp=3600+n*90;event.sender_clock_epoch=9;event.rtcp_reports=2;
            frames.submit(event);
        }
        require(frames.report()["scheduled_queue_high_water"].toInt()<=frames.report()["scheduled_queue_capacity"].toInt()&&frames.report()["scheduled_queue_high_water"].toInt()>=6&&frames.report()["scheduled_drops"].toInt()==0,
            (std::string("highWater=")+std::to_string(frames.report()["scheduled_queue_high_water"].toInt())+" capacity="+std::to_string(frames.report()["scheduled_queue_capacity"].toInt())+" drops="+std::to_string(frames.report()["scheduled_drops"].toInt())).c_str());
        require(frames.report()["next_due_preserved_on_overflow"].toInt()==0,
            "Queue replacement failed to distinguish a sole next-due picture from older due pictures");
        QImage canvas(320,180,QImage::Format_RGB32);auto draw=[&]{canvas.fill(Qt::black);QPainter painter(&canvas);return frames.draw(painter,canvas.rect());};
        f.now=admissionOffset==32000?1072000:1070000;
        require(frames.readyToPresent()&&draw()&&canvas.pixelColor(100,100).red()==(admissionOffset==32000?202:200),
            "Full future queue discarded the next-due picture and invented a presentation hole");
        frames.frameQueued();
        f.now=1077000;
        require(draw()&&canvas.pixelColor(100,100).red()==(admissionOffset==32000?202:200)&&
            frames.report()["presentation_catchup_drops"].toInt()>0,
            "A sub-10-ms burst frame accelerated visible motion");
        require(frames.report()["queued_presentations"].toInt()==1&&f.output.report()["reset_serial"].toInt()==2,
            "Catch-up rejection reset the common clock or invented presentation acknowledgments");
        f.sound->encryption("alice","audio",livekit::EncryptionState::MissingKey);
        require(!draw()&&canvas.pixelColor(100,100)==QColor(Qt::black)&&f.output.pump()==Pump::Closed&&f.sink.hardware==pcm(0),
            "A preserved next-due image bypassed final paired revocation");
    }
    for(const bool revoke:{false,true}){
        auto storage=std::make_unique<Fixture>();auto& f=*storage;f.prime();
        require(f.output.stageVideo(f.high),"Early selected-video control did not stage candidate");Fixture::verify(f.high);
        const auto before=f.output.report();const auto resets=f.sink.resets;f.now=1045000;
        RemoteVideo frames([&]{return f.now;});std::optional<PresentationTicket> issued;
        frames.setTiming({[&](livekit::VideoFrameEvent& e){issued=f.output.scheduleVideo(f.low,e);return issued;},
            [&](PresentationTicket t){return f.output.ticketValid(f.low,t);},
            [&](PresentationTicket t){f.output.presented(f.low,t);},true,f.output.mediaSelection()});frames.bind(f.low);
        auto event=livekit::VideoFrameEvent{};event.frame=livekit::VideoFrame::create(1280,720,livekit::VideoBufferType::RGBA);
        std::fill(event.frame.data(),event.frame.data()+event.frame.dataSize(),static_cast<std::uint8_t>(199));
        for(std::size_t n=3;n<event.frame.dataSize();n+=4)event.frame.data()[n]=255;
        event.rotation=livekit::VideoRotation::VIDEO_ROTATION_0;event.metadata=livekit::VideoFrameMetadata{};
        event.metadata->user_data=encodeFrameIdentity({999999999999999ull,202,false});
        event.sender_ntp_us=sender+96667;event.rtp_timestamp=9000;event.sender_clock_epoch=9;event.rtcp_reports=2;
        frames.submit(event);require(issued&&issued->deadlineUs==1125000&&issued->timelineDeadlineUs==1126667,
            "Split selected picture lost its separate bounded presentation target");
        QImage canvas(320,180,QImage::Format_RGB32);auto draw=[&]{canvas.fill(Qt::black);QPainter painter(&canvas);return frames.draw(painter,canvas.rect());};
        f.now=1124999;require(!draw(),"Earlier picture painted before its bounded target");
        f.now=1125000;require(draw()&&canvas.pixelColor(100,100).red()==199&&f.output.report()["advanced_video_presentations"].toInt()==0,
            "Earlier picture skipped drawing or acknowledged before the frame queue");
        if(revoke)f.sound->encryption("alice","audio",livekit::EncryptionState::MissingKey);
        frames.frameQueued();
        require(f.output.report()["advanced_video_presentations"].toInt()==(revoke?0:1)&&
            f.output.report()["reset_serial"]==before["reset_serial"]&&f.sink.resets==resets&&f.output.selectedVideo()==f.low,
            "Earlier frame queue reset audio, adopted the candidate or skipped final paired permission");
        if(revoke)require(!draw()&&canvas.pixelColor(100,100)==QColor(Qt::black)&&f.output.pump()==Pump::Closed&&f.sink.hardware==pcm(0),
            "Revocation retained earlier pixels or fake-device PCM");
        else require(f.output.cancelVideo(f.high)&&f.output.report()["reset_serial"]==before["reset_serial"],"Candidate cancellation invalidated earlier selected picture");
    }
    {
        Fixture f;f.prime();f.output.audio(stamp(10000),pcm(777));f.now=1040000;
        std::promise<void> painting,release;auto released=release.get_future();
        auto paint=std::async(std::launch::async,[&]{return f.output.presentVideo(f.low,[&]{painting.set_value();released.wait();});});painting.get_future().wait();
        auto audio=std::async(std::launch::async,[&]{return f.output.pump();});
        const bool concurrent=audio.wait_for(std::chrono::milliseconds(100))==std::future_status::ready;
        release.set_value();const auto pumped=audio.get();require(paint.get(),"Earlier admitted paint failed");
        require(concurrent&&pumped==Pump::Committed&&f.sink.hardware==pcm(777),"An authorized picture operation blocked the independent audio commit");
    }
    {
        Fixture f;f.prime();require(f.output.stageVideo(f.high),"Renderer candidate stage failed");f.now=1040000;
        auto gate=f.output.mediaSelection();RemoteVideo frames([&]{return f.now;});frames.setWarming(true);
        unsigned adopted=0;
        frames.setTiming({[&](livekit::VideoFrameEvent& e){return f.output.scheduleVideo(f.high,e);},
            [&](PresentationTicket t){return f.output.ticketValid(f.high,t);},[&](PresentationTicket t){if(f.output.presented(f.high,t))adopted++;},true,gate});frames.bind(f.high);
        auto event=livekit::VideoFrameEvent{};event.frame=livekit::VideoFrame::create(1920,1080,livekit::VideoBufferType::RGBA);
        std::fill(event.frame.data(),event.frame.data()+event.frame.dataSize(),static_cast<std::uint8_t>(201));
        for(std::size_t n=3;n<event.frame.dataSize();n+=4)event.frame.data()[n]=255;
        event.rotation=livekit::VideoRotation::VIDEO_ROTATION_0;event.metadata=livekit::VideoFrameMetadata{};
        event.metadata->user_data=encodeFrameIdentity({999999999999999ull,201,false});
        event.sender_ntp_us=sender+40000;event.rtp_timestamp=0;event.sender_clock_epoch=1;event.rtcp_reports=2;
        frames.submit(event);require(frames.report()["pending_images"].toInt()==0&&!frames.readyToPresent(),"Video-only candidate readiness skipped its unverified SID");
        Fixture::verify(f.high);frames.submit(event);
        for(int n=1;n<10;n++){event.sender_ntp_us=sender+40000+n*1000;event.rtp_timestamp=n*90;frames.submit(event);}
        require(frames.report()["pending_images"].toInt()==1&&frames.report()["warm_queue_high_water"].toInt()==1,"Candidate warming retained more than its first image");
        QImage canvas(320,180,QImage::Format_RGB32);auto draw=[&]{canvas.fill(Qt::black);QPainter painter(&canvas);return frames.draw(painter,canvas.rect());};
        f.now=1050000;require(frames.finishWarmup(),"Clocked candidate did not finish warming before its deadline");
        event.sender_ntp_us=sender+56667;event.rtp_timestamp=1500;frames.submit(event);
        f.now=1060000;require(!frames.readyToPresent()&&!draw()&&f.output.selectedVideo()==f.low,"Future candidate replaced current selection");
        require(frames.report()["pending_images"].toInt()==2,"Clocked candidate discarded the next 60 FPS picture while waiting for its first deadline");
        f.output.audio(stamp(40000),pcm(444));const auto resets=f.sink.resets;f.now=1070000;
        require(frames.readyToPresent(),"Due verified candidate not ready");frames.setWarming(false);
        require(draw()&&canvas.pixelColor(100,100).red()==201&&f.output.selectedVideo()==f.low&&adopted==0,"Painting pretended to queue a new quality");
        frames.frameQueued();require(adopted==1&&f.output.selectedVideo()==f.high&&f.output.pump()==Pump::Committed&&f.sink.hardware==pcm(444)&&f.sink.resets==resets,
            "Actual frame queue did not adopt the new video while preserving PCM/device");
        frames.frameQueued();require(draw()&&adopted==1,"Held candidate pixels invented another presentation");frames.frameQueued();require(adopted==1,"Held-image frame queue refreshed audio admission");
        f.now=1086667;require(draw(),"Buffered successor did not meet its first deadline");frames.frameQueued();
        require(adopted==2&&f.output.report()["reset_serial"].toInt()==2&&f.sink.resets==resets,"Buffered successor reset the adopted clock or failed to queue");
        f.sound->revoke();require(f.high->presentVideo([]{})&&!draw()&&canvas.pixelColor(100,100)==QColor(Qt::black),"Separate video permission bypassed revoked paired audio during paint");
        f.high->revoke();frames.discardRevoked();frames.submit(event);require(frames.report()["pending_images"].toInt()==0,"Retired image storage refilled after revocation/release");
    }
    {
        std::shared_ptr<const RemoteMediaSelection> retained;std::shared_ptr<RemoteShare> audio,video;
        {Fixture f;f.prime();retained=f.output.mediaSelection();audio=f.sound;video=f.low;}
        require(audio->presentAudio([]{})&&video->presentVideo([]{})&&!retained->presentVideo(video,[]{}),"Presentation gate outlived its stopped scheduler as open permission");
        Fixture f;f.prime();auto gate=f.output.mediaSelection();f.sink.onPrepare=[]{throw std::runtime_error("Injected endpoint fault");};
        require(f.output.pump()==Pump::Failed&&!gate->presentVideo(f.low,[]{}),"Failed endpoint left split presentation admission open");
    }
    {
        Fixture f;bool rejected=false;
        RemoteAudioPlayout second(f.sink,[&]{return f.now;});second.requirePresentation();
        auto foreign=f.make(ShareTrack::Video720,"foreign","alice",1,std::make_shared<std::atomic_bool>(true));
        try{second.bindMedia(f.sound,foreign);}catch(const std::invalid_argument&){rejected=true;}
        require(rejected,"Identical owner/id/generation from different room admission was combined");
        require(!f.output.stageVideo(foreign)&&!f.output.stageVideo(f.make(ShareTrack::Video1080,"bob-high","bob"))&&
            !f.output.stageVideo(f.make(ShareTrack::Video1080,"next-high","alice",2)),"Candidate crossed room, owner or generation");
        auto otherManifest=f.high->manifest();otherManifest.id="abcdef0123456789abcdef0123456789";
        auto otherShare=std::make_shared<RemoteShare>(otherManifest,RemoteShare::Callbacks{[](livekit::VideoFrameEvent&){},{}},f.room);
        require(!f.output.stageVideo(otherShare),"Candidate crossed share identity");
        require(!f.output.stageVideo(f.low)&&!f.output.stageVideo(f.sound),"Duplicate quality or audio lease became candidate video");
        RemoteAudioPlayout uncoordinated(f.sink);rejected=false;
        try{uncoordinated.bindMedia(f.sound,f.low);}catch(const std::invalid_argument&){rejected=true;}
        require(rejected,"Split media bypassed coordinated presentation");
    }
    {
        Fixture f;Fixture::verify(f.sound);
        require(!f.output.audio(stamp(),pcm(111))&&!f.output.scheduleVideo(f.low,stamp())&&f.output.pump()!=Pump::Committed,
            "Audio-only verification granted selected video playback");
        Fixture::verify(f.low);auto ticket=f.output.scheduleVideo(f.low,stamp());f.output.audio(stamp(),pcm(222));f.output.pump();f.now+=30000;
        require(ticket&&!f.output.presented(*ticket)&&f.output.presented(f.low,*ticket)&&f.output.pump()==Pump::Committed,"Split output skipped exact video identity or failed recovery");
    }
    {
        Fixture f;f.prime();const auto initial=f.output.report();
        for(int n=1;n<=4;n++)require(f.output.audio(stamp(n*10000),pcm(100+n)),"Continuous PCM rejected");
        auto candidate=f.candidate();const auto queued=f.output.report()["queued_blocks"].toInt();
        require(!f.output.adoptVideo(f.high,candidate)&&f.output.selectedVideo()==f.low,"Candidate adopted before its due presentation");
        require(f.output.report()["reset_serial"]==initial["reset_serial"]&&queued==4,"Warm-up changed the clock or queued PCM");
        const auto resets=f.sink.resets;
        for(int n=1;n<=3;n++){f.now=1030000+n*10000;require(f.output.pump()==Pump::Committed&&f.sink.hardware==pcm(100+n),"Warming candidate interrupted existing PCM");}
        f.now=1070000;require(f.output.adoptVideo(f.high,candidate)&&f.output.selectedVideo()==f.high,"Due candidate presentation did not commit");
        require(f.output.pump()==Pump::Committed&&f.sink.hardware==pcm(104)&&f.sink.resets==resets&&
            f.output.report()["reset_serial"]==initial["reset_serial"],"Handover flushed, reordered or lost queued PCM");
        f.low->revoke();require(!f.output.scheduleVideo(f.low,stamp(50000,100))&&!f.output.video(stamp(50000,100))&&
            !f.output.presentVideo(f.low,[]{}),"Retired video revived through late ingress or painting");
        auto next=f.output.scheduleVideo(f.high,stamp(50000));require(next&&f.output.audio(stamp(50000),pcm(105)),"New independent SSRC epoch was compared to retired epoch");
        f.now=1080000;require(f.output.presented(f.high,*next)&&f.output.pump()==Pump::Committed&&f.sink.hardware==pcm(105)&&f.sink.resets==resets,
            "Retiring old video stopped persistent audio");
        require(f.output.report()["video_handovers"].toInt()==1&&f.output.report()["committed_media_frames"].toInt()==6*480,"Handover diagnostics or exact PCM totals incorrect");
        // Return to 720p requires a fresh immutable lease, never the revoked one.
        require(!f.output.stageVideo(f.low),"Revoked old quality was revived");
        auto fresh=f.make(ShareTrack::Video720,"low");Fixture::verify(fresh);require(f.output.stageVideo(fresh),"Fresh return lease rejected");
        auto back=f.output.projectVideo(fresh,stamp(80000,2));require(back.has_value(),"Return candidate failed to project");
        require(f.output.audio(stamp(80000),pcm(108)),"Persistent audio rejected before return");f.now=1110000;
        require(f.output.adoptVideo(fresh,*back)&&f.output.selectedVideo()==fresh&&f.output.pump()==Pump::Committed&&f.sink.hardware==pcm(108)&&f.sink.resets==resets,
            "Return handover reset the endpoint or lost PCM");
        f.high->revoke();require(f.output.presentVideo(fresh,[]{}),"Retiring high quality revoked the fresh low quality");
    }
    {
        Fixture f;auto active=f.prime();const auto initial=f.output.report();require(f.output.stageVideo(f.high),"Candidate stage failed");
        f.now=1040000;require(!f.output.projectVideo(f.high,stamp(40000))&&f.output.ticketValid(active)&&f.output.presentVideo(f.low,[]{}),"Unverified candidate disturbed selected media");
        require(!f.output.stageVideo(f.make(ShareTrack::Video1080,"second-high")),"More than one candidate was admitted");
        f.high->encryption("alice","high",livekit::EncryptionState::MissingKey);
        require(!f.output.projectVideo(f.high,stamp(40000))&&f.output.cancelVideo(f.high)&&f.output.presentVideo(f.low,[]{}),"Failed candidate revoked current selection");
        require(f.output.report()["reset_serial"]==initial["reset_serial"],"Candidate failure reset active audio");
    }
    {
        Fixture f;f.prime();auto first=f.candidate();require(f.output.cancelVideo(f.high)&&!f.output.candidateValid(f.high,first),"Cancelled candidate retained presentation permission");
        require(f.output.stageVideo(f.high)&&!f.output.candidateValid(f.high,first),"Restaging revived a ticket from an earlier attempt");
        auto next=f.output.projectVideo(f.high,stamp(40000));require(next.has_value(),"Restaged candidate could not project");
        f.output.setMuted(true);require(!f.output.candidateValid(f.high,*next)&&!f.output.adoptVideo(f.high,*next),"Reset timeline retained candidate tickets");
    }
    for(int role=0;role<3;role++){
        Fixture f;f.prime();auto candidate=f.candidate();auto revoked=role==0?f.sound:role==1?f.low:f.high;
        revoked->revoke();f.now=1070000;
        require(!f.output.adoptVideo(f.high,candidate)&&!f.output.presentVideo(f.high,[]{}),"Revoked handover role granted candidate presentation");
        require(f.output.selectedVideo()==f.low,"Rejected handover changed active selection");
        if(role<2)require(f.output.pump()==Pump::Closed&&f.sink.hardware==pcm(0),"Revoked active role retained endpoint PCM");
        else require(f.output.presentVideo(f.low,[]{}),"Candidate revoke stopped the verified current picture");
    }
    for(int role=0;role<2;role++){
        Fixture f;f.prime();f.output.audio(stamp(10000),pcm(777));f.now=1040000;
        f.sink.onPrepare=[&]{(role?f.low:f.sound)->revoke();};
        require(f.output.pump()==Pump::Closed&&f.sink.commits==1&&f.sink.hardware==pcm(0)&&!f.sink.reserved,
            "Paired permission revoke between reservation and commit leaked PCM");
    }
    for(int role=0;role<2;role++){
        Fixture f;f.prime();f.output.audio(stamp(10000),pcm(777));f.now=1040000;
        std::atomic_bool attempting=false;std::future<void> revoke;
        f.sink.onCommit=[&]{revoke=std::async(std::launch::async,[&]{attempting=true;(role?f.low:f.sound)->revoke();});
            while(!attempting)std::this_thread::yield();
            require(revoke.wait_for(std::chrono::milliseconds(20))==std::future_status::timeout,"Paired lease released before an admitted device commit completed");};
        require(f.output.pump()==Pump::Committed,"Earlier admitted paired commit failed");revoke.get();f.sink.onCommit={};
        require(f.output.pump()==Pump::Closed&&f.sink.commits==2&&f.sink.hardware==pcm(0),"PCM committed after paired revoke returned");
    }
    for(int role=0;role<3;role++){
        Fixture f;f.prime();auto candidate=f.candidate();std::atomic_bool attempting=false;std::future<void> revoke;
        require(f.output.presentVideo(f.high,[&]{revoke=std::async(std::launch::async,[&]{attempting=true;(role==0?f.sound:role==1?f.low:f.high)->revoke();});
            while(!attempting)std::this_thread::yield();
            require(revoke.wait_for(std::chrono::milliseconds(20))==std::future_status::timeout,"Candidate paint did not retain every required lease");}),"Verified candidate paint rejected");
        revoke.get();require(!f.output.presentVideo(f.high,[]{}),"Candidate paint occurred after revoke returned");
    }
    {
        Fixture f;f.prime();auto candidate=f.candidate();*f.room=false;
        require(!f.output.presentVideo(f.high,[]{}),"Room admission invalidation retained candidate output");
        f.now=1070000;require(!f.output.adoptVideo(f.high,candidate)&&f.output.pump()==Pump::Closed&&f.sink.hardware==pcm(0),"Room invalidation bypassed handover/endpoint admission");
    }
    {
        Fixture f;f.prime();auto candidate=f.candidate();const auto resets=f.sink.resets;
        f.now=1230001;require(!f.output.candidateValid(f.high,candidate)&&!f.output.adoptVideo(f.high,candidate),"Expired candidate was adopted");
        require(f.sink.resets==resets&&f.output.selectedVideo()==f.low,"Rejected stale candidate altered the endpoint/selection");
    }
    {
        Fixture f;f.prime();require(f.output.stageVideo(f.high),"SDK candidate stage failed");Fixture::verify(f.high);f.now=1040000;
        livekit::VideoFrameEvent frame;frame.frame=livekit::VideoFrame::create(1920,1080,livekit::VideoBufferType::RGBA);
        frame.rotation=livekit::VideoRotation::VIDEO_ROTATION_0;frame.metadata=livekit::VideoFrameMetadata{};
        frame.metadata->user_data=encodeFrameIdentity({999999999999999ull,1,false});
        frame.sender_ntp_us=sender+40000;frame.rtp_timestamp=0;frame.sender_clock_epoch=1;frame.rtcp_reports=2;
        require(f.output.projectVideo(f.high,frame).has_value(),"Valid native candidate metadata was lost");
        frame.metadata->user_data=encodeFrameIdentity({999999999999999ull,2,true});
        require(!f.output.projectVideo(f.high,frame),"Priming candidate granted content presentation");
        frame.metadata->user_data=encodeFrameIdentity({999999999999999ull,3,false});frame.rtcp_reports=1;
        require(!f.output.projectVideo(f.high,frame),"Candidate accepted an unestablished RTCP clock");
        frame.rtcp_reports=2;frame.frame=livekit::VideoFrame::create(1280,720,livekit::VideoBufferType::RGBA);
        require(!f.output.projectVideo(f.high,frame)&&f.output.report()["clock_anchored"].toBool(),"Wrong candidate dimensions altered active timeline");
    }
    {
        AudioPlayout core;core.enableAudioAlignment();require(core.video(stamp(),1000000)&&core.audio(stamp(),pcm(1),1000000),"Core control did not start");
        core.acknowledgeReset(core.stats().resetSerial);core.mediaCommitted();const auto serial=core.stats().resetSerial;
        require(!core.projectVideo(stamp(40000),999999)&&!core.projectVideo(stamp(40000),1061000)&&!core.projectVideo(stamp(200000),1040000),"Candidate projection bypassed time/headroom bounds");
        require(!core.adoptVideo(stamp(40000),1040000,1070000,INT64_MIN,serial)&&!core.adoptVideo(stamp(40000),1040000,1070000,INT64_MAX,serial),"Extreme candidate deadlines were accepted");
        require(core.stats().queued==1&&core.stats().resetSerial==serial,"Invalid projection/adoption mutated PCM or reset serial");
    }
    std::cout<<"Persistent audio, two quality handovers, exact PCM ordering, independent video epochs, candidate rollback, paired final leases and revoke races passed; fake endpoint only; scheduler bytes="<<sizeof(AudioPlayout)<<'\n';return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
