#include "remote_video.h"
#include <algorithm>
#include <random>
#include <thread>
#include <cstdlib>
#include <QJsonArray>
namespace goodsharing {
namespace {
// Test-owned: proves at load time that the impairment variable reached this
// process, so a silent zero is distinguishable from an unused hook.
struct DropProbe {
    DropProbe(){const int percent=qEnvironmentVariableIntValue("GOODSHARING_TEST_DROP");if(percent>0)qWarning("impairment: dropping %d%% of arriving pictures",percent);}
};
const DropProbe dropProbe;
}
namespace {
unsigned sourceFrameId(const livekit::VideoFrameEvent& event,int width,int height) {
    const auto* bytes=event.frame.data();
    unsigned id=0;
    for(int bit=0;bit<16;bit++){
        const int x=(bit*40+20)*width/960,top=20*height/540,bottom=60*height/540;
        const auto first=bytes[4*(top*width+x)],second=bytes[4*(bottom*width+x)];
        if(first==second)return 0;
        if(first>128)id|=1u<<bit;
    }
    return id;
}
}
std::int64_t RemoteVideo::localUs()const{return localClock_?localClock_():std::chrono::duration_cast<std::chrono::microseconds>(Clock::now().time_since_epoch()).count();}
void RemoteVideo::setPresentationFloor(std::int64_t spacingUs){
    std::lock_guard lock(mutex_);
    if(bound_)throw std::logic_error("Presentation floor must precede binding");
    if(spacingUs<0||spacingUs>100000)throw std::invalid_argument("Invalid presentation floor");
    minimumPresentationSpacingUs_=spacingUs;
}
void RemoteVideo::setBuffer(std::int64_t bufferUs,std::int64_t waitLimitUs){
    std::lock_guard lock(mutex_);
    if(bound_)throw std::logic_error("Presentation buffer must precede binding");
    if(bufferUs<0||bufferUs>500000)throw std::invalid_argument("Invalid presentation buffer");
    // A picture needs the buffer plus one 60 Hz frame and a little slack before it
    // can be called stale; smaller values expire buffered pictures on arrival.
    lifetime=std::max(nominalLifetime,std::chrono::milliseconds(bufferUs/1000+50000/1000));
    // The queued span may cover the presentation delay plus one 60 Hz frame and a
    // little slack, because that is the only time a picture legitimately waits: it
    // arrives at most one buffer before its deadline. Sizing this from the wait
    // bound instead let 60 FPS queue seven pictures, which is more than a 55 ms
    // buffer can ever need. waitLimitUs is only checked for sanity.
    if(waitLimitUs<0||waitLimitUs>500000)throw std::invalid_argument("Invalid presentation buffer");
    windowUs_=std::max<std::int64_t>(50000,bufferUs+25000);
}
void RemoteVideo::setTiming(Timing timing){std::lock_guard lock(mutex_);if(bound_||!timing.schedule||!timing.valid||!timing.presented)throw std::logic_error("Invalid or late presentation timing binding");timing_=std::move(timing);}
void RemoteVideo::setFrameReady(std::function<void()> callback){std::lock_guard lock(mutex_);if(bound_||!callback)throw std::logic_error("Invalid or late frame-ready binding");frameReady_=std::move(callback);}
void RemoteVideo::popPending(){pending_[pendingHead_]={};pendingHead_=(pendingHead_+1)%pending_.size();pendingCount_--;}
void RemoteVideo::recordMotion(unsigned sourceFrameId,std::int64_t drawnUs,std::int64_t deadlineUs,std::uint64_t revision){
    if(!motionTraceEnabled_)return;
    if(!motionTrace_)motionTrace_=std::make_unique<std::array<MotionTrace,8192>>();
    if(motionTraceCount_<motionTrace_->size()){
        (*motionTrace_)[motionTraceCursor_++%motionTrace_->size()]={sourceFrameId,drawnUs,deadlineUs,revision};
        motionTraceCount_++;
    }else motionTraceOmitted_++;
}
bool RemoteVideo::admit(const std::shared_ptr<RemoteShare>& watch,const std::function<void()>& operation)const{
    return watch&&(timing_.permission?timing_.permission->presentVideo(watch,operation):watch->presentVideo(operation));
}
void RemoteVideo::setWarming(bool warming){std::lock_guard lock(mutex_);warming_=warming;
    if(warming){while(pendingCount_>1){pending_[(pendingHead_+pendingCount_-1)%pending_.size()]={};pendingCount_--;}swapTicket_.reset();}
}
bool RemoteVideo::finishWarmup(){
    std::lock_guard lock(mutex_);if(!timing_.valid||!admit(watch_.lock(),[]{}))return false;
    const auto now=localUs();
    for(std::size_t n=0;n<pendingCount_;n++){const auto& ticket=pending_[(pendingHead_+n)%pending_.size()].ticket;
        if(now-ticket.deadlineUs<=80000&&timing_.valid(ticket)){warming_=false;return true;}
    }return false;
}
bool RemoteVideo::readyToPresent()const{
    std::lock_guard lock(mutex_);if(!timing_.valid||!admit(watch_.lock(),[]{}))return false;
    if(!image_.isNull()&&shownTicket_&&timing_.valid(*shownTicket_))return true;
    const auto now=localUs();
    for(std::size_t n=0;n<pendingCount_;n++){const auto& ticket=pending_[(pendingHead_+n)%pending_.size()].ticket;
        if(ticket.deadlineUs<=now&&now-ticket.deadlineUs<=80000&&timing_.valid(ticket))return true;
    }return false;
}
void RemoteVideo::discardRevoked(){
    std::lock_guard lock(mutex_);if(auto watch=watch_.lock();watch&&!watch->closed())throw std::logic_error("Close video admission before releasing its selection");
    while(pendingCount_)popPending();image_={};shownTicket_.reset();swapTicket_.reset();visible_=false;dirty_=true;
}
void RemoteVideo::bind(const std::shared_ptr<RemoteShare>& watch) {
    std::lock_guard lock(mutex_);if(bound_||!watch)throw std::logic_error("Remote video is one selection only");watch_=watch;bound_=true;
}
void RemoteVideo::submit(livekit::VideoFrameEvent& event) {
    // Test-only receive impairment (GOODSHARING_TEST_DROP=percent): this share of
    // arriving pictures never reaches the handoff, like network loss.
    static const int dropPercent=[](){const char* v=std::getenv("GOODSHARING_TEST_DROP");return v?std::atoi(v):0;}();
    if(dropPercent>0){static thread_local std::mt19937 rng{std::random_device{}()};
        if(std::uniform_int_distribution<int>(1,100)(rng)<=dropPercent){++impairmentDrops_;return;}}
    const auto arrival=Clock::now();
    ++submittedFrames_;
    const auto ingressBegan=Clock::now();
    const auto identity=event.metadata&&event.metadata->user_data?decodeFrameIdentity(*event.metadata->user_data):std::nullopt;
    std::unique_lock lock(mutex_);const auto watch=watch_.lock();if(!watch){denied_++;return;}
    {const auto waited=std::chrono::duration_cast<std::chrono::microseconds>(Clock::now()-ingressBegan).count();
     lockWaitTotalUs_+=waited;lockWaitMaxUs_=std::max<std::int64_t>(lockWaitMaxUs_,waited);++lockWaitCount_;}
    const int height=watch->manifest().quality==ShareTrack::Video1080?1080:720,width=height==1080?1920:1280;
    if(!identity||event.rotation!=livekit::VideoRotation::VIDEO_ROTATION_0||event.frame.type()!=livekit::VideoBufferType::RGBA||event.frame.width()!=width||event.frame.height()!=height||event.frame.dataSize()!=static_cast<std::size_t>(width)*height*4){invalid_++;return;}
    if(identity->priming)return;
    if(timing_.schedule){
        auto ticket=timing_.schedule(event);
        bool untimed=false;
        if(!ticket&&qEnvironmentVariableIntValue("GOODSHARING_TEST_NO_UNTIMED")!=0){unclocked_++;return;}
        if(!ticket){
            // No audio anchor for this picture: present it at arrival time instead
            // of discarding a frame the viewer already received.
            unclocked_++;
            PresentationTicket fallback;
            fallback.deadlineUs=localUs()+10000;
            fallback.senderUs=fallback.deadlineUs;
            fallback.serial=pendingCount_?pending_[(pendingHead_+pendingCount_-1)%pending_.size()].ticket.serial+1:++untimedSerial_;
            fallback.receivedUs=arrival.time_since_epoch().count();
            ticket=fallback;untimed=true;
        }
        if(warming_){
            while(pendingCount_&&(!timing_.valid(pending_[pendingHead_].ticket)||localUs()-pending_[pendingHead_].ticket.deadlineUs>80000))popPending();
            if(pendingCount_){pendingDrops_++;return;}
        }
        // Clone borrowed pixels before taking the final presentation leases.
        // Only the bounded queue move is admitted; revocation during copying
        // discards this private clone without publishing it to the renderer.
        if(!admit(watch,[]{})){denied_++;return;}
        auto copy=QImage(event.frame.data(),width,height,width*4,QImage::Format_RGBA8888).copy();
        const auto sourceId=motionTraceEnabled_?sourceFrameId(event,width,height):0;
        bool notify=false;
        if(!admit(watch,[&]{
            // Bound the queue by the span it covers, not by each frame's offset:
            // the pictures already held are due sooner, so a newcomer beyond
            // the window would stretch the whole queue past what can be painted.
            if(!untimed&&pendingCount_&&ticket->deadlineUs-pending_[pendingHead_].ticket.deadlineUs>windowUs_){pendingDrops_++;return;}
            if(!untimed&&pendingCount_&&pending_[(pendingHead_+pendingCount_-1)%pending_.size()].ticket.serial!=ticket->serial)while(pendingCount_)popPending();
            if(pendingCount_==pending_.size()){
                // Keep a picture available on the next display deadline. Drop
                // an older due picture only when its successor is also due;
                // otherwise retain that head plus the newest successors. The
                // second slot is removed with bounded moves, without copying
                // pixels. Dropping a future head can invent a presentation hole.
                if(pending_[(pendingHead_+1)%pending_.size()].ticket.deadlineUs<=localUs())popPending();
                else{
                    for(std::size_t n=1;n+1<pendingCount_;n++)pending_[(pendingHead_+n)%pending_.size()]=std::move(pending_[(pendingHead_+n+1)%pending_.size()]);
                    pending_[(pendingHead_+pendingCount_-1)%pending_.size()]={};pendingCount_--;nextDuePreserved_++;
                }
                pendingDrops_++;
            }
            auto& next=pending_[(pendingHead_+pendingCount_)%pending_.size()];
            next={std::move(copy),*ticket,arrival,++revision_,sourceId,untimed};
            pendingCount_++;pendingHigh_=std::max(pendingHigh_,pendingCount_);if(warming_)warmHigh_=std::max(warmHigh_,pendingCount_);admitted_++;dirty_=true;notify=true;
        }))denied_++;
        const auto callback=frameReady_;
        lock.unlock();
        if(notify&&callback)callback();
        return;
    }
    if(!admit(watch,[]{})){denied_++;return;}
    auto copy=QImage(event.frame.data(),width,height,width*4,QImage::Format_RGBA8888).copy();
    const auto sourceId=motionTraceEnabled_?sourceFrameId(event,width,height):0;
    bool notify=false;
    if(!admit(watch,[&]{image_=std::move(copy);received_=arrival;admitted_++;revision_++;selectedSourceFrameId_=sourceId;dirty_=true;notify=true;}))denied_++;
    const auto callback=frameReady_;
    lock.unlock();
    if(notify&&callback)callback();
}
bool RemoteVideo::changed() {
    std::lock_guard lock(mutex_);const auto watch=watch_.lock();
    if(timing_.schedule){
        const auto now=localUs();
        while(pendingCount_&&(!watch||!admit(watch,[]{})||(!pending_[pendingHead_].untimed&&!timing_.valid(pending_[pendingHead_].ticket))||now-pending_[pendingHead_].ticket.deadlineUs>80000)){popPending();expired_++;dirty_=true;}
        if(!image_.isNull()&&(!watch||!admit(watch,[]{})||(!shownUntimed_&&(!shownTicket_||!timing_.valid(*shownTicket_))))){image_={};shownTicket_.reset();expired_++;dirty_=true;}
        // Degrade rather than freeze: once the picture has held for half a second the
        // A/V sync this drop policy protects is already lost, so present the newest
        // pending picture late instead of leaving a still frame on screen.
        static const std::int64_t freezeGraceUs=[](const char* v){return v?static_cast<std::int64_t>(std::atol(v))*1000:200000;} (std::getenv("GOODSHARING_TEST_FREEZE_GRACE_MS"));
        if(pendingCount_&&lastPresentationUs_&&now-lastPresentationUs_>freezeGraceUs&&watch&&admit(watch,[]{})){
            auto& next=pending_[pendingHead_];
            if(next.untimed||timing_.valid(next.ticket)){
                image_=std::move(next.image);shownTicket_=next.ticket;shownUntimed_=true;received_=next.received;
                selectedRevision_=next.revision;selectedSourceFrameId_=next.sourceFrameId;++latePresentations_;popPending();dirty_=true;}
        }
        const bool due=pendingCount_&&pending_[pendingHead_].ticket.deadlineUs<=now;
        const bool result=dirty_||due;dirty_=false;return result;
    }
    if(!image_.isNull()&&(!watch||!admit(watch,[]{}))){image_={};expired_++;dirty_=true;}
    const bool result=dirty_;dirty_=false;return result;
}
bool RemoteVideo::preparePaint(const QRectF& bounds,std::shared_ptr<RemoteShare>& watch,QImage& image,QRectF& target,bool& distinct,std::int64_t& selected) {
    std::lock_guard lock(mutex_);watch=watch_.lock();visible_=false;
    if(timing_.schedule){
        const auto now=localUs();
        while(pendingCount_&&pending_[pendingHead_].ticket.deadlineUs<=now){
            auto& next=pending_[pendingHead_];
            const bool spaced=!minimumPresentationSpacingUs_||!lastPresentationUs_||now-lastPresentationUs_>=minimumPresentationSpacingUs_;
            if(spaced&&now-next.ticket.deadlineUs<=80000&&(next.untimed||timing_.valid(next.ticket))){
                image_=std::move(next.image);shownTicket_=next.ticket;shownUntimed_=next.untimed;received_=next.received;selectedRevision_=next.revision;selectedSourceFrameId_=next.sourceFrameId;
            }else if(!spaced&&(next.untimed||timing_.valid(next.ticket)))cadenceCatchupDrops_++;
            else expired_++;
            popPending();
        }
        // Expired lease: keep showing the last picture, do not blank the screen.
        if(shownTicket_&&!shownUntimed_&&!timing_.valid(*shownTicket_)){shownTicket_.reset();}
    }
    if(image_.isNull()||!watch)return false;
    selected=timing_.schedule?selectedRevision_:revision_;
    // A stale picture is frozen, not blanked: clearing it turned every brief stall
    // into a black flash. Only a lost watch or a revoked lease removes it.
    if(drawnRevision_!=selected&&Clock::now()-received_>lifetime)expired_++;
    distinct=drawnRevision_!=selected;
    {
        const auto gapMs=lastPresentationUs_?(localUs()-lastPresentationUs_)/1000:0;
        if(!distinct&&lastPresentationUs_&&gapMs>150&&!frozenNotified_){
            frozenNotified_=true;++freezeCount_;freezeStartedUs_=localUs();
            if(logger_)logger_(QString("video freeze: no new picture for %1 ms (queued=%2)").arg(gapMs).arg(pendingCount_));
        }else if(distinct&&frozenNotified_){frozenNotified_=false;
            if(freezeStartedUs_){const auto frozen=localUs()-lastPresentationUs_;if(frozen>0){longestFreezeUs_=std::max(longestFreezeUs_,frozen);totalFrozenUs_+=frozen;}freezeStartedUs_=0;}
            if(logger_)logger_("video resumed after freeze");}
    }image=image_;
    QSizeF fitted=image_.size();fitted.scale(bounds.size(),Qt::KeepAspectRatio);
    target=QRectF(bounds.x()+(bounds.width()-fitted.width())/2,bounds.y()+(bounds.height()-fitted.height())/2,fitted.width(),fitted.height());
    return true;
}
void RemoteVideo::markPainted(bool distinct,std::int64_t selected) {
    if(!distinct)return;
    drawnRevision_=selected;drawn_++;
    // Only timed presentations are sampled: an arrival-paced picture after the source
    // stops is painted immediately and would drag the profile median down.
    if(timing_.schedule&&!shownUntimed_){const auto paintedUs=localUs();const auto arrivalUs=std::chrono::duration_cast<std::chrono::microseconds>(received_.time_since_epoch()).count();
     if(paintedUs>arrivalUs){const auto residence=paintedUs-arrivalUs;residence_[residenceCursor_++%residence_.size()]=residence;residenceCount_=std::min(residenceCount_+1,residence_.size());maximumResidenceUs_=std::max(maximumResidenceUs_,residence);}}
    lastPresentationUs_=localUs();
    if(!timing_.schedule){untimedSwapPending_=true;untimedSwapSourceFrameId_=selectedSourceFrameId_;untimedSwapRevision_=selected;}
    else if(!timing_.waitForSwap)recordMotion(selectedSourceFrameId_,localUs(),shownTicket_?shownTicket_->deadlineUs:0,selected);
}
bool RemoteVideo::draw(QPainter& painter,const QRectF& bounds) {
    std::shared_ptr<RemoteShare> watch;QImage image;QRectF target;bool distinct=false;std::int64_t selected=0;
    if(!preparePaint(bounds,watch,image,target,distinct,selected))return false;
    const bool allowed=admit(watch,[&]{
        const auto paintBegan=std::chrono::steady_clock::now();
        // Resampling is only worth its cost when shrinking a picture. Enlarging a
        // video frame by a few percent is visually identical with the fast path,
        // and that is where a weak viewer loses frames.
        const bool shrinking=target.width()<image.width()*0.98;
        painter.setRenderHint(QPainter::SmoothPixmapTransform,!fastPaint_&&shrinking);
        painter.drawImage(target,image);visible_=true;
        const auto paintUs=std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now()-paintBegan).count();
        paintMaxUs_=std::max<std::int64_t>(paintMaxUs_,paintUs);paintTotalUs_+=paintUs;++paintCount_;
        {std::lock_guard lock(mutex_);markPainted(distinct,selected);}
    });
    if(!allowed){std::lock_guard lock(mutex_);image_={};denied_++;}
    finishPresented(allowed&&distinct,selected);
    return allowed;
}
bool RemoteVideo::snapshot(const QRectF& bounds,QImage& image,QRectF& target) {
    std::shared_ptr<RemoteShare> watch;bool distinct=false;std::int64_t selected=0;
    if(!preparePaint(bounds,watch,image,target,distinct,selected))return false;
    const bool allowed=admit(watch,[&]{
        std::lock_guard lock(mutex_);
        markPainted(distinct,selected);
        visible_=true;
    });
    if(!allowed){std::lock_guard lock(mutex_);image_={};denied_++;return false;}
    if(distinct&&shownTicket_){
        std::lock_guard lock(presentMutex_);
        presentedPending_=(distinct&&!shownUntimed_)?std::optional<PresentationTicket>{*shownTicket_}:std::nullopt;
    }
    return true;
}
void RemoteVideo::setLogger(std::function<void(const QString&)> logger){std::lock_guard lock(mutex_);logger_=std::move(logger);}
void RemoteVideo::deliverPresented() {
    std::optional<PresentationTicket> ticket;
    {std::lock_guard lock(presentMutex_);ticket.swap(presentedPending_);}
    if(!ticket)return;
    if(!timing_.presented)return;
    const auto late=localUs()-ticket->deadlineUs;maxLateness_=std::max(maxLateness_,late);
    lateness_[latenessCursor_++%lateness_.size()]=late;latenessCount_=std::min(latenessCount_+1,lateness_.size());timedDraws_++;
    if(timing_.waitForSwap){swapTicket_=ticket;swapSourceFrameId_=selectedSourceFrameId_;swapRevision_=selectedRevision_;}
    else timing_.presented(*ticket);
}
void RemoteVideo::finishPresented(bool distinct,std::int64_t) {
    if(!distinct||!shownTicket_||!timing_.presented||shownUntimed_)return;
    const auto late=localUs()-shownTicket_->deadlineUs;maxLateness_=std::max(maxLateness_,late);
    lateness_[latenessCursor_++%lateness_.size()]=late;latenessCount_=std::min(latenessCount_+1,lateness_.size());timedDraws_++;
    if(timing_.waitForSwap){swapTicket_=shownTicket_;swapSourceFrameId_=selectedSourceFrameId_;swapRevision_=selectedRevision_;}
    else timing_.presented(*shownTicket_);
}
void RemoteVideo::frameQueued(){
    deliverPresented();
    std::lock_guard lock(mutex_);
    if(!swapTicket_){
        if(untimedSwapPending_){
            const auto now=localUs();recordMotion(untimedSwapSourceFrameId_,now,0,untimedSwapRevision_);
            untimedSwapPending_=false;untimedSwapSourceFrameId_=0;untimedSwapRevision_=0;
        }
        return;
    }
    const auto ticket=*swapTicket_;swapTicket_.reset();
    const auto sourceFrameId=swapSourceFrameId_;swapSourceFrameId_=0;
    const auto revision=swapRevision_;swapRevision_=0;
    const auto watch=watch_.lock();if(!watch||!timing_.valid(ticket)||!admit(watch,[]{}))return;
    const auto now=localUs();maxQueueLateness_=std::max(maxQueueLateness_,now-ticket.deadlineUs);queuedPresentations_++;
    recordMotion(sourceFrameId,now,ticket.deadlineUs,revision);timing_.presented(ticket);
}
QJsonObject RemoteVideo::report()const {
    std::lock_guard lock(mutex_);auto late=lateness_;std::sort(late.begin(),late.begin()+latenessCount_);
    std::array<std::int64_t,512> residence=residence_;std::sort(residence.begin(),residence.begin()+residenceCount_);
    const auto residencePct=[&](std::size_t pct){return residenceCount_?residence[(residenceCount_-1)*pct/100]/1000.0:0;};
    QJsonObject result{{"ingress_lock_wait_avg_ms",lockWaitCount_?double(lockWaitTotalUs_)/lockWaitCount_/1000.0:0.0},{"ingress_lock_wait_max_ms",lockWaitMaxUs_/1000.0},{"submitted_frames",static_cast<double>(submittedFrames_)},{"paint_residence_p50_ms",residencePct(50)},{"paint_residence_p95_ms",residencePct(95)},{"paint_residence_max_ms",maximumResidenceUs_/1000.0},{"paint_residence_samples",static_cast<double>(residenceCount_)},{"impairment_drops",static_cast<double>(impairmentDrops_)},{"pending_drops",static_cast<double>(pendingDrops_)},{"admitted_frames",static_cast<double>(admitted_)},{"invalid_frames",static_cast<double>(invalid_)},{"denied_frames_or_draws",static_cast<double>(denied_)},{"drawn_distinct_frames",static_cast<double>(drawn_)},{"expired_or_revoked_images",static_cast<double>(expired_)},{"pending_images",static_cast<int>(pendingCount_)+(image_.isNull()?0:1)},{"last_draw_visible",visible_},{"maximum_first_paint_residence_ms",static_cast<double>(lifetime.count())},{"freeze_count",static_cast<double>(freezeCount_)},{"late_presentations",static_cast<double>(latePresentations_)},{"longest_freeze_ms",static_cast<double>(std::max(longestFreezeUs_,freezeStartedUs_&&localUs()>lastPresentationUs_?localUs()-lastPresentationUs_:0))/1000.0},{"total_frozen_ms",static_cast<double>(totalFrozenUs_)/1000.0},{"held_image_without_update",!image_.isNull()&&Clock::now()-received_>lifetime},{"audio_playback_enabled",bool(timing_.schedule)},
        {"timed_presentation",bool(timing_.schedule)},{"scheduled_queue_high_water",static_cast<int>(pendingHigh_)},{"scheduled_queue_capacity",static_cast<int>(pending_.size())},
        {"warming",warming_},{"warm_queue_high_water",static_cast<int>(warmHigh_)},{"scheduled_drops",static_cast<double>(pendingDrops_)},{"next_due_preserved_on_overflow",static_cast<double>(nextDuePreserved_)},{"unclocked_frames",static_cast<double>(unclocked_)},{"timed_draws",static_cast<double>(timedDraws_)},
        {"wait_for_frame_queue",timing_.waitForSwap},{"queued_presentations",static_cast<double>(queuedPresentations_)},{"maximum_queue_lateness_ms",maxQueueLateness_/1000.0},
        {"maximum_draw_lateness_ms",maxLateness_/1000.0},{"recent_p95_draw_lateness_ms",latenessCount_?late[(latenessCount_-1)*95/100]/1000.0:0},{"maximum_paint_ms",paintMaxUs_/1000.0},{"average_paint_ms",paintCount_?static_cast<double>(paintTotalUs_)/paintCount_/1000.0:0.0}};
    result["minimum_presentation_spacing_ms"]=minimumPresentationSpacingUs_/1000.0;
    result["presentation_catchup_drops"]=static_cast<double>(cadenceCatchupDrops_);
    if(motionTraceEnabled_&&motionTrace_){
        QJsonArray trace;
        for(std::size_t n=0;n<motionTraceCount_;n++){
            const auto& item=(*motionTrace_)[(motionTraceCursor_+motionTrace_->size()-motionTraceCount_+n)%motionTrace_->size()];
            trace.append(QJsonObject{{"source_frame_id",static_cast<int>(item.sourceFrameId)},{"drawn_us",static_cast<double>(item.drawnUs)},
                {"deadline_us",static_cast<double>(item.deadlineUs)},{"revision",static_cast<double>(item.revision)}});
        }
        result["motion_trace"]=trace;result["motion_trace_capacity"]=static_cast<int>(motionTrace_->size());
        result["motion_trace_omitted"]=static_cast<double>(motionTraceOmitted_);
    }
    return result;
}
}
