#pragma once
#include <cmath>
#include <cstdint>
#include <optional>

namespace goodsharing {
// QPC is explicitly bridged to the caller's steady clock at each observation.
// Device position units are given by frequency, never assumed to be frames.
struct EndpointClockReading {
    std::uint64_t position=0,frequency=0,positionQpc100ns=0,nowQpc100ns=0,submittedFrames=0;
    std::int64_t localNowUs=0;
};
inline std::optional<std::int64_t> endpointLeadUs(const EndpointClockReading& r){
    constexpr std::uint64_t limit=100000000000000000ull;
    if(!r.frequency||r.frequency>1000000000000ull||!r.position||r.position>limit||
       !r.positionQpc100ns||r.nowQpc100ns<r.positionQpc100ns||r.nowQpc100ns-r.positionQpc100ns>200000||
       r.submittedFrames>48000ull*60*60*24||r.localNowUs<=0||r.localNowUs>1000000000000000ll)return {};
    const long double elapsed=static_cast<long double>(r.nowQpc100ns-r.positionQpc100ns)/10;
    long double lead=static_cast<long double>(r.submittedFrames)*1000000/48000-
        static_cast<long double>(r.position)*1000000/r.frequency-elapsed;
    // WASAPI position and QPC sampling are quantized independently. A deficit
    // within a few microseconds is measurement noise, not an underrun.
    if(lead<0&&lead>=-3000)lead=0;
    // An exhausted stream has lost its write-position relationship. Never
    // invent a new timestamp from padding or treat an underrun as fresh PCM.
    if(!std::isfinite(lead)||lead < -1000000000000.0L||lead > 1000000000000.0L)return {};
    return static_cast<std::int64_t>(std::llround(lead));
}
inline std::optional<std::int64_t> endpointDeadline(const EndpointClockReading& r){
    const auto lead=endpointLeadUs(r);
    if(!lead||*lead<0||*lead>30000)return {};
    return r.localNowUs+*lead;
}
// Consecutive writes address consecutive device frames. A small backward
// clock observation must not schedule the next packet over committed frames.
// Bound the correction itself and the resulting lead; never accumulate an
// unbounded offset or replace a missing device observation.
inline std::optional<std::int64_t> contiguousEndpointDeadline(std::int64_t observed,std::int64_t committedEnd,std::int64_t now){
    if(now<=0||now>1000000000000000ll||observed<now||observed>now+30000||committedEnd<0)return {};
    if(committedEnd&&observed<committedEnd){
        if(committedEnd-observed>5000||committedEnd>now+30000)return {};
        return committedEnd;
    }
    return observed;
}
}
