#pragma once
#include <QString>
#include <atomic>
#include <cstdint>
#include <functional>
#include <thread>
#include <vector>

namespace goodsharing {
enum class AudioMode { Off, Application, Desktop };
struct AudioBlock {
    std::vector<std::int16_t> samples;
    int sampleRate = 48000;
    int channels = 2;
    std::int64_t timestampUs = 0;
};
class AudioCapture {
public:
    using Callback = std::function<void(AudioBlock)>;
    using Error = std::function<void(QString)>;
    ~AudioCapture();
    bool start(AudioMode mode, std::uint32_t processId, Callback callback, Error error);
    void stop();
    static QString probeProcessLoopback();
private:
    void run(AudioMode, std::uint32_t, Callback, Error);
    std::atomic_bool running_{false};
    std::thread worker_;
};
}
