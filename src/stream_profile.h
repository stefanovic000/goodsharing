#pragma once
#include <cstdint>
#include <stdexcept>

namespace goodsharing {
// Local latency/smoothness preference. It tunes the viewer's common playout lead
// and video wait bound, the minimum spacing between presented pictures, and the
// sender's capture freshness / encoded handoff budget. Balanced is the previously
// validated configuration; the others trade latency for jitter tolerance.
enum class StreamProfile : int { LowLatency=0, Balanced=1, Smooth=2 };

struct StreamTiming {
    std::int64_t leadUs=30000;             // common audio/video target lead
    std::int64_t videoWaitLimitUs=80000;   // how long a due picture may wait
    // A 60 fps cadence is 16.7 ms, so a 10 ms floor dropped every frame whose
    // deadline bunched with the previous one; 4 ms keeps a guard against
    // double-painting while letting a late picture catch up instead of vanish.
    std::int64_t presentationFloorUs=4000; // minimum spacing between presented pictures
    std::int64_t maximumCaptureAgeUs=150000; // sender: capture freshness / handoff age
};

inline StreamTiming streamTiming(StreamProfile profile) {
    switch(profile) {
    case StreamProfile::LowLatency: return {45000,80000,4000,120000};
    case StreamProfile::Smooth: return {55000,110000,4000,250000};
    case StreamProfile::Balanced: break;
    }
    return {55000,90000,4000,150000};
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
