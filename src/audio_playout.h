#pragma once
#include <array>
#include <atomic>
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>
#include <optional>

namespace goodsharing {
// Metadata only: RTCP sender NTP is not encryption/presentation permission.
struct PlayoutClock {
    std::int64_t senderUs=0;
    std::uint32_t rtp=0,epoch=0,reports=0;
    bool hasRtp=false;
    bool valid()const;
};

// Single-owner scheduling core. The adapter serializes producer/render access.
// Local timestamps are receiver steady-clock microseconds; senderUs is separate.
class AudioPlayout {
public:
    static constexpr std::size_t blockFrames=480,channels=2,capacity=6;
    static constexpr std::int64_t maximumResidenceUs=60000,videoLifetimeUs=150000;
    static constexpr std::int64_t pcmWaitGuardUs=4000;
    static constexpr std::int64_t maximumCadenceCorrectionUs=20000;
    static constexpr std::int64_t maximumPresentationLeadUs=80000,maximumVideoAdvanceUs=20000;
    struct Stats {
        std::uint64_t admitted=0,unclocked=0,invalid=0,lateInput=0,overflowBlocks=0,
            expiredBlocks=0,lateFrames=0,renderedFrames=0,silentFrames=0,resets=0;
        std::size_t queued=0,highWater=0;
        bool anchored=false,closed=false,resetPending=true;
        std::uint64_t resetSerial=1;
        std::uint64_t alignments=0,alignmentRejected=0;
        std::uint64_t alignmentRejectResets=0;
        std::uint64_t lowHeadroomBlocks=0,sustainedHeadroomResets=0;
        std::uint64_t headroomRebases=0,audioAnchorAdvances=0;
        std::int64_t maximumHeadroomOffsetUs=0;
        std::uint64_t pcmArrivalWaits=0;
        std::uint64_t earlyVideoDrops=0;
        std::uint64_t boundedVideoAdvances=0;
        std::int64_t maximumVideoAdvanceUs=0,maximumPresentationLeadUs=0;
        std::uint64_t cadenceAdjustedBlocks=0,cadenceLimitResets=0,cadenceDiscontinuities=0;
        std::uint64_t cadenceLateDrops=0;
        std::int64_t maximumCadenceDropUs=0;
        std::uint64_t cadencePhaseGaps=0;
        std::int64_t maximumCadenceGapUs=0;
        std::uint64_t sourceHorizonSilentFrames=0;
        std::int64_t maximumCadenceCorrectionUs=0;
        bool alignmentLocked=false;
        std::int64_t maximumAlignedVideoLeadUs=0;
    };
    struct SourceWindow {std::int64_t minimum=0,maximum=0;};
    struct Render {bool ready=false,resetRequired=false;std::uint64_t resetSerial=0;std::size_t mediaFrames=0,outputFrames=0;
        std::int64_t firstSourceUs=0,lastSourceUs=0;bool sourceLeadWait=false,sourceLagReset=false;};
    bool video(PlayoutClock clock,std::int64_t nowUs,bool allowBoundedAdvance=false);
    // Candidate clocks cannot reanchor, consume PCM or alter the active epoch.
    std::optional<std::int64_t> projectVideo(PlayoutClock clock,std::int64_t receivedUs)const;
    bool adoptVideo(PlayoutClock clock,std::int64_t receivedUs,std::int64_t nowUs,std::int64_t deadlineUs,std::uint64_t serial);
    bool audio(PlayoutClock clock,std::span<const std::int16_t> pcm,std::int64_t nowUs);
    Render render(std::int64_t nowUs,std::int64_t firstAudibleUs,std::span<std::int16_t> pcm,std::optional<SourceWindow> sourceWindow={});
    bool acknowledgeReset(std::uint64_t serial);
    void resetTiming(){invalidate();}
    void enableAudioAlignment(){alignAudio_=true;}
    void enableAudioContinuity(){audioContinuity_=true;}
    // Stream profile: common audio/video target lead, how long a due frame may
    // wait before it is re-anchored, and the presentation delay they are derived
    // from. Must be set before the first frame.
    void configureTiming(std::int64_t leadUs,std::int64_t videoWaitLimitUs,std::int64_t bufferUs=0);
    // Output gain applied to committed samples; 1.0 is unity, 0.0 is mute.
    void setGain(float gain){gain_=std::clamp(gain,0.0f,1.0f);}
    float gain()const{return gain_;}
    std::int64_t configuredLeadUs()const{return leadUs_;}
    std::int64_t configuredVideoWaitLimitUs()const{return videoWaitLimitUs_;}
    // How long a decoded block may wait for its deadline. Follows the buffer so a
    // larger presentation delay cannot expire its own backlog (docs/70 step 3).
    std::int64_t configuredResidenceUs()const{return residenceUs_;}
    std::int64_t configuredVideoLifetimeUs()const{return videoLifetimeValueUs_;}
    std::int64_t configuredPresentationLeadLimitUs()const{return presentationLeadLimitUs_;}
    // Only after a successful final-permission device commit containing media.
    void mediaCommitted(){if(alignAudio_&&stats_.anchored)stats_.alignmentLocked=true;}
    void revoke(); // terminal; a new watch requires a new object
    Stats stats()const{return stats_;}
    PlayoutClock lastVideoClock()const{return lastVideo_;}
    PlayoutClock lastAudioClock()const{return lastAudio_;}
    std::optional<std::int64_t> audioDeadline()const{return audioTailClock_.valid()?std::optional{audioTailDeadline_}:std::nullopt;}
    std::int64_t lastVideoArrival()const{return lastVideoLocal_;}
    std::int64_t lastOutputEnd()const{return lastOutputEnd_;}
    std::optional<std::int64_t> videoDeadline()const{return stats_.anchored?std::optional{anchorLocal_+(lastVideo_.senderUs-anchorSender_)}:std::nullopt;}
    // Advance only the picture's presentation; never move the common PCM clock.
    std::optional<std::int64_t> videoPresentationDeadline()const{const auto mapped=videoDeadline();return mapped?std::optional{alignAudio_&&stats_.alignmentLocked?std::min(*mapped,lastVideoLocal_+videoWaitLimitUs_):*mapped}:std::nullopt;}
    std::optional<std::int64_t> senderAt(std::int64_t localUs)const{return stats_.anchored&&localUs>0&&localUs<=1000000000000000ll?std::optional{anchorSender_+(localUs-anchorLocal_)}:std::nullopt;}
    std::optional<std::int64_t> localFor(std::int64_t senderUs)const{if(senderUs<=0||senderUs>5000000000000000ll)return {};const auto delta=senderUs-anchorSender_;return stats_.anchored&&delta>=-2000000&&delta<=2000000?std::optional{anchorLocal_+delta}:std::nullopt;}
    std::optional<std::int64_t> frontDeadline()const{if(!stats_.anchored||!stats_.queued)return {};const auto& b=blocks_[head_];return b.deadlineUs+static_cast<std::int64_t>(b.used)*1000000/48000;}
private:
    struct Block {std::array<std::int16_t,blockFrames*channels> pcm{};std::int64_t senderUs=0,receivedUs=0,deadlineUs=0,sourceDurationUs=10000;std::size_t used=0;};
    std::array<Block,capacity> blocks_{};
    Stats stats_;
    std::size_t head_=0;
    PlayoutClock lastVideo_,lastAudio_;
    std::int64_t anchorSender_=0,anchorLocal_=0,lastVideoLocal_=0,lastNow_=0,lastOutputEnd_=0;
    PlayoutClock audioTailClock_;
    std::int64_t audioTailDeadline_=0,audioTailArrival_=0;
    std::int64_t audioTimelineOffsetUs_=0;
    bool alignAudio_=false;
    bool audioContinuity_=false;
    std::int64_t leadUs_=30000,videoWaitLimitUs_=80000;
    // Both bounds are the shipped floors raised by the configured buffer: a picture
    // cannot survive longer than the presentation delay, and a ticket cannot be
    // trusted further ahead than that delay either.
    std::int64_t videoLifetimeValueUs_=videoLifetimeUs,presentationLeadLimitUs_=maximumPresentationLeadUs;
    std::int64_t residenceUs_=maximumResidenceUs;
    float gain_=1.0f;
    unsigned consecutiveLate_=0;
    unsigned consecutiveAlignmentRejected_=0;
    bool localTime(std::int64_t now);
    void invalidate();
    void pop();
    void resetDevice();
    void clearCadence(){audioTailClock_={};audioTailDeadline_=audioTailArrival_=0;}
};
}


