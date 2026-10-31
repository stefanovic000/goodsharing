#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <mutex>

namespace goodsharing {
// RTCP sender-report NTP domain, NOT receiver steady_clock or authenticated
// application capture metadata. Zero means unavailable only for NTP, not RTP.
struct ReceiverClockStamp {
    bool has_rtp = false;
    std::uint32_t rtp = 0, reports = 0, epoch = 0;
    std::int64_t sender_ntp_us = 0;
};
inline std::int64_t rtpDelta(std::uint32_t value, std::uint32_t reference) {
    const auto delta = static_cast<std::uint32_t>(value - reference);
    return delta < 0x80000000u ? delta : static_cast<std::int64_t>(delta) - 0x100000000ll;
}
class SenderReportClock {
public:
    explicit SenderReportClock(std::uint32_t rate = 90000) : rate_(rate) {}
    void update(std::int64_t ntpUs, std::uint32_t rtp, std::int64_t arrivalUs) {
        if (ntpUs <= 0 || ntpUs > 5000000000000000ll || arrivalUs <= 0) { reset(); return; }
        if (reports_ && ntpUs == ntp_ && rtp == rtp_) return; // Duplicate SR is not fresh evidence.
        if (reports_) {
            const auto elapsed = ntpUs - ntp_;
            const auto ticks = rtpDelta(rtp, rtp_);
            // Known negotiated 48/90 kHz clock, 3 ms quantization + 2000 ppm.
            if (elapsed < 100000 || elapsed > 10000000 || arrivalUs < arrival_ ||
                arrivalUs - arrival_ > 5000000 || ticks <= 0) reset();
            else {
                const auto error = ticks * 1000000 - elapsed * rate_;
                if (error > static_cast<std::int64_t>(rate_) * (3000 + elapsed / 500) ||
                    error < -static_cast<std::int64_t>(rate_) * (3000 + elapsed / 500)) reset();
            }
        }
        ntp_ = ntpUs; rtp_ = rtp; arrival_ = arrivalUs;
        if (reports_ < 1000000) ++reports_;
    }
    ReceiverClockStamp stamp(std::uint32_t rtp, std::int64_t nowUs) const {
        ReceiverClockStamp result{true, rtp, reports_, epoch_, 0};
        const auto ticks = rtpDelta(rtp, rtp_);
        if (reports_ < 2 || !rate_ || nowUs < arrival_ || nowUs - arrival_ > 5000000 ||
            ticks > static_cast<std::int64_t>(rate_) * 5 || ticks < -static_cast<std::int64_t>(rate_) * 5) return result;
        const auto ntp = ntp_ + ticks * 1000000 / rate_;
        if (ntp > 0) result.sender_ntp_us = ntp;
        return result;
    }
private:
    void reset() { reports_ = 0; if (++epoch_ == 0) ++epoch_; }
    std::uint32_t rate_ = 0, reports_ = 0, epoch_ = 1, rtp_ = 0;
    std::int64_t ntp_ = 0, arrival_ = 0;
};

// Fixed storage, scoped to actual native receiver lifetimes. No per-frame
// allocation. Ambiguous simultaneous video SSRCs produce no clock mapping.
class ReceiverClockRegistry {
public:
    void add(const void* owner, bool audio, std::uint32_t ssrc) {
        std::lock_guard lock(mutex_);
        if (!owner) return;
        for (const auto& slot : slots_) if (slot.owner == owner) return;
        for (auto& slot : slots_) if (!slot.owner) {
            slot = {owner, audio, ssrc, SenderReportClock(audio ? 48000 : 90000)}; return;
        }
    }
    void remove(const void* owner) {
        std::lock_guard lock(mutex_);
        for (auto& slot : slots_) if (slot.owner == owner) slot.owner = nullptr;
    }
    void update(const void* owner, std::int64_t ntpUs, std::uint32_t rtp, std::int64_t arrivalUs) {
        std::lock_guard lock(mutex_);
        for (auto& slot : slots_) if (slot.owner == owner) { slot.clock.update(ntpUs, rtp, arrivalUs); return; }
    }
    ReceiverClockStamp audio(const void* owner, std::uint32_t rtp, std::int64_t nowUs) {
        std::lock_guard lock(mutex_);
        for (const auto& slot : slots_) if (slot.owner == owner && slot.audio) return slot.clock.stamp(rtp, nowUs);
        return {true, rtp};
    }
    ReceiverClockStamp video(std::uint32_t ssrc, std::uint32_t rtp, std::int64_t nowUs) {
        std::lock_guard lock(mutex_);
        const Slot* found = nullptr;
        for (const auto& slot : slots_) if (slot.owner && !slot.audio && slot.ssrc == ssrc) {
            if (found) return {true, rtp};
            found = &slot;
        }
        return found ? found->clock.stamp(rtp, nowUs) : ReceiverClockStamp{true, rtp};
    }
private:
    struct Slot { const void* owner = nullptr; bool audio = false; std::uint32_t ssrc = 0; SenderReportClock clock{90000}; };
    std::mutex mutex_;
    std::array<Slot, 32> slots_{};
};
inline ReceiverClockRegistry& receiverClocks() { static ReceiverClockRegistry registry; return registry; }

struct AudioClockContext {
    const void* pcm;
    int rate;
    std::size_t channels, frames;
    ReceiverClockStamp stamp;
};
inline thread_local const AudioClockContext* currentAudioClock = nullptr;
class AudioClockScope {
public:
    explicit AudioClockScope(const AudioClockContext& context) : previous_(currentAudioClock) { currentAudioClock = &context; }
    ~AudioClockScope() { currentAudioClock = previous_; }
    AudioClockScope(const AudioClockScope&) = delete;
    AudioClockScope& operator=(const AudioClockScope&) = delete;
private:
    const AudioClockContext* previous_;
};
inline ReceiverClockStamp decodedAudioClock(const void* pcm, int rate, std::size_t channels, std::size_t frames) {
    const auto* context = currentAudioClock;
    return context && context->pcm == pcm && context->rate == rate && context->channels == channels && context->frames == frames
        ? context->stamp : ReceiverClockStamp{};
}
}
