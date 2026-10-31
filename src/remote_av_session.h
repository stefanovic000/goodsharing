#pragma once
#include "remote_video.h"
#include "remote_audio_playout.h"
#include <atomic>
#include <condition_variable>
#include <functional>
#include <thread>

namespace goodsharing {
// One fixed audio watch with staged video replacement. The endpoint/scheduler live on
// this session's worker. SDK ingress borrows the scheduler under mutex_.
class RemoteAvSession:public std::enable_shared_from_this<RemoteAvSession> {
public:
    RemoteAvSession();
    ~RemoteAvSession();
    void configure(const std::shared_ptr<RemoteVideo>& frames,const std::shared_ptr<RemoteShare>& video); // before frame bind
    void configureTiming(std::int64_t leadUs,std::int64_t videoWaitLimitUs); // before start
    void setLogger(std::function<void(const QString&)> logger); // before start
    void setGain(float gain); // applied now and on the next worker start
    void setAudioRestart(std::function<void()> callback); // before start; called off Qt
    void start(const std::shared_ptr<RemoteShare>& audio,const std::shared_ptr<RemoteShare>& video);
    bool stageVideo(const std::shared_ptr<RemoteShare>& video);
    bool cancelVideo(const std::shared_ptr<RemoteShare>& video);
    std::shared_ptr<RemoteShare> selectedVideo()const;
    bool workerReady()const;
    void audio(livekit::AudioFrameEvent& event);
    void stop(); // owner only, never an SDK/media callback
    QJsonObject report()const;
    bool readyForTest()const;
private:
    mutable std::mutex mutex_;
    std::condition_variable wake_;
    std::jthread worker_;
    RemoteAudioPlayout* playout_=nullptr;
    // Lock-free views for the per-frame video callbacks: taking mutex_ there
    // starves the pump that has a 5 ms reservation lease to meet.
    std::atomic<RemoteAudioPlayout*> playoutAtomic_{nullptr};
    std::atomic<bool> closedAtomic_{false};
    std::weak_ptr<RemoteShare> audioWatch_;
    const std::shared_ptr<RemoteMediaSelection> permission_=std::make_shared<RemoteMediaSelection>();
    const std::uint64_t sessionId_;
    unsigned workerStarts_=0;
    std::int64_t leadUs_=30000,videoWaitLimitUs_=80000;
    float gain_=1.0f;
    bool started_=false,closed_=false;
    std::uint64_t sequence_=0;
    std::int64_t maximumLiveSnapshotUs_=0;
    QJsonObject last_;
    std::optional<PresentationTicket> video(const std::shared_ptr<RemoteShare>& video,livekit::VideoFrameEvent& event);
    std::function<void()> audioRestart_;
    std::function<void(const QString&)> logger_;
    bool valid(const std::shared_ptr<RemoteShare>& video,PresentationTicket ticket)const;
    void presented(const std::shared_ptr<RemoteShare>& video,PresentationTicket ticket);
    void run(std::stop_token stop,std::shared_ptr<RemoteShare> audio,std::shared_ptr<RemoteShare> video);
};
}
