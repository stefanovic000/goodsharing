// docs/70-presentation-buffer-design.md step 1: the profile carries one buffer
// and one headroom, and the lead/wait the playout core consumes are derived from
// them. This keeps the tuned numbers visible in one place and fails loudly if a
// future buffer value outgrows the core's validation.
#include "audio_playout.h"
#include "stream_profile.h"
#include <iostream>
#include <stdexcept>

using namespace goodsharing;

namespace {
struct Expected {StreamProfile profile;const char* name;std::int64_t lead,wait,captureAge;};
// Leads 45/55/55 and waits 80/90/110 ms are the values each profile was tuned
// with; the refactor must not move them.
const Expected expected[]={
    {StreamProfile::LowLatency,"low",45000,80000,120000},
    {StreamProfile::Balanced,"balanced",55000,90000,150000},
    {StreamProfile::Smooth,"smooth",55000,110000,250000},
};
bool check(bool condition,const char* what) {
    if(condition)return true;
    std::cerr<<"stream profile timing: "<<what<<"\n";
    return false;
}
}
int main() {
    for(const auto& want: expected) {
        const auto timing=streamTiming(want.profile);
        const std::string scope=std::string("profile ")+want.name+" ";
        if(!check(timing.leadUs()==want.lead,(scope+"lead differs").c_str()))return 1;
        if(!check(timing.videoWaitLimitUs()==want.wait,(scope+"wait differs").c_str()))return 1;
        if(!check(timing.maximumCaptureAgeUs==want.captureAge,(scope+"capture age differs").c_str()))return 1;
        // The derived values must be the single buffer plus headroom, never a
        // second independently tuned number.
        if(!check(timing.leadUs()==timing.bufferUs,(scope+"lead is not the buffer").c_str()))return 2;
        if(!check(timing.videoWaitLimitUs()==timing.bufferUs+timing.headroomUs,
            (scope+"wait is not buffer plus headroom").c_str()))return 2;
        if(!check(timing.bufferUs>=0&&timing.headroomUs>=0,(scope+"has a negative allowance").c_str()))return 2;
        if(!check(timing.videoWaitLimitUs()>=timing.leadUs(),(scope+"waits less than its lead").c_str()))return 2;
        if(!check(timing.presentationFloorUs>0,(scope+"has no presentation floor").c_str()))return 2;
        // A buffer the core refuses would throw "Invalid playout timing" on the
        // first watch; prove every shipped profile is still accepted.
        try { AudioPlayout core; core.configureTiming(timing.leadUs(),timing.videoWaitLimitUs()); }
        catch(const std::invalid_argument&) {
            std::cerr<<"stream profile timing: "<<scope<<"rejected by configureTiming\n";
            return 3;
        }
    }
    // Balanced is the validated default, including for a defaulted StreamTiming.
    const auto balanced=streamTiming(StreamProfile::Balanced), byDefault=StreamTiming{};
    if(!check(balanced.leadUs()==byDefault.leadUs()&&balanced.videoWaitLimitUs()==byDefault.videoWaitLimitUs(),
        "default StreamTiming is not balanced"))return 4;
    for(const auto& want: expected) {
        if(!check(parseStreamProfile(want.name)==want.profile,"a profile name does not round-trip"))return 5;
        if(!check(std::string(streamProfileName(want.profile))==want.name,"a profile label does not round-trip"))return 5;
    }
    if(!check(parseStreamProfile("default")==StreamProfile::Balanced,"'default' is not balanced"))return 5;
    if(!check(parseStreamProfile("latency")==StreamProfile::LowLatency,"'latency' is not low"))return 5;
    if(!check(parseStreamProfile("smoothness")==StreamProfile::Smooth,"'smoothness' is not smooth"))return 5;
    for(const char* rejected: {static_cast<const char*>(nullptr),"fast",""}) {
        try { parseStreamProfile(rejected); return 6; } catch(const std::invalid_argument&) {}
    }
    std::cout<<"Stream profiles carry one buffer and headroom; derived leads 45/55/55 and waits 80/90/110 ms are unchanged and accepted by the playout core\n";
}
