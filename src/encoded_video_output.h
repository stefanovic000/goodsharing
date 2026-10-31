#pragma once
#include "hardware_encoder.h"
#include "frame_metadata_codec.h"
#include <livekit/encoded_video_source.h>
#include <livekit/local_video_track.h>
#include <livekit/room_event_types.h>
#include <QJsonObject>
#include <functional>
#include <map>
#include <memory>
#include <chrono>

namespace goodsharing {
// One COM/MF owner, no encoded access-unit queue. With a local age budget,
// obsolete output requires explicit fresh-keyframe recovery; otherwise ordered
// output is handed off in full or fails. Zero budget is for diagnostic clients.
class EncodedVideoOutput {
public:
    using MetadataTransform=std::function<livekit::VideoFrameMetadata(livekit::VideoFrameMetadata,bool)>;
    using Observer=std::function<void(const EncodedFrame&,FrameIdentity)>;
    using ContentCheck=std::function<bool(bool priming)>;
    using BeforeHandoff=std::function<void(FrameIdentity)>;
    EncodedVideoOutput(int width,int height,int bitrate,std::string name,
                       MetadataTransform transform={},Observer observer={},bool paced=true,ContentCheck check={},std::chrono::milliseconds maximumAge={},BeforeHandoff beforeHandoff={},int fps=60);
    ~EncodedVideoOutput();
    bool submit(const std::vector<std::uint8_t>& nv12,FrameIdentity identity);
    void poll();
    livekit::EncodedVideoSource::Feedback updateFeedback(std::uint64_t diagnosticBitrate=0);
    void requestKeyframe();
    std::shared_ptr<livekit::LocalVideoTrack> track() const;
    livekit::TrackPublishOptions publishOptions() const;
    QJsonObject report(bool includeTestTrace=false) const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
