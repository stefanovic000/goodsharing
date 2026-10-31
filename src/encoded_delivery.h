#pragma once
#include <cstdint>
#include <stdexcept>

namespace goodsharing {
// Sender-local capture clock only. A discarded H.264 reference closes delivery
// until a fresh keyframe; never resume with a delta from the discarded chain.
// No frame storage, clock synchronization or claim about SDK/network queues.
class EncodedDelivery {
public:
    enum class Decision {Send,DropStale,DropDependent};
    explicit EncodedDelivery(std::uint64_t maximumAgeUs=0):maximumAgeUs_(maximumAgeUs){if(maximumAgeUs>1000000)throw std::invalid_argument("Invalid encoded age budget");}
    bool expired(std::uint64_t captureUs,std::uint64_t nowUs)const{return maximumAgeUs_&&nowUs>captureUs&&nowUs-captureUs>maximumAgeUs_;}
    Decision inspect(std::uint64_t captureUs,std::uint64_t nowUs,bool keyframe) {
        if(expired(captureUs,nowUs)){recovering_=true;request_=true;stale_++;return Decision::DropStale;}
        if(recovering_&&!keyframe){request_=true;dependent_++;if(++recoveryDeltas_>32)throw std::runtime_error("Encoder failed to produce a fresh recovery keyframe");return Decision::DropDependent;}
        return Decision::Send;
    }
    // Commit only after synchronous SDK acceptance. A post-pacer age check
    // must not complete recovery for a keyframe that then becomes obsolete.
    void handedOff(std::uint64_t captureUs,std::uint64_t nowUs,bool keyframe) {
        if(recovering_){if(!keyframe)throw std::logic_error("Dependent frame crossed recovery gate");recovering_=false;recoveryDeltas_=0;recoveries_++;request_=false;}
        if(nowUs>=captureUs&&nowUs-captureUs>maximumHandoffAgeUs_)maximumHandoffAgeUs_=nowUs-captureUs;
    }
    bool takeKeyframeRequest(){const bool request=request_;request_=false;return request;}
    std::uint64_t budgetUs()const{return maximumAgeUs_;}
    std::uint64_t stale()const{return stale_;}
    std::uint64_t dependent()const{return dependent_;}
    std::uint64_t recoveries()const{return recoveries_;}
    std::uint64_t maximumHandoffAgeUs()const{return maximumHandoffAgeUs_;}
    bool recovering()const{return recovering_;}
private:
    const std::uint64_t maximumAgeUs_;
    std::uint64_t stale_=0,dependent_=0,recoveries_=0,maximumHandoffAgeUs_=0;
    unsigned recoveryDeltas_=0;
    bool recovering_=false,request_=false;
};
}
