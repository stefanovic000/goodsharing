#pragma once
#include "audio_playout.h"
#include "audio_playout_sink.h"
#include "remote_share.h"
#include "presentation_ticket.h"
#include "remote_media_selection.h"
#include <optional>

namespace goodsharing {
// One sink and audio watch for life. A split selection may replace video only.
// Sink outlives this object. pump/stop/destruction
// run on its owner; ingress is concurrent. Sink calls must be bounded and must
// not re-enter this object/RemoteShare. Clock is read after acquiring our mutex.
class RemoteAudioPlayout {
public:
    using LocalClock=std::function<std::int64_t()>;
    enum class Pump {Waiting,Reset,Committed,Closed,Failed};
    explicit RemoteAudioPlayout(AudioPlayoutSink& sink,LocalClock clock={},std::shared_ptr<RemoteMediaSelection> selection={});
    ~RemoteAudioPlayout();
    void bind(const std::shared_ptr<RemoteShare>& watch);
    void bindMedia(const std::shared_ptr<RemoteShare>& audio,const std::shared_ptr<RemoteShare>& video);
    struct CandidateTicket {PresentationTicket presentation;PlayoutClock clock;std::int64_t receivedUs=0;std::uint64_t attempt=0;};
    bool stageVideo(const std::shared_ptr<RemoteShare>& video);
    bool cancelVideo(const std::shared_ptr<RemoteShare>& video);
    std::optional<PresentationTicket> scheduleVideo(const std::shared_ptr<RemoteShare>& video,PlayoutClock clock);
    std::optional<PresentationTicket> scheduleVideo(const std::shared_ptr<RemoteShare>& video,livekit::VideoFrameEvent& frame);
    bool ticketValid(const std::shared_ptr<RemoteShare>& video,PresentationTicket ticket)const;
    std::optional<CandidateTicket> projectVideo(const std::shared_ptr<RemoteShare>& video,PlayoutClock clock);
    std::optional<CandidateTicket> projectVideo(const std::shared_ptr<RemoteShare>& video,livekit::VideoFrameEvent& frame);
    bool candidateValid(const std::shared_ptr<RemoteShare>& video,const CandidateTicket& ticket)const;
    bool adoptVideo(const std::shared_ptr<RemoteShare>& video,const CandidateTicket& ticket);
    bool presented(const std::shared_ptr<RemoteShare>& video,PresentationTicket ticket);
    // Avoids holding the scheduler mutex during paint. The final lease section
    // admits paint and PCM concurrently; revocation waits for earlier output.
    bool presentVideo(const std::shared_ptr<RemoteShare>& video,const std::function<void()>& operation)const;
    std::shared_ptr<RemoteShare> selectedVideo()const{return selection_->selected();}
    std::shared_ptr<const RemoteMediaSelection> mediaSelection()const{return selection_;}
    bool video(PlayoutClock clock);
    bool audio(PlayoutClock clock,std::span<const std::int16_t> pcm);
    bool video(livekit::VideoFrameEvent& frame);
    std::optional<PresentationTicket> scheduleVideo(livekit::VideoFrameEvent& frame);
    std::optional<PresentationTicket> scheduleVideo(PlayoutClock clock);
    void requirePresentation(); // before bind; controlled A/V integration
    void enableAudioContinuity(); // keep aligned audio through a video presentation stall
    // Stream profile applied before binding: target lead and video wait bound.
    void configureTiming(std::int64_t leadUs,std::int64_t videoWaitLimitUs);
    // Listener volume: applied to every committed sample, safe at any time.
    void setGain(float gain);
    void setLogger(std::function<void(const QString&)> logger);
    bool ticketValid(PresentationTicket ticket)const;
    bool presented(PresentationTicket ticket);
    bool hasTimeline()const;
    bool needsDevicePolling()const;
    bool audio(livekit::AudioFrameEvent& frame);
    Pump pump();
    void setVolume(float volume);
    void setMuted(bool muted);
    void stop();
    QJsonObject report(bool includeTestHistory=true)const;
private:
    AudioPlayoutSink& sink_;
    const LocalClock clock_;
    mutable std::mutex mutex_;
    std::weak_ptr<RemoteShare> watch_;
    const std::shared_ptr<RemoteMediaSelection> selection_;
    bool split_=false;
    std::uint64_t candidateAttempt_=0,videoHandovers_=0;
    AudioPlayout core_;
    std::array<std::int16_t,AudioPlayout::blockFrames*2> output_{};
    unsigned holdWindow_=0,holdsThisSecond_=0;std::int64_t holdBucketUs_=0;
    bool bound_=false,closed_=false,failed_=false,stopped_=false,muted_=false;
    float volume_=1;
    std::uint64_t denied_=0,commits_=0,deviceResets_=0,committedMedia_=0;
    double decodedAudioSignalAbs_=0,decodedAudioDifferenceAbs_=0;
    bool presentationRequired_=false;
    bool audioContinuity_=false;
    std::uint64_t presentedSerial_=0,presentedFrames_=0,presentationWaits_=0,presentationResets_=0;
    std::uint64_t presentationLeadWaits_=0,presentationAgeResets_=0,presentationLagResets_=0;
    std::uint64_t presentationPrefixCommits_=0;
    std::uint64_t advancedVideoPresentations_=0;
    std::int64_t presentedSender_=0,presentedLocal_=0,maximumAvLagUs_=0;
    std::uint64_t endpointGapResets_=0;
    std::uint64_t endpointTimelinePreserved_=0;
    std::int64_t firstMediaAudibleUs_=0,lastMediaEndUs_=0;
    std::int64_t anchorWaitStartUs_=0,audioStartedWithoutVideo_=0;
    std::int64_t horizonUs_=80000;
    // Delayed timeline (docs/70 step 2): the presentation delay D in microseconds,
    // 0 meaning the pre-model behaviour of measuring against the last painted
    // picture. The common clock already anchors sender time D after capture, so
    // senderAt(now) is the sender position that should be audible at now.
    std::int64_t bufferUs_=0;
    std::int64_t videoWaitLimitUs_=80000; // profile wait bound; age gates follow it
    std::optional<std::int64_t> delayedSender(std::int64_t now)const;
    void logGap(const char* reason,std::int64_t detailMs=0);
    std::function<void(const QString&)> logger_;
    std::int64_t lastGapLogUs_=0;
    std::int64_t startedWallUs_=0,firstAudibleWallUs_=0;
    std::array<std::int64_t,512> avLags_{};
    std::size_t avCount_=0,avCursor_=0;
    std::array<std::int64_t,512> audioLeads_{};std::size_t audioLeadCount_=0,audioLeadCursor_=0;
    struct Trace {int kind=0;std::int64_t now=0,sender=0,deadline=0,front=0;std::size_t queued=0,media=0,frames=0;std::uint64_t serial=0;};
    std::array<Trace,128> trace_{};std::size_t traceCursor_=0,traceCount_=0;const bool traceEnabled_=qEnvironmentVariableIntValue("GOODSHARING_TEST_AV_TRACE")!=0;
    struct ResetTrace {int kind=0;QString videoSid;std::int64_t now=0,sender=0,oldDeadline=0,videoArrival=0;
        std::uint32_t epoch=0,previousEpoch=0;std::uint64_t before=0,after=0,media=0;
        std::int64_t videoSender=0,videoDeadline=0,presented=0;
        std::uint32_t reports=0;bool hasRtp=false,anchored=false,locked=false;
        std::uint64_t rejectedBefore=0,headroomBefore=0,earlyBefore=0;bool alignmentRejected=false,headroomReset=false,earlyDropped=false;
        std::int64_t windowAudible=0,previousOutputEnd=0;bool outputPast=false,outputOverlaps=false;
        std::int64_t pumpGapUs=0,sinceCommitUs=0,prepareElapsedUs=0,endpointGapUs=0;bool deviceOnlyRecovery=false;};
    std::unique_ptr<std::array<ResetTrace,16>> resetTrace_;
    struct FirstFault {ResetTrace event;std::array<Trace,128> preceding;std::size_t count=0;};
    std::unique_ptr<FirstFault> firstFault_;
    std::size_t resetCursor_=0,resetCount_=0;
    std::int64_t previousPumpUs_=0,pumpGapUs_=0,lastCommitUs_=0;
    ResetTrace resetContext(int kind,PlayoutClock clock,std::int64_t now,const std::shared_ptr<RemoteShare>& video)const;
    void rememberReset(ResetTrace event);
    void trace(Trace event);
    bool videoLocked(PlayoutClock clock);
    bool admit(bool audio,const std::function<void()>& operation)const;
    bool validLocked(PresentationTicket ticket)const;
    PresentationTicket activeTicket(PlayoutClock clock)const;
    bool presentedLocked(PresentationTicket ticket);
    bool candidateValidLocked(const std::shared_ptr<RemoteShare>& video,const CandidateTicket& ticket)const;
    bool adoptLocked(const std::shared_ptr<RemoteShare>& video,const CandidateTicket& ticket);
    std::optional<CandidateTicket> projectLocked(const std::shared_ptr<RemoteShare>& video,PlayoutClock clock);
    void denied(const std::shared_ptr<RemoteShare>& watch);
    bool flush();
    Pump fail();
};
}
