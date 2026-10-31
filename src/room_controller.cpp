#include "room_controller.h"
#include "room_connection.h"
#include "share_model.h"
#include <livekit/room_delegate.h>
#include <livekit/local_participant.h>
#include <livekit/remote_participant.h>
#include <livekit/remote_track_publication.h>
#include <QMetaObject>
#include <QThread>
#include <QUuid>
#include <windows.h>
#include <mfapi.h>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <map>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <set>
#include <algorithm>
#include <QJsonArray>
#include <QJsonDocument>
#include <cstdio>

namespace goodsharing {
struct RoomController::Impl : livekit::RoomDelegate {
    RoomController* owner;
    quint64 epoch;
    std::atomic_bool stopping{false},ready{false},invalidated{false};
    std::atomic_bool userLeave{false},notificationOverflow{false};
    std::shared_ptr<std::atomic<unsigned>> notifications=std::make_shared<std::atomic<unsigned>>(0);
    mutable std::mutex mutex;
    std::condition_variable wake;
    std::deque<std::function<void(livekit::Room&)>> commands;
    struct Publication {std::shared_ptr<livekit::RemoteTrackPublication> track;std::string owner;};
    std::map<std::string,Publication> publications;
    RemoteShareCatalog catalog;
    std::shared_ptr<std::atomic_bool> receiveOpen=std::make_shared<std::atomic_bool>(true);
    enum class Subscription {None,Requested,On,Stopping,Off};
    struct Watch {
        std::shared_ptr<RemoteShare> media;
        std::array<std::shared_ptr<livekit::RemoteTrackPublication>,2> publications;
        std::array<Subscription,2> state{};
        std::array<std::chrono::steady_clock::time_point,2> deadline{};
        bool readersStopped=false;
    };
    std::map<RemoteShareCatalog::Key,Watch> watches;
    bool receiveChanged=false;
    std::jthread worker;
    std::atomic<std::shared_ptr<EncryptionGate>> activeGate;
    std::unique_ptr<SharePublisher> publisher;
    SharePublisher::State lastPublisherState=SharePublisher::State::Stopped;
    bool havePublisherState=false;
    const bool testLifecycle=qEnvironmentVariableIntValue("GOODSHARING_TEST_AV_TRACE")!=0;
    std::atomic_uint testEventCount{0};
    void testEvent(const char* name,QJsonObject fields={}){
        if(!testLifecycle)return;
        const auto count=testEventCount.fetch_add(1);if(count>=128)return;
        fields.insert("event",QLatin1String(name));fields.insert("sequence",static_cast<int>(count));
        fields.insert("local_us",static_cast<double>(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count()));
        fields.insert("ready",ready.load());fields.insert("invalidated",invalidated.load());
        const auto line=QJsonDocument(fields).toJson(QJsonDocument::Compact);std::fprintf(stderr,"GoodSharing test room event: %s\n",line.constData());
    }
    // Delegate and all callback storage outlive the room and its event thread.
    std::unique_ptr<RoomConnection> connection;
    Impl(RoomController* o,quint64 e):owner(o),epoch(e){}
    ~Impl(){if(auto gate=activeGate.load())gate->close();revokeWatches();stopping=true;wake.notify_one();if(worker.joinable())worker.join();connection.reset();}
    void revokeWatches(){*receiveOpen=false;std::lock_guard lock(mutex);for(auto& [key,watch]:watches)watch.media->revoke();receiveChanged=true;wake.notify_one();}
    void receiveWakeLocked(){receiveChanged=true;wake.notify_one();}
    void remoteFailureLocked() {
        *receiveOpen=false;if(auto gate=activeGate.load())gate->close(true);
        invalidated=true;ready=false;stopping=true;wake.notify_one();state(State::Failed,"Remote track history limit exceeded; rejoin");
    }
    // Only the transport worker joins readers or changes SDK subscriptions.
    void pumpWatches() {
        std::vector<std::shared_ptr<RemoteShare>> stopReaders,finished;
        std::vector<std::shared_ptr<livekit::RemoteTrackPublication>> unsubscribe;
        const auto now=std::chrono::steady_clock::now();
        {std::lock_guard lock(mutex);
         for(auto& [key,watch]:watches) {
            if(!watch.media->error().empty())watch.media->revoke();
            for(std::size_t i=0;i<2;i++)if(watch.state[i]==Subscription::Requested&&now>=watch.deadline[i]) {
                testEvent("subscription_timeout",{{"quality",static_cast<int>(watch.media->manifest().quality)}});
                watch.media->revoke();
            }
            if(watch.media->closed()) {
                if(!watch.readersStopped){watch.readersStopped=true;stopReaders.push_back(watch.media);}
                for(std::size_t i=0;i<2;i++)if(watch.state[i]==Subscription::On||watch.state[i]==Subscription::Requested){watch.state[i]=Subscription::Stopping;watch.deadline[i]=now+std::chrono::seconds(5);unsubscribe.push_back(watch.publications[i]);}
            }
            // setSubscribed(false) synchronously detaches the native track.
            // If no track ever arrived, there is no unsubscribe callback to
            // await. Retire this already-revoked watch after a bounded grace;
            // a failed candidate must not take down healthy room media.
            for(std::size_t i=0;i<2;i++)if(watch.state[i]==Subscription::Stopping&&now>=watch.deadline[i]) {
                testEvent("unsubscribe_ack_timeout");watch.state[i]=Subscription::Off;
            }
         }}
        for(const auto& media:stopReaders)media->stop();
        for(const auto& publication:unsubscribe)publication->setSubscribed(false);
        {std::lock_guard lock(mutex);
         for(auto it=watches.begin();it!=watches.end();) {
            const auto& watch=it->second;
            const bool off=std::all_of(watch.state.begin(),watch.state.end(),[](Subscription s){return s==Subscription::None||s==Subscription::Off;});
            if(watch.media->closed()&&watch.readersStopped&&off){finished.push_back(watch.media);it=watches.erase(it);}else ++it;
         }}
        for(const auto& media:finished){media->finish();const auto report=media->report();notify([report](RoomController& o){Q_EMIT o.watchStopped(report);});}
    }
    void notify(std::function<void(RoomController&)> callback,bool completion=false) {
        auto* target=owner;const auto generation=epoch;
        const auto pending=notifications;
        if(pending->fetch_add(1)>=256&&!completion){pending->fetch_sub(1);if(auto gate=activeGate.load())gate->close(true);*receiveOpen=false;notificationOverflow=true;invalidated=true;ready=false;stopping=true;wake.notify_one();return;}
        QMetaObject::invokeMethod(target,[target,generation,pending,callback=std::move(callback)] {
            pending->fetch_sub(1);
            if(target->epoch_==generation&&target->impl_)callback(*target);
        },Qt::QueuedConnection);
    }
    void state(State state,QString message={}){testEvent("state_requested",{{"state",static_cast<int>(state)}});notify([state,message=std::move(message)](RoomController& o){
        if(o.state_==State::Leaving)return;
        if(state==State::Ready&&(!o.impl_||!o.impl_->ready||o.impl_->invalidated))return;
        o.transition(state,message);
    });}
    bool enqueue(std::function<void(livekit::Room&)> command) {
        std::lock_guard lock(mutex);
        if(stopping||!ready||commands.size()>=64)return false;
        commands.push_back(std::move(command));wake.notify_one();return true;
    }
    void run(std::string url,std::string token,std::vector<std::uint8_t> clientKey) {
        const auto com=CoInitializeEx(nullptr,COINIT_MULTITHREADED);bool mf=false;
        try {
            if(FAILED(com)||FAILED(MFStartup(MF_VERSION)))throw std::runtime_error("Media initialization failed");mf=true;
            connection=std::make_unique<RoomConnection>(this);
            testEvent("join_started");
            livekit::RoomOptions options;options.auto_subscribe=false;options.dynacast=false;options.adaptive_stream=false;
            // Keep receiver subscription negotiation on the subscriber peer.
            // Publishing/data channels use the separate publisher connection.
            options.single_peer_connection=false;
            testEvent("transport_policy",{{"single_peer_connection",false}});
            options.join_retries=0;options.connect_timeout=std::chrono::milliseconds(1000);
            livekit::E2EEOptions encryption;encryption.key_provider_options.shared_key=std::move(clientKey);encryption.key_provider_options.failure_tolerance=0;options.encryption=std::move(encryption);
            connection->start(std::move(url),std::move(token),std::move(options));
            while(!stopping&&connection->state()==RoomConnection::State::Connecting) {
                std::unique_lock lock(mutex);wake.wait_for(lock,std::chrono::milliseconds(5),[this]{return stopping.load();});
            }
            if(!stopping) {
                if(connection->state()!=RoomConnection::State::Connected)throw std::runtime_error("Room join failed");
                auto manager=connection->room().e2eeManager().lock();
                if(!manager)throw std::runtime_error("Encryption manager missing");
                manager->setEnabled(true);if(!manager->enabled())throw std::runtime_error("Encryption activation failed");
                // Hold our event-map lock while copying under the SDK room
                // lock. SDK delegates run after that room lock is released;
                // subsequent unpublish/disconnect events follow this snapshot.
                {std::lock_guard lock(mutex);
                 for(const auto& [identity,publication]:connection->room().remotePublicationSnapshot())announceLocked(identity,publication);}
                if(invalidated||stopping)throw std::runtime_error("Connection invalidated during join");
                ready=true;testEvent("join_ready");state(State::Ready);
            }
            const bool testTelemetry=qEnvironmentVariableIntValue("GOODSHARING_TEST_AV_TRACE")!=0;
            std::vector<std::future<std::vector<livekit::RtcStats>>> testStats;
            std::optional<std::future<livekit::SessionStats>> testOutbound;
            auto nextTestOutbound=std::chrono::steady_clock::now();unsigned testOutboundCount=0;
            auto nextTestStats=std::chrono::steady_clock::now();unsigned testStatsCount=0,testRatesCount=0;
            auto nextTestRates=nextTestStats;
            while(!stopping) {
                std::function<void(livekit::Room&)> command;
                {std::unique_lock lock(mutex);
                 // Only an active encoder needs periodic output/feedback pumps.
                 // An idle room sleeps until a command or shutdown notification.
                 const bool active=publisher&&(publisher->state()==SharePublisher::State::Starting||publisher->state()==SharePublisher::State::Sharing);
                 const auto changed=[this]{return stopping||!commands.empty()||receiveChanged;};
                 if(active)wake.wait_for(lock,std::chrono::milliseconds(5),changed);
                 else if(!watches.empty())wake.wait_for(lock,std::chrono::milliseconds(50),changed);
                 else wake.wait(lock,changed);
                 receiveChanged=false;
                 if(stopping)break;if(!commands.empty()){command=std::move(commands.front());commands.pop_front();}}
                if(ready&&command)command(connection->room());
                pumpWatches();
                if(publisher){publisher->tick();publicationChanged();if(publisher->state()==SharePublisher::State::Failed){ready=false;state(State::Failed,"Share publication failed; reconnect explicitly");break;}}
                if(testTelemetry&&testStatsCount<60){
                    if(!testStats.empty()&&std::all_of(testStats.begin(),testStats.end(),[](auto& request){return request.wait_for(std::chrono::milliseconds(0))==std::future_status::ready;})){
                        try{
                            QJsonArray tracks;
                            for(auto& request:testStats)for(const auto& item:request.get())if(const auto* r=std::get_if<livekit::RtcInboundRtpStats>(&item.stats)){
                                const auto& s=r->inbound;tracks.append(QJsonObject{{"kind",QString::fromStdString(r->stream.kind)},
                                    {"ssrc",static_cast<double>(r->stream.ssrc)},{"jitter_seconds",r->received.jitter},
                                    {"jitter_buffer_seconds",s.jitter_buffer_delay},{"jitter_target_seconds",s.jitter_buffer_target_delay},
                                    {"jitter_minimum_seconds",s.jitter_buffer_minimum_delay},{"emitted",static_cast<double>(s.jitter_buffer_emitted_count)},
                                    {"received_samples",static_cast<double>(s.total_samples_received)},{"concealed_samples",static_cast<double>(s.concealed_samples)},
                                    {"frames_decoded",static_cast<double>(s.frames_decoded)},{"packets_lost",static_cast<double>(r->received.packets_lost)}});
                                auto last=tracks.last().toObject();last["packets_received"]=static_cast<double>(r->received.packets_received);
                                last["bytes_received"]=static_cast<double>(s.bytes_received);last["frames_received"]=static_cast<double>(s.frames_received);
                                last["width"]=static_cast<int>(s.frame_width);last["height"]=static_cast<int>(s.frame_height);tracks.replace(tracks.size()-1,last);
                            }
                            const auto now=std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
                            const auto line=QJsonDocument(QJsonObject{{"receiver_us",static_cast<double>(now)},{"tracks",tracks}}).toJson(QJsonDocument::Compact);
                            std::fprintf(stderr,"GoodSharing test track stats: %s\n",line.constData());testStatsCount++;
                        }catch(...){testStatsCount=60;}
                        testStats.clear();
                    }
                    if(testStatsCount<60&&testStats.empty()&&std::chrono::steady_clock::now()>=nextTestStats){
                        try{
                            std::vector<std::shared_ptr<RemoteShare>> observed;
                            {std::lock_guard lock(mutex);for(const auto& [key,watch]:watches)if(!watch.media->closed())observed.push_back(watch.media);}
                            for(const auto& media:observed)for(auto& request:media->requestStatistics())testStats.push_back(std::move(request));
                        }catch(...){testStatsCount=60;}
                        nextTestStats=std::chrono::steady_clock::now()+std::chrono::seconds(1);
                    }
                }
                if(testTelemetry&&publisher&&testOutboundCount<60){
                    try{
                        if(testOutbound&&testOutbound->wait_for(std::chrono::milliseconds(0))==std::future_status::ready){
                            QJsonArray tracks;const auto stats=testOutbound->get();testOutbound.reset();
                            for(const auto& item:stats.publisher_stats)if(const auto* r=std::get_if<livekit::RtcOutboundRtpStats>(&item.stats)){
                                const auto& s=r->outbound;tracks.append(QJsonObject{{"kind",QString::fromStdString(r->stream.kind)},
                                    {"ssrc",static_cast<double>(r->stream.ssrc)},{"packets_sent",static_cast<double>(r->sent.packets_sent)},
                                    {"bytes_sent",static_cast<double>(r->sent.bytes_sent)},{"frames_sent",static_cast<double>(s.frames_sent)},
                                    {"frames_encoded",static_cast<double>(s.frames_encoded)},{"width",static_cast<int>(s.frame_width)},{"height",static_cast<int>(s.frame_height)}});
                            }
                            const auto now=std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
                            const auto line=QJsonDocument(QJsonObject{{"local_us",static_cast<double>(now)},{"tracks",tracks}}).toJson(QJsonDocument::Compact);
                            std::fprintf(stderr,"GoodSharing test outbound stats: %s\n",line.constData());testOutboundCount++;
                        }
                        if(!testOutbound&&std::chrono::steady_clock::now()>=nextTestOutbound){testOutbound.emplace(connection->room().getStats());nextTestOutbound=std::chrono::steady_clock::now()+std::chrono::seconds(1);}
                    }catch(...){testOutboundCount=60;testOutbound.reset();}
                }
                if(testTelemetry&&publisher&&testRatesCount<60&&std::chrono::steady_clock::now()>=nextTestRates){
                    const auto now=std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
                    const auto snapshot=publisher->report();
                    const auto line=QJsonDocument(QJsonObject{{"local_us",static_cast<double>(now)},{"outputs",snapshot.value("outputs")},
                        {"input",snapshot.value("input")},{"gate",snapshot.value("gate")},{"state",snapshot.value("state")},
                        {"submitted_audio_blocks",snapshot.value("submitted_audio_blocks")},{"submitted_video_pairs",snapshot.value("submitted_video_pairs")}}).toJson(QJsonDocument::Compact);
                    std::fprintf(stderr,"GoodSharing test sender rates: %s\n",line.constData());testRatesCount++;
                    nextTestRates=std::chrono::steady_clock::now()+std::chrono::seconds(1);
                }
            }
        } catch(const std::exception&) {
            testEvent("worker_exception");
            ready=false;
            if(auto gate=activeGate.load())gate->close(true);
            // Tokens/keys/SDK exception bodies never become UI diagnostics.
            state(State::Failed,"Room operation failed; reconnect explicitly");
        }
        testEvent("cleanup_started");ready=false;
        revokeWatches();
        std::vector<std::shared_ptr<RemoteShare>> endingWatches;
        {std::lock_guard lock(mutex);for(auto& [key,watch]:watches)endingWatches.push_back(watch.media);}
        for(const auto& media:endingWatches)media->stop();
        if(publisher){publisher->stop();publicationChanged();publisher.reset();}
        bool closeFailed=false;
        if(connection){connection->stop();closeFailed=!connection->error().empty();}
        testEvent("cleanup_finished",{{"close_failed",closeFailed}});
        for(const auto& media:endingWatches)media->finish();
        {std::lock_guard lock(mutex);commands.clear();publications.clear();watches.clear();}
        connection.reset();
        if(mf)MFShutdown();if(SUCCEEDED(com))CoUninitialize();
        const bool left=userLeave,overflow=notificationOverflow;
        notify([left,closeFailed,overflow](RoomController& o){
            o.impl_.reset(); // All SDK operations completed before this notification.
            if(o.input_&&(o.shareState_==SharePublisher::State::Starting||o.shareState_==SharePublisher::State::Sharing||o.shareState_==SharePublisher::State::Stopping))
                o.publicationState(o.input_->gate()->failed()?SharePublisher::State::Failed:SharePublisher::State::Stopped,{},o.shareGeneration_);
            if(overflow)o.transition(State::Failed,"Room event queue overflow; reconnect explicitly");
            else if(closeFailed)o.transition(State::Failed,"Room cleanup reported a failure");
            else if(left)o.transition(State::Idle);
        },true);
    }
    void publicationChanged() {
        if(!publisher||(havePublisherState&&publisher->state()==lastPublisherState))return;
        const auto phase=publisher->state();lastPublisherState=phase;havePublisherState=true;const auto generation=publisher->gate()->generation();const auto error=publisher->error();const auto report=publisher->report();
        testEvent("publication_state",{{"state",static_cast<int>(phase)},{"input",report.value("input")},{"gate_closed",publisher->gate()->closed()},{"gate_failed",publisher->gate()->failed()}});
        notify([phase,generation,error,report](RoomController& o){if(o.shareGeneration_==generation){o.publicationState(phase,error,generation);Q_EMIT o.shareDiagnostics(report);}});
    }
    void announceLocked(const std::string& authenticatedOwner,const std::shared_ptr<livekit::RemoteTrackPublication>& p) {
        if(!p||stopping||publications.contains(p->sid()))return;
        const auto identity=QString::fromStdString(authenticatedOwner);
        const auto sid=QString::fromStdString(p->sid()),name=QString::fromStdString(p->name());
        const auto parsed=parseShareTrackName(name);
        const bool audio=p->kind()==livekit::TrackKind::KIND_AUDIO,encrypted=p->encryptionType()==livekit::EncryptionType::GCM;
        const bool screen=p->source()==(audio?livekit::TrackSource::SOURCE_SCREENSHARE_AUDIO:livekit::TrackSource::SOURCE_SCREENSHARE);
        const auto width=p->width(),height=p->height();
        if(!parsed||identity.isEmpty()||identity.size()>128||sid.isEmpty()||sid.size()>128||!encrypted||!screen)return;
        if(audio!=(parsed->track==ShareTrack::Audio))return;
        if(!audio&&(p->kind()!=livekit::TrackKind::KIND_VIDEO||width!=(parsed->track==ShareTrack::Video1080?1920u:1280u)||height!=(parsed->track==ShareTrack::Video1080?1080u:720u)))return;
        const auto change=catalog.announce({identity,sid,name,audio,encrypted,screen,width,height});
        if(!catalog.error().isEmpty()||publications.size()>=192){remoteFailureLocked();return;}
        if(!change.accepted)return;
        for(auto& [key,watch]:watches)if(key.first==identity&&watch.media->manifest().id==parsed->id&&watch.media->manifest().generation<parsed->generation){watch.media->revoke();receiveWakeLocked();}
        publications[p->sid()]={p,authenticatedOwner};
        notify([identity,sid,name,audio,encrypted,screen,width,height](RoomController& o){if(o.state_!=State::Leaving&&o.state_!=State::Failed)Q_EMIT o.trackAvailable(identity,sid,name,audio,encrypted,screen,width,height);});
    }
    void onTrackPublished(livekit::Room&,const livekit::TrackPublishedEvent& e)override {
        if(stopping||!e.publication||!e.participant)return;
        std::lock_guard lock(mutex);announceLocked(e.participant->identity(),e.publication);
    }
    void onUserPacketReceived(livekit::Room&,const livekit::UserDataPacketEvent& e)override {
        // Only our own bounded share-state topic is interpreted; everything else
        // in the room stays unread.
        if(stopping||!e.participant||e.topic!="gs-source")return;
        try {
            const auto document=QJsonDocument::fromJson(QByteArray(reinterpret_cast<const char*>(e.data.data()),static_cast<qsizetype>(e.data.size())));
            if(!document.isObject())return;
            const auto packet=document.object();
            const auto shareId=packet.value("share_id").toString();
            const auto minimized=packet.value("source_minimized");
            if(shareId.isEmpty()||shareId.size()>64||!minimized.isBool())return;
            const auto owner=QString::fromStdString(e.participant->identity());
            const bool state=minimized.toBool();
            notify([owner,shareId,state](RoomController& o){Q_EMIT o.sourceStateChanged(owner,shareId,state);});
        } catch(...) {}
    }    void onTrackUnpublished(livekit::Room&,const livekit::TrackUnpublishedEvent& e)override {
        testEvent("track_unpublished");
        if(stopping||!e.publication||!e.participant)return;const auto sid=e.publication->sid();
        {std::lock_guard lock(mutex);const auto publication=publications.find(sid);if(publication==publications.end()||publication->second.owner!=e.participant->identity())return;
         catalog.remove(QString::fromStdString(sid));publications.erase(publication);
         if(!catalog.error().isEmpty())remoteFailureLocked();
         for(auto& [key,watch]:watches)for(std::size_t i=0;i<2;i++)if(watch.publications[i]&&watch.publications[i]->sid()==sid){watch.state[i]=Subscription::Off;watch.media->revoke();}
         receiveWakeLocked();}
        notify([sid=QString::fromStdString(sid)](RoomController& o){Q_EMIT o.trackRemoved(sid);});
    }
    void onParticipantConnected(livekit::Room&,const livekit::ParticipantConnectedEvent& e)override {
        if(stopping||!e.participant)return;
        const auto identity=QString::fromStdString(e.participant->identity());
        const auto name=QString::fromStdString(e.participant->name());
        notify([identity,name](RoomController& o){Q_EMIT o.participantJoined(identity,name);});
    }
    void onParticipantDisconnected(livekit::Room&,const livekit::ParticipantDisconnectedEvent& e)override {
        testEvent("participant_disconnected");
        if(stopping||!e.participant)return;const auto identity=QString::fromStdString(e.participant->identity());
        {std::lock_guard lock(mutex);catalog.removeOwner(identity);if(!catalog.error().isEmpty())remoteFailureLocked();std::erase_if(publications,[&](const auto& entry){return entry.second.owner==e.participant->identity();});
         for(auto& [key,watch]:watches)if(key.first==identity){watch.media->revoke();watch.state={Subscription::Off,Subscription::Off};}receiveWakeLocked();}
        notify([identity](RoomController& o){Q_EMIT o.ownerRemoved(identity);});
    }
    void onTrackSubscribed(livekit::Room&,const livekit::TrackSubscribedEvent& e)override {
        if(stopping||!e.publication||!e.participant||!e.track)return;const auto sid=QString::fromStdString(e.publication->sid());
        {std::lock_guard lock(mutex);for(auto& [key,watch]:watches)for(std::size_t i=0;i<2;i++)if(watch.publications[i]&&watch.publications[i]->sid()==e.publication->sid()&&key.first==QString::fromStdString(e.participant->identity())&&watch.state[i]==Subscription::Requested){
            if(testLifecycle&&qEnvironmentVariableIntValue("GOODSHARING_TEST_SKIP_1080_SUBSCRIPTION")!=0&&watch.media->manifest().quality==ShareTrack::Video1080){testEvent("test_subscription_callback_withheld");return;}
            watch.state[i]=Subscription::On;if(!watch.media->closed())watch.media->subscribed(key.first,e.track);
         }receiveWakeLocked();}
        notify([sid](RoomController& o){if(o.state_==State::Ready)Q_EMIT o.subscriptionChanged(sid,true);});
    }
    void onTrackUnsubscribed(livekit::Room&,const livekit::TrackUnsubscribedEvent& e)override {
        if(stopping||!e.publication||!e.participant)return;const auto sid=QString::fromStdString(e.publication->sid());
        {std::lock_guard lock(mutex);for(auto& [key,watch]:watches)for(std::size_t i=0;i<2;i++)if(key.first==QString::fromStdString(e.participant->identity())&&watch.publications[i]&&watch.publications[i]->sid()==e.publication->sid()) {
            if(watch.state[i]==Subscription::Stopping)watch.state[i]=Subscription::Off;
            else if(watch.state[i]==Subscription::On){watch.media->unsubscribed(e.track);if(watch.media->closed())watch.state[i]=Subscription::Off;}
         }receiveWakeLocked();}
        notify([sid](RoomController& o){if(o.state_==State::Ready)Q_EMIT o.subscriptionChanged(sid,false);});
    }
    void onTrackSubscriptionFailed(livekit::Room&,const livekit::TrackSubscriptionFailedEvent& e)override {
        if(stopping||!e.participant)return;std::lock_guard lock(mutex);
        for(auto& [key,watch]:watches)for(std::size_t i=0;i<2;i++)if(key.first==QString::fromStdString(e.participant->identity())&&watch.publications[i]&&watch.publications[i]->sid()==e.track_sid&&watch.state[i]==Subscription::Requested){watch.state[i]=Subscription::Off;watch.media->revoke();}
        receiveWakeLocked();
    }
    void invalidate() {
        if(auto gate=activeGate.load())gate->close(true);
        revokeWatches();
        ready=false;invalidated=true;
        notify([](RoomController& o){Q_EMIT o.mediaInvalidated();});
    }
    void onReconnecting(livekit::Room&,const livekit::ReconnectingEvent&)override {
        testEvent("reconnecting");
        if(stopping)return;invalidate();state(State::Reconnecting,"Connection interrupted; sharing requires fresh encryption readiness");
    }
    void onReconnected(livekit::Room&,const livekit::ReconnectedEvent&)override {
        testEvent("reconnected");
        if(stopping)return;
        // Until publication recovery is integrated, fail closed and require a
        // fresh room. Do not resume old commands/tracks on stale readiness.
        invalidate();state(State::Failed,"Connection recovered; leave and rejoin before sharing");
    }
    void onDisconnected(livekit::Room&,const livekit::DisconnectedEvent&)override {
        testEvent("disconnected");
        if(stopping)return;invalidate();state(State::Failed,"Room disconnected; reconnect explicitly");
    }
    void onE2eeStateChanged(livekit::Room& room,const livekit::E2eeStateChangedEvent& e)override {
        testEvent("encryption_state",{{"state",static_cast<int>(e.state)}});
        const auto local=room.localParticipant().lock();
        if(stopping||!e.participant)return;
        if(!local||e.participant!=local.get()) {
            const auto identity=QString::fromStdString(e.participant->identity()),sid=QString::fromStdString(e.track_sid);
            std::lock_guard lock(mutex);for(auto& [key,watch]:watches)for(std::size_t i=0;i<2;i++)
                if(watch.state[i]==Subscription::On&&watch.publications[i]&&QString::fromStdString(watch.publications[i]->sid())==sid&&watch.media->accepts(identity,sid))watch.media->encryption(identity,sid,e.state);
            receiveWakeLocked();return;
        }
        const auto gate=activeGate.load();
        // A delayed error from a retired SID must not stop its replacement.
        // The gate also rejects events before the current manifest is bound;
        // no captured content can enter that generation in the meantime.
        if(!gate||!gate->observe(local->identity(),e.track_sid,e.state))return;
        if(e.state==livekit::EncryptionState::EncryptionFailed||e.state==livekit::EncryptionState::MissingKey||e.state==livekit::EncryptionState::InternalError) {
            invalidate();state(State::Failed,"Outgoing encryption failed; restart required");
        }
    }
};
RoomController::RoomController(QObject* parent):QObject(parent),shareId_(QUuid::createUuid().toString(QUuid::Id128)){}
RoomController::~RoomController(){if(input_){input_->gate()->close();input_->close();}++epoch_;impl_.reset();}
void RoomController::ownerThread()const{if(QThread::currentThread()!=thread())throw std::logic_error("Room controller API requires its Qt owner thread");}
void RoomController::transition(State state,QString message){state_=state;Q_EMIT stateChanged(state,std::move(message));}
bool RoomController::join(QString url,QString token,QByteArray key) {
    ownerThread();if(state_!=State::Idle&&state_!=State::Failed)return false;
    if(key.size()!=32||url.isEmpty()||token.isEmpty()){transition(State::Failed,"A room address, invitation and 32-byte client-held key are required");return false;}
    // A Failed worker can still be in SDK teardown. Require asynchronous leave
    // to finish before replacing its storage; never join it on the GUI thread.
    if(impl_)return false;
    ++epoch_;impl_=std::make_unique<Impl>(this,epoch_);transition(State::Joining);
    impl_->worker=std::jthread([p=impl_.get(),url=url.toStdString(),token=token.toStdString(),key=std::vector<std::uint8_t>(key.begin(),key.end())]()mutable{p->run(std::move(url),std::move(token),std::move(key));});
    return true;
}
void RoomController::leave() {
    ownerThread();stopShare();if(!impl_){transition(State::Idle);return;}
    if(state_==State::Leaving)return;
    transition(State::Leaving);impl_->revokeWatches();impl_->ready=false;impl_->userLeave=true;impl_->stopping=true;impl_->wake.notify_one();
}
void RoomController::publicationState(SharePublisher::State state,QString message,quint64 generation) {
    if(generation!=shareGeneration_)return;
    if(shareState_==SharePublisher::State::Stopping&&(state==SharePublisher::State::Starting||state==SharePublisher::State::Sharing))return;
    shareState_=state;Q_EMIT shareStateChanged(static_cast<int>(state),std::move(message),generation);
}
std::shared_ptr<ShareInput> RoomController::startShare(bool audio,std::int64_t maximumCaptureAgeUs,int fps,int bitrate1080,int bitrate720) {
    ownerThread();if(!impl_||state_!=State::Ready||shareState_==SharePublisher::State::Starting||shareState_==SharePublisher::State::Sharing||shareState_==SharePublisher::State::Stopping)return {};
    auto gate=std::make_shared<EncryptionGate>(++shareGeneration_);
    auto input=std::make_shared<ShareInput>(shareId_,audio,gate,fps);auto* p=impl_.get();
    SharePublisher::Config config{shareId_,shareGeneration_,audio,std::chrono::milliseconds(5000),maximumCaptureAgeUs,fps,bitrate1080,bitrate720};
    p->activeGate.store(gate);input_=input;publicationState(SharePublisher::State::Starting,{},shareGeneration_);
    if(!p->enqueue([p,config,input](livekit::Room& room){
        if(p->publisher)p->publisher->stop();
        p->publisher=std::make_unique<SharePublisher>(room,config,input);p->havePublisherState=false;
        p->publisher->start();p->publicationChanged();
    })) {gate->close(true);input->close();publicationState(SharePublisher::State::Failed,"Share command rejected",shareGeneration_);return {};}
    return input;
}
void RoomController::stopShare() {
    ownerThread();
    if(input_){input_->gate()->close();input_->close();}
    if(shareState_==SharePublisher::State::Starting||shareState_==SharePublisher::State::Sharing)
        publicationState(SharePublisher::State::Stopping,{},shareGeneration_);
    if(impl_)impl_->wake.notify_one();
}
bool RoomController::hasShareQuality(QString owner,QString id,int quality)const {
    ownerThread();
    if(!impl_)return false;
    std::lock_guard lock(impl_->mutex);
    return impl_->catalog.hasVideo(owner,id,quality);
}
quint64 RoomController::currentShareGeneration(QString owner,QString id)const {
    ownerThread();
    if(!impl_)return 0;
    std::lock_guard lock(impl_->mutex);
    return impl_->catalog.generation(owner,id).value_or(0);
}
void RoomController::refreshShareDiagnostics() {
    ownerThread();
    if(!impl_||state_!=State::Ready)return;
    QJsonObject report;
    {
        auto* p=impl_.get();
        std::lock_guard lock(p->mutex);
        if(!p->publisher)return;
        report=p->publisher->report();
    }
    Q_EMIT shareDiagnostics(report);
}
bool RoomController::publishShareSourceState(bool minimized) {
    ownerThread();if(!impl_||state_!=State::Ready)return false;
    auto* p=impl_.get();
    return p->enqueue([p,minimized](livekit::Room&){
        if(p->publisher&&p->publisher->state()==SharePublisher::State::Sharing)p->publisher->publishSourceState(minimized);
    });
}bool RoomController::execute(std::function<void(livekit::Room&)> operation){ownerThread();return operation&&impl_&&state_==State::Ready&&impl_->enqueue(std::move(operation));}
std::shared_ptr<RemoteShare> RoomController::watchShare(QString owner,QString id,quint64 generation,ShareTrack quality,bool audio,RemoteShare::Callbacks callbacks) {
    return watchImpl(std::move(owner),std::move(id),generation,quality,audio,std::move(callbacks),false);
}
std::shared_ptr<RemoteShare> RoomController::watchTrack(QString owner,QString id,quint64 generation,ShareTrack track,RemoteShare::Callbacks callbacks) {
    return watchImpl(std::move(owner),std::move(id),generation,track,track==ShareTrack::Audio,std::move(callbacks),true);
}
std::shared_ptr<RemoteShare> RoomController::watchImpl(QString owner,QString id,quint64 generation,ShareTrack quality,bool audio,RemoteShare::Callbacks callbacks,bool separate) {
    ownerThread();
    if(!impl_||state_!=State::Ready){watchError_="room not ready";return {};}
    if(quality==ShareTrack::Audio?(!separate||!callbacks.audio):(!callbacks.video||(audio&&!callbacks.audio))){watchError_="callback missing";return {};}
    auto* p=impl_.get();std::lock_guard lock(p->mutex);
    const RemoteShareCatalog::Key key{owner,separate?id+"/"+QString::number(static_cast<int>(quality)):id};
    if(!p->ready||p->stopping){watchError_="room busy";return {};}
    if(p->watches.contains(key)){watchError_="already watching";return {};}
    if(p->watches.size()>=9||p->commands.size()>=64){watchError_="backlog";return {};}
    std::set<RemoteShareCatalog::Key> selections;
    for(const auto& [otherKey,watch]:p->watches)selections.emplace(otherKey.first,watch.media->manifest().id);
    if(!selections.contains({owner,id})&&selections.size()>=3){watchError_="3-share limit";return {};}
    const auto manifest=p->catalog.manifest(owner,id,generation,quality,audio);
    if(!manifest){watchError_=QString("share changed (g%1; catalog 1080:%2 720:%3; %4)")
        .arg(generation)
        .arg(p->catalog.hasVideo(owner,id,1080)?"yes":"no")
        .arg(p->catalog.hasVideo(owner,id,720)?"yes":"no")
        .arg(p->catalog.manifestError());return {};}
    for(const auto& [otherKey,watch]:p->watches){const auto& m=watch.media->manifest();
        if((!manifest->videoSid.isEmpty()&&(m.videoSid==manifest->videoSid||m.audioSid==manifest->videoSid))||
           (!manifest->audioSid.isEmpty()&&(m.audioSid==manifest->audioSid||m.videoSid==manifest->audioSid))){
            watchError_="track already watched";return {};
        }
    }
    std::array<std::shared_ptr<livekit::RemoteTrackPublication>,2> publications;
    for(std::size_t i=0;i<2;i++) {
        const auto sid=(i?manifest->audioSid:manifest->videoSid).toStdString();if(sid.empty())continue;
        const auto it=p->publications.find(sid);
        if(it==p->publications.end()||it->second.owner!=owner.toStdString()){watchError_="not announced yet";return {};}
        publications[i]=it->second.track;
    }
    auto media=std::make_shared<RemoteShare>(*manifest,std::move(callbacks),p->receiveOpen);p->watches.emplace(key,Impl::Watch{media,publications});
    p->commands.push_back([p,key,media](livekit::Room&) {
        for(std::size_t i=0;i<2;i++) {
            std::shared_ptr<livekit::RemoteTrackPublication> publication;
            {std::lock_guard lock(p->mutex);const auto it=p->watches.find(key);
             if(it==p->watches.end()||it->second.media!=media||media->closed())return;
             auto& watch=it->second;publication=watch.publications[i];if(!publication)continue;
             watch.state[i]=Impl::Subscription::Requested;watch.deadline[i]=std::chrono::steady_clock::now()+std::chrono::seconds(5);}
            publication->setSubscribed(true);
        }
    });
    p->wake.notify_one();watchError_.clear();return media;
}
bool RoomController::unwatchShare(QString owner,QString id) {
    ownerThread();if(!impl_)return false;std::lock_guard lock(impl_->mutex);
    bool found=false;for(auto& [key,watch]:impl_->watches)if(key.first==owner&&watch.media->manifest().id==id){watch.media->revoke();found=true;}
    if(found)impl_->receiveWakeLocked();return found;
}
bool RoomController::unwatchTrack(const std::shared_ptr<RemoteShare>& media){
    ownerThread();if(!impl_||!media)return false;std::lock_guard lock(impl_->mutex);
    for(auto& [key,watch]:impl_->watches)if(watch.media==media){media->revoke();impl_->receiveWakeLocked();return true;}return false;
}
bool RoomController::setSubscribed(QString sid,bool subscribed) {
    ownerThread();if(!impl_)return false;auto* p=impl_.get();
    return execute([p,sid=sid.toStdString(),subscribed](livekit::Room&) {
        std::shared_ptr<livekit::RemoteTrackPublication> publication;
        {std::lock_guard lock(p->mutex);auto it=p->publications.find(sid);if(it==p->publications.end())return;publication=it->second.track;}
        publication->setSubscribed(subscribed);
    });
}
}


