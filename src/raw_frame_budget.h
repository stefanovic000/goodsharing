#pragma once
#include <algorithm>
#include <cstdint>
#include <stdexcept>

namespace goodsharing {
// Byte feedback controls RAW admission, before H.264 creates references. Debt
// is a scalar, not stored video. No credit builds up during idle intervals.
// The next frame's size is unknown: this is average-rate backpressure with a
// one-frame/pipeline burst, not a packet queue age or strict wire-rate bound.
class RawFrameBudget {
public:
    explicit RawFrameBudget(std::uint64_t bitrate):bitrate_(bitrate){if(!bitrate)throw std::invalid_argument("Missing initial bitrate");}
    void rate(std::uint64_t bitrate,std::uint64_t now){if(!bitrate)return;drain(now);bitrate_=bitrate;}
    bool admit(std::uint64_t now){drain(now);if(debtBytes_>static_cast<double>(bitrate_)*.300/8){limited_++;return false;}return true;}
    void sent(std::uint64_t encodedBytes,std::uint64_t now){drain(now);payloadBytes_+=encodedBytes;
        // Conservative envelope, SRTP/RTP and fragmentation allowance. Actual
        // wire overhead is measured separately by the impairment fixture.
        debtBytes_+=estimatedWireBytes(encodedBytes);}
    static double estimatedWireBytes(std::uint64_t encodedBytes){return static_cast<double>(encodedBytes)+128+(encodedBytes/1000+(encodedBytes%1000!=0))*64.;}
    double debtBytes()const{return debtBytes_;}
    std::uint64_t limited()const{return limited_;}
    std::uint64_t payloadBytes()const{return payloadBytes_;}
private:
    std::uint64_t bitrate_,lastUs_=0,limited_=0,payloadBytes_=0;
    double debtBytes_=0;
    void drain(std::uint64_t now){if(lastUs_&&now>=lastUs_)debtBytes_=std::max(0.,debtBytes_-(now-lastUs_)*static_cast<double>(bitrate_)/8000000.);lastUs_=now;}
};
}
