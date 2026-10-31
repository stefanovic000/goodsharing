#include "frame_pacer.h"
#include <iostream>
#include <thread>
#include <stdexcept>
int main() {
    goodsharing::FramePacer pacer(200);
    auto previous=pacer.wait();
    for(int i=0;i<60;i++) {
        if(i==20) std::this_thread::sleep_for(std::chrono::milliseconds(25));
        const auto now=pacer.wait();
        if(now-previous<std::chrono::microseconds(4500)) { std::cerr<<"Video publication caught up in a burst\n"; return 1; }
        previous=now;
    }
    try { goodsharing::FramePacer invalid(0); return 2; } catch(const std::invalid_argument&) {}
    try { goodsharing::FramePacer invalid(241); return 2; } catch(const std::invalid_argument&) {}
    goodsharing::FramePacer slow(1); const auto first=slow.wait();
    if(slow.wait()-first<std::chrono::seconds(1)) return 3;
    std::cout<<"Pacing bounds handoff spacing after stalls, rejects invalid rates, and supports the 1 FPS boundary\n";
}
