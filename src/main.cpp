#include "preview_backend.h"
#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QFile>
#include <QSaveFile>
#include <QElapsedTimer>
#include <QDebug>
#include <livekit/livekit.h>
#include "../tools/own_audio_observer.h"
#include <QThread>
#include <algorithm>
#include <chrono>

int main(int argc,char** argv) {
    QGuiApplication app(argc,argv); app.setApplicationName("GoodSharing"); app.setOrganizationName("GoodSharing");
    struct SDK {SDK(){livekit::initialize(livekit::LogLevel::Warn);}~SDK(){livekit::shutdown();}} sdk;
    QQuickStyle::setStyle("Basic"); qmlRegisterType<VideoItem>("GoodSharing",1,0,"VideoSurface");
    PreviewBackend backend;
    QQmlApplicationEngine engine; engine.rootContext()->setContextProperty("preview",&backend);
    engine.load(QUrl("qrc:/ui/Main.qml")); if(engine.rootObjects().isEmpty()) return 1;
    if(auto* surfaceItem=engine.rootObjects().first();qEnvironmentVariableIsSet("GOODSHARING_TEST_UI_PANEL")&&surfaceItem)
        surfaceItem->setProperty("panelOpen",true);
    auto* window=qobject_cast<QQuickWindow*>(engine.rootObjects().first());
    backend.setVideoItem(window->findChild<VideoItem*>("videoSurface"));
    backend.setRemoteVideoItem(window->findChild<VideoItem*>("remoteVideoSurface"));
    backend.setExtraVideoItem(1,window->findChild<VideoItem*>("remoteVideoSurface1"));
    backend.setExtraVideoItem(2,window->findChild<VideoItem*>("remoteVideoSurface2"));
    auto* remoteSurface=window->findChild<VideoItem*>("remoteVideoSurface");
    QObject::connect(window,&QQuickWindow::frameSwapped,window,[remoteSurface]{if(remoteSurface)remoteSurface->frameQueued();},Qt::DirectConnection);
    QObject::connect(&app,&QCoreApplication::aboutToQuit,&backend,&PreviewBackend::stopPreview);
    const auto args=app.arguments();
    if(args.contains("--test-window-minimized")&&!args.contains("--test-view-visible"))window->showMinimized();
    auto value=[&](const QString& name,QString fallback={}) { int i=args.indexOf(name); return i>=0&&i+1<args.size()?args[i+1]:fallback; };
    // Join-only launch: opens the normal window already in the room, so no token
    // has to be typed. Sharing is still a click, as it should be.
    if(args.contains("--join")) {
        const auto url=qEnvironmentVariable("GOODSHARING_JOIN_URL"),token=qEnvironmentVariable("GOODSHARING_JOIN_TOKEN"),key=qEnvironmentVariable("GOODSHARING_JOIN_KEY");
        qunsetenv("GOODSHARING_JOIN_URL");qunsetenv("GOODSHARING_JOIN_TOKEN");qunsetenv("GOODSHARING_JOIN_KEY");
        if(token.isEmpty()||key.size()!=64){qCritical()<<"--join needs GOODSHARING_JOIN_URL, GOODSHARING_JOIN_TOKEN and GOODSHARING_JOIN_KEY";return 2;}
        QTimer::singleShot(0,&backend,[&backend,url,token,key]{backend.joinRoom(url,token,key);});
    }
    if(args.contains("--local-demo")) {
        const auto url=qEnvironmentVariable("GOODSHARING_DEMO_URL"),token=qEnvironmentVariable("GOODSHARING_DEMO_TOKEN"),key=qEnvironmentVariable("GOODSHARING_DEMO_KEY");
        qunsetenv("GOODSHARING_DEMO_URL");qunsetenv("GOODSHARING_DEMO_TOKEN");qunsetenv("GOODSHARING_DEMO_KEY");
        if(url!="ws://127.0.0.1:8765"||token.isEmpty()||key.size()!=64){qCritical()<<"Start the local demo with scripts/start-local-demo.ps1";return 2;}
        backend.enableTestAvPlayback();
        window->setTitle("GoodSharing · Local demo · "+value("--demo-role","Client"));
        QTimer::singleShot(0,&backend,[&backend,url,token,key]{backend.joinRoom(url,token,key);});
        const auto smokeReport=value("--demo-smoke-report");
        if(!smokeReport.isEmpty())QTimer::singleShot(7000,&app,[&]{
            const bool passed=backend.roomReady()&&!backend.active()&&!backend.shareActive()&&!backend.watching()&&backend.testAvPlayback();
            const QJsonObject report{{"passed",passed},{"room_ready",backend.roomReady()},{"capture_active",backend.active()},
                {"share_active",backend.shareActive()},{"watching",backend.watching()},{"audio_video_enabled",backend.testAvPlayback()}};
            QSaveFile file(value("--demo-smoke-report"));
            const auto bytes=QJsonDocument(report).toJson();
            const bool saved=file.open(QIODevice::WriteOnly)&&file.write(bytes)==bytes.size()&&file.commit();
            app.exit(passed&&saved?0:3);
        });
    }
    if(args.contains("--smoke-watch")) {
        const bool av=args.contains("--test-av-playback");if(av)backend.enableTestAvPlayback();
        if(av&&args.contains("--test-view-visible")){window->setFlag(Qt::WindowStaysOnTopHint);window->show();}
        // Test-only observation; a grab never substitutes for a media ticket.
        const auto markerReport=[window](const QImage& image){
            auto* surface=window->findChild<VideoItem*>("remoteVideoSurface");QImage pixels;
            if(surface&&surface->isVisible()){
                const auto point=surface->mapToScene(QPointF{});pixels=image.copy(QRect(qRound(point.x()),qRound(point.y()),qRound(surface->width()),qRound(surface->height())));
            }
            bool markers=!pixels.isNull();unsigned pixelId=0;
            if(markers){
                QSizeF size(960,540);size.scale(pixels.size(),Qt::KeepAspectRatio);const double left=(pixels.width()-size.width())/2,top=(pixels.height()-size.height())/2;
                for(int bit=0;bit<16;bit++){
                    const int x=qBound(0,qRound(left+(bit*40+20)*size.width()/960),pixels.width()-1);
                    const int y=qBound(0,qRound(top+20*size.height()/540),pixels.height()-1),inverse=qBound(0,qRound(top+60*size.height()/540),pixels.height()-1);
                    const bool a=pixels.pixelColor(x,y).red()>128,b=pixels.pixelColor(x,inverse).red()>128;markers&=a!=b;if(a)pixelId|=1u<<bit;
                }
            }
            return QJsonObject{{"composited_marker_valid",markers},{"composited_pixel_id",static_cast<int>(pixelId)}};
        };
        auto checkpoints=std::make_shared<QJsonArray>();
        auto maximumGuiGap=std::make_shared<qint64>(0);
        if(av&&(args.contains("--test-av-checkpoints")||args.contains("--test-paint-stall")||args.contains("--test-av-switch")||args.contains("--test-av-switch-cancel")||args.contains("--test-av-switch-source-exit"))){
            auto elapsed=std::make_shared<QElapsedTimer>();elapsed->start();
            auto lastTick=std::make_shared<qint64>(0);auto* heartbeat=new QTimer(&app);heartbeat->setTimerType(Qt::PreciseTimer);heartbeat->setInterval(16);
            QObject::connect(heartbeat,&QTimer::timeout,&app,[elapsed,lastTick,maximumGuiGap]{const auto now=elapsed->elapsed();*maximumGuiGap=std::max(*maximumGuiGap,now-*lastTick);*lastTick=now;});heartbeat->start();
            if(args.contains("--test-av-checkpoints"))for(const int at:{12000,14000,17000})QTimer::singleShot(at,Qt::PreciseTimer,&app,[&,checkpoints,elapsed,maximumGuiGap]{
                QElapsedTimer observing;observing.start();
                auto report=backend.viewingReport();
                report["receiver_local_us"]=static_cast<double>(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
                report["observation_duration_ms"]=observing.elapsed();report["maximum_gui_tick_gap_ms"]=*maximumGuiGap;
                report["test_elapsed_ms"]=elapsed->elapsed();checkpoints->append(report);
                const auto path=value("--test-av-checkpoint-file");
                if(!path.isEmpty()){QSaveFile file(path);if(file.open(QIODevice::WriteOnly)){file.write(QJsonDocument(*checkpoints).toJson());file.commit();}}
            });
        }
        auto audioObserver=av?std::make_shared<goodsharing_test::OwnAudioObserver>():nullptr;
        auto paintStall=std::make_shared<bool>(false);
        auto stallObservations=std::make_shared<QJsonArray>();
        if(av&&args.contains("--test-paint-stall")){
            auto* waiting=new QTimer(&app);waiting->setInterval(100);
            auto observe=[&backend,stallObservations](QString phase){auto sample=backend.viewingReport();sample["phase"]=phase;
                sample["receiver_local_us"]=static_cast<double>(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());stallObservations->append(sample);};
            QObject::connect(waiting,&QTimer::timeout,&app,[&app,&backend,waiting,paintStall,observe]{
                if(!backend.testAvReady())return;waiting->stop();observe("before");*paintStall=true;QThread::msleep(220);observe("after_block");
                for(const int delay:{100,500,2000})QTimer::singleShot(delay,Qt::PreciseTimer,&app,[observe,delay]{observe(QString("recovery_%1_ms").arg(delay));});
            });waiting->start();
        }
        backend.joinRoom(qEnvironmentVariable("GOODSHARING_TEST_URL"),qEnvironmentVariable("GOODSHARING_TEST_SUBSCRIBER_TOKEN"),qEnvironmentVariable("GOODSHARING_TEST_E2EE_KEY"));
        auto requested=std::make_shared<bool>(false);auto beforeSwitch=std::make_shared<QJsonObject>();
        const int quality=value("--quality","720").toInt();auto* starting=new QTimer(&app);starting->setInterval(100);
        QObject::connect(starting,&QTimer::timeout,&app,[&,starting,requested,quality,av]{
            if(*requested||!backend.roomReady()||backend.remoteShares().isEmpty())return;
            const auto choice=backend.remoteShares().first().toMap();if(!choice.value(quality==1080?"has1080":"has720").toBool())return;
            if(av&&!choice.value("hasAudio").toBool())return;
            backend.watchShare(0,quality);*requested=true;starting->stop();
        });starting->start();
        if(args.contains("--view-ready-file")){
            auto* observed=new QTimer(&app);observed->setInterval(50);
            QObject::connect(observed,&QTimer::timeout,&app,[&,observed,av]{
                const auto report=backend.viewingReport();const auto drawn=report["video"].toObject()["drawn_distinct_frames"].toInt();
                if(!backend.watching()||drawn<120||(av&&!backend.testAvReady()))return;
                const auto audio=report["playback"].toObject()["audio"].toObject();
                QSaveFile file(value("--view-ready-file"));if(file.open(QIODevice::WriteOnly)){file.write(QJsonDocument(QJsonObject{{"drawn_frames",drawn},{"av_required",av},
                    {"committed_media_frames",audio["committed_media_frames"]},{"presented_video_frames",audio["presented_video_frames"]}}).toJson());if(file.commit())observed->stop();}
            });observed->start();
        }
        auto switchObservations=std::make_shared<QJsonArray>();
        struct SwitchProbe{int phase=0,interval=0;std::int64_t deadline=0;bool complete=false;};auto switches=std::make_shared<SwitchProbe>();
        if(av&&(args.contains("--test-av-switch")||args.contains("--test-av-switch-cancel")||args.contains("--test-av-switch-source-exit"))){
            const bool cancel=args.contains("--test-av-switch-cancel"),sourceExit=args.contains("--test-av-switch-source-exit");
            const bool subscriptionTimeout=qEnvironmentVariableIntValue("GOODSHARING_TEST_SKIP_1080_SUBSCRIPTION")!=0;
            auto* monitor=new QTimer(&app);monitor->setInterval(50);
            auto observe=[&backend,switchObservations,maximumGuiGap](QString phase){auto sample=backend.viewingReport();sample["phase"]=phase;sample["maximum_gui_tick_gap_ms"]=*maximumGuiGap;
                sample["receiver_local_us"]=static_cast<double>(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());switchObservations->append(sample);};
            QObject::connect(monitor,&QTimer::timeout,&app,[&,monitor,switches,observe,beforeSwitch,audioObserver,quality,cancel,sourceExit,subscriptionTimeout]{
                const auto now=std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
                const auto report=backend.viewingReport();const auto count=report["committed_handovers"].toInt();
                if(switches->phase==0){if(!backend.testAvReady())return;*beforeSwitch=report;observe("before_first");switches->interval=audioObserver->beginInterval();
                    backend.watchShare(0,quality==720?1080:720);switches->deadline=now+200000;switches->phase=1;}
                else if(switches->phase==1){
                    if(sourceExit&&!report["candidate_watch"].toObject().isEmpty()){
                        observe("candidate_warming");QSaveFile file(value("--switch-stage-ready-file"));if(file.open(QIODevice::WriteOnly)){file.write(QJsonDocument(QJsonObject{{"candidate_staged",true},{"drawn_frames",report["video"].toObject()["drawn_distinct_frames"]}}).toJson());file.commit();}switches->phase=6;
                    }else if(cancel&&now>=switches->deadline&&!report["candidate_watch"].toObject().isEmpty()){
                        observe("before_cancel");backend.watchShare(0,quality);observe("after_cancel");switches->deadline=now+2000000;switches->phase=5;
                    }else if(subscriptionTimeout&&report["cancelled_handovers"].toInt()==1&&backend.watchedQuality()==quality&&backend.watching()){
                        observe("after_timeout");switches->deadline=now+2000000;switches->phase=5;
                    }else if(!cancel&&!sourceExit&&count==1&&backend.watchedQuality()!=quality){observe("after_first");switches->deadline=now+1000000;switches->phase=2;}
                }else if(switches->phase==2&&now>=switches->deadline){
                    audioObserver->endInterval(switches->interval);observe("before_return");switches->interval=audioObserver->beginInterval();backend.watchShare(0,quality);switches->phase=3;
                }else if(switches->phase==3&&count==2&&backend.watchedQuality()==quality){observe("after_return");switches->deadline=now+1000000;switches->phase=4;}
                else if((switches->phase==4||switches->phase==5)&&now>=switches->deadline){audioObserver->endInterval(switches->interval);observe("completed");switches->complete=true;monitor->stop();}
                else if(switches->phase==6&&!backend.watching()&&!backend.watchBusy()){audioObserver->endInterval(switches->interval);observe("source_closed");switches->complete=true;monitor->stop();}
            });monitor->start();
        }
        if(args.contains("--smoke-switch")&&!av)QTimer::singleShot(5000,&app,[&,beforeSwitch,quality]{*beforeSwitch=backend.viewingReport();backend.watchShare(0,quality==720?1080:720);});
        const auto watchDuration=qBound(1000,value("--watch-duration-ms",av?"18000":"9000").toInt(),60000);
        QTimer::singleShot(watchDuration,&app,[&,requested,beforeSwitch,audioObserver,paintStall,stallObservations,checkpoints,markerReport,maximumGuiGap,switches,switchObservations]{
            // A grab can catch a frame between paints; retry a couple of times so the
            // marker observation reflects the picture instead of the grab timing.
            QImage image;QJsonObject pixels;
            for(int attempt=0;attempt<3;attempt++){
                image=window->grabWindow();pixels=markerReport(image);
                if(pixels["composited_marker_valid"].toBool()||attempt==2)break;
                QThread::msleep(60);
            }
            const bool saved=image.save(value("--screenshot"));
            auto report=backend.viewingReport();report["requested"]=*requested;report["screenshot_saved"]=saved;report["before_switch"]=*beforeSwitch;
            if(auto* control=window->findChild<QObject*>("watchQuality"))report["quality_control"]=control->property("currentText").toString();
            report["composited_marker_valid"]=pixels["composited_marker_valid"];report["composited_pixel_id"]=pixels["composited_pixel_id"];
            report["av_checkpoints"]=*checkpoints;report["paint_stall_performed"]=*paintStall;
            report["paint_stall_observations"]=*stallObservations;
            report["switch_observations"]=*switchObservations;report["switch_complete"]=switches->complete;report["switch_phase"]=switches->phase;
            report["maximum_test_gui_tick_gap_ms"]=*maximumGuiGap;
            if(audioObserver)audioObserver->cut();QElapsedTimer stopping;stopping.start();backend.stopWatching();report["watch_stop_ms"]=stopping.elapsed();
            QTimer::singleShot(500,&app,[&,report=std::move(report),audioObserver]()mutable{
                if(audioObserver){audioObserver->stop();report["audio_observer"]=audioObserver->report();}
                report["after_stop"]=backend.viewingReport();backend.leaveRoom();
                QTimer::singleShot(500,&app,[&,report=std::move(report)]()mutable{
                    report["room_idle"]=!backend.roomBusy();QFile file(value("--report"));if(file.open(QIODevice::WriteOnly))file.write(QJsonDocument(report).toJson());app.exit(report["requested"].toBool()&&report["screenshot_saved"].toBool()&&report["room_idle"].toBool()?0:3);
                });
            });
        });
    }else if(args.contains("--smoke-share-hwnd")) {
        backend.joinRoom(qEnvironmentVariable("GOODSHARING_TEST_URL"),qEnvironmentVariable("GOODSHARING_TEST_PUBLISHER_TOKEN"),qEnvironmentVariable("GOODSHARING_TEST_E2EE_KEY"));
        auto started=std::make_shared<bool>(false);auto startingBadge=std::make_shared<QString>();auto* starting=new QTimer(&app);starting->setInterval(100);
        const auto startedAt=std::make_shared<qint64>(QDateTime::currentMSecsSinceEpoch());
        QObject::connect(starting,&QTimer::timeout,&app,[&,starting,started,startingBadge,startedAt]{
            if(*started||!backend.roomReady())return;
            *started=backend.startControlledShare(value("--smoke-share-hwnd").toULongLong(),value("--smoke-pid").toUInt(),!args.contains("--audio-off"));
            *startingBadge=backend.shareBadge();
            if(!*started&&QDateTime::currentMSecsSinceEpoch()-*startedAt<6000)return;
            starting->stop();if(!*started)app.exit(2);
        });starting->start();
        const auto shareDuration=qBound(1000,value("--smoke-duration-ms","11000").toInt(),60000);
        QTimer::singleShot(shareDuration,&app,[&,started,startingBadge]{
            const auto image=window->grabWindow();const bool saved=image.save(value("--screenshot"));
            auto report=backend.sharingReport();report["started"]=*started;report["screenshot_saved"]=saved;
            report["badge_when_requested"]=*startingBadge;report["badge_at_capture_check"]=backend.shareBadge();
            report["capture_was_active"]=backend.active();backend.stopSharing();
            QTimer::singleShot(1000,&app,[&,report=std::move(report)]()mutable{
                report["after_stop"]=backend.sharingReport();backend.leaveRoom();
                QFile file(value("--report"));if(file.open(QIODevice::WriteOnly))file.write(QJsonDocument(report).toJson());
                app.exit(report["started"].toBool()&&report["capture_was_active"].toBool()&&report["captured_frames"].toInt()>100&&report["screenshot_saved"].toBool()?0:3);
            });
        });
    }else if(args.contains("--smoke-hwnd")) {
        const auto scenario=value("--smoke-scenario","normal");
        auto lifecyclePassed=std::make_shared<bool>(true);
        auto stopTime=std::make_shared<qint64>(0);
        QTimer::singleShot(500,&app,[&] {
            if(!backend.startControlledSource(value("--smoke-hwnd").toULongLong(),value("--smoke-pid").toUInt())) app.exit(2);
        });
        if(scenario=="restart") {
            QTimer::singleShot(2500,&app,[&,lifecyclePassed,stopTime] {
                *lifecyclePassed=backend.active()&&backend.capturedFrames()>50;
                QElapsedTimer elapsed; elapsed.start(); backend.stopPreview(); *stopTime=elapsed.elapsed();
                *lifecyclePassed=*lifecyclePassed&&!backend.active()&&*stopTime<1000;
            });
            QTimer::singleShot(2800,&app,[&,lifecyclePassed] { *lifecyclePassed=*lifecyclePassed&&backend.startControlledSource(value("--smoke-hwnd").toULongLong(),value("--smoke-pid").toUInt()); });
        }
        QTimer::singleShot(5500,&app,[&,scenario,lifecyclePassed,stopTime] {
            const int frames=backend.capturedFrames();
            const auto image=window->grabWindow(); const auto screenshot=value("--screenshot"); bool imageSaved=false;
            if(!screenshot.isEmpty()) imageSaved=image.save(screenshot);
            const bool expected=scenario=="source-exit"?(!backend.active()&&backend.status().contains("closed")):(backend.active()&&frames>(scenario=="restart"?100:200));
            QJsonObject report{{"scenario",scenario},{"captured_frames",frames},{"active",backend.active()},{"lifecycle_passed",*lifecyclePassed&&expected},{"screenshot_saved",imageSaved},{"window_width",window->width()},{"window_height",window->height()}};
            QElapsedTimer elapsed; elapsed.start(); backend.stopPreview();
            report["stop_ms"]=elapsed.elapsed(); report["stopped"]=!backend.active(); report["restart_stop_ms"]=*stopTime;
            QFile file(value("--report")); if(file.open(QIODevice::WriteOnly)) file.write(QJsonDocument(report).toJson(QJsonDocument::Indented));
            app.exit(expected&&*lifecyclePassed&&!image.isNull()&&report["stop_ms"].toInteger()<1000?0:3);
        });
    }
    const int result=app.exec(); backend.stopWatching();backend.stopPreview(); backend.setExtraVideoItem(1,nullptr);backend.setExtraVideoItem(2,nullptr);backend.setRemoteVideoItem(nullptr);backend.setVideoItem(nullptr); return result;
}
