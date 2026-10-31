#include "encryption_gate.h"
#include "share_publisher.h"
#include "share_receiver.h"
#include <QCoreApplication>
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <future>
#include <thread>
using namespace goodsharing;
using E=livekit::EncryptionState;
namespace goodsharing {struct ShareInputTestPeer {static auto take(ShareInput& input,std::stop_token stop={}){return input.waitAudio(stop);}};}
void require(bool value,const char* message){if(!value)throw std::runtime_error(message);}
std::uint64_t now(){return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();}
std::shared_ptr<EncryptionGate> readyGate(std::uint64_t generation=1) {
    auto gate=std::make_shared<EncryptionGate>(generation);gate->configure("alice",{"high","low"});
    gate->observe("alice","high",E::Ok);gate->observe("alice","low",E::Ok);return gate;
}
ShareVideoInput frame(std::uint64_t timestamp,std::uint32_t id) {
    ShareVideoInput input;input.identity={timestamp,id,false};input.nv12[0].resize(1920*1080*3/2);input.nv12[1].resize(1280*720*3/2);return input;
}
int main(int argc,char**argv) {
    QCoreApplication app(argc,argv);
    try {
        const QString id="0123456789abcdef0123456789abcdef";
        auto gate=std::make_shared<EncryptionGate>(1);ShareInput input(id,true,gate);
        require(!input.video(frame(now(),1))&&!input.audio({now(),std::vector<std::int16_t>(960)}),"Captured content admitted before readiness");
        require(gate->configure("alice",{"high","low","audio"}),"Manifest rejected");
        require(!gate->observe("bob","high",E::Ok)&&!gate->observe("alice","",E::Ok)&&!gate->observe("alice","old-high",E::Ok),"Foreign/missing/stale SID accepted");
        require(!gate->observe("alice","old-high",E::MissingKey)&&!gate->failed(),"Retired track error stopped the new generation");
        for(int i=0;i<100;i++)gate->observe("alice","high",E::Ok);
        require(!gate->ready(),"Repeated events for one handler granted all tracks");
        gate->observe("alice","low",E::Ok);require(!gate->ready(),"Audio readiness skipped");
        gate->observe("alice","audio",E::Ok);require(gate->ready(),"Distinct handlers did not grant readiness");
        const auto timestamp=now();require(input.video(frame(timestamp,2))&&input.video(frame(timestamp+1,3)),"Fresh video input rejected");
        require(input.report()["pending_video_pairs"].toInt()==1&&input.report()["replaced_raw_frames"].toDouble()==1,"Raw mailbox did not stay bounded");
        require(!input.video(frame(timestamp,4))&&!input.video(frame(now()-200000,5)),"Out-of-order/stale input accepted");
        auto forged=frame(now()+2,6);forged.identity.priming=true;require(!input.video(std::move(forged)),"Captured content forged synthetic priming");
        gate->observe("alice","low",E::KeyRatcheted);gate->observe("alice","low",E::Ok);
        require(!gate->ready()&&gate->failed()&&!input.video(frame(now(),7)),"Revocation was reopened by a later OK");
        input.close();require(input.report()["pending_video_pairs"].toInt()==0,"Stop retained raw content");
        auto next=readyGate(2);ShareInput audio(id,true,next);const auto audioStart=now()-90000;
        for(int i=0;i<11;i++)require(audio.audio({audioStart+i*10000,std::vector<std::int16_t>(960,static_cast<std::int16_t>(i))}),"Audio pressure permanently revoked a healthy publication");
        require(next->ready()&&!next->failed()&&audio.report()["pending_audio_ms"].toInt()==100&&audio.report()["overflow_audio_drops"].toInt()==1,"Fresh audio did not replace the oldest bounded backlog");
        const auto kept=ShareInputTestPeer::take(audio);
        require(kept&&kept->timestampUs==audioStart+10000&&kept->stereo==std::vector<std::int16_t>(960,1),"Audio overflow changed timestamps or retained the oldest samples");
        next->observe("alice","high",E::MissingKey);
        require(!ShareInputTestPeer::take(audio)&&!audio.audio({now(),std::vector<std::int16_t>(960)}),"Pressure recovery bypassed subsequent crypto revocation");audio.close();
        {
            auto g=readyGate(7);ShareInput stalled(id,true,g);const auto stamp=now();
            for(int i=0;i<3;i++)require(stalled.audio({stamp+i,std::vector<std::int16_t>(960,222)}),"Expiry control failed admission");
            std::this_thread::sleep_for(std::chrono::milliseconds(110));
            std::stop_source cancellation;
            auto consumer=std::async(std::launch::async,[&]{return ShareInputTestPeer::take(stalled,cancellation.get_token());});
            const auto waited=consumer.wait_for(std::chrono::milliseconds(10))==std::future_status::timeout;
            const auto fresh=now();const auto admitted=stalled.audio({fresh,std::vector<std::int16_t>(960,333)});
            if(consumer.wait_for(std::chrono::seconds(1))!=std::future_status::ready)cancellation.request_stop();
            const auto block=consumer.get();
            require(waited&&admitted&&block&&block->timestampUs==fresh&&block->stereo==std::vector<std::int16_t>(960,333)&&
                g->ready()&&stalled.report()["expired_audio_drops"].toInt()==3&&stalled.report()["pending_audio_ms"].toInt()==0,
                "A stalled audio consumer replayed expired PCM or could not resume fresh samples");
        }
        auto silent=readyGate(3);ShareInput noAudio(id,false,silent);require(!noAudio.audio({now(),std::vector<std::int16_t>(960)})&&!silent->failed(),"Audio-off behavior invalid");
        noAudio.close();require(silent->closed()&&!silent->ready(),"Closing input did not revoke publication");
        for(auto state:{E::EncryptionFailed,E::DecryptionFailed,E::MissingKey,E::InternalError,E::New}) {
            auto g=readyGate(4);g->observe("alice","high",state);g->observe("alice","high",E::Ok);require(g->failed()&&!g->ready(),"Encryption failure resumed an old generation");
        }
        auto invalid=std::make_shared<EncryptionGate>(5);bool rejected=false;try{invalid->configure("alice",{"same","same"});}catch(const std::invalid_argument&){rejected=true;}require(rejected&&!invalid->ready(),"Duplicate manifest SID accepted");
        auto stopped=std::make_shared<EncryptionGate>(6);stopped->close();require(!stopped->configure("alice",{"high","low"})&&!stopped->ready(),"Cancelled startup reopened");
        ShareReceiver receiver;
        require(receiver.encryption("alice","remote-high",E::New),"Receive state was not registered");
        require(!receiver.encryption("bob","remote-high",E::Ok)&&receiver.report()["verified_tracks"].toInt()==0,"Other owner granted receive permission");
        require(receiver.encryption("alice","remote-high",E::Ok)&&receiver.report()["verified_tracks"].toInt()==1,"Verified owner/SID was not recorded");
        receiver.encryption("alice","remote-high",E::MissingKey);
        require(!receiver.encryption("alice","remote-high",E::Ok),"Failed receive handler was reopened");
        receiver.stop();require(!receiver.encryption("alice","new-track",E::Ok),"Receive state admitted after Stop");
        std::cout<<"Per-track/owner readiness, duplicate/late event rejection, sticky revocation, raw replacement, capture-preroll isolation and bounded audio passed\n";
        return 0;
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
