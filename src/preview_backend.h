#pragma once
#include "audio_capture.h"
#include "screen_capture.h"
#include "capture_publisher_input.h"
#include "room_controller.h"
#include "remote_video.h"
#include "remote_av_session.h"
#include "stream_profile.h"
#include <QObject>
#include <QVariantList>
#include <QQuickPaintedItem>
#include <QMutex>
#include <QTimer>
#include <mutex>
#include <QJsonArray>

class VideoItem:public QQuickPaintedItem {
    Q_OBJECT
public:
    explicit VideoItem(QQuickItem* parent=nullptr):QQuickPaintedItem(parent) {}
    void paint(QPainter*) override;
    void setFrame(QImage image);
    void setRemote(std::shared_ptr<goodsharing::RemoteVideo> remote);
    void frameQueued();
private:
    QMutex mutex_;
    QImage image_;
    std::shared_ptr<goodsharing::RemoteVideo> remote_;
};

class PreviewBackend final:public QObject {
    Q_OBJECT
    Q_PROPERTY(QVariantList sources READ sources NOTIFY sourcesChanged)
    Q_PROPERTY(QVariantList applications READ applications NOTIFY sourcesChanged)
    Q_PROPERTY(bool active READ active NOTIFY stateChanged)
    Q_PROPERTY(QString status READ status NOTIFY stateChanged)
    Q_PROPERTY(double audioLevel READ audioLevel NOTIFY meterChanged)
    Q_PROPERTY(int selectedSourceIndex READ selectedSourceIndex NOTIFY stateChanged)
    Q_PROPERTY(bool roomReady READ roomReady NOTIFY stateChanged)
    Q_PROPERTY(bool roomBusy READ roomBusy NOTIFY stateChanged)
    Q_PROPERTY(bool shareActive READ shareActive NOTIFY stateChanged)
    Q_PROPERTY(QString roomStatus READ roomStatus NOTIFY stateChanged)
    Q_PROPERTY(QString shareBadge READ shareBadge NOTIFY stateChanged)
    Q_PROPERTY(int selectedAudioMode READ selectedAudioMode NOTIFY stateChanged)
    Q_PROPERTY(int selectedAudioApplication READ selectedAudioApplication NOTIFY stateChanged)
    Q_PROPERTY(QVariantList remoteShares READ remoteShares NOTIFY remoteSharesChanged)
    Q_PROPERTY(bool watching READ watching NOTIFY stateChanged)
    Q_PROPERTY(bool watchBusy READ watchBusy NOTIFY stateChanged)
    Q_PROPERTY(QString remoteStatus READ remoteStatus NOTIFY stateChanged)
    Q_PROPERTY(bool remoteSourceMinimized READ remoteSourceMinimized NOTIFY stateChanged)
    Q_PROPERTY(int watchedQuality READ watchedQuality NOTIFY watchSelectionChanged)
    Q_PROPERTY(bool testAvPlayback READ testAvPlayback NOTIFY stateChanged)
    Q_PROPERTY(int streamProfile READ streamProfile NOTIFY stateChanged)
    Q_PROPERTY(QVariantList watchSlots READ watchSlots NOTIFY stateChanged)
    Q_PROPERTY(int slotCount READ slotCount NOTIFY stateChanged)
    Q_PROPERTY(int volume READ volume NOTIFY stateChanged)
    Q_PROPERTY(bool muted READ muted NOTIFY stateChanged)
    Q_PROPERTY(bool selfPreview READ selfPreview NOTIFY stateChanged)
    Q_PROPERTY(QString sharingLabel READ sharingLabel NOTIFY stateChanged)
    Q_PROPERTY(int shareFps READ shareFps NOTIFY stateChanged)
    Q_PROPERTY(QVariantList participants READ participants NOTIFY stateChanged)
public:
    PreviewBackend();
    ~PreviewBackend() override;
    QVariantList sources() const { return sources_; }
    QVariantList applications() const { return applications_; }
    bool active() const { return active_; }
    QString status() const { return status_; }
    double audioLevel() const { return audioLevel_; }
    int selectedSourceIndex() const { return selectedSourceIndex_; }
    bool roomReady()const{return room_.state()==goodsharing::RoomController::State::Ready;}
    bool roomBusy()const{return room_.state()!=goodsharing::RoomController::State::Idle&&room_.state()!=goodsharing::RoomController::State::Failed;}
    bool shareActive()const;
    QString roomStatus()const{return roomStatus_;}
    QString shareBadge()const;
    int selectedAudioMode()const{return selectedAudioMode_;}
    int selectedAudioApplication()const{return selectedAudioApplication_;}
    void setVideoItem(VideoItem* item) { video_=item; }
    void setRemoteVideoItem(VideoItem* item){remoteItem_=item;}
    void setExtraVideoItem(int slot,VideoItem* item){if(slot>=1&&slot<=2)extraItems_[slot-1]=item;}
    QVariantList remoteShares()const;
    bool watching()const{return watch_&&!watch_->closed()&&(!audioWatch_||!audioWatch_->closed());}
    bool watchBusy()const{return watch_||retiringWatch_||!retiringTracks_.empty()||pendingWatch_.has_value();}
    QString remoteStatus()const{return remoteStatus_;}
    bool remoteSourceMinimized()const{return remoteSourceMinimized_;}
    Q_INVOKABLE void watchShare(int index,int quality);
    Q_INVOKABLE void stopWatching();
    Q_INVOKABLE void addWatch(int index,int quality);   // next free gallery slot, up to three
    Q_INVOKABLE void stopWatchSlot(int slot);           // 0 = main picture, 1..2 = gallery tiles
    Q_INVOKABLE void focusSlot(int slot);               // move audio to a tile
    Q_INVOKABLE void setVolume(int percent);
    Q_INVOKABLE void toggleMute();
    Q_INVOKABLE void setSelfPreview(bool enabled);
    Q_INVOKABLE void setShareFps(int fps);
    int shareFps()const{return shareFps_;}
    QVariantList participants()const;
    Q_INVOKABLE void watchOwner(QString owner);
    bool selfPreview()const{return selfPreview_;}
    QString sharingLabel()const;
    int volume()const{return volume_;}
    bool muted()const{return muted_;}
    QVariantList watchSlots()const;
    int slotCount()const;
    QJsonObject viewingReport()const;
    void enableTestAvPlayback(){testAvPlayback_=true;Q_EMIT stateChanged();}
    bool testAvPlayback()const{return testAvPlayback_;}
    int watchedQuality()const{return watch_?(watch_->manifest().quality==goodsharing::ShareTrack::Video1080?1080:720):0;}
    int streamProfile()const{return static_cast<int>(profile_);}
    goodsharing::StreamTiming timing()const{return goodsharing::streamTiming(profile_);}
    Q_INVOKABLE void setStreamProfile(int profile);
    bool testAvReady()const{return watching()&&remoteAv_&&remoteAv_->readyForTest();}
    Q_INVOKABLE void refreshSources();
    Q_INVOKABLE void startPreview(int source,int audioMode,int audioApplication);
    Q_INVOKABLE void stopPreview();
    Q_INVOKABLE void joinRoom(QString url,QString token,QString keyHex);
    Q_INVOKABLE void leaveRoom();
    Q_INVOKABLE void startSharing(int source,int audioMode,int audioApplication);
    Q_INVOKABLE void stopSharing();
    bool startControlledSource(quintptr hwnd,unsigned pid);
    bool startControlledShare(quintptr hwnd,unsigned pid,bool audio);
    QJsonObject sharingReport()const;
    int capturedFrames() const;
Q_SIGNALS:
    void sourcesChanged();
    void stateChanged();
    void meterChanged();
    void remoteSharesChanged();
    void watchSelectionChanged();
private:
    struct Source { quintptr handle=0; unsigned pid=0; bool monitor=false; QString label; };
    std::vector<Source> nativeSources_,nativeApps_;
    QVariantList sources_,applications_;
    goodsharing::ScreenCapture capture_;
    goodsharing::AudioCapture audio_;
    goodsharing::RoomController room_;
    goodsharing::ShareModel remoteModel_;
    std::shared_ptr<goodsharing::RemoteShare> watch_,retiringWatch_;
    std::shared_ptr<goodsharing::RemoteVideo> remoteVideo_;
    std::shared_ptr<goodsharing::RemoteAvSession> remoteAv_;
    std::shared_ptr<goodsharing::RemoteShare> audioWatch_,candidateWatch_;
    std::shared_ptr<goodsharing::RemoteVideo> candidateFrames_;
    std::vector<std::shared_ptr<goodsharing::RemoteShare>> retiringTracks_;
    bool candidateDisplayed_=false;
    std::chrono::steady_clock::time_point candidateDeadline_;
    QJsonArray handoverHistory_;
    QJsonObject handoverRequest_;
    std::uint64_t committedHandovers_=0,cancelledHandovers_=0;
    bool testAvPlayback_=!qEnvironmentVariableIntValue("GOODSHARING_TEST_DISABLE_AV_PLAYBACK");
    struct Selection{QString owner,id;quint64 generation=0;int quality=720;};
    std::optional<Selection> pendingWatch_;
    std::optional<Selection> pendingQuality_;
    QString audioRetryKey_;
    int audioRetryAttempts_=0;
    const int audioRetryLimit_=qEnvironmentVariableIntValue("GOODSHARING_TEST_DISABLE_AUDIO_RETRY")?0:2;
    QString remoteStatus_="Choose a remote share to watch.";
    bool remoteSourceMinimized_=false,shareSourceMinimized_=false;
    goodsharing::StreamProfile profile_=goodsharing::StreamProfile::Balanced;
    QJsonObject lastViewingReport_;
    QJsonArray testCompletedWatches_;
    bool remoteReady_=false,remoteStalled_=false;
    int lastAdmittedFrames_=0;
    std::int64_t lastFrameAtUs_=0;
    VideoItem* remoteItem_=nullptr;
    void beginWatch(Selection selection);
    bool beginExtra(Selection selection);
    void stopExtra(int index);
    void reconcileExtras();
    struct ExtraWatch{Selection selection;std::shared_ptr<goodsharing::RemoteShare> watch;std::shared_ptr<goodsharing::RemoteVideo> frames;};
    std::vector<std::shared_ptr<ExtraWatch>> extras_;
    VideoItem* extraItems_[2]={nullptr,nullptr};
    bool galleryAutoAdd_=false;
    int volume_=100;
    bool muted_=false;
    bool selfPreview_=false;
    int shareFps_=30;
    unsigned shareLogTick_=0;
    std::chrono::steady_clock::time_point shareStartUs_{},audioFirstBlockUs_{};
    int shareBitrateOverride_=0;
    bool profileExplicit_=false;
    QTimer watchRetryTimer_;
    std::optional<Selection> retryWatch_;
    std::optional<Selection> currentSelection(const Selection& stale)const;
    int retryWatchAttempts_=0;
    QMap<QString,QString> participantNames_;
    void applyGain();
    unsigned galleryTick_=0;
    void pollHandover();
    void commitCandidate();
    bool cancelCandidate(QString reason);
    void stageQuality(Selection selection);
    void recordHandover(QJsonObject record);
    goodsharing::CapturePublisherInput sender_;
    std::shared_ptr<goodsharing::ShareInput> shareInput_;
    std::optional<goodsharing::CapturePublisherInput::Config> pendingShare_;
    QString roomStatus_="Not connected.";
    QJsonObject publicationReport_;
    bool captureSharing_=false;
    int selectedAudioMode_=0,selectedAudioApplication_=0;
    mutable std::mutex mutex_;
    QImage latest_;
    int frames_=0,presented_=0;
    double pendingLevel_=0,audioLevel_=0;
    bool active_=false;
    int selectedSourceIndex_=0;
    QString status_="Choose a window or display to preview.";
    VideoItem* video_=nullptr;
    QTimer timer_;
    unsigned generation_=0;
    void fail(unsigned generation,QString message);
};

