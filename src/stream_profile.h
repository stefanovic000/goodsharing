#pragma once
#include <cstdint>
#include <cstdlib>
#include <stdexcept>
#include <string_view>

namespace goodsharing {
// Local latency/smoothness preference. It tunes the viewer's common playout lead
// and video wait bound, the minimum spacing between presented pictures, and the
// sender's capture freshness / encoded handoff budget. Balanced is the previously
// validated configuration; the others trade latency for jitter tolerance.
enum class StreamProfile : int { LowLatency=0, Balanced=1, Smooth=2 };

// One presentation delay for both streams (docs/70-presentation-buffer-design.md):
// a picture or audio block captured at t is due at t+bufferUs. headroomUs is how
// long a due picture may still wait before the timeline is re-anchored, so the
// video wait bound is the sum. Keeping the sum means the leads and waits each
// profile was tuned with are unchanged; only bufferUs is expected to grow once the
// delayed timeline replaces the presented-reference coupling.
struct StreamTiming {
    std::int64_t bufferUs=150000;            // presentation delay for audio and video
    std::int64_t headroomUs=50000;           // extra wait allowed for a due picture
    // A 60 fps cadence is 16.7 ms, so a 10 ms floor dropped every frame whose
    // deadline bunched with the previous one; 4 ms keeps a guard against
    // double-painting while letting a late picture catch up instead of vanish.
    std::int64_t presentationFloorUs=4000;   // minimum spacing between presented pictures
    std::int64_t maximumCaptureAgeUs=150000; // sender: capture freshness / handoff age
    // Derived values; the playout core still takes them as plain microseconds.
    constexpr std::int64_t leadUs()const{return bufferUs;}
    constexpr std::int64_t videoWaitLimitUs()const{return bufferUs+headroomUs;}
};

inline StreamTiming streamTiming(StreamProfile profile) {
    // Test-only: exercise a buffer the shipped profiles do not use, so the large
    // values in docs/70 can be measured without changing the defaults.
    const char* overrideUs=std::getenv("GOODSHARING_TEST_BUFFER_US");
    StreamTiming timing;
    switch(profile) {
    // buffer + headroom are 70+35 = 105, 150+50 = 200 and 300+50 = 350 ms. The 150 and
    // 300 ms values in docs/70 are deliberately not shipped: measured 2026-11-02
    // they bring back the reset storm (711-1429 resets, no audio admitted).
    case StreamProfile::LowLatency: timing={70000,35000,4000,120000};break;
    case StreamProfile::Smooth: timing={300000,50000,4000,250000};break;
    case StreamProfile::Balanced: timing={150000,50000,4000,150000};break;
    }
    if(overrideUs){
        const auto value=std::atol(overrideUs);
        if(value>=5000&&value<=600000){timing.bufferUs=value;timing.headroomUs=50000;}
    }
    return timing;
}

inline const char* streamProfileName(StreamProfile profile) {
    switch(profile) {
    case StreamProfile::LowLatency: return "low";
    case StreamProfile::Smooth: return "smooth";
    case StreamProfile::Balanced: break;
    }
    return "balanced";
}

// Parses "low"/"balanced"/"smooth"; throws std::invalid_argument otherwise.
inline StreamProfile parseStreamProfile(const char* name) {
    if(!name)throw std::invalid_argument("Missing stream profile");
    const std::string_view text{name};
    if(text=="low"||text=="low-latency"||text=="latency")return StreamProfile::LowLatency;
    if(text=="balanced"||text=="balance"||text=="default")return StreamProfile::Balanced;
    if(text=="smooth"||text=="smoothness")return StreamProfile::Smooth;
    throw std::invalid_argument("Unknown stream profile");
}
}
