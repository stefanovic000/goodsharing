#include "preview_backend.h"
#include <QPainter>
#include <QMutexLocker>
#include <windows.h>
#include <dwmapi.h>
#include <cmath>
#include <algorithm>
#include <QRegularExpression>
#include <QSettings>
#include <QDateTime>
#include <QDir>
#include <QFile>
static void logWatch(const QString& line);
// Portable installs keep their settings next to the executable; a read-only
// location falls back to the per-user registry store.
static QSettings appSettings() {
    const QDir dir(QCoreApplication::applicationDirPath());
    if(QFileInfo(dir.absolutePath()).isWritable())
        return QSettings(dir.filePath("GoodSharing.ini"),QSettings::IniFormat);
    return QSettings();
}

void VideoItem::setFrame(QImage image) { { QMutexLocker lock(&mutex_); image_=std::move(image); } update(); }
void VideoItem::setRemote(std::shared_ptr<goodsharing::RemoteVideo> remote){{QMutexLocker lock(&mutex_);remote_=std::move(remote);image_={};}update();}
void VideoItem::frameQueued(){QMutexLocker lock(&mutex_);if(remote_)remote_->frameQueued();}
void VideoItem::paint(QPainter* painter) {
    QMutexLocker lock(&mutex_); painter->fillRect(boundingRect(),QColor("#0a0e15"));
    if(remote_){remote_->draw(*painter,boundingRect());return;}
    if(image_.isNull()) return;
    QSizeF fitted=image_.size(); fitted.scale(boundingRect().size(),Qt::KeepAspectRatio);
    const QRectF target((width()-fitted.width())/2,(height()-fitted.height())/2,fitted.width(),fitted.height());
    painter->setRenderHint(QPainter::SmoothPixmapTransform,target.width()<image_.width()*0.98); painter->drawImage(target,image_);
}
PreviewBackend::PreviewBackend() {
    // Local latency/smoothness preference: an explicit test override wins over
    // the persisted UI choice.
    const auto profileOverride=qEnvironmentVariable("GOODSHARING_STREAM_PROFILE");
    if(!profileOverride.isEmpty()){
        try{profile_=goodsharing::parseStreamProfile(profileOverride.toUtf8().constData());}catch(const std::invalid_argument&){profile_=goodsharing::StreamProfile::Balanced;}
    }else{
        profile_=static_cast<goodsharing::StreamProfile>(std::clamp(appSettings().value("streamProfile",1).toInt(),0,2));
    }
    // These settings apply in every mode, including test runs with a profile override.
    profileExplicit_=appSettings().value("streamProfileExplicit",false).toBool();
    volume_=std::clamp(appSettings().value("volume",100).toInt(),0,100);
    if(const auto forced=qEnvironmentVariableIntValue("GOODSHARING_TEST_VOLUME");forced>0)volume_=std::clamp(forced,0,100);
    muted_=volume_==0;
    selfPreview_=appSettings().value("selfPreview",false).toBool();
    shareFps_=appSettings().value("shareFps",30).toInt()==60?60:30;
    if(const auto forced=qEnvironmentVariableIntValue("GOODSHARING_TEST_SHARE_FPS");forced==30||forced==60)shareFps_=forced;
    // Persist the effective defaults so a portable folder carries its settings
    // from the first run (and users get a visible GoodSharing.ini to edit).
    {
        auto settings=appSettings();
        settings.setValue("streamProfile",static_cast<int>(profile_));
        settings.setValue("volume",volume_);
        settings.setValue("selfPreview",selfPreview_);
        settings.setValue("shareFps",shareFps_);
    }
    watchRetryTimer_.setSingleShot(true);
    connect(&watchRetryTimer_,&QTimer::timeout,this,[this]{
        if(watching()||!retryWatch_||!roomReady())return;
        const auto pending=*retryWatch_;retryWatch_.reset();
        beginWatch(currentSelection(pending).value_or(pending));
    });
    connect(&room_,&goodsharing::RoomController::stateChanged,this,[this](auto state,QString message){
        using State=goodsharing::RoomController::State;
        roomStatus_=!message.isEmpty()?message:state==State::Ready?"Connected Â· ready to start a share":state==State::Joining?"Connectingâ€¦":state==State::Leaving?"Leavingâ€¦":"Not connected.";
        if(state==State::Failed&&message.contains("duplicate",Qt::CaseInsensitive))
            roomStatus_="This identity is still connected to the room; wait about 15 seconds and join again.";
        if(state==State::Failed||state==State::Idle||state==State::Reconnecting){stopSharing();stopWatching();remoteModel_.clear();}
        Q_EMIT stateChanged();
    });
    connect(&room_,&goodsharing::RoomController::participantJoined,this,[this](QString identity,QString name){
        participantNames_[identity]=name.isEmpty()?identity:name;
        Q_EMIT stateChanged();
    });
    connect(&room_,&goodsharing::RoomController::ownerRemoved,this,[this](QString identity){
        participantNames_.remove(identity);
        Q_EMIT stateChanged();
    });
    connect(&room_,&goodsharing::RoomController::mediaInvalidated,this,&PreviewBackend::stopSharing);
    connect(&room_,&goodsharing::RoomController::mediaInvalidated,this,[this]{stopWatching();remoteModel_.clear();});
    connect(&room_,&goodsharing::RoomController::trackAvailable,this,[this](QString owner,QString sid,QString name,bool audio,bool encrypted,bool screen,quint32 w,quint32 h){remoteModel_.announce({owner,sid,name,audio,encrypted,screen,w,h});});
    connect(&room_,&goodsharing::RoomController::trackRemoved,&remoteModel_,&goodsharing::ShareModel::remove);
    connect(&room_,&goodsharing::RoomController::ownerRemoved,&remoteModel_,&goodsharing::ShareModel::removeOwner);
    connect(&remoteModel_,&QAbstractItemModel::dataChanged,this,&PreviewBackend::remoteSharesChanged);
    connect(&remoteModel_,&QAbstractItemModel::rowsInserted,this,&PreviewBackend::remoteSharesChanged);
    connect(&remoteModel_,&QAbstractItemModel::rowsRemoved,this,&PreviewBackend::remoteSharesChanged);
    connect(&remoteModel_,&QAbstractItemModel::modelReset,this,&PreviewBackend::remoteSharesChanged);
    connect(&room_,&goodsharing::RoomController::sourceStateChanged,this,[this](QString owner,QString shareId,bool minimized){
        if(!watch_||watch_->closed())return;
        const auto& manifest=watch_->manifest();
        if(manifest.owner!=owner||manifest.id!=shareId)return;
        remoteSourceMinimized_=minimized;
        remoteStatus_=minimized?"User minimized app Â· waiting for it to be restored"
            :testAvPlayback_?"Watching encrypted audio/video Â· development test":"Watching encrypted video Â· sound playback pending synchronization";
        Q_EMIT stateChanged();
    });    connect(&room_,&goodsharing::RoomController::shareDiagnostics,this,[this](QJsonObject report){publicationReport_=std::move(report);});
    if(qEnvironmentVariableIsSet("GOODSHARING_TEST_AV_TRACE"))connect(&room_,&goodsharing::RoomController::watchStopped,this,[this](QJsonObject report){
        if(testCompletedWatches_.size()<16)testCompletedWatches_.append(std::move(report));
    });
    connect(&room_,&goodsharing::RoomController::shareStateChanged,this,[this](int state,QString message,quint64){
        using State=goodsharing::SharePublisher::State;
        if(state==static_cast<int>(State::Sharing)&&pendingShare_&&shareInput_){
            try {
                sender_.start(*pendingShare_,shareInput_,
                    [this](goodsharing::ScreenFrame frame){std::lock_guard lock(mutex_);latest_=std::move(frame.image);frames_++;},
                    [this](double rms){std::lock_guard lock(mutex_);pendingLevel_=std::min(1.,rms*8);
                if(audioFirstBlockUs_==std::chrono::steady_clock::time_point{}){audioFirstBlockUs_=std::chrono::steady_clock::now();
                    logWatch(QString("audio: first block %1 ms after share start").arg(
                        std::chrono::duration_cast<std::chrono::milliseconds>(audioFirstBlockUs_-shareStartUs_).count()));}},
                    [this](bool minimized){
                        // Capture thread: hand the transition to the Qt owner.
                        QMetaObject::invokeMethod(this,[this,minimized]{
                            shareSourceMinimized_=minimized;
                            if(captureSharing_){
                                room_.publishShareSourceState(minimized);
                                status_=minimized?"Sharing Â· source window minimized; viewers are waiting":"Sharing Â· encrypted 1080p + 720p";
                            }
                            Q_EMIT stateChanged();
                        },Qt::QueuedConnection);
                    });
                audioFirstBlockUs_={};shareStartUs_=std::chrono::steady_clock::now();
            captureSharing_=true;active_=true;status_="Sharing Â· encrypted 1080p + 720p";
            logWatch(QString("share started audio=%1 previewFrames=%2").arg(selectedAudioMode_).arg(capturedFrames()));
            }catch(const std::exception&){stopSharing();status_="Capture could not start; share stopped.";}
            pendingShare_.reset();
        }else if(state==static_cast<int>(State::Starting))status_="Preparing encrypted video and audioâ€¦";
        else if(state==static_cast<int>(State::Stopped)||state==static_cast<int>(State::Failed)){
            logWatch(QString("share stopped previewFrames=%1 state=%2").arg(capturedFrames()).arg(state));
            sender_.stop();captureSharing_=false;shareInput_.reset();pendingShare_.reset();active_=false;
            {std::lock_guard lock(mutex_);latest_={};pendingLevel_=audioLevel_=0;}if(video_)video_->setFrame({});
            status_=message.isEmpty()?"Share stopped.":message;
        }
        Q_EMIT stateChanged();
    });
    galleryAutoAdd_=qEnvironmentVariableIntValue("GOODSHARING_TEST_GALLERY")!=0;
    refreshSources(); timer_.setInterval(8); timer_.setTimerType(Qt::PreciseTimer);
    connect(&timer_,&QTimer::timeout,this,[this] {
        if(watch_&&(watch_->closed()||(audioWatch_&&audioWatch_->closed()))){stopWatching();remoteStatus_="Remote share ended or encryption permission was revoked.";Q_EMIT stateChanged();}
        std::erase_if(retiringTracks_,[](const auto& track){return track->stopped();});
        if(retiringWatch_&&retiringWatch_->stopped()){retiringWatch_.reset();Q_EMIT stateChanged();}
        if(!watch_&&!retiringWatch_&&retiringTracks_.empty()&&pendingWatch_&&roomReady()){const auto next=*pendingWatch_;pendingWatch_.reset();beginWatch(next);Q_EMIT stateChanged();}
        reconcileExtras();
        pollHandover();
        // Test hook: fill the gallery from every announced share.
        if(galleryAutoAdd_&&roomReady()&&(++galleryTick_%30)==0&&!pendingWatch_&&!candidateWatch_){
            const auto choices=remoteShares();
            for(int i=0;i<choices.size()&&slotCount()<3;i++){
                const auto choice=choices[i].toMap();
                const auto owner=choice["owner"].toString(),id=choice["id"].toString();
                bool seen=false;
                if(watch_&&watch_->manifest().owner==owner&&watch_->manifest().id==id)seen=true;
                for(const auto& extra:extras_)if(extra->selection.owner==owner&&extra->selection.id==id)seen=true;
                if(seen)continue;
                if(!watching()&&!watchBusy())watchShare(i,720);
                else addWatch(i,720);
            }
        }
        if(remoteVideo_){
            if(remoteVideo_->changed()&&remoteItem_&&!candidateDisplayed_)remoteItem_->update();const auto diagnostics=remoteVideo_->report();
            // A stall is "no admitted frame for 1.5 s", not "no frame since the
            // last 16 ms tick": the tick is faster than the picture and the
            // instantaneous flag made the status flicker every few seconds.
            const auto admitted=diagnostics["admitted_frames"].toInt();
            const auto nowUs=static_cast<std::int64_t>(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
            if(admitted>lastAdmittedFrames_){lastAdmittedFrames_=admitted;lastFrameAtUs_=nowUs;}
            const bool stalled=admitted>0&&lastFrameAtUs_>0&&nowUs-lastFrameAtUs_>1500000;
            if(!candidateWatch_&&admitted>0&&(!remoteReady_||stalled!=remoteStalled_)){
                remoteReady_=true;remoteStalled_=stalled;
                // A minimized source explains the frozen picture; do not replace
                // that notice with the generic stall text.
                if(!remoteSourceMinimized_)remoteStatus_=stalled?"No new video frames Â· picture may be static or stalled":testAvPlayback_?"Watching encrypted audio/video Â· development test":"Watching encrypted video Â· sound playback pending synchronization";Q_EMIT stateChanged();
            }
        }
        if(captureSharing_&&(++shareLogTick_%125)==0)room_.refreshShareDiagnostics();
        if(captureSharing_&&(shareLogTick_%625)==0){
            const auto c=sender_.report();
            int keyframes=0,encoded=0,handed=0;
            for(const auto& entry:publicationReport_.value("outputs").toArray()){
                const auto output=entry.toObject();
                keyframes+=output.value("keyframes_handed_off").toInt();
                encoded+=output.value("frames_encoded").toInt();
                handed+=output.value("frames_handed_off").toInt();
            }
            QJsonObject viewer;
            if(remoteVideo_)viewer=remoteVideo_->report();
            logWatch(QString("sender 5s: captured=%1 encoded=%2 handed=%3 replaced=%4 rateLimited=%5 stale=%6 keyframes=%7 | viewer catchup=%8 schedDrops=%9 p95=%10ms")
                .arg(c.value("captured_frames").toDouble()).arg(encoded).arg(handed)
                .arg(c.value("raw_replaced").toDouble()).arg(c.value("rate_limited_raw_frames").toDouble())
                .arg(c.value("stale_or_rejected_video").toDouble()).arg(keyframes)
                .arg(viewer.value("presentation_catchup_drops").toDouble())
                .arg(viewer.value("scheduled_drops").toDouble())
                .arg(viewer.value("recent_p95_draw_lateness_ms").toDouble()));
        }
        if(captureSharing_){const auto failure=sender_.error();if(!failure.isEmpty()){stopSharing();status_=failure;Q_EMIT stateChanged();}}
        QImage image;
        { std::lock_guard lock(mutex_); if(frames_!=presented_) { image=latest_; presented_=frames_; } audioLevel_=pendingLevel_; }
        if(video_&&!image.isNull()) video_->setFrame(std::move(image));
        Q_EMIT meterChanged();
    }); timer_.start();
}
PreviewBackend::~PreviewBackend() { stopWatching();stopSharing();stopPreview();room_.leave(); }
void PreviewBackend::refreshSources() {
    if(active_||shareActive()) return;
    nativeSources_.clear(); nativeApps_.clear(); sources_.clear(); applications_.clear();
    selectedSourceIndex_=0;
    EnumDisplayMonitors(nullptr,nullptr,[](HMONITOR monitor,HDC,LPRECT rect,LPARAM data)->BOOL {
        auto* self=reinterpret_cast<PreviewBackend*>(data);
        const auto label=QString("Display %1 Â· %2 Ã— %3").arg(self->nativeSources_.size()+1).arg(rect->right-rect->left).arg(rect->bottom-rect->top);
        self->nativeSources_.push_back({reinterpret_cast<quintptr>(monitor),0,true,label}); return TRUE;
    },reinterpret_cast<LPARAM>(this));
    EnumWindows([](HWND hwnd,LPARAM data)->BOOL {
        if(!IsWindowVisible(hwnd)||GetWindowTextLengthW(hwnd)==0) return TRUE;
        DWORD pid=0; GetWindowThreadProcessId(hwnd,&pid); if(pid==GetCurrentProcessId()) return TRUE;
        DWORD cloaked=0; DwmGetWindowAttribute(hwnd,DWMWA_CLOAKED,&cloaked,sizeof(cloaked)); if(cloaked) return TRUE;
        if(GetWindowLongPtrW(hwnd,GWL_EXSTYLE)&WS_EX_TOOLWINDOW) return TRUE;
        wchar_t title[512]{}; GetWindowTextW(hwnd,title,512);
        auto* self=reinterpret_cast<PreviewBackend*>(data);
        Source source{reinterpret_cast<quintptr>(hwnd),pid,false,QString::fromWCharArray(title)};
        self->nativeSources_.push_back(source); self->nativeApps_.push_back(source); return TRUE;
    },reinterpret_cast<LPARAM>(this));
    for(auto& s:nativeSources_) sources_.append(QVariantMap{{"label",s.label}});
    for(auto& s:nativeApps_) applications_.append(QVariantMap{{"label",s.label}});
    Q_EMIT sourcesChanged();
    Q_EMIT stateChanged();
}
void PreviewBackend::startPreview(int index,int audioMode,int audioApplication) {
    if(shareActive())return;
    stopPreview();
    if(index<0||index>=static_cast<int>(nativeSources_.size())) { status_="Choose a capture source."; Q_EMIT stateChanged(); return; }
    const auto source=nativeSources_[index];
    selectedSourceIndex_=index;
    selectedAudioMode_=audioMode;selectedAudioApplication_=audioApplication;
    unsigned audioPid=source.pid;
    if(audioMode==1) {
        if(audioApplication<0||audioApplication>=static_cast<int>(nativeApps_.size())) { status_="Choose an application for audio."; Q_EMIT stateChanged(); return; }
        audioPid=nativeApps_[audioApplication].pid;
    }
    active_=true; status_="Previewing Â· "+source.label;
    const auto generation=++generation_;
    const auto error=[this,generation](QString message) { QMetaObject::invokeMethod(this,[this,generation,message] { fail(generation,message); },Qt::QueuedConnection); };
    capture_.start(source.handle,source.monitor,[this](goodsharing::ScreenFrame frame) { std::lock_guard lock(mutex_); latest_=std::move(frame.image); frames_++; },error);
    const auto mode=audioMode==1?goodsharing::AudioMode::Application:audioMode==2?goodsharing::AudioMode::Desktop:goodsharing::AudioMode::Off;
    audio_.start(mode,audioPid,[this](goodsharing::AudioBlock block) { double sum=0; for(auto sample:block.samples) sum+=static_cast<double>(sample)*sample; const double rms=block.samples.empty()?0:std::sqrt(sum/block.samples.size())/32768.0; std::lock_guard lock(mutex_); pendingLevel_=std::min(1.0,rms*8); },error);
    Q_EMIT stateChanged();
}
void PreviewBackend::stopPreview() {
    if(shareActive())stopSharing();
    ++generation_; capture_.stop(); audio_.stop();
    { std::lock_guard lock(mutex_); latest_={}; frames_=presented_=0; pendingLevel_=audioLevel_=0; }
    if(video_) video_->setFrame({}); active_=false; status_="Preview stopped.";
    Q_EMIT stateChanged(); Q_EMIT meterChanged();
}
void PreviewBackend::fail(unsigned generation,QString message) { if(generation!=generation_) return; stopPreview(); status_=std::move(message); Q_EMIT stateChanged(); }
bool PreviewBackend::startControlledSource(quintptr hwnd,unsigned pid) {
    DWORD actual=0; GetWindowThreadProcessId(reinterpret_cast<HWND>(hwnd),&actual); if(actual!=pid||!pid) return false;
    for(std::size_t i=0;i<nativeSources_.size();i++) if(!nativeSources_[i].monitor&&nativeSources_[i].handle==hwnd) { startPreview(static_cast<int>(i),0,0); return true; }
    return false;
}
int PreviewBackend::capturedFrames() const { std::lock_guard lock(mutex_); return frames_; }
bool PreviewBackend::shareActive()const {
    using State=goodsharing::SharePublisher::State;
    return room_.shareState()==State::Starting||room_.shareState()==State::Sharing||room_.shareState()==State::Stopping;
}
void PreviewBackend::setStreamProfile(int profile) {
    if(profile<0||profile>2)return;
    const auto next=static_cast<goodsharing::StreamProfile>(profile);
    if(next==profile_)return;
    profile_=next;
    appSettings().setValue("streamProfile",profile);
    appSettings().setValue("streamProfileExplicit",true);
    profileExplicit_=true;
    Q_EMIT stateChanged();
}
QString PreviewBackend::shareBadge()const {
    using State=goodsharing::SharePublisher::State;
    if(captureSharing_&&room_.shareState()==State::Sharing&&shareInput_&&shareInput_->gate()->ready())return "ENCRYPTED SHARE";
    if(room_.shareState()==State::Starting)return "PREPARING SHARE";
    if(room_.shareState()==State::Stopping)return "STOPPING SHARE";
    return roomReady()?"CONNECTED":"LOCAL PREVIEW";
}
void PreviewBackend::joinRoom(QString url,QString token,QString keyHex) {
    // A room that is not on this machine has RTT and jitter the local profiles
    // were not tuned for; pick the smooth buffer unless the user chose one.
    if(!profileExplicit_&&qEnvironmentVariableIsEmpty("GOODSHARING_STREAM_PROFILE")){
        const auto host=QUrl(url).host();
        const bool loopback=host=="127.0.0.1"||host=="localhost"||host=="::1"||host.isEmpty();
        if(!loopback&&profile_!=goodsharing::StreamProfile::Smooth){profile_=goodsharing::StreamProfile::Smooth;Q_EMIT stateChanged();}
    }
    static const QRegularExpression keyPattern("^[0-9a-fA-F]{64}$");
    if(!keyPattern.match(keyHex).hasMatch()){roomStatus_="Enter a 64-digit room key.";Q_EMIT stateChanged();return;}
    if(!room_.join(url.trimmed(),std::move(token),QByteArray::fromHex(keyHex.toLatin1()))){roomStatus_="Join rejected; leave the current room before joining again.";Q_EMIT stateChanged();}
}
void PreviewBackend::setShareFps(int fps) {
    if(fps!=30&&fps!=60)return;
    shareFps_=fps;
    appSettings().setValue("shareFps",fps);
    Q_EMIT stateChanged();
}
std::optional<PreviewBackend::Selection> PreviewBackend::currentSelection(const Selection& stale)const {
    for(const auto& choice:remoteShares()){
        const auto item=choice.toMap();
        if(item["owner"].toString()!=stale.owner||item["id"].toString()!=stale.id)continue;
        return Selection{stale.owner,stale.id,item["generation"].toULongLong(),stale.quality};
    }
    return std::nullopt;
}
QVariantList PreviewBackend::participants()const {
    QVariantList rows;
    QMap<QString,QString> sharingByOwner;
    for(const auto& choice:remoteShares()){
        const auto item=choice.toMap();
        sharingByOwner[item["owner"].toString()]=item["id"].toString();
    }
    auto add=[&](const QString& identity,const QString& name,bool self){
        rows.append(QVariantMap{{"identity",identity},{"name",name},{"self",self},
            {"sharing",self?shareActive():sharingByOwner.contains(identity)}});
    };
    if(roomBusy())add("self","You",true);
    for(auto it=sharingByOwner.begin();it!=sharingByOwner.end();++it)
        add(it.key(),participantNames_.value(it.key(),it.key()),false);
    for(auto it=participantNames_.begin();it!=participantNames_.end();++it)
        if(!sharingByOwner.contains(it.key()))add(it.key(),it.value(),false);
    return rows;
}
void PreviewBackend::watchOwner(QString owner) {
    const auto choices=remoteShares();
    for(int i=0;i<choices.size();i++){
        const auto item=choices[i].toMap();
        if(item["owner"].toString()!=owner)continue;
        const int quality=item["has1080"].toBool()?1080:720;
        addWatch(i,quality);
        return;
    }
}
QString PreviewBackend::sharingLabel()const {
    if(!captureSharing_||selectedSourceIndex_<0||selectedSourceIndex_>=static_cast<int>(nativeSources_.size()))return {};
    return nativeSources_[selectedSourceIndex_].label;
}
void PreviewBackend::setSelfPreview(bool enabled) {
    if(selfPreview_==enabled)return;
    selfPreview_=enabled;
    appSettings().setValue("selfPreview",enabled);
    Q_EMIT stateChanged();
}
void PreviewBackend::leaveRoom(){
    while(!extras_.empty())stopExtra(static_cast<int>(extras_.size())-1);
    stopWatching();stopSharing();remoteModel_.clear();
    remoteStatus_="Choose a remote share to watch.";status_="Not connected.";active_=false;
    room_.leave();
    Q_EMIT stateChanged();
}
void PreviewBackend::startSharing(int index,int audioMode,int audioApplication) {
    if(!roomReady()||shareActive())return;
    stopPreview();
    if(index<0||index>=static_cast<int>(nativeSources_.size())||audioMode<0||audioMode>2){status_="Choose a capture source and audio mode.";Q_EMIT stateChanged();return;}
    const auto source=nativeSources_[index];unsigned pid=0;
    if(audioMode==1){if(audioApplication<0||audioApplication>=static_cast<int>(nativeApps_.size())){status_="Choose an audio application.";Q_EMIT stateChanged();return;}pid=nativeApps_[audioApplication].pid;}
    const auto mode=audioMode==1?goodsharing::AudioMode::Application:audioMode==2?goodsharing::AudioMode::Desktop:goodsharing::AudioMode::Off;
    pendingShare_=goodsharing::CapturePublisherInput::Config{source.handle,source.monitor,mode,pid,shareFps_};
    if(const auto budget=qEnvironmentVariableIntValue("GOODSHARING_TEST_SHARE_BITRATE");budget>0)
        shareBitrateOverride_=budget;selectedSourceIndex_=index;selectedAudioMode_=audioMode;selectedAudioApplication_=audioApplication;
    // 60 fps doubles the frame count for the same picture, so it needs a larger
    // budget to keep text sharp; 30 fps keeps the tuned reference numbers.
    // 60 fps no longer gets a bigger budget: that budget made every keyframe a
    // burst large enough to stall the viewer and starve audio.
    const auto rateScale=5;
    const auto bitrate1080=shareBitrateOverride_?shareBitrateOverride_:10000000*rateScale/5;
    const auto bitrate720=shareBitrateOverride_?shareBitrateOverride_/2:5000000*rateScale/5;
    shareInput_=room_.startShare(audioMode!=0,timing().maximumCaptureAgeUs,shareFps_,
                                  static_cast<int>(bitrate1080),static_cast<int>(bitrate720));
    if(!shareInput_){pendingShare_.reset();status_="Share could not start.";}
    Q_EMIT stateChanged();
}
void PreviewBackend::stopSharing() {
    if(const auto c=sender_.report();!c.isEmpty())
        logWatch(QString("sender drops: replaced=%1 rateLimited=%2 expired=%3 frames=%4")
            .arg(c.value("raw_replaced").toDouble()).arg(c.value("rate_limited_raw_frames").toDouble())
            .arg(c.value("stale_or_rejected_video").toDouble()).arg(c.value("captured_frames").toDouble()));
    // Close publication permission first; capture teardown never grants time
    // for another content handoff. No network wait runs on this Qt owner.
    const bool wasSharing=captureSharing_||pendingShare_.has_value()||shareActive();
    room_.stopShare();sender_.stop();captureSharing_=false;pendingShare_.reset();shareInput_.reset();
    if(wasSharing){active_=false;{std::lock_guard lock(mutex_);latest_={};pendingLevel_=audioLevel_=0;}if(video_)video_->setFrame({});}
    Q_EMIT stateChanged();
}
bool PreviewBackend::startControlledShare(quintptr hwnd,unsigned pid,bool audio) {
    DWORD actual=0;GetWindowThreadProcessId(reinterpret_cast<HWND>(hwnd),&actual);if(actual!=pid||!pid||!roomReady())return false;
    // The owned window can appear after the initial enumeration.
    if(std::none_of(nativeSources_.begin(),nativeSources_.end(),[hwnd](const Source& source){return !source.monitor&&source.handle==hwnd;}))refreshSources();
    int application=-1;for(std::size_t i=0;i<nativeApps_.size();i++)if(nativeApps_[i].handle==hwnd&&nativeApps_[i].pid==pid)application=static_cast<int>(i);
    for(std::size_t i=0;i<nativeSources_.size();i++)if(!nativeSources_[i].monitor&&nativeSources_[i].handle==hwnd){startSharing(static_cast<int>(i),audio?1:0,application);return shareActive();}
    return false;
}
QJsonObject PreviewBackend::sharingReport()const{return {{"capture",sender_.report()},{"publication",publicationReport_},{"share_active",shareActive()},{"room_ready",roomReady()},{"source_minimized",shareSourceMinimized_},{"captured_frames",capturedFrames()}};}
QVariantList PreviewBackend::remoteShares()const {
    QVariantList result;using M=goodsharing::ShareModel;
    for(int row=0;row<remoteModel_.rowCount();row++){
        const auto index=remoteModel_.index(row);const auto high=remoteModel_.data(index,M::Has1080Role).toBool(),low=remoteModel_.data(index,M::Has720Role).toBool();if(!high&&!low)continue;
        const auto owner=remoteModel_.data(index,M::OwnerRole).toString(),id=remoteModel_.data(index,M::ShareIdRole).toString();
        result.append(QVariantMap{{"label",owner+" Â· "+id.left(6)},{"owner",owner},{"id",id},{"generation",remoteModel_.data(index,M::GenerationRole)},{"has1080",high},{"has720",low},{"hasAudio",remoteModel_.data(index,M::HasAudioRole)}});
    }return result;
}
namespace {std::int64_t watchLocalUs(){return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();}}
static void logWatch(const QString& line) {
    QFile file(QDir::temp().filePath("goodsharing-watch.log"));
    if(file.open(QIODevice::Append|QIODevice::Text))
        file.write((QDateTime::currentDateTime().toString(Qt::ISODateWithMs)+" "+line+"\n").toUtf8());
}
void PreviewBackend::watchShare(int index,int quality) {
    logWatch(QString("watchShare index=%1 quality=%2 roomReady=%3 watchBusy=%4 extras=%5 choices=%6")
        .arg(index).arg(quality).arg(roomReady()).arg(watchBusy()).arg(extras_.size()).arg(remoteShares().size()));
    if(!roomReady()||(quality!=720&&quality!=1080)){logWatch(QString("watchShare ignored ready=%1 quality=%2").arg(roomReady()).arg(quality));return;}const auto choices=remoteShares();
    if(index<0||index>=choices.size()){logWatch(QString("watchShare bad index=%1 of %2").arg(index).arg(choices.size()));return;}const auto choice=choices[index].toMap();
    if(!choice.value(quality==1080?"has1080":"has720").toBool()){remoteStatus_="That quality is not available.";Q_EMIT stateChanged();return;}
    const Selection next{choice["owner"].toString(),choice["id"].toString(),choice["generation"].toULongLong(),quality};
    if(remoteAv_&&watching()&&audioWatch_&&watch_->manifest().owner==next.owner&&watch_->manifest().id==next.id&&watch_->manifest().generation==next.generation){
        commitCandidate();
        if(candidateWatch_&&(candidateWatch_->manifest().quality==goodsharing::ShareTrack::Video1080?1080:720)==quality)return;
        if(candidateWatch_)cancelCandidate("Quality change cancelled.");
        if(watchedQuality()==quality){pendingQuality_.reset();Q_EMIT watchSelectionChanged();Q_EMIT stateChanged();return;}
        pendingQuality_=next;remoteStatus_="Preparing the new video qualityâ€¦";pollHandover();Q_EMIT stateChanged();return;
    }
    stopWatching();if(retiringWatch_||!retiringTracks_.empty()){pendingWatch_=next;remoteStatus_="Changing remote shareâ€¦";}else beginWatch(next);Q_EMIT stateChanged();
}
void PreviewBackend::beginWatch(Selection selection) {
    logWatch(QString("beginWatch owner=%1 id=%2 gen=%3 quality=%4 catalogGen=%5 cat1080=%6 cat720=%7")
        .arg(selection.owner,selection.id).arg(selection.generation).arg(selection.quality)
        .arg(room_.currentShareGeneration(selection.owner,selection.id))
        .arg(room_.hasShareQuality(selection.owner,selection.id,1080))
        .arg(room_.hasShareQuality(selection.owner,selection.id,720)));
    remoteSourceMinimized_=false;
    // The model can hold a slightly older snapshot than the catalog; the catalog
    // is authoritative for the generation that a subscription must use.
    if(const auto authoritative=room_.currentShareGeneration(selection.owner,selection.id);
       authoritative&&authoritative!=selection.generation)selection.generation=authoritative;
    // An earlier tile may still hold this share; the main picture takes priority.
    for(int i=static_cast<int>(extras_.size())-1;i>=0;i--)
        if(extras_[i]->selection.owner==selection.owner&&extras_[i]->selection.id==selection.id)stopExtra(i);
    // If the catalog lacks the requested layer, take the other one instead of
    // failing; never upgrade a 720p request to 1080p.
    if(!room_.hasShareQuality(selection.owner,selection.id,selection.quality)){
        const int other=selection.quality==1080?720:1080;
        if(room_.hasShareQuality(selection.owner,selection.id,other))selection.quality=other;
    }
    auto frames=std::make_shared<goodsharing::RemoteVideo>();
    frames->setPresentationFloor(timing().presentationFloorUs);frames->setBuffer(timing().bufferUs);
    frames->setFrameReady([this,frames]{
        QMetaObject::invokeMethod(this,[this,frames]{
            if(remoteVideo_==frames&&remoteItem_&&!candidateDisplayed_)remoteItem_->update();
        },Qt::QueuedConnection);
    });
    std::shared_ptr<goodsharing::RemoteShare> watch,audio;
    std::shared_ptr<goodsharing::RemoteAvSession> playback;
    const auto track=selection.quality==1080?goodsharing::ShareTrack::Video1080:goodsharing::ShareTrack::Video720;
    if(testAvPlayback_){
        playback=std::make_shared<goodsharing::RemoteAvSession>();
        playback->setLogger([](const QString& line){logWatch(line);});
        audio=room_.watchTrack(selection.owner,selection.id,selection.generation,goodsharing::ShareTrack::Audio,{{},[playback](livekit::AudioFrameEvent& event){playback->audio(event);}});
        // A share published without audio must still show its video; the audio
        // subscription is optional, not a precondition for the picture.
        watch=room_.watchTrack(selection.owner,selection.id,selection.generation,track,{[frames](livekit::VideoFrameEvent& event){frames->submit(event);},{}});
        if(!audio){
            // No audio track: do not start a playout session that expects one.
            playback.reset();
            logWatch("audio unavailable; watching video only");
        }
    }else watch=room_.watchShare(selection.owner,selection.id,selection.generation,track,false,{[frames](livekit::VideoFrameEvent& event){frames->submit(event);},{}});
    if(!watch){
        if(audio){room_.unwatchTrack(audio);retiringTracks_.push_back(audio);}
        // A share can be announced before its track is subscribable; retry
        // briefly instead of asking the viewer to select it again.
        // A share that offers only 720p must not be a dead end when 1080p was
        // selected; try the lower quality once before any retry.
        if(retryWatchAttempts_==0&&selection.quality==1080){
            Selection lower=selection;lower.quality=720;
            remoteStatus_="1080p is unavailable; trying 720p...";
            retryWatchAttempts_=1;beginWatch(lower);return;
        }
        // The announcement may have moved to a newer generation (a sender that
        // stopped and restarted sharing); re-resolve before spending a retry.
        if(retryWatchAttempts_==0){
            if(const auto fresh=currentSelection(selection);fresh&&fresh->generation!=selection.generation){
                remoteStatus_="Share was restarted; reconnecting...";beginWatch(*fresh);return;
            }else if(!fresh){
                remoteStatus_="That share has ended.";Q_EMIT stateChanged();return;
            }
        }
        if(roomReady()&&retryWatchAttempts_<3){retryWatchAttempts_++;retryWatch_=selection;watchRetryTimer_.start(400);
            remoteStatus_="Waiting for the share to become available ("+room_.lastWatchError()+")...";}
        else {
            remoteStatus_="Remote share is unavailable ("+room_.lastWatchError()+"); select it again.";
            logWatch(QString("watch FAILED owner=%1 id=%2 gen=%3 quality=%4 error=%5")
                .arg(selection.owner,selection.id).arg(selection.generation).arg(selection.quality)
                .arg(room_.lastWatchError()));
        }
        return;
    }
    retryWatchAttempts_=0;retryWatch_.reset();
    if(playback){
        playback->configureTiming(timing().leadUs(),timing().videoWaitLimitUs());
        playback->configure(frames,watch);
        playback->setAudioRestart([this,selection]{
            QMetaObject::invokeMethod(this,[this,selection]{
                const auto key=selection.owner+QChar(0)+selection.id+QChar(0)+QString::number(selection.generation);
                if(audioRetryKey_!=key){audioRetryKey_=key;audioRetryAttempts_=0;}
                if(watch_&&remoteAv_&&watching()&&audioRetryAttempts_<audioRetryLimit_){
                    audioRetryAttempts_++;stopWatching();pendingWatch_=selection;
                    remoteStatus_="Retrying stereo audio subscriptionâ€¦";Q_EMIT stateChanged();
                }
            },Qt::QueuedConnection);
        });
    }
    frames->bind(watch);
        frames->setLogger([](const QString& line){logWatch(line);});
        watch_=std::move(watch);audioWatch_=std::move(audio);remoteVideo_=std::move(frames);remoteReady_=remoteStalled_=false;
    handoverHistory_={};committedHandovers_=cancelledHandovers_=0;
    remoteAv_=std::move(playback);applyGain();if(remoteAv_)remoteAv_->start(audioWatch_,watch_);
    logWatch(QString("watch OK owner=%1 id=%2 quality=%3")
        .arg(selection.owner,selection.id).arg(selection.quality));
    Q_EMIT watchSelectionChanged();
    if(remoteItem_)remoteItem_->setRemote(remoteVideo_);remoteStatus_="Waiting for encrypted videoâ€¦";
}
void PreviewBackend::recordHandover(QJsonObject record){handoverHistory_.append(record);while(handoverHistory_.size()>8)handoverHistory_.removeAt(0);}
void PreviewBackend::stageQuality(Selection selection){
    if(!remoteAv_||!watching()||candidateWatch_||!remoteAv_->workerReady())return;
    const auto track=selection.quality==1080?goodsharing::ShareTrack::Video1080:goodsharing::ShareTrack::Video720;
    for(const auto& retiring:retiringTracks_)if(retiring->manifest().quality==track)return;
    auto frames=std::make_shared<goodsharing::RemoteVideo>();
    frames->setPresentationFloor(timing().presentationFloorUs);frames->setBuffer(timing().bufferUs);frames->setWarming(true);
    frames->setFrameReady([this,frames]{
        QMetaObject::invokeMethod(this,[this,frames]{
            if(candidateFrames_==frames&&candidateDisplayed_&&remoteItem_)remoteItem_->update();
        },Qt::QueuedConnection);
    });
    auto candidate=room_.watchTrack(selection.owner,selection.id,selection.generation,track,{[frames](livekit::VideoFrameEvent& event){frames->submit(event);},{}});
    if(!candidate){pendingQuality_.reset();remoteStatus_="New quality is unavailable; keeping the current quality.";Q_EMIT watchSelectionChanged();Q_EMIT stateChanged();return;}
    if(!remoteAv_->stageVideo(candidate)){room_.unwatchTrack(candidate);retiringTracks_.push_back(candidate);return;}
    remoteAv_->configure(frames,candidate);frames->bind(candidate);
    handoverRequest_={{"requested_receiver_us",static_cast<double>(watchLocalUs())},{"from",watchedQuality()},{"to",selection.quality},{"before_playback",remoteAv_->report()}};
    candidateWatch_=candidate;candidateFrames_=frames;candidateDisplayed_=false;pendingQuality_.reset();
    candidateDeadline_=std::chrono::steady_clock::now()+std::chrono::seconds(10);
    remoteStatus_="Preparing the new video qualityâ€¦";Q_EMIT stateChanged();
}
void PreviewBackend::commitCandidate(){
    if(!candidateWatch_||!remoteAv_||remoteAv_->selectedVideo()!=candidateWatch_)return;
    auto record=handoverRequest_;record["outcome"]="committed";record["completed_receiver_us"]=static_cast<double>(watchLocalUs());
    record["after_playback"]=remoteAv_->report();record["old_video"]=remoteVideo_?remoteVideo_->report():QJsonObject{};record["new_video"]=candidateFrames_->report();
    const auto retiringAt=watchLocalUs();watch_->revoke();const auto revokedAt=watchLocalUs();
    room_.unwatchTrack(watch_);const auto unwatchedAt=watchLocalUs();
    if(remoteVideo_)remoteVideo_->discardRevoked();const auto discardedAt=watchLocalUs();
    record["retire_revoke_us"]=static_cast<double>(revokedAt-retiringAt);
    record["retire_unwatch_us"]=static_cast<double>(unwatchedAt-revokedAt);
    record["retire_discard_us"]=static_cast<double>(discardedAt-unwatchedAt);
    record["retire_completed_receiver_us"]=static_cast<double>(discardedAt);
    recordHandover(record);committedHandovers_++;
    retiringTracks_.push_back(watch_);
        if(candidateFrames_)candidateFrames_->setLogger([](const QString& line){logWatch(line);});
        watch_=std::move(candidateWatch_);remoteVideo_=std::move(candidateFrames_);
    candidateDisplayed_=false;handoverRequest_={};remoteReady_=true;remoteStalled_=false;
    if(remoteItem_)remoteItem_->setRemote(remoteVideo_);remoteStatus_="Watching encrypted audio/video Â· development test";
    Q_EMIT watchSelectionChanged();Q_EMIT stateChanged();
}
bool PreviewBackend::cancelCandidate(QString reason){
    if(!candidateWatch_)return true;
    if(remoteAv_&&!remoteAv_->cancelVideo(candidateWatch_)&&remoteAv_->selectedVideo()==candidateWatch_){commitCandidate();return false;}
    auto record=handoverRequest_;record["outcome"]="cancelled";record["completed_receiver_us"]=static_cast<double>(watchLocalUs());record["reason"]=reason;
    record["after_playback"]=remoteAv_?remoteAv_->report():QJsonObject{};record["new_video"]=candidateFrames_?candidateFrames_->report():QJsonObject{};
    candidateWatch_->revoke();record["candidate_watch"]=candidateWatch_->report();recordHandover(record);cancelledHandovers_++;
    room_.unwatchTrack(candidateWatch_);if(candidateFrames_)candidateFrames_->discardRevoked();retiringTracks_.push_back(std::move(candidateWatch_));candidateFrames_.reset();candidateDisplayed_=false;handoverRequest_={};
    if(remoteItem_)remoteItem_->setRemote(remoteVideo_);remoteStatus_=std::move(reason);Q_EMIT watchSelectionChanged();Q_EMIT stateChanged();return true;
}
void PreviewBackend::pollHandover(){
    if(!remoteAv_||!watching())return;commitCandidate();
    if(candidateWatch_){
        if(candidateWatch_->closed()||std::chrono::steady_clock::now()>=candidateDeadline_)cancelCandidate("New quality could not become ready; keeping the current quality.");
        else{
            // A clocked first picture has a future deadline. Buffer its
            // successors immediately, while the old quality stays visible.
            // Waiting until that deadline would discard a whole video lead.
            candidateFrames_->finishWarmup();
            const bool changed=candidateFrames_->changed(),ready=candidateFrames_->readyToPresent();
            if(ready&&!candidateDisplayed_){candidateFrames_->setWarming(false);candidateDisplayed_=true;if(remoteItem_)remoteItem_->setRemote(candidateFrames_);}
            else if(!ready&&candidateDisplayed_){candidateDisplayed_=false;candidateFrames_->setWarming(true);if(remoteItem_)remoteItem_->setRemote(remoteVideo_);}
            if(candidateDisplayed_&&changed&&remoteItem_)remoteItem_->update();
        }
    }
    if(!candidateWatch_&&pendingQuality_)stageQuality(*pendingQuality_);
}
int PreviewBackend::slotCount()const {
    const int main=watching()||watchBusy()?1:0;
    return main+static_cast<int>(extras_.size());
}
QVariantList PreviewBackend::watchSlots()const {
    QVariantList result;
    if(watching()||watchBusy()){
        const auto manifest=watch_?watch_->manifest():goodsharing::RemoteShare::Manifest{};
        result.append(QVariantMap{{"slot",0},{"owner",manifest.owner},{"id",manifest.id},
            {"quality",manifest.quality==goodsharing::ShareTrack::Video1080?1080:720}});
    }
    for(std::size_t i=0;i<extras_.size();i++)
        result.append(QVariantMap{{"slot",static_cast<int>(i)+1},{"owner",extras_[i]->selection.owner},
            {"id",extras_[i]->selection.id},{"quality",extras_[i]->selection.quality}});
    return result;
}
void PreviewBackend::applyGain() {
    if(remoteAv_)remoteAv_->setGain(muted_?0.0f:static_cast<float>(volume_)/100.0f);
}
void PreviewBackend::setVolume(int percent) {
    volume_=std::clamp(percent,0,100);
    logWatch(QString("setVolume=%1 muted=%2 playback=%3").arg(volume_).arg(muted_).arg(remoteAv_!=nullptr));
    muted_=volume_==0;
    applyGain();
    appSettings().setValue("volume",volume_);
    Q_EMIT stateChanged();
}
void PreviewBackend::toggleMute() {
    muted_=!muted_;
    applyGain();
    Q_EMIT stateChanged();
}
void PreviewBackend::focusSlot(int slot) {
    if(slot<=0||slot>static_cast<int>(extras_.size()))return;
    const auto target=extras_[slot-1]->selection;
    std::optional<Selection> previous;
    if(watch_&&!watch_->closed())previous=Selection{watch_->manifest().owner,watch_->manifest().id,watch_->manifest().generation,watchedQuality()};
    stopExtra(slot-1);
    if(previous)stopWatching();
    beginWatch(target);
    if(previous)beginExtra(*previous);
}
void PreviewBackend::addWatch(int index,int quality) {
    if(!roomReady()||(quality!=720&&quality!=1080))return;
    if(!watching()&&!watchBusy()){watchShare(index,quality);return;}
    if(extras_.size()>=2){remoteStatus_="Three watched shares is the current limit.";Q_EMIT stateChanged();return;}
    const auto choices=remoteShares();
    if(index<0||index>=choices.size())return;
    const auto choice=choices[index].toMap();
    if(!choice.value(quality==1080?"has1080":"has720").toBool()){remoteStatus_="That quality is not available.";Q_EMIT stateChanged();return;}
    Selection next{choice["owner"].toString(),choice["id"].toString(),choice["generation"].toULongLong(),quality};
    beginExtra(next);
}
bool PreviewBackend::beginExtra(Selection selection) {
    if(extras_.size()>=2||!roomReady())return false;
    if(watch_&&!watch_->closed()&&watch_->manifest().owner==selection.owner&&watch_->manifest().id==selection.id){
        remoteStatus_="Already watching this share.";Q_EMIT stateChanged();return false;
    }
    for(const auto& extra:extras_)if(extra->selection.owner==selection.owner&&extra->selection.id==selection.id){
        remoteStatus_="That share is already on screen.";Q_EMIT stateChanged();return false;
    }
    const auto track=selection.quality==1080?goodsharing::ShareTrack::Video1080:goodsharing::ShareTrack::Video720;
    auto frames=std::make_shared<goodsharing::RemoteVideo>();
    frames->setPresentationFloor(timing().presentationFloorUs);frames->setBuffer(timing().bufferUs);
    const int slot=static_cast<int>(extras_.size());
    VideoItem* item=extraItems_[slot];
    // Same order as the main picture: the ready hook precedes binding.
    if(item)frames->setFrameReady([item]{QMetaObject::invokeMethod(item,[item]{item->update();},Qt::QueuedConnection);});
    auto watch=room_.watchShare(selection.owner,selection.id,selection.generation,track,false,
        {[frames](livekit::VideoFrameEvent& event){frames->submit(event);},{}});
    if(!watch){remoteStatus_="That share is unavailable.";Q_EMIT stateChanged();return false;}
    frames->bind(watch);
    if(item){item->setRemote(frames);item->update();}
    auto extra=std::make_shared<ExtraWatch>();extra->selection=selection;extra->watch=watch;extra->frames=frames;
    extras_.push_back(extra);
    Q_EMIT stateChanged();
    return true;
}
void PreviewBackend::stopExtra(int index) {
    if(index<0||index>=static_cast<int>(extras_.size()))return;
    auto extra=extras_[index];extras_.erase(extras_.begin()+index);
    extra->watch->revoke();
    if(extra->frames)extra->frames->discardRevoked();
    room_.unwatchTrack(extra->watch);
    for(std::size_t i=0;i<extras_.size();i++)if(extraItems_[i]){extraItems_[i]->setRemote(extras_[i]->frames);extraItems_[i]->update();}
    for(std::size_t i=extras_.size();i<2;i++)if(extraItems_[i]){extraItems_[i]->setRemote({});extraItems_[i]->update();}
    Q_EMIT stateChanged();
}
void PreviewBackend::stopWatchSlot(int slot) {
    if(slot<=0){stopWatching();return;}
    stopExtra(slot-1);
}
void PreviewBackend::reconcileExtras() {
    // A tile ends when its track closes or its share is no longer announced
    // (the publisher stopped, left, or lost encryption readiness).
    const auto choices=remoteShares();
    for(int i=static_cast<int>(extras_.size())-1;i>=0;i--){
        const auto& selection=extras_[i]->selection;
        bool listed=false;
        for(const auto& choice:choices){
            const auto item=choice.toMap();
            if(item["owner"].toString()==selection.owner&&item["id"].toString()==selection.id){listed=true;break;}
        }
        if(extras_[i]->watch->closed()||!listed)stopExtra(i);
    }
}
void PreviewBackend::stopWatching() {
    remoteSourceMinimized_=false;
    pendingWatch_.reset();pendingQuality_.reset();commitCandidate();
    if(watch_){
        watch_->revoke();if(audioWatch_)audioWatch_->revoke();if(candidateWatch_)candidateWatch_->revoke();
        if(remoteAv_)remoteAv_->stop();room_.unwatchShare(watch_->manifest().owner,watch_->manifest().id);
        lastViewingReport_={{"video",remoteVideo_?remoteVideo_->report():QJsonObject{}},{"watch",watch_->report()},
            {"audio_watch",audioWatch_?audioWatch_->report():QJsonObject{}},{"playback",remoteAv_?remoteAv_->report():QJsonObject{}},
            {"candidate_watch",candidateWatch_?candidateWatch_->report():QJsonObject{}},{"candidate_video",candidateFrames_?candidateFrames_->report():QJsonObject{}},
            {"handover_history",handoverHistory_},{"committed_handovers",static_cast<double>(committedHandovers_)},{"cancelled_handovers",static_cast<double>(cancelledHandovers_)}};
        retiringWatch_=std::move(watch_);
        if(remoteVideo_)remoteVideo_->discardRevoked();if(candidateFrames_)candidateFrames_->discardRevoked();
        if(audioWatch_)retiringTracks_.push_back(std::move(audioWatch_));
        if(candidateWatch_)retiringTracks_.push_back(std::move(candidateWatch_));
    }
    remoteAv_.reset();candidateFrames_.reset();candidateDisplayed_=false;handoverRequest_={};
    Q_EMIT watchSelectionChanged();
    if(remoteItem_)remoteItem_->setRemote({});remoteVideo_.reset();remoteReady_=false;remoteStatus_="Viewing stopped.";Q_EMIT stateChanged();
}
QJsonObject PreviewBackend::viewingReport()const {
    QJsonArray extraReports;
    for(std::size_t i=0;i<extras_.size();i++)
        extraReports.append(QJsonObject{{"slot",static_cast<int>(i)+1},{"quality",extras_[i]->selection.quality},
            {"watch",extras_[i]->watch?extras_[i]->watch->report():QJsonObject{}},
            {"video",extras_[i]->frames?extras_[i]->frames->report():QJsonObject{}}});
    return {{"watching",watching()},{"busy",watchBusy()},{"status",remoteStatus_},{"source_minimized",remoteSourceMinimized_},
        {"slot_count",slotCount()},{"slots",QJsonValue::fromVariant(watchSlots())},{"extras",extraReports},
        {"video",remoteVideo_?remoteVideo_->report():QJsonObject{}},
        {"watch",watch_?watch_->report():retiringWatch_?retiringWatch_->report():QJsonObject{}},{"audio_watch",audioWatch_?audioWatch_->report():QJsonObject{}},
        {"playback",remoteAv_?remoteAv_->report():QJsonObject{}},{"test_av_playback",testAvPlayback_},{"last_selection",lastViewingReport_},{"test_completed_watches",testCompletedWatches_},
        {"candidate_watch",candidateWatch_?candidateWatch_->report():QJsonObject{}},{"candidate_video",candidateFrames_?candidateFrames_->report():QJsonObject{}},
        {"candidate_displayed",candidateDisplayed_},{"quality_request_pending",pendingQuality_.has_value()},{"retiring_tracks",static_cast<int>(retiringTracks_.size())},
        {"handover_history",handoverHistory_},{"committed_handovers",static_cast<double>(committedHandovers_)},{"cancelled_handovers",static_cast<double>(cancelledHandovers_)}};
}



