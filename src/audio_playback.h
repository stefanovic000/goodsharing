#pragma once
#include <QJsonObject>
#include <QString>
#include <cstdint>
#include <memory>
#include <span>

namespace goodsharing {
// A bounded stereo PCM output. Device access stays on its owned worker thread.
// start/stop are owner-thread operations; submit/report may run on other threads.
class AudioPlayback {
public:
    AudioPlayback();
    ~AudioPlayback();
    AudioPlayback(const AudioPlayback&)=delete;
    AudioPlayback& operator=(const AudioPlayback&)=delete;
    void start(float volume=1.0f);
    bool submit(std::span<const std::int16_t> stereo48000);
    void stop();
    QJsonObject report() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
