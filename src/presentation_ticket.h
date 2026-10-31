#pragma once
#include <cstdint>
#include "audio_playout.h"
namespace goodsharing {
struct PresentationTicket {
    std::int64_t deadlineUs=0,senderUs=0;
    std::uint64_t serial=0;
    PlayoutClock candidateClock;
    std::int64_t receivedUs=0;
    std::uint64_t attempt=0;
    // Nonzero only for a running active picture presented before this mapped
    // target. receivedUs and candidateClock retain its validated source stamp.
    std::int64_t timelineDeadlineUs=0;
};
}
