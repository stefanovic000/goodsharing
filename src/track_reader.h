#pragma once
#include <livekit/video_stream.h>
#include <livekit/audio_stream.h>
#include <functional>
#include <memory>
#include <string>
#include <QJsonObject>

namespace goodsharing {
// Start/stop on a transport owner, never on the SDK event thread or a reader's
// callback. Callbacks run on owned readers; they must be bounded. Destruction
// closes streams and joins before callback storage can be released.
class TrackReader {
public:
    using VideoCallback=std::function<void(livekit::VideoFrameEvent&)>;
    using AudioCallback=std::function<void(livekit::AudioFrameEvent&)>;
    TrackReader();
    ~TrackReader();
    void startVideo(const std::shared_ptr<livekit::Track>& track,VideoCallback callback);
    void startAudio(const std::shared_ptr<livekit::Track>& track,AudioCallback callback,unsigned channels=2);
    void stop();
    std::string error() const;
    QJsonObject report() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
