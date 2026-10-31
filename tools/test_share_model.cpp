#include "share_model.h"
#include <QCoreApplication>
#include <QAbstractItemModelTester>
#include <iostream>
#include <stdexcept>
using namespace goodsharing;
void require(bool value,const char* message){if(!value)throw std::runtime_error(message);}
int main(int argc,char**argv) {
    QCoreApplication app(argc,argv);
    try {
        const QString id="0123456789abcdef0123456789abcdef";
        for(const auto generation:{1ULL,255ULL,0xffffffffffffffffULL})for(auto track:{ShareTrack::Video1080,ShareTrack::Video720,ShareTrack::Audio}) {
            const ShareIdentity value{id,generation,track};require(parseShareTrackName(shareTrackName(value))==value,"Identity did not round-trip");
        }
        const auto valid=shareTrackName({id,1,ShareTrack::Video1080});
        for(const auto& name:QStringList{"",valid+".junk",valid.toUpper(),QString(valid).replace("0000000000000001","0000000000000000"),"gs1.owner.fake.1080",QString(100000,'a')})require(!parseShareTrackName(name),"Malformed remote identity accepted");
        ShareModel model;
        QAbstractItemModelTester tester(&model,QAbstractItemModelTester::FailureReportingMode::Fatal);
        auto announcement=[&](const QString& owner,const QString& sid,quint64 generation,ShareTrack track) {
            const bool audio=track==ShareTrack::Audio;
            return ShareModel::Announcement{owner,sid,shareTrackName({id,generation,track}),audio,true,true,audio?0u:track==ShareTrack::Video1080?1920u:1280u,audio?0u:track==ShareTrack::Video1080?1080u:720u};
        };
        auto high=announcement("alice","old-high",1,ShareTrack::Video1080);
        auto low=announcement("alice","old-low",1,ShareTrack::Video720);
        auto audio=announcement("alice","old-audio",1,ShareTrack::Audio);
        require(model.announce(high)&&model.announce(low)&&model.announce(audio)&&model.rowCount()==1,"Paired tracks were not grouped");
        require(model.announce(high)&&model.rowCount()==1,"Duplicate publication was not idempotent");
        auto invalid=high;invalid.sid="bad";invalid.encrypted=false;require(!model.announce(invalid),"Clear publication accepted");
        invalid=high;invalid.width=640;require(!model.announce(invalid),"Wrong dimensions accepted");
        invalid=high;invalid.audio=true;require(!model.announce(invalid),"Mismatched media kind accepted");
        invalid=high;invalid.screenSource=false;require(!model.announce(invalid),"Non-share source accepted");
        invalid=high;invalid.sid="conflict";require(!model.announce(invalid),"Conflicting duplicate quality accepted");
        invalid=high;invalid.owner="bob";require(!model.announce(invalid),"Publication SID reassigned to another owner");
        require(model.announce(announcement("bob","bob-high",1,ShareTrack::Video1080))&&model.rowCount()==2,"Owners were conflated");
        require(model.announce(announcement("alice","new-high",2,ShareTrack::Video1080)),"New generation rejected");
        require(model.sid("alice",id,ShareTrack::Video720).isEmpty()&&model.sid("alice",id,ShareTrack::Audio).isEmpty(),"Restart retained old associated tracks");
        require(!model.announce(low)&&!model.remove("old-high")&&!model.retire("alice",id,1),"Stale events changed current generation");
        require(model.sid("alice",id,ShareTrack::Video1080)=="new-high","Old unpublish removed replacement track");
        require(model.retire("alice",id,2)&&!model.announce(announcement("alice","late-high",2,ShareTrack::Video1080)),"Retired generation revived");
        require(model.announce(announcement("alice","restart-high",3,ShareTrack::Video1080)),"Explicit fresh generation rejected");
        model.removeOwner("alice");require(model.rowCount()==1&&model.sid("bob",id,ShareTrack::Video1080)=="bob-high","Participant cleanup removed another owner's share");
        model.clear();require(model.rowCount()==0,"Room leave did not clear model");
        for(int i=0;i<64;i++) {auto a=high;a.owner=QString::number(i);a.sid="sid"+QString::number(i);require(model.announce(a),"Bound rejected valid row early");}
        invalid=high;invalid.owner="overflow";invalid.sid="overflow";require(!model.announce(invalid)&&model.rowCount()==64,"Untrusted announcements exceeded resource bound");
        std::cout<<"Identity validation, paired grouping, cross-owner SID conflicts, partial publication, stale restart events, tombstones, cleanup and model bounds passed\n";
        return 0;
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
