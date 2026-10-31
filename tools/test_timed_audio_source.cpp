#include <livekit/livekit.h>
#include "../src/publication_failure.h"
#include <chrono>
#include <condition_variable>
#include <iostream>
#include <mutex>
#include <thread>
#include <vector>

using Clock = std::chrono::steady_clock;
static std::int64_t nowUs() {
    return std::chrono::duration_cast<std::chrono::microseconds>(Clock::now().time_since_epoch()).count();
}
int main() {
    try {
        struct Sdk { Sdk() { livekit::initialize(livekit::LogLevel::Warn); } ~Sdk() { livekit::shutdown(); } } sdk;
        auto source = std::make_shared<livekit::AudioSource>(48000, 2);
        auto track = livekit::LocalAudioTrack::createLocalAudioTrack("owned-timing-test", source);
        livekit::AudioStream::Options options; options.capacity = 8; options.num_channels = 2;
        auto stream = livekit::AudioStream::fromTrack(track, options);
        struct Sample { int value; std::optional<std::int64_t> capture, sink, senderNtp; std::optional<std::uint32_t> rtp; };
        std::mutex mutex; std::condition_variable ready; std::vector<Sample> samples;
        std::string readerError;
        std::jthread reader([&] {
            try {
                livekit::AudioFrameEvent event;
                while (stream->read(event)) {
                    std::lock_guard lock(mutex);
                    if (event.frame.data().empty()) throw std::runtime_error("Empty native audio");
                    samples.push_back({event.frame.data()[0], event.capture_time_ms, event.sink_time_us, event.sender_ntp_us, event.rtp_timestamp});
                    ready.notify_all();
                }
            } catch (const std::exception& error) { std::lock_guard lock(mutex); readerError = error.what(); ready.notify_all(); }
        });
        // Close before joining on every exception path; blocking read must wake.
        struct Close { std::shared_ptr<livekit::AudioStream> stream; ~Close() { stream->close(); } } close{stream};
        auto require = [](bool value, const char* error) { if (!value) throw std::runtime_error(error); };
        auto frame = [](int value, int frames = 480, int channels = 2) {
            return livekit::AudioFrame(std::vector<std::int16_t>(frames * channels, static_cast<std::int16_t>(value)), 48000, channels, frames);
        };
        int rejected = 0;
        auto reject = [&](auto action,bool nativeExpired=false) { bool threw = false;const char* code=""; try { action(); } catch (const std::exception& e) { threw = true;code=goodsharing::publicationFailureCode(e.what()); } require(threw, "Invalid timed source input was admitted");if(nativeExpired)require(std::string_view(code)=="audio_native_rejected","Expired native rejection does not match the pinned known completion"); ++rejected; };
        reject([&] { source->captureFrameAt(frame(1), 0); });
        reject([&] { source->captureFrameAt(frame(2), -1); });
        reject([&] { source->captureFrameAt(frame(3), nowUs() + 1000000); });
        reject([&] { source->captureFrameAt(frame(4), nowUs() - 150000); },true);
        reject([&] { source->captureFrameAt(frame(5, 960), nowUs()); });
        reject([&] { source->captureFrameAt(frame(6, 480, 1), nowUs()); });
        auto buffered = std::make_shared<livekit::AudioSource>(48000, 2, 20);
        reject([&] { buffered->captureFrameAt(frame(7), nowUs()); });
        const auto source25 = nowUs() - 25000;
        source->captureFrameAt(frame(101), source25);
        const auto maximumAge25 = nowUs() - source25 + 2000;
        const auto source70 = nowUs() - 70000;
        source->captureFrameAt(frame(102), source70);
        const auto maximumAge70 = nowUs() - source70 + 2000;
        source->captureFrame(frame(103)); // Untimed PCM must remain untimed.
        {
            std::unique_lock lock(mutex);
            require(ready.wait_for(lock, std::chrono::seconds(3), [&] { return samples.size() >= 3 || !readerError.empty(); }), "Native audio sink timed out");
        }
        stream->close(); reader.join();
        require(readerError.empty(), "Native audio reader failed");
        require(samples.size() == 3, "Rejected source input reached the sink");
        for (int i = 0; i < 3; ++i) {
            require(samples[i].value == 101 + i, "PCM timestamp association changed");
            require(!samples[i].senderNtp && !samples[i].rtp, "Local capture time was relabeled as received RTP/RTCP time");
        }
        for (int i = 0; i < 2; ++i) {
            require(samples[i].capture && samples[i].sink, "Timed local PCM lost its capture timestamp");
            const auto ageUs = *samples[i].sink - *samples[i].capture * 1000;
            const auto expected = i == 0 ? 25000 : 70000;
            std::cout << "PCM " << samples[i].value << " capture age " << ageUs << " us; allowed " << expected << ".." << (i == 0 ? maximumAge25 : maximumAge70) << " us\n";
            require(ageUs >= expected && ageUs <= (i == 0 ? maximumAge25 : maximumAge70), "Capture age was replaced by submission/arrival time");
            std::cout << "PCM " << samples[i].value << " preserved capture age " << ageUs << " us\n";
        }
        require(!samples[2].capture && samples[2].sink, "Untimed PCM acquired an invented capture time");
        std::cout << "Timed source: 2 delayed frames preserved, " << rejected << " invalid calls rejected, untimed frame remains untimed\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
