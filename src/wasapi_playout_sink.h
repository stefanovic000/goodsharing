#pragma once
#include "audio_playout_sink.h"
#include <QJsonObject>
#include <memory>

namespace goodsharing {
// Create/use/destroy on one COM-initialized endpoint thread. No PCM queue.
// Opened without starting; only silent bootstrap precedes a timed media commit.
class WasapiPlayoutSink final:public AudioPlayoutSink {
public:
    WasapiPlayoutSink();
    ~WasapiPlayoutSink();
    WasapiPlayoutSink(const WasapiPlayoutSink&)=delete;
    WasapiPlayoutSink& operator=(const WasapiPlayoutSink&)=delete;
    std::optional<Window> prepare(std::int64_t nowUs)override;
    bool commit(std::span<const std::int16_t> pcm)override;
    bool repeatLast()override;
    void cancel()noexcept override;
    bool reset()noexcept override;
    bool hardwareEndpoint()const noexcept override{return true;}
    void wait(unsigned milliseconds);
    QJsonObject report()const; // endpoint thread only
private:
    bool writeReserved(std::span<const std::int16_t> pcm);
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
