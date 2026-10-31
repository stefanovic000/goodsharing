#pragma once
#include "encryption_gate.h"
#include "encoded_video_output.h"
#include "share_model.h"
#include <QJsonObject>
#include <QJsonArray>
#include <livekit/audio_source.h>
#include <array>
#include <chrono>
#include <deque>
#include <optional>
#include <span>
#include <atomic>
#include <thread>
#include <condition_variable>
#include <livekit/room.h>

namespace goodsharing {
struct ShareVideoInput {
    FrameIdentity identity;
    std::array<std::vector<std::uint8_t>,2> nv12; // exact 1080p / 720p canvases
};
struct ShareAudioInput {std::uint64_t timestampUs=0;std::vector<std::int16_t> stereo;};

// Safe to keep in capture callbacks after Stop. A new share gets a new object.
// Captured input is admitted only with a ready gate. Raw replacement happens
// before encoding; encoded reference frames are never dropped here.
class ShareInput {
public:
    ShareInput(QString id,bool audio,std::shared_ptr<EncryptionGate> gate,int fps=60);
    bool video(ShareVideoInput frame);
    bool audio(ShareAudioInput block);
    void close();
    QJsonObject report()const;
    std::shared_ptr<EncryptionGate> gate()const{return gate_;}
    QString id()const{return id_;}
private:
    friend class SharePublisher;
    friend struct ShareInputTestPeer;
    const QString id_;
    const bool audio_;
    const std::shared_ptr<EncryptionGate> gate_;
    mutable std::mutex mutex_;
    std::condition_variable_any audioReady_;
    bool closed_=false;
    std::optional<ShareVideoInput> latest_;
    std::deque<ShareAudioInput> audioQueue_;
    // Incoming capture packets are re-chunked to exact 10 ms blocks.
    std::vector<std::int16_t> pendingSamples_;std::uint64_t pendingStartUs_=0;
    std::uint64_t lastVideo_=0,lastAudio_=0,replaced_=0,rejectedVideo_=0,rejectedAudio_=0;
    int fps_=60;
    std::uint64_t frameRateDrops_=0;
    std::size_t audioHighWater_=0;
    std::uint64_t overflowAudioDrops_=0,expiredAudioDrops_=0,firstAudioPressureUs_=0;
    void discardExpiredAudio(std::uint64_t now);
    std::optional<ShareVideoInput> takeVideo();
    std::optional<ShareAudioInput> waitAudio(std::stop_token stop);
};

// Transport-owner-only, with COM and Media Foundation initialized. Room must
// outlive this object. SDK delegates feed gate(), never this owner-thread object.
class SharePublisher {
public:
    enum class State { Starting,Sharing,Stopping,Stopped,Failed };
    struct Config {QString id;std::uint64_t generation=0;bool audio=true;std::chrono::milliseconds readinessTimeout{5000};std::int64_t maximumCaptureAgeUs=150000;int fps=60;int bitrate1080=10000000;int bitrate720=5000000;};
    struct Observers {
        std::function<void(int,const EncodedFrame&,FrameIdentity)> video;
        // Called after each successful publication. A throwing observer is a
        // supported fault-injection seam for verifying partial rollback.
        std::function<void(std::size_t)> published;
        // Bounded instrumentation/fault seam, on the video owner, before age checks.
        std::function<void(int,FrameIdentity)> beforeVideoHandoff;
        std::function<void(std::uint64_t)> beforeAudioHandoff;
        // Test seam after the application age check, modeling delay entering
        // the SDK. Permission is checked again after this hook.
        std::function<void(std::uint64_t)> beforeAudioCapture;
        std::function<void(std::uint64_t,std::uint64_t)> audioHandoff;
    };
    SharePublisher(livekit::Room& room,Config config,std::shared_ptr<ShareInput> input,Observers observers={});
    ~SharePublisher();
    void start();
    // Poll from an owner event loop. Optional per-track priming mask allows
    // readiness tests to withhold a handler; only internal black/silence is used.
    void tick(std::array<bool,3> primeTracks={true,true,true});
    void stop()noexcept;
    State state()const{return state_;}
    QString error()const{return error_;}
    std::shared_ptr<EncryptionGate> gate()const{return gate_;}
    QJsonObject report()const;
    // Tells viewers that the shared window is minimized or restored. Advisory and
    // bounded: a failure here never revokes the share.
    void publishSourceState(bool minimized);
private:
    livekit::Room& room_;
    Config config_;
    std::shared_ptr<ShareInput> input_;
    std::shared_ptr<EncryptionGate> gate_;
    Observers observers_;
    State state_=State::Stopped;
    bool started_=false;
    QString error_;
    std::vector<std::shared_ptr<livekit::Track>> tracks_;
    std::array<std::unique_ptr<EncodedVideoOutput>,2> outputs_;
    std::array<std::vector<std::uint8_t>,2> black_;
    std::shared_ptr<livekit::AudioSource> audio_;
    std::jthread audioWorker_;
    std::atomic_bool audioFailed_=false;
    std::chrono::steady_clock::time_point deadline_,nextPrimeVideo_{},nextPrimeAudio_{};
    std::uint64_t lastVideo_=0,expiredVideo_=0,submittedVideo_=0;
    std::atomic_uint64_t expiredAudio_=0,submittedAudio_=0;
    std::atomic_uint64_t nativeExpiredAudio_=0;
    QJsonArray finalOutputs_;
    mutable std::mutex failureMutex_;
    QJsonObject firstFailure_;
    void recordFailure(const char* operation,const char* code,std::uint64_t began,std::uint64_t capture,int height=0);
    bool allowed(bool priming)const;
    void fail(QString message);
    void runAudio(std::stop_token stop);
};
}

