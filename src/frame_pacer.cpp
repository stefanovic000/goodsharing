#include "frame_pacer.h"
#include <windows.h>
#include <stdexcept>
#include <thread>
#include <algorithm>

namespace goodsharing {
struct FramePacer::Impl {
    HANDLE timer=nullptr;
    std::chrono::nanoseconds period{};
    Clock::time_point next{};
    bool armed=false;
    ~Impl() { if(timer) CloseHandle(timer); }
};
FramePacer::FramePacer(unsigned fps):impl_(std::make_unique<Impl>()) {
    if(!fps||fps>240) throw std::invalid_argument("Video pacing requires 1 to 240 FPS");
    impl_->period=std::chrono::nanoseconds(1000000000/fps);
    impl_->timer=CreateWaitableTimerExW(nullptr,nullptr,CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,TIMER_MODIFY_STATE|SYNCHRONIZE);
    if(!impl_->timer) impl_->timer=CreateWaitableTimerExW(nullptr,nullptr,0,TIMER_MODIFY_STATE|SYNCHRONIZE);
    if(!impl_->timer) throw std::runtime_error("Cannot create video pacing timer");
}
FramePacer::~FramePacer()=default;
FramePacer::Clock::time_point FramePacer::wait() {
    auto& p=*impl_;
    auto now=Clock::now();
    while(p.armed&&now<p.next) {
        const auto ns=std::chrono::duration_cast<std::chrono::nanoseconds>(p.next-now).count();
        LARGE_INTEGER due{}; due.QuadPart=-((ns+99)/100);
        if(!SetWaitableTimerEx(p.timer,&due,0,nullptr,nullptr,nullptr,0)||WaitForSingleObject(p.timer,2000)!=WAIT_OBJECT_0) throw std::runtime_error("Video pacing timer failed");
        now=Clock::now();
    }
    // Preserve the nominal clock despite timer jitter, but bound catch-up
    // to one handoff per 90% of a frame period (15 ms at 60 FPS). Using
    // now+period alone accumulates timer overshoot and falls below 60 FPS.
    p.next=p.armed?std::max(p.next+p.period,now+p.period*9/10):now+p.period;
    p.armed=true;
    return now;
}
}
