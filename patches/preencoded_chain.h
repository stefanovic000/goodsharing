#pragma once
#include <cstdint>

namespace goodsharing {
inline bool sdkInputFresh(std::int64_t ageUs) { return ageUs >= 0 && ageUs <= 100000; }
// Access-unit order, not capture cadence. Called on the encoder sequence.
// A frame omitted before the pass-through encoder breaks external references.
class PreencodedChain {
public:
    bool admit(std::uint64_t sequence, bool keyframe, bool fresh = true) {
        if (!sequence || (last_ && sequence <= last_)) {
            waiting_ = true;
            ++gaps_;
            return false;
        }
        if (last_ && sequence != last_ + 1) {
            waiting_ = true;
            ++gaps_;
        }
        last_ = sequence;
        if (!fresh) { waiting_ = true; return false; }
        if (waiting_ && !keyframe) return false;
        waiting_ = false;
        return true;
    }
    std::uint64_t gaps() const { return gaps_; }
private:
    std::uint64_t last_ = 0, gaps_ = 0;
    bool waiting_ = true;
};
}
