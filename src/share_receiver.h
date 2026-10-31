#pragma once
#include "track_reader.h"
#include <memory>
#include <string>
#include <QJsonObject>
#include <livekit/room_event_types.h>

namespace goodsharing {
// SDK callbacks enqueue bounded commands only. Stream creation/teardown runs on
// an owned control thread; decoded frame callbacks run on TrackReader workers.
// One instance belongs to a room/share generation. stop() closes admission,
// discards pending attachments, then joins control/readers outside its mutex.
// Verified decryption for the authenticated owner/SID is required by default.
// Call encryption() from SDK events; decoder/PLC output alone is not proof.
class ShareReceiver {
public:
    ShareReceiver();
    ~ShareReceiver();
    bool video(std::shared_ptr<livekit::Track> track,TrackReader::VideoCallback callback,std::string owner={});
    bool audio(std::shared_ptr<livekit::Track> track,TrackReader::AudioCallback callback,unsigned channels=2,std::string owner={});
    bool encryption(std::string owner,std::string sid,livekit::EncryptionState state);
    bool remove(std::string sid);
    bool canPresent(const std::string& owner,const std::string& sid)const;
    void stop();
    std::string error() const;
    QJsonObject report()const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
