#pragma once
#include "raw_frame_budget.h"
#include <algorithm>
#include <cstdint>

namespace goodsharing {
// Optional time-based refreshes must not turn sparse video into IDR-only
// output. Allow at most 25% of the current byte budget for a refresh the size
// of the last accepted keyframe. This is an estimate, not a wire-rate bound:
// codec GOPs and explicit receiver/reference-recovery requests take priority.
class PeriodicKeyframe {
public:
    void accepted(std::uint64_t captureUs, std::uint64_t payloadBytes) {
        lastUs_=captureUs;
        wireBytes_=RawFrameBudget::estimatedWireBytes(payloadBytes);
        requested_=false;
    }
    bool request(std::uint64_t captureUs, std::uint64_t bitrate) {
        if (!lastUs_ || !bitrate || requested_ || captureUs<lastUs_) return false;
        // Only a safety refresh: about 30 s at a 1 MB keyframe on 10 Mbps. Loss
        // recovery and explicit requests still produce keyframes when needed, so a
        // healthy stream is not interrupted by periodic IDR bursts at all.
        const double intervalUs=std::max(5000000.,wireBytes_*320000000./bitrate);
        if (static_cast<double>(captureUs-lastUs_)<intervalUs) return false;
        requested_=true;
        ++requests_;
        return true;
    }
    std::uint64_t requests() const {return requests_;}
private:
    std::uint64_t lastUs_=0,requests_=0;
    double wireBytes_=0;
    bool requested_=false;
};
}
