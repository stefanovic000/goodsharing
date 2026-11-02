#include "remote_share.h"
#include <QCoreApplication>
#include <iostream>
#include <stdexcept>
using namespace goodsharing;
void require(bool value,const char* message){if(!value)throw std::runtime_error(message);}
int main(int argc,char** argv) {
    QCoreApplication app(argc,argv);
    try {
        const QString id="0123456789abcdef0123456789abcdef";
        auto announcement=[&](QString owner,QString sid,quint64 generation,ShareTrack kind){const bool audio=kind==ShareTrack::Audio;return ShareModel::Announcement{owner,sid,shareTrackName({id,generation,kind}),audio,true,true,audio?0u:kind==ShareTrack::Video1080?1920u:1280u,audio?0u:kind==ShareTrack::Video1080?1080u:720u};};
        RemoteShareCatalog catalog;
        const auto high=announcement("alice","high",1,ShareTrack::Video1080),low=announcement("alice","low",1,ShareTrack::Video720),audio=announcement("alice","audio",1,ShareTrack::Audio);
        require(catalog.announce(high).accepted&&!catalog.manifest("alice",id,1,ShareTrack::Video720,false),"Missing quality was invented");
        require(!catalog.manifest("alice",id,1,ShareTrack::Video1080,true),"Missing audio was invented");
        require(catalog.announce(low).accepted&&catalog.announce(audio).accepted,"Partial manifest rejected");
        require(catalog.announce(high).accepted&&!catalog.announce(high).changed,"Duplicate was not idempotent");
        require(!catalog.announce(announcement("bob","high",1,ShareTrack::Video1080)).accepted,"Cross-owner SID reuse accepted");
        require(!catalog.announce(announcement("alice","other",1,ShareTrack::Video1080)).accepted,"Conflicting quality accepted");
        auto malformed=high;malformed.encrypted=false;require(!catalog.announce(malformed).accepted,"Cleartext accepted");
        malformed=high;malformed.width=640;require(!catalog.announce(malformed).accepted,"Wrong dimensions accepted");
        const auto manifest=catalog.manifest("alice",id,1,ShareTrack::Video720,true);require(bool(manifest)&&manifest->videoSid=="low"&&manifest->audioSid=="audio","Selection is not one quality plus audio");
        const auto sound=catalog.manifest("alice",id,1,ShareTrack::Audio,true);
        require(sound&&sound->videoSid.isEmpty()&&sound->audioSid=="audio","Audio-only manifest invented a video selection");
        RemoteShare audioOnly(*sound,{{},[](livekit::AudioFrameEvent&){}});
        require(!audioOnly.presentAudio([]{})&&!audioOnly.presentVideo([]{}),"Unverified audio-only lease presented media");
        require(!audioOnly.encryption("alice","low",livekit::EncryptionState::Ok)&&audioOnly.encryption("alice","audio",livekit::EncryptionState::Ok),"Audio-only encryption accepted an unrelated SID");
        require(audioOnly.presentAudio([]{})&&!audioOnly.presentVideo([]{})&&audioOnly.report()["quality"].toInt()==0,"Audio-only lease granted video or reported a video quality");
        audioOnly.revoke();require(!audioOnly.encryption("alice","audio",livekit::EncryptionState::Ok)&&!audioOnly.presentAudio([]{}),"Revoked audio-only lease revived");
        require(!catalog.manifest("bob",id,1,ShareTrack::Video720,true)&&!catalog.manifest("alice",id,2,ShareTrack::Video720,true),"Owner or generation mismatch accepted");
        require(!catalog.manifest("alice",id,1,static_cast<ShareTrack>(99),false),"Invalid quality accepted");
        RemoteShare selected(*manifest,{[](livekit::VideoFrameEvent&){},[](livekit::AudioFrameEvent&){}});
        using E=livekit::EncryptionState;
        require(!selected.encryption("alice","high",E::MissingKey)&&!selected.encryption("bob","low",E::Ok)&&!selected.closed(),"Unselected/foreign event changed watch");
        int presentations=0;
        selected.encryption("alice","low",E::Ok);require(!selected.report()["ready"].toBool()&&!selected.presentVideo([&]{presentations++;}),"Video readiness skipped audio");
        selected.encryption("alice","audio",E::Ok);require(selected.report()["ready"].toBool()&&selected.presentVideo([&]{presentations++;})&&presentations==1,"Matching readiness rejected");
        selected.encryption("alice","low",E::New);require(selected.closed()&&!selected.encryption("alice","low",E::Ok),"A revoked watch reopened");selected.finish();require(selected.stopped(),"Watch did not finish");
        auto roomAdmission=std::make_shared<std::atomic_bool>(true);RemoteShare interrupted(*manifest,{[](livekit::VideoFrameEvent&){},[](livekit::AudioFrameEvent&){}},roomAdmission);
        interrupted.encryption("alice","low",E::Ok);interrupted.encryption("alice","audio",E::Ok);*roomAdmission=false;
        require(interrupted.closed()&&!interrupted.report()["ready"].toBool()&&!interrupted.encryption("alice","low",E::Ok),"Room invalidation left receive permission open");interrupted.finish();
        ShareReceiver receiver;require(!receiver.canPresent("alice","low"),"Missing permission was invented");
        require(receiver.encryption("alice","low",E::Ok)&&receiver.canPresent("alice","low")&&!receiver.canPresent("bob","low")&&!receiver.canPresent("alice","high"),"Presentation was not bound to the verified owner/SID");
        receiver.stop();require(!receiver.canPresent("alice","low"),"Stopped receiver retained presentation permission");
        const auto next=catalog.announce(announcement("alice","next",2,ShareTrack::Video1080));require(next.accepted&&next.retired.size()==3,"New generation retained prior tracks");
        require(!catalog.find("high")&&!catalog.announce(low).accepted&&!catalog.remove("high"),"Stale generation revived");
        require(!catalog.announce(announcement("alice","high",3,ShareTrack::Video1080)).accepted,"Retired SID reused");
        require(catalog.remove("next")&&!catalog.announce(announcement("alice","late",2,ShareTrack::Video1080)).accepted,"Fully unpublished generation revived");
        require(catalog.announce(announcement("alice","third",3,ShareTrack::Video720)).accepted,"Fresh generation rejected");
        require(catalog.removeOwner("alice").size()==1&&!catalog.manifest("alice",id,3,ShareTrack::Video720,false)&&!catalog.announce(announcement("alice","late-owner",3,ShareTrack::Video720)).accepted,"Disconnected owner revived");
        RemoteShareCatalog bounded;
        for(int i=1;i<=600;i++)require(bounded.announce(announcement("alice","sid"+QString::number(i),i,ShareTrack::Video720)).accepted,"History eviction rejected a fresh announcement");
        require(bounded.error().isEmpty(),"A long but legal history poisoned the catalog");
        require(!bounded.announce(announcement("alice","sid1",1,ShareTrack::Video720)).accepted,"A retreating generation was accepted after eviction");
        std::cout<<"Remote owner/generation/quality selection, partial manifests, SID tombstones, sticky verification and bounded history passed\n";return 0;
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
