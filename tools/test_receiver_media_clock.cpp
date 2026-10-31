#include "receiver_media_clock.h"
#include <iostream>
#include <limits>
#include <stdexcept>
#include <thread>

using namespace goodsharing;
static void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
int main() {
    try {
        constexpr std::int64_t ntp = 4000000000000000ll, receive = 1000000000;
        SenderReportClock audio(48000), video(90000);
        audio.update(ntp, 0xffffa000, receive);
        require(!audio.stamp(0xffffa000, receive).sender_ntp_us, "One SR invented a trained clock");
        audio.update(ntp + 1000000, static_cast<std::uint32_t>(0xffffa000u + 48000u), receive + 1000000);
        auto a = audio.stamp(static_cast<std::uint32_t>(0xffffa000u + 72000u), receive + 1500000);
        require(a.has_rtp && a.sender_ntp_us == ntp + 1500000 && a.reports == 2, "Audio RTP wrap or sender clock mapping failed");
        video.update(ntp, 700000, receive);
        video.update(ntp + 1000000, 790000, receive + 1000000);
        require(video.stamp(835000, receive + 1500000).sender_ntp_us == a.sender_ntp_us, "Different RTP origins/rates did not map to the common sender clock");
        require(!video.stamp(835000, receive + 6000001).sender_ntp_us, "Stale SR clock survived");
        video.update(ntp + 1000000, 790000, receive + 7000000);
        require(!video.stamp(835000, receive + 7000000).sender_ntp_us, "Duplicate SR refreshed old evidence");
        require(!audio.stamp(0x80000000, receive + 1500000).sender_ntp_us, "Huge RTP jump extrapolated");
        require(!audio.stamp(a.rtp, receive - 1).sender_ntp_us, "Backward receiver clock accepted");
        const auto epoch = a.epoch;
        audio.update(ntp + 2000000, 123, receive + 2000000);
        require(!audio.stamp(123, receive + 2000000).sender_ntp_us && audio.stamp(123, receive + 2000000).epoch != epoch, "Inconsistent SR did not revoke the clock");
        audio.update(ntp + 3000000, 48123, receive + 3000000);
        require(audio.stamp(48123, receive + 3000000).sender_ntp_us == ntp + 3000000, "Consistent new epoch did not retrain");
        audio.update(std::numeric_limits<std::int64_t>::max(), 0, receive);
        require(!audio.stamp(0, receive).sender_ntp_us, "Invalid NTP value accepted");

        ReceiverClockRegistry registry; int owner1, owner2;
        registry.add(&owner1, false, 42); registry.add(&owner1, false, 42);
        registry.update(&owner1, ntp, 100, receive); registry.update(&owner1, ntp + 1000000, 90100, receive + 1000000);
        require(registry.video(42, 90100, receive + 1000000).sender_ntp_us == ntp + 1000000, "Idempotent receiver registration failed");
        registry.add(&owner2, false, 42);
        require(!registry.video(42, 90100, receive + 1000000).sender_ntp_us, "Ambiguous simultaneous SSRC admitted");
        registry.remove(&owner2);
        require(registry.video(42, 90100, receive + 1000000).sender_ntp_us, "Removing unrelated receiver lost a unique clock");
        registry.remove(&owner1); registry.add(&owner1, false, 42);
        require(!registry.video(42, 90100, receive + 1000000).sender_ntp_us, "Reused receiver address inherited an old clock");
        int owners[40]{}; ReceiverClockRegistry bounded;
        for (auto& owner : owners) bounded.add(&owner, true, 99);
        bounded.update(&owners[39], ntp, 100, receive); bounded.update(&owners[39], ntp + 1000000, 48100, receive + 1000000);
        require(!bounded.audio(&owners[39], 48100, receive + 1000000).sender_ntp_us, "Full registry invented a mapping");

        int pcm[4]{}; AudioClockContext context{pcm, 48000, 2, 2, a};
        require(!decodedAudioClock(pcm, 48000, 2, 2).has_rtp, "Audio timing leaked outside callback");
        {
            AudioClockScope scope(context);
            require(decodedAudioClock(pcm, 48000, 2, 2).sender_ntp_us == a.sender_ntp_us, "PCM metadata association lost");
            require(!decodedAudioClock(pcm + 1, 48000, 2, 2).has_rtp && !decodedAudioClock(pcm, 48000, 1, 2).has_rtp, "Unrelated/remixed PCM inherited timing");
            bool leaked = false;
            std::thread other([&] { leaked = decodedAudioClock(pcm, 48000, 2, 2).has_rtp; }); other.join();
            require(!leaked, "Audio timing leaked across threads");
            AudioClockContext inner{pcm + 1, 48000, 2, 2, {}};
            { AudioClockScope nested(inner); require(!decodedAudioClock(pcm, 48000, 2, 2).has_rtp, "Nested callback inherited outer timing"); }
            require(decodedAudioClock(pcm, 48000, 2, 2).has_rtp, "Nested callback failed to restore timing");
        }
        require(!decodedAudioClock(pcm, 48000, 2, 2).has_rtp, "Callback retained borrowed timing");
        std::cout << "RTCP clock training, RTP wrap, stale/duplicate reports, clock jumps, SSRC ambiguity, bounded lifetime and exact callback association passed\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
