#pragma once
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <stdexcept>

namespace goodsharing {
class EndpointDiscontinuity:public std::runtime_error {
public:
    explicit EndpointDiscontinuity(std::int64_t gapUs):std::runtime_error("Short output stream discontinuity"),gapUs(gapUs){}
    const std::int64_t gapUs;
};
// Endpoint-owner thread only. reset must stop/reset the device before success.
// prepare reserves at most 480 stereo frames; commit may supply fewer complete
// frames. The endpoint must resize its reservation, never queue unwritten PCM.
// cancel releases without media.
class AudioPlayoutSink {
public:
    struct Window {std::int64_t firstAudibleUs=0;std::size_t frames=0;};
    virtual ~AudioPlayoutSink()=default;
    virtual std::optional<Window> prepare(std::int64_t nowUs)=0;
    virtual bool commit(std::span<const std::int16_t> pcm)=0;
    // Re-write the last committed block into the current reservation; false when the
    // implementation cannot hold (queue-backed sinks repeat their own queue instead).
    virtual bool repeatLast(){return false;}
    virtual void cancel()noexcept=0;
    virtual bool reset()noexcept=0;
    virtual bool hardwareEndpoint()const noexcept{return false;}
};
}
