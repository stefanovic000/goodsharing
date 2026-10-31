#pragma once
#include "share_model.h"
#include "share_receiver.h"
#include <array>
#include <atomic>
#include <map>
#include <set>
#include <mutex>
#include <shared_mutex>

namespace goodsharing {
// Serialized by the room event-map mutex, independent of the Qt model thread.
// Tombstones last for this room epoch; SID reuse and stale generations fail shut.
class RemoteShareCatalog {
public:
    using Key=std::pair<QString,QString>; // authenticated owner, share ID
    struct Record {ShareModel::Announcement announcement;ShareIdentity identity;};
    struct Change {bool accepted=false,changed=false;std::vector<QString> retired;};
    struct Manifest {QString owner,id;quint64 generation=0;ShareTrack quality=ShareTrack::Video720;QString videoSid,audioSid;};
    Change announce(const ShareModel::Announcement& announcement);
    std::optional<Record> find(const QString& sid)const;
    std::optional<Manifest> manifest(const QString& owner,const QString& id,quint64 generation,ShareTrack quality,bool audio)const;
    // Latest accepted generation for a share, independent of any model snapshot.
    std::optional<quint64> generation(const QString& owner,const QString& id)const;
    // Which video layers the catalog currently holds for a share.
    bool hasVideo(const QString& owner,const QString& id,int quality)const;
    bool remove(const QString& sid);
    std::vector<QString> removeOwner(const QString& owner);
    QString error()const{return error_;}
    // Why the last manifest() call refused (diagnostics only).
    QString manifestError()const{return manifestError_;}
private:
    struct Entry {quint64 generation=0;bool retired=false;std::array<QString,3> sids;};
    std::map<Key,Entry> entries_;
    std::map<QString,Record> tracks_;
    std::set<QString> retired_;
    QString error_;
    mutable QString manifestError_;
    void tombstone(const QString& sid);
};

// One fixed selection of one remote generation. The room owns subscription
// changes; SDK callbacks only enqueue bounded reader work. Callbacks run on
// readers and borrow their frame; consumers must not retain the reference or
// queue unbounded copies. An already-running callback may finish after revoke.
// A replacement watch has separate readiness and admission, even for the same SID.
class RemoteShare {
public:
    using Manifest=RemoteShareCatalog::Manifest;
    struct Callbacks {TrackReader::VideoCallback video;TrackReader::AudioCallback audio;};
    RemoteShare(Manifest manifest,Callbacks callbacks,std::shared_ptr<std::atomic_bool> roomAdmission={});
    ~RemoteShare();
    const Manifest& manifest()const{return manifest_;}
    bool subscribed(const QString& owner,const std::shared_ptr<livekit::Track>& track);
    bool encryption(const QString& owner,const QString& sid,livekit::EncryptionState state);
    void unsubscribed(const std::shared_ptr<livekit::Track>& track);
    bool accepts(const QString& owner,const QString& sid)const;
    // Final CPU presentation admission for this exact selection. The operation
    // runs under the revocation mutex; never call back into RemoteShare in it.
    bool presentVideo(const std::function<void()>& operation)const;
    bool presentAudio(const std::function<void()>& operation)const;
    bool sameRoom(const RemoteShare& other)const{return roomAdmission_==other.roomAdmission_;}
    void revoke(); // non-joining, safe on SDK/Qt threads
    void stop();   // transport owner only; never call from a media callback
    void finish(); // owner: readers stopped and SDK unsubscribe/room close completed
    bool closed()const{return !open_.load()||!roomAdmission_->load();}
    bool stopped()const{return stopped_.load();}
    std::string error()const{return receiver_.error();}
    QJsonObject report()const;
    // Read-only asynchronous SDK counters; no media/presentation permission.
    std::vector<std::future<std::vector<livekit::RtcStats>>> requestStatistics()const;
private:
    const Manifest manifest_;
    const std::string owner_,videoSid_,audioSid_;
    const Callbacks callbacks_;
    const std::shared_ptr<std::atomic_bool> roomAdmission_;
    std::atomic_bool open_=true;
    std::atomic_bool stopped_=false;
    mutable std::shared_mutex mutex_;
    std::array<bool,2> verified_{};
    std::array<std::shared_ptr<livekit::Track>,2> tracks_;
    ShareReceiver receiver_;
    bool present(const std::function<void()>& operation,bool audio)const;
};
}
