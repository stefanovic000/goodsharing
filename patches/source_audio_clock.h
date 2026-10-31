#pragma once
#include <cstdint>
#include <optional>

namespace goodsharing {
// Both steady values are from MSVC steady_clock on the sender PC. Translate
// elapsed capture age, never the epoch, into the native WebRTC clock. Sample
// the clocks at the native source, after any FFI/runtime scheduling delay.
inline std::optional<std::int64_t> sourceAudioCaptureMs(
    std::int64_t sourceUs, std::int64_t steadyUs, std::int64_t nativeUs) {
    if (sourceUs <= 0 || steadyUs < sourceUs || nativeUs <= 0) return {};
    const auto ageUs = steadyUs - sourceUs;
    if (ageUs > 100000 || nativeUs <= ageUs) return {};
    const auto captureMs = (nativeUs - ageUs) / 1000;
    return captureMs > 0 ? std::optional<std::int64_t>(captureMs) : std::nullopt;
}
}
