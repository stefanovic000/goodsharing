#include "room_controller.h"
#include "share_model.h"
#include <livekit/livekit.h>
#include <livekit/local_participant.h>
#include <livekit/local_audio_track.h>
#include <QCoreApplication>
#include <QEventLoop>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTimer>
#include <QThread>
#include <atomic>
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <windows.h>
using namespace goodsharing;
using Clock=std::chrono::steady_clock;
void require(bool value,const char* message){if(!value)throw std::runtime_error(message);}
bool waitFor(const std::function<bool()>& predicate,int milliseconds=5000) {
    const auto deadline=Clock::now()+std::chrono::milliseconds(milliseconds);
    while(!predicate()&&Clock::now()<deadline){QCoreApplication::processEvents();std::this_thread::sleep_for(std::chrono::milliseconds(1));}
    QCoreApplication::processEvents();return predicate();
}
double elapsed(Clock::time_point start){return std::chrono::duration<double,std::milli>(Clock::now()-start).count();}
int main(int argc,char**argv) {
    QCoreApplication app(argc,argv);QJsonObject report;QJsonArray cycles;
    try {
        struct SDK{SDK(){livekit::initialize(livekit::LogLevel::Warn);}~SDK(){livekit::shutdown();}}sdk;
        const auto url=qEnvironmentVariable("GOODSHARING_TEST_URL"),token=qEnvironmentVariable("GOODSHARING_TEST_PUBLISHER_TOKEN");
        const auto key=QByteArray::fromHex(qgetenv("GOODSHARING_TEST_E2EE_KEY"));
        RoomController controller;
        int ticks=0;QTimer heartbeat;QObject::connect(&heartbeat,&QTimer::timeout,[&]{ticks++;});heartbeat.start(1);
        require(!controller.join(url,token,{})&&controller.state()==RoomController::State::Failed,"Missing key accepted");
        controller.leave();require(controller.state()==RoomController::State::Idle,"Empty leave failed");
        quint64 previous=0;
        for(int i=0;i<8;i++) {
            const auto joinStart=Clock::now();require(controller.join(url,token,key),"Join command rejected");const auto apiMs=elapsed(joinStart);
            require(!controller.join(url,token,key),"Concurrent join accepted");
            require(waitFor([&]{return controller.state()==RoomController::State::Ready||controller.state()==RoomController::State::Failed;})&&controller.state()==RoomController::State::Ready,"Asynchronous join failed");
            require(controller.epoch()>previous,"Room epoch was reused");previous=controller.epoch();
            struct Command {std::atomic_bool done=false,offQt=false;};auto command=std::make_shared<Command>();auto* qtThread=app.thread();
            require(controller.execute([command,qtThread](livekit::Room& room){command->offQt=QThread::currentThread()!=qtThread;require(room.connectionState()==livekit::ConnectionState::Connected,"Command ran without room");command->done=true;}),"Ready command rejected");
            require(waitFor([&]{return command->done.load();})&&command->offQt,"SDK command ran on Qt owner");
            const auto leaveStart=Clock::now();controller.leave();controller.leave();const auto leaveApiMs=elapsed(leaveStart);
            require(!controller.execute([](livekit::Room&){}),"Command admitted after leave");
            require(waitFor([&]{return controller.state()==RoomController::State::Idle;},2000),"Asynchronous leave failed");
            cycles.append(QJsonObject{{"cycle",i},{"join_api_ms",apiMs},{"leave_api_ms",leaveApiMs},{"leave_complete_ms",elapsed(leaveStart)},{"epoch",static_cast<double>(previous)}});
            require(apiMs<100&&leaveApiMs<100,"Qt API blocked on network operation");
        }
        const int idleSeconds=qEnvironmentVariableIntValue("GOODSHARING_CONTROLLER_IDLE_SECONDS");
        require(idleSeconds==0||(idleSeconds>=3&&idleSeconds<=30),"Use zero or 3 to 30 idle seconds");
        if(idleSeconds) {
            require(controller.join(url,token,key)&&waitFor([&]{return controller.state()==RoomController::State::Ready;}),"Idle room join failed");
            auto cpuSeconds=[] {
                FILETIME created{},exited{},kernel{},user{};
                require(GetProcessTimes(GetCurrentProcess(),&created,&exited,&kernel,&user),"Own CPU query failed");
                auto seconds=[](FILETIME t){return static_cast<double>((static_cast<std::uint64_t>(t.dwHighDateTime)<<32)|t.dwLowDateTime)/10000000.;};
                return seconds(kernel)+seconds(user);
            };
            // Suspend this harness's 1 ms heartbeat and use a real event loop;
            // do not measure the waitFor() inspection poll as client idle work.
            heartbeat.stop();QEventLoop idleLoop;QTimer::singleShot(idleSeconds*1000,&idleLoop,&QEventLoop::quit);
            const auto cpuStart=cpuSeconds();const auto idleStart=Clock::now();idleLoop.exec();
            const auto duration=std::chrono::duration<double>(Clock::now()-idleStart).count();
            const auto cores=(cpuSeconds()-cpuStart)/duration;heartbeat.start(1);
            const auto processors=std::max(1u,std::thread::hardware_concurrency());
            auto awake=std::make_shared<std::atomic_bool>(false);const auto wakeStart=Clock::now();
            require(controller.execute([awake](livekit::Room&){*awake=true;})&&waitFor([&]{return awake->load();},500),"Idle worker lost a command notification");
            const auto wakeMs=elapsed(wakeStart);
            report["idle_room"]=QJsonObject{{"duration_s",duration},{"logical_processors",static_cast<int>(processors)},
                {"cpu_core_equivalents",cores},{"cpu_machine_percent",100*cores/processors},{"wake_command_complete_ms",wakeMs}};
            require(wakeMs<100,"Idle room wake blocked");
            controller.leave();require(waitFor([&]{return controller.state()==RoomController::State::Idle;}),"Idle room cleanup failed");
        }
        // Two independent controllers: announced encrypted audio, explicit
        // subscribe/unsubscribe, model grouping and remote owner cleanup.
        {
            RoomController viewer;ShareModel model;QString remoteSid;int subscribed=0,unsubscribed=0;
            QObject::connect(&viewer,&RoomController::trackAvailable,[&](QString owner,QString sid,QString name,bool audio,bool encrypted,bool screen,quint32 w,quint32 h){
                if(model.announce({owner,sid,name,audio,encrypted,screen,w,h}))remoteSid=sid;
            });
            QObject::connect(&viewer,&RoomController::ownerRemoved,&model,&ShareModel::removeOwner);
            QObject::connect(&viewer,&RoomController::subscriptionChanged,[&](QString sid,bool active){if(sid==remoteSid){if(active)subscribed++;else unsubscribed++;}});
            require(viewer.join(url,qEnvironmentVariable("GOODSHARING_TEST_SUBSCRIBER_TOKEN"),key),"Viewer join rejected");
            require(waitFor([&]{return viewer.state()==RoomController::State::Ready;}),"Viewer join failed");
            require(controller.join(url,token,key)&&waitFor([&]{return controller.state()==RoomController::State::Ready;}),"Publisher rejoin failed");
            struct Publication {std::shared_ptr<livekit::AudioSource> source;std::shared_ptr<livekit::LocalAudioTrack> track;std::atomic_bool done=false;};
            auto publication=std::make_shared<Publication>();
            require(controller.execute([publication](livekit::Room& room){
                publication->source=std::make_shared<livekit::AudioSource>(48000,2);
                publication->track=livekit::LocalAudioTrack::createLocalAudioTrack(shareTrackName({"0123456789abcdef0123456789abcdef",1,ShareTrack::Audio}).toStdString(),publication->source);
                livekit::TrackPublishOptions options;options.source=livekit::TrackSource::SOURCE_SCREENSHARE_AUDIO;options.dtx=false;
                room.localParticipant().lock()->publishTrack(publication->track,options);
                publication->source->captureFrame(livekit::AudioFrame::create(48000,2,480),100);publication->done=true;
            }),"Publication command rejected");
            require(waitFor([&]{return publication->done.load()&&!remoteSid.isEmpty();}),"Encrypted announcement did not reach Qt model");
            require(model.rowCount()==1&&model.data(model.index(0),ShareModel::HasAudioRole).toBool(),"Announced audio was not grouped");
            require(viewer.setSubscribed(remoteSid,true)&&waitFor([&]{return subscribed==1;}),"Explicit subscription failed");
            require(viewer.setSubscribed(remoteSid,false)&&waitFor([&]{return unsubscribed==1;}),"Explicit unsubscribe failed");
            viewer.leave();require(waitFor([&]{return viewer.state()==RoomController::State::Idle;}),"Viewer leave before late join failed");
            model.clear();remoteSid.clear();
            require(viewer.join(url,qEnvironmentVariable("GOODSHARING_TEST_SUBSCRIBER_TOKEN"),key),"Late viewer join rejected");
            require(waitFor([&]{return viewer.state()==RoomController::State::Ready&&!remoteSid.isEmpty();}),"Late join did not discover existing publication");
            require(viewer.setSubscribed(remoteSid,true)&&waitFor([&]{return subscribed==2;}),"Late-join subscription failed");
            require(viewer.setSubscribed(remoteSid,false)&&waitFor([&]{return unsubscribed==2;}),"Late-join unsubscribe failed");
            controller.leave();require(waitFor([&]{return controller.state()==RoomController::State::Idle&&model.rowCount()==0;}),"Remote owner cleanup failed");
            publication.reset();viewer.leave();require(waitFor([&]{return viewer.state()==RoomController::State::Idle;}),"Viewer cleanup failed");
            report["announcement_subscription_cleanup_passed"]=true;
            report["late_join_snapshot_passed"]=true;
        }
        if(qEnvironmentVariableIntValue("GOODSHARING_CONTROLLER_PUBLISHER")) {
            QJsonObject diagnostics;QJsonArray announcements;ShareModel model;QObject callbackContext;RoomController viewer;
            QObject::connect(&controller,&RoomController::shareDiagnostics,&callbackContext,[&](QJsonObject value){diagnostics=std::move(value);});
            QObject::connect(&viewer,&RoomController::trackAvailable,&callbackContext,[&](QString owner,QString sid,QString name,bool audio,bool encrypted,bool screen,quint32 width,quint32 height){announcements.append(QJsonObject{{"name",name},{"width",static_cast<int>(width)},{"height",static_cast<int>(height)}});model.announce({owner,sid,name,audio,encrypted,screen,width,height});});
            QObject::connect(&viewer,&RoomController::trackRemoved,&model,[&](QString sid){model.remove(sid);});
            QObject::connect(&viewer,&RoomController::ownerRemoved,&model,&ShareModel::removeOwner);
            require(controller.join(url,token,key)&&waitFor([&]{return controller.state()==RoomController::State::Ready;}),"Owned-publisher join failed");
            require(viewer.join(url,qEnvironmentVariable("GOODSHARING_TEST_SUBSCRIBER_TOKEN"),key)&&waitFor([&]{return viewer.state()==RoomController::State::Ready;}),"Publisher observer join failed");
            std::shared_ptr<ShareInput> previousInput;std::vector<std::string> previousSids;quint64 generation=0;double maximumStop=0;
            for(int round=0;round<2;round++) {
                auto input=controller.startShare(round==1);require(bool(input),"Owned publisher command rejected");
                require(controller.shareGeneration()>generation,"Share generation reused");generation=controller.shareGeneration();
                if(previousInput) {
                    require(previousInput->gate()->closed(),"Old publisher input reopened");
                    for(const auto& sid:previousSids)require(!input->gate()->observe("publisher",sid,livekit::EncryptionState::Ok),"Old handler event changed new generation");
                }
                const bool sharing=waitFor([&]{return controller.shareState()==SharePublisher::State::Sharing||controller.state()==RoomController::State::Failed;},8000);
                report["publisher_diagnostics"]=diagnostics;report["publisher_announcements"]=announcements;
                require(sharing&&controller.shareState()==SharePublisher::State::Sharing,"Owned publisher failed readiness");
                const auto manifest=diagnostics["gate"].toObject()["tracks"].toArray();require(manifest.size()==(round?3:2),"Wrong owned-publisher handler count");
                previousSids.clear();for(const auto& value:manifest){const auto entry=value.toObject();require(entry["ready"].toBool(),"Owned handler not ready");previousSids.push_back(entry["sid"].toString().toStdString());}
                const bool grouped=waitFor([&]{return model.rowCount()==1&&model.data(model.index(0),ShareModel::Has1080Role).toBool()&&model.data(model.index(0),ShareModel::Has720Role).toBool()&&model.data(model.index(0),ShareModel::HasAudioRole).toBool()==(round==1);},2000);
                report["publisher_announcements"]=announcements;require(grouped,"Published video/audio did not reach grouped model");
                if(round==1) {
                    // Wake an idle audio consumer, let it sleep again, then Stop.
                    // This catches lost input notifications and shutdown hangs.
                    const auto audioNow=static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(Clock::now().time_since_epoch()).count());
                    require(input->audio({audioNow,std::vector<std::int16_t>(960)}),"Idle audio input rejected");
                    require(waitFor([&]{return input->report()["pending_audio_ms"].toInt()==0;},500),"Idle audio worker did not wake for input");
                    require(controller.shareState()==SharePublisher::State::Sharing,"Audio wake stopped the share");
                    report["audio_idle_wake_passed"]=true;
                }
                ShareVideoInput frame;frame.identity={static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(Clock::now().time_since_epoch()).count()),42,false};frame.nv12[0].assign(1920*1080*3/2,90);frame.nv12[1].assign(1280*720*3/2,90);
                require(input->video(std::move(frame)),"Ready owner input rejected");
                const auto stopStart=Clock::now();controller.stopShare();require(input->gate()->closed(),"Stop did not immediately revoke capture");
                require(waitFor([&]{return controller.shareState()==SharePublisher::State::Stopped;},1000),"Owned publisher Stop failed");maximumStop=std::max(maximumStop,elapsed(stopStart));
                require(controller.state()==RoomController::State::Ready,"Normal share Stop left the room");
                auto publications=std::make_shared<std::atomic_int>(-1);require(controller.execute([publications](livekit::Room& room){*publications=static_cast<int>(room.localParticipant().lock()->trackPublications().size());}),"Publication cleanup query rejected");
                require(waitFor([&]{return publications->load()>=0;})&&publications->load()==0,"Owned publisher left orphan tracks");
                previousInput=std::move(input);
            }
            auto cancelled=controller.startShare(false);require(bool(cancelled),"Cancel-start command rejected");controller.stopShare();require(waitFor([&]{return controller.shareState()==SharePublisher::State::Stopped;},2000),"Queued publisher cancellation failed");
            auto abandoned=controller.startShare(true);require(bool(abandoned)&&waitFor([&]{return controller.shareState()==SharePublisher::State::Sharing;},8000),"Leave-while-sharing setup failed");
            controller.leave();require(abandoned->gate()->closed()&&waitFor([&]{return controller.state()==RoomController::State::Idle&&controller.shareState()==SharePublisher::State::Stopped;},2000),"Room leave did not stop owned publisher");
            viewer.leave();require(waitFor([&]{return viewer.state()==RoomController::State::Idle;}),"Publisher observer cleanup failed");
            report["owned_publisher_passed"]=true;report["owned_publisher_max_stop_ms"]=maximumStop;
        }
        require(controller.join(url,"invalid-test-token",key),"Invalid-token check did not start");
        require(waitFor([&]{return controller.state()==RoomController::State::Failed;}),"Invalid token was not rejected");
        controller.leave();require(waitFor([&]{return controller.state()==RoomController::State::Idle;}),"Rejected join cleanup failed");report["invalid_token_rejected"]=true;
        const auto stalled=qEnvironmentVariable("GOODSHARING_CANCEL_URL");
        require(!stalled.isEmpty()&&controller.join(stalled,token,key),"Stalled join did not start");
        require(waitFor([&]{return QFile::exists(qEnvironmentVariable("GOODSHARING_CANCEL_ACCEPTED"));},3000),"Stall fixture did not receive handshake");
        const int before=ticks;const auto cancelStart=Clock::now();controller.leave();const auto cancelApiMs=elapsed(cancelStart);
        require(waitFor([&]{return controller.state()==RoomController::State::Idle;},2000),"Stalled join cancellation exceeded bound");
        report["cancel_complete_ms"]=elapsed(cancelStart);report["cancel_api_ms"]=cancelApiMs;report["qt_ticks_during_cancel"]=ticks-before;
        require(cancelApiMs<100&&ticks-before>=10,"Qt event loop stalled during network cancellation");
        report["cycles"]=cycles;report["passed"]=true;
    }catch(const std::exception& e){report["error"]=QString::fromUtf8(e.what());report["cycles"]=cycles;report["passed"]=false;}
    const auto data=QJsonDocument(report).toJson();std::cout<<data.constData();
    const auto args=app.arguments();const int at=args.indexOf("--report");if(at>=0&&at+1<args.size()){QFile file(args[at+1]);if(file.open(QIODevice::WriteOnly))file.write(data);}
    return report["passed"].toBool()?0:1;
}
