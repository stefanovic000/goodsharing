#pragma once
#include <QObject>
#include <QString>
#include <QByteArray>
#include <functional>
#include <memory>
#include <livekit/room.h>
#include "share_publisher.h"
#include "remote_share.h"

namespace goodsharing {
// Qt-owner API. SDK operations execute on an owned transport thread. A fresh
// connection and epoch are created for every join; queued old notifications are
// ignored. Ready means room/key setup, never that a media track is encrypted.
class RoomController : public QObject {
    Q_OBJECT
public:
    enum class State { Idle,Joining,Ready,Reconnecting,Leaving,Failed };
    Q_ENUM(State)
    explicit RoomController(QObject* parent=nullptr);
    ~RoomController();
    bool join(QString url,QString token,QByteArray clientKey);
    void leave();
    State state()const{return state_;}
    quint64 epoch()const{return epoch_;}
    // Bounded command bridge for publisher/receiver integration. This runs only
    // while Ready, on the transport owner. Never retain Room or block on Qt.
    bool execute(std::function<void(livekit::Room&)> operation);
    bool setSubscribed(QString sid,bool subscribed);
    // Fixed selection; unwatch and wait for stopped() before selecting again.
    // Media callbacks run off Qt. A normal watch decodes one video quality.
    std::shared_ptr<RemoteShare> watchShare(QString owner,QString shareId,quint64 generation,ShareTrack quality,bool audio,RemoteShare::Callbacks callbacks);
    bool unwatchShare(QString owner,QString shareId);
    // Separate immutable track leases permit one audio + active/candidate video.
    std::shared_ptr<RemoteShare> watchTrack(QString owner,QString shareId,quint64 generation,ShareTrack track,RemoteShare::Callbacks callbacks);
    bool unwatchTrack(const std::shared_ptr<RemoteShare>& watch);
    std::shared_ptr<ShareInput> startShare(bool audio,std::int64_t maximumCaptureAgeUs=150000,int fps=60,int bitrate1080=10000000,int bitrate720=5000000);
    // Bounded advisory state for viewers of the current share (window sources).
    bool publishShareSourceState(bool minimized);
    // Refresh the publication counters while a share runs; the state transitions
    // alone leave the report frozen at the first snapshot.
    void refreshShareDiagnostics();
    void stopShare();
    SharePublisher::State shareState()const{return shareState_;}
    // Authoritative current generation for a share, 0 when unknown.
    quint64 currentShareGeneration(QString owner,QString id)const;
    // Whether the catalog currently holds a given video layer for a share.
    bool hasShareQuality(QString owner,QString id,int quality)const;
    // Reason for the last refused watch, for the integration status line.
    QString lastWatchError()const{return watchError_;}
    quint64 shareGeneration()const{return shareGeneration_;}
Q_SIGNALS:
    void stateChanged(goodsharing::RoomController::State state,QString message);
    void trackAvailable(QString owner,QString sid,QString name,bool audio,bool encrypted,bool screenSource,quint32 width,quint32 height);
    void trackRemoved(QString sid);
    void ownerRemoved(QString owner);
    void participantJoined(QString identity,QString name);
    void subscriptionChanged(QString sid,bool subscribed);
    void shareStateChanged(int state,QString message,quint64 generation);
    void shareDiagnostics(QJsonObject report);
    // A sharing participant reports that its source window was minimized/restored.
    void sourceStateChanged(QString owner,QString shareId,bool minimized);
    void watchStopped(QJsonObject report);
    // Reconnect invalidates outgoing-content permission immediately. The
    // integration must stop/recreate publication and repeat synthetic priming.
    void mediaInvalidated();
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    QString watchError_;
    State state_=State::Idle;
    quint64 epoch_=0;
    QString shareId_;
    quint64 shareGeneration_=0;
    SharePublisher::State shareState_=SharePublisher::State::Stopped;
    std::shared_ptr<ShareInput> input_;
    void publicationState(SharePublisher::State state,QString message,quint64 generation);
    void transition(State state,QString message={});
    void ownerThread()const;
    std::shared_ptr<RemoteShare> watchImpl(QString owner,QString id,quint64 generation,ShareTrack quality,bool audio,RemoteShare::Callbacks callbacks,bool separate);
};
}

