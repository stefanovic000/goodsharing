#include "source_audio_clock.h"
#include <iostream>
#include <limits>
#include <stdexcept>

int main() {
    using goodsharing::sourceAudioCaptureMs;
    auto check = [](bool value) { if (!value) throw std::runtime_error("source audio clock regression"); };
    try {
        // Sender and WebRTC epochs deliberately differ by hours, in either direction.
        check(sourceAudioCaptureMs(9000000000, 9000025000, 2000000000) == 1999975);
        check(sourceAudioCaptureMs(2000000000, 2000070000, 9000000000) == 8999930);
        // Runtime scheduling delay must remain in capture age, including ms rounding.
        check(sourceAudioCaptureMs(1000000, 1000000, 8000123) == 8000);
        check(sourceAudioCaptureMs(1000000, 1100000, 8100123) == 8000);
        check(!sourceAudioCaptureMs(1000000, 1100001, 8100124));
        check(!sourceAudioCaptureMs(1000001, 1000000, 8000000));
        check(!sourceAudioCaptureMs(0, 1000000, 8000000));
        check(!sourceAudioCaptureMs(-1, 1000000, 8000000));
        check(!sourceAudioCaptureMs(1000000, 1000010, 0));
        check(!sourceAudioCaptureMs(1000000, 1010000, 9000));
        const auto huge = std::numeric_limits<std::int64_t>::max();
        check(!sourceAudioCaptureMs(1, huge, huge));
        check(sourceAudioCaptureMs(huge - 500, huge, huge).has_value());
        std::cout << "Source capture age preserved across unrelated native epochs; stale/future/invalid rejected\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
