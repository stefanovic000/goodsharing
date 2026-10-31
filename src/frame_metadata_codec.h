#pragma once
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace goodsharing {
struct FrameIdentity {
    std::uint64_t captureTimestampUs=0;
    std::uint32_t frameId=0;
    bool priming=false;
    bool operator==(const FrameIdentity&) const=default;
};

// The SDK XORs trailer bytes with 0xff before appending them to H.264.
// ASCII-only data stays >= 0x80 after XOR, so it cannot emulate a NAL start
// code. Do not place arbitrary binary IDs/timestamps in the built-in fields.
std::vector<std::uint8_t> encodeFrameIdentity(FrameIdentity identity);
std::optional<FrameIdentity> decodeFrameIdentity(std::span<const std::uint8_t> bytes);
}
