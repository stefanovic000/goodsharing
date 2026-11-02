#include "encoded_delivery.h"
#include "raw_frame_budget.h"
#include "periodic_keyframe.h"
#include "publication_failure.h"
#include "../patches/preencoded_chain.h"
#include <iostream>
#include <stdexcept>
using namespace goodsharing;
void require(bool value,const char* message){if(!value)throw std::runtime_error(message);}
int main() {
    try {
        require(std::string_view(publicationFailureCode("Audio capture callback timed out"))=="audio_callback_timeout_ambiguous","Ambiguous audio completion was hidden");
        require(std::string_view(publicationFailureCode("Hardware encoder output exceeded its age budget"))=="encoder_output_expired","Encoder expiry was hidden");
        require(std::string_view(publicationFailureCode("Encoded video handoff rejected; restart from a fresh keyframe"))=="video_sdk_rejected","SDK rejection was hidden");
        require(std::string_view(publicationFailureCode("remote secret or key material"))=="unclassified_exception","Remote exception text escaped sanitization");
        require(std::string_view(publicationFailureCode("Audio capture callback timed out remote data"))=="unclassified_exception","Untrusted suffix matched an owned error");
        constexpr auto rejected="an RtcError occurred: InvalidState - failed to capture frame without buffering";
        require(!expiredAudioRejected(rejected,1000000,1100000)&&expiredAudioRejected(rejected,1000000,1100001),"Definite audio rejection lost its exact expiry boundary");
        require(!expiredAudioRejected(rejected,1000000,999999)&&!expiredAudioRejected(rejected,0,1100001)&&
            !expiredAudioRejected("Audio capture callback timed out",1000000,1100001)&&!expiredAudioRejected("unknown SDK error",1000000,1100001),
            "Ambiguous, future or unknown capture failure enabled continued publication");
        using D=EncodedDelivery::Decision;EncodedDelivery policy(150000);
        require(policy.inspect(1000000,1010000,true)==D::Send,"Fresh keyframe rejected");policy.handedOff(1000000,1010000,true);
        for(const auto period:{16667ULL,18182ULL,22222ULL,33333ULL})require(policy.inspect(1000000+period,1020000+period,false)==D::Send,"Variable cadence rejected");
        require(policy.inspect(2000000,2150001,false)==D::DropStale&&policy.recovering()&&policy.takeKeyframeRequest(),"Stale reference did not close delivery/request keyframe");
        require(!policy.takeKeyframeRequest(),"Request was not consumed");
        require(policy.inspect(2160000,2170000,false)==D::DropDependent,"Dependent frame resumed corrupt chain");
        require(policy.inspect(2180000,2190000,true)==D::Send&&policy.recovering(),"Recovery committed before SDK acceptance");
        require(policy.inspect(2180000,2340000,true)==D::DropStale&&policy.recoveries()==0,"Keyframe that aged while pacing reopened delivery");
        require(policy.inspect(2350000,2360000,false)==D::DropDependent,"Post-pacer expiry did not retain recovery gate");
        require(policy.inspect(2370000,2380000,true)==D::Send,"Fresh recovery keyframe rejected");policy.handedOff(2370000,2380000,true);
        require(!policy.recovering()&&policy.recoveries()==1&&policy.stale()==2&&policy.dependent()==2,"Recovery accounting incorrect");
        EncodedDelivery diagnostic;require(diagnostic.inspect(1,999999999,false)==D::Send&&!diagnostic.recovering(),"Diagnostic clock path was changed");
        EncodedDelivery stuck(150000);stuck.inspect(1,200000,false);bool failed=false;
        try{for(int i=0;i<33;i++)stuck.inspect(300000+i,310000+i,false);}catch(const std::runtime_error&){failed=true;}
        require(failed,"Encoder ignored keyframe requests without a bounded failure");
        require(!policy.expired(1000,151000)&&policy.expired(1000,151001),"Age boundary incorrect");
        RawFrameBudget budget(8000);unsigned admitted=0;
        for(std::uint64_t now=1000000;now<11000000;now+=16667)if(budget.admit(now)){budget.sent(808,now);admitted++;}
        require(admitted>=9&&admitted<=11&&budget.limited()>500,"Raw admission did not constrain measured output bytes");
        require(budget.admit(100000000),"Idle debt did not drain");budget.sent(808,100000000);
        require(!budget.admit(100000001),"Idle accumulated unlimited burst credit");
        budget.rate(8000000,100000002);require(budget.admit(100010002),"Recovered capacity did not release admission");
        PeriodicKeyframe refresh;
        require(!refresh.request(1000000,10000000),"Refresh before the first keyframe");
        // A 1 MB keyframe on 10 Mbps: max(5 s, wireBytes*320e6/bitrate) = 34.052096 s.
        refresh.accepted(1000000,1000000);
        const std::uint64_t interval=34052096,keyframes=1000000;
        require(!refresh.request(keyframes+interval-1,10000000),"Refresh arrived before its interval");
        require(refresh.request(keyframes+interval,10000000)&&!refresh.request(keyframes+interval+1,10000000),
            "Due refresh was not granted once, or was not coalesced while waiting for output");
        refresh.accepted(keyframes+interval,1000000);
        require(!refresh.request(keyframes+interval,10000000)&&!refresh.request(keyframes+interval-1,10000000)&&
            !refresh.request(keyframes+interval,0),"Backwards or unbudgeted refresh admitted");
        require(refresh.request(keyframes+2*interval,10000000)&&refresh.requests()==2,
            "The next interval did not release the periodic refresh");
        // The floor keeps a small keyframe from refreshing faster than every 5 s.
        PeriodicKeyframe sparse;sparse.accepted(1000000,1000);
        require(!sparse.request(5999999,1000000000)&&sparse.request(6000000,1000000000),"The five-second floor moved");
        PreencodedChain chain;
        require(!chain.admit(5,false)&&chain.admit(6,true)&&chain.admit(7,false),"New encoder accepted a dependent first frame");
        require(!chain.admit(9,false)&&!chain.admit(10,false)&&chain.gaps()==1,"Missing access unit did not close reference delivery");
        require(chain.admit(11,true)&&chain.admit(12,false),"Fresh keyframe did not restore sequence delivery");
        require(!chain.admit(12,true)&&!chain.admit(13,false)&&chain.admit(14,true),"Repeated/reordered keyframe reopened broken delivery");
        require(chain.admit(99,true)&&chain.admit(100,false),"Fresh keyframe could not bridge an omitted range");
        chain=PreencodedChain{};require(!chain.admit(1,false)&&chain.admit(2,true),"Source replacement reused old readiness");
        require(!chain.admit(3,true,false)&&!chain.admit(4,false),"Stale SDK keyframe reopened delivery");
        require(chain.admit(5,true)&&!chain.admit(6,false,false)&&!chain.admit(7,false)&&chain.admit(8,true),"Stale SDK reference did not require a fresh keyframe");
        require(sdkInputFresh(0)&&sdkInputFresh(100000)&&!sdkInputFresh(100001)&&!sdkInputFresh(-1),"SDK input age boundary incorrect");
        std::cout<<"Age boundary, variable cadence, reference-chain isolation, post-pacer expiry and bounded keyframe recovery passed\n";return 0;
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
