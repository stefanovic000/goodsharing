#pragma once
#include "remote_share.h"
#include "frame_metadata_codec.h"
#include "presentation_ticket.h"
#include "remote_media_selection.h"
#include <QImage>
#include <QPainter>
#include <chrono>
#include <mutex>
#include <array>

namespace goodsharing {
// Reader -> Qt paint handoff, one image, one immutable watch generation.
// Local first-paint residence age is not a cross-machine latency measurement.
// An already-presented static image can be held while its permission stays valid.
class RemoteVideo {
public:
    using LocalClock=std::function<std::int64_t()>;
    explicit RemoteVideo(LocalClock clock={}):localClock_(std::move(clock)){}
    struct Timing {
        std::function<std::optional<PresentationTicket>(livekit::VideoFrameEvent&)> schedule;
        std::function<bool(PresentationTicket)> valid;
        std::function<void(PresentationTicket)> presented;
        bool waitForSwap=false;
        std::shared_ptr<const RemoteMediaSelection> permission;
    };
    void setTiming(Timing timing);
    // Stream profile: minimum spacing between presented pictures, before binding.
    void setPresentationFloor(std::int64_t spacingUs); // before bind; absent means video-only
    // Presentation delay this handoff must tolerate (docs/70 step 3): keeps the
    // 150 ms staleness budget as the floor and lets a larger buffer hold a picture
    // long enough to be painted instead of expiring it on arrival.
    void setBuffer(std::int64_t bufferUs,std::int64_t waitLimitUs); // before bind
    void setFrameReady(std::function<void()> callback); // before bind; called off Qt
    void bind(const std::shared_ptr<RemoteShare>& watch);
    void submit(livekit::VideoFrameEvent& event);
    bool changed(); // Qt owner: includes expiry/revocation of a held image
    bool draw(QPainter& painter,const QRectF& bounds);
    // Same picture selection without painting, for a GPU paint node.
    bool snapshot(const QRectF& bounds,QImage& image,QRectF& target);
    // Render thread hands the presented ticket over; the GUI thread delivers it.
    void deliverPresented();
    void setLogger(std::function<void(const QString&)> logger);
    void frameQueued(); // Qt frameSwapped: queued for presentation, not scanout
    void setWarming(bool warming);
    bool finishWarmup(); // keep successors once the first clocked image exists
    bool readyToPresent()const;
    void discardRevoked(); // release image storage after closing this watch
    QJsonObject report()const;
private:
    using Clock=std::chrono::steady_clock;
    mutable std::mutex mutex_;
    std::mutex presentMutex_;std::optional<PresentationTicket> presentedPending_;
    std::weak_ptr<RemoteShare> watch_;
    QImage image_;
    Clock::time_point received_{};
    bool bound_=false,dirty_=false,visible_=false;
    std::uint64_t admitted_=0,invalid_=0,denied_=0,drawn_=0,expired_=0;
    std::uint64_t revision_=0,drawnRevision_=0;
    std::uint64_t selectedRevision_=0;
    Timing timing_;
    std::function<void()> frameReady_;
    bool warming_=false;std::size_t warmHigh_=0;
    struct Pending{QImage image;PresentationTicket ticket;Clock::time_point received{};std::uint64_t revision=0;unsigned sourceFrameId=0;bool untimed=false;};
    std::array<Pending,24> pending_{}; // 24 slots cover a 300 ms buffer at 60 FPS
    std::size_t pendingHead_=0,pendingCount_=0,pendingHigh_=0;
    std::optional<PresentationTicket> shownTicket_;
    std::uint64_t pendingDrops_=0,nextDuePreserved_=0,unclocked_=0,timedDraws_=0;
    std::uint64_t cadenceCatchupDrops_=0;
    std::int64_t lastPresentationUs_=0;
    std::array<std::int64_t,512> lateness_{};std::size_t latenessCount_=0,latenessCursor_=0;
    std::int64_t maxLateness_=0;
    bool fastPaint_=qEnvironmentVariableIntValue("GOODSHARING_TEST_FAST_PAINT")!=0;
    std::uint64_t submittedFrames_=0,untimedSerial_=0,lockWaitCount_=0,impairmentDrops_=0;
    std::function<void(const QString&)> logger_;
    bool frozenNotified_=false;
    std::int64_t freezeCount_=0;std::int64_t lockWaitTotalUs_=0,lockWaitMaxUs_=0;
    bool shownUntimed_=false;
    std::int64_t paintMaxUs_=0,paintTotalUs_=0,paintCount_=0;
    std::optional<PresentationTicket> swapTicket_;
    unsigned swapSourceFrameId_=0;
    std::uint64_t swapRevision_=0;
    bool untimedSwapPending_=false;
    unsigned untimedSwapSourceFrameId_=0;
    std::uint64_t untimedSwapRevision_=0;
    std::uint64_t queuedPresentations_=0;
    std::int64_t maxQueueLateness_=0;
    unsigned selectedSourceFrameId_=0;
    struct MotionTrace {unsigned sourceFrameId=0;std::int64_t drawnUs=0,deadlineUs=0;std::uint64_t revision=0;};
    std::unique_ptr<std::array<MotionTrace,8192>> motionTrace_;
    std::size_t motionTraceCursor_=0,motionTraceCount_=0,motionTraceOmitted_=0;
    const bool motionTraceEnabled_=qEnvironmentVariableIntValue("GOODSHARING_TEST_AV_TRACE")!=0;
    const LocalClock localClock_;
    std::int64_t localUs()const;
    void popPending();
    bool preparePaint(const QRectF& bounds,std::shared_ptr<RemoteShare>& watch,QImage& image,QRectF& target,bool& distinct,std::int64_t& selected);
    void markPainted(bool distinct,std::int64_t selected);
    void finishPresented(bool distinct,std::int64_t selected);
    void recordMotion(unsigned sourceFrameId,std::int64_t drawnUs,std::int64_t deadlineUs,std::uint64_t revision);
    bool admit(const std::shared_ptr<RemoteShare>& watch,const std::function<void()>& operation)const;
    static constexpr auto nominalLifetime=std::chrono::milliseconds(150);
    std::chrono::milliseconds lifetime=nominalLifetime; // setBuffer raises it with the buffer
    std::int64_t windowUs_=130000; // presentation window the queued span may cover
    static constexpr std::int64_t nominalMinimumPresentationSpacingUs=10000;
    std::int64_t minimumPresentationSpacingUs_=qEnvironmentVariableIntValue("GOODSHARING_TEST_DISABLE_PRESENTATION_FLOOR")?0:nominalMinimumPresentationSpacingUs;
};
}
