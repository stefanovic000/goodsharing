#pragma once
#include <chrono>
#include <memory>

namespace goodsharing {
// Paces handoffs, allowing 10% timing jitter without catch-up bursts. A
// pre-encoded H.264 delta depends on earlier frames, so an encoder queue must
// not shed those frames as if they were disposable raw capture buffers.
class FramePacer {
public:
    using Clock=std::chrono::steady_clock;
    explicit FramePacer(unsigned framesPerSecond=60);
    ~FramePacer();
    FramePacer(const FramePacer&)=delete;
    FramePacer& operator=(const FramePacer&)=delete;
    Clock::time_point wait();
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
