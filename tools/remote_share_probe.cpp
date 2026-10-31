#include "room_controller.h"
#include "frame_pacer.h"
#include "audio_test_spectrum.h"
#include <livekit/livekit.h>
#include <QCoreApplication>
#include <QFile>
#include <QJsonDocument>
#include <QJsonArray>
#include <atomic>
#include <chrono>
#include <cmath>
#include <iostream>
#include <mutex>
#include <thread>
#include <windows.h>
#include <mmsystem.h>
using namespace goodsharing;
using Clock=std::chrono::steady_clock;
void require(bool value,const char* message){if(!value)throw std::runtime_error(message);}
std::uint64_t nowUs(){return std::chrono::duration_cast<std::chrono::microseconds>(Clock::now().time_since_epoch()).count();}
bool waitFor(const std::function<bool()>& test,int milliseconds=5000){const auto end=Clock::now()+std::chrono::milliseconds(milliseconds);while(!test()&&Clock::now()<end){QCoreApplication::processEvents();std::this_thread::sleep_for(std::chrono::milliseconds(1));}QCoreApplication::processEvents();return test();}
struct Source {
    std::shared_ptr<ShareInput> input;std::atomic_int fps=60;std::atomic_uint64_t video=0,audio=0;
    std::jthread pictures,sound;
    Source(std::shared_ptr<ShareInput> in,bool withAudio):input(std::move(in)) {
        pictures=std::jthread([this](std::stop_token stop){std::uint64_t index=0;int current=fps;auto pacing=std::make_unique<FramePacer>(current);
            while(!stop.stop_requested()){if(current!=fps){current=fps;pacing=std::make_unique<FramePacer>(current);}pacing->wait();if(stop.stop_requested())break;
                ShareVideoInput frame;frame.identity={nowUs(),static_cast<std::uint32_t>((index++*40503+7)&65535),false};
                for(std::size_t i=0;i<2;i++){const int w=i?1280:1920,h=i?720:1080;frame.nv12[i].assign(static_cast<std::size_t>(w)*h*3/2,128);std::fill_n(frame.nv12[i].begin(),static_cast<std::size_t>(w)*h,90);for(int y=0;y<40;y++)for(int x=0;x<w;x++)frame.nv12[i][y*w+x]=(frame.identity.frameId>>(x/40%16)&1)?235:16;}
                if(input->video(std::move(frame)))video++;
            }});
        if(withAudio)sound=std::jthread([this](std::stop_token stop){FramePacer pacing(100);std::uint64_t offset=0;
            while(!stop.stop_requested()){pacing.wait();if(stop.stop_requested())break;ShareAudioInput block{nowUs(),std::vector<std::int16_t>(960)};
                for(int i=0;i<480;i++){block.stereo[i*2]=static_cast<std::int16_t>(4000*std::sin((offset+i)*440*2*goodsharing_test::pi/48000));block.stereo[i*2+1]=static_cast<std::int16_t>(4000*std::sin((offset+i)*880*2*goodsharing_test::pi/48000));}offset+=480;if(input->audio(std::move(block)))audio++;
            }});
    }
    ~Source(){stop();}
    void stop(){pictures.request_stop();sound.request_stop();if(pictures.joinable())pictures.join();if(sound.joinable())sound.join();}
};
struct Inspected : std::enable_shared_from_this<Inspected> {
    explicit Inspected(int h):height(h){}
    const int height;
    mutable std::mutex mutex;
    int frames=0,pixels=0,metadata=0,dimensions=0,audioCallbacks=0;std::vector<std::int16_t> samples;std::vector<double> latency;
    RemoteShare::Callbacks callbacks(bool audio) {
        RemoteShare::Callbacks callbacks;
        callbacks.video=[this,keep=shared_from_this()](livekit::VideoFrameEvent& e){std::lock_guard lock(mutex);if(e.frame.height()!=height||e.frame.width()!=(height==1080?1920:1280)){dimensions++;return;}
            const auto identity=e.metadata&&e.metadata->user_data?decodeFrameIdentity(*e.metadata->user_data):std::nullopt;if(!identity){metadata++;return;}if(identity->priming)return;
            unsigned decoded=0;for(int bit=0;bit<16;bit++)if(e.frame.data()[4*(20*e.frame.width()+bit*40+20)]>128)decoded|=1u<<bit;
            frames++;if(decoded!=identity->frameId)pixels++;
            const auto current=nowUs();if(current>=identity->captureTimestampUs)latency.push_back((current-identity->captureTimestampUs)/1000.);
        };
        if(audio)callbacks.audio=[this,keep=shared_from_this()](livekit::AudioFrameEvent& e){std::lock_guard lock(mutex);audioCallbacks++;if(e.frame.numChannels()!=2||e.frame.sampleRate()!=48000){dimensions++;return;}if(samples.size()<48000*2*6){const auto& data=e.frame.data();const auto count=std::min(data.size(),static_cast<std::size_t>(48000*2*6)-samples.size());samples.insert(samples.end(),data.begin(),data.begin()+count);}};
        return callbacks;
    }
    int count()const{std::lock_guard lock(mutex);return frames;}
    int audioCount()const{std::lock_guard lock(mutex);return audioCallbacks;}
    QJsonObject report(){std::lock_guard lock(mutex);auto sorted=latency;std::sort(sorted.begin(),sorted.end());return {{"height",height},{"frames",frames},{"audio_callbacks",audioCallbacks},{"pixel_errors",pixels},{"metadata_errors",metadata},{"dimension_errors",dimensions},{"p95_ms",sorted.empty()?0:sorted[static_cast<std::size_t>((sorted.size()-1)*.95)]},{"audio",goodsharing_test::spectrum(samples,2)}};}
};
QJsonObject splitSubscriptions(RoomController& viewer,RoomController& sender,Source& source,const QString& id,quint64 generation,bool wrong){
    QJsonArray selections;auto sound=std::make_shared<Inspected>(720),low=std::make_shared<Inspected>(720),high=std::make_shared<Inspected>(1080);
    auto soundCallbacks=sound->callbacks(true);soundCallbacks.video={};
    auto audio=viewer.watchTrack("publisher",id,generation,ShareTrack::Audio,std::move(soundCallbacks));
    auto video=viewer.watchTrack("publisher",id,generation,ShareTrack::Video720,low->callbacks(false));
    require(audio&&video,"Independent audio/video subscriptions rejected");
    require(!viewer.watchTrack("publisher",id,generation,ShareTrack::Video720,low->callbacks(false)),"Duplicate selected video SID subscribed twice");
    require(!viewer.watchShare("publisher",id,generation,ShareTrack::Video1080,true,high->callbacks(true)),"Combined watch reused persistent audio SID");
    auto append=[&](const std::shared_ptr<RemoteShare>& watch,const std::shared_ptr<Inspected>& inspected){selections.append(QJsonObject{{"watch",watch->report()},{"inspection",inspected->report()}});};
    QJsonObject report{{"actual_playback",false},{"shared_audio_subscription",true},{"unrelated_processes_stopped",false}};
    if(wrong){
        require(waitFor([&]{return audio->stopped()&&video->stopped();},7000),"Wrong-key split subscriptions did not close/stop");
        require(sound->audioCount()==0&&low->count()==0,"Wrong-key split content reached callbacks");
        append(audio,sound);append(video,low);
    }else{
        require(waitFor([&]{return low->count()>=60&&audio->report()["ready"].toBool()&&video->report()["ready"].toBool()&&sound->report()["audio"].toObject()["stereo_passed"].toBool();},8000),"Initial split subscriptions did not decode verified stereo/video");
        auto candidate=viewer.watchTrack("publisher",id,generation,ShareTrack::Video1080,high->callbacks(false));require(bool(candidate),"Concurrent candidate quality rejected");
        const int beforeWarm=sound->audioCount();
        require(waitFor([&]{return high->count()>=60&&candidate->report()["ready"].toBool();},8000),"Candidate quality did not decode");
        require(sound->audioCount()>beforeWarm&&!audio->closed(),"Audio subscription stopped while candidate warmed");
        require(viewer.unwatchTrack(video)&&video->closed()&&waitFor([&]{return video->stopped();},2000),"Exact low-quality retirement failed");
        append(video,low);const int retiredLow=low->count(),afterHigh=sound->audioCount();
        require(waitFor([&]{return sound->audioCount()>=afterHigh+30;},2000)&&!candidate->closed()&&!audio->closed(),"Retiring low quality stopped shared audio/candidate");
        auto returned=std::make_shared<Inspected>(720);
        auto fresh=viewer.watchTrack("publisher",id,generation,ShareTrack::Video720,returned->callbacks(false));
        require(fresh&&fresh!=video&&waitFor([&]{return returned->count()>=60&&fresh->report()["ready"].toBool();},8000),"Fresh return to retired quality failed");
        require(viewer.unwatchTrack(candidate)&&waitFor([&]{return candidate->stopped();},2000),"Exact high-quality retirement failed");
        append(candidate,high);const int afterLow=sound->audioCount();
        require(waitFor([&]{return sound->audioCount()>=afterLow+30;},2000)&&!audio->closed()&&!fresh->closed()&&low->count()==retiredLow,
            "Return quality stopped shared audio or revived retired callbacks");
        report["audio_callbacks_before_candidate"]=beforeWarm;report["audio_callbacks_after_high"]=afterHigh;report["audio_callbacks_after_return"]=sound->audioCount();
        const auto sid=audio->manifest().audioSid;
        source.stop();sender.stopShare();
        require(waitFor([&]{return sender.shareState()==SharePublisher::State::Stopped&&audio->stopped()&&fresh->stopped();},3000),"Generation unpublish did not stop every split role");
        require(audio->manifest().audioSid==sid,"Audio identity changed during quality replacement");report["audio_sid_unchanged"]=true;
        report["same_audio_watch_for_both_quality_changes"]=true;report["generation_retired_all_roles"]=true;
        append(fresh,returned);append(audio,sound);
    }
    source.stop();viewer.leave();sender.leave();
    require(waitFor([&]{return viewer.state()==RoomController::State::Idle&&sender.state()==RoomController::State::Idle;},3000),"Split room cleanup stalled");
    report["selections"]=selections;report["passed"]=true;return report;
}
int main(int argc,char** argv) {
    QCoreApplication app(argc,argv);QJsonObject report;QJsonArray selections;
    try {
        struct SDK {SDK(){timeBeginPeriod(1);livekit::initialize(livekit::LogLevel::Warn);}~SDK(){livekit::shutdown();timeEndPeriod(1);}}sdk;
        RoomController sender,viewer;ShareModel catalog;
        QObject::connect(&viewer,&RoomController::trackAvailable,&catalog,[&](QString owner,QString sid,QString name,bool audio,bool encrypted,bool screen,quint32 w,quint32 h){catalog.announce({owner,sid,name,audio,encrypted,screen,w,h});});
        QObject::connect(&viewer,&RoomController::trackRemoved,&catalog,&ShareModel::remove);
        const auto url=qEnvironmentVariable("GOODSHARING_TEST_URL");const auto key=QByteArray::fromHex(qgetenv("GOODSHARING_TEST_E2EE_KEY"));auto viewKey=key;
        const auto scenario=qEnvironmentVariable("GOODSHARING_REMOTE_CASE","normal");if(scenario=="wrong-key"||scenario=="split-wrong-key")viewKey[0]=static_cast<char>(viewKey[0]^0xff);
        require(sender.join(url,qEnvironmentVariable("GOODSHARING_TEST_PUBLISHER_TOKEN"),key)&&viewer.join(url,qEnvironmentVariable("GOODSHARING_TEST_SUBSCRIBER_TOKEN"),viewKey),"Join request rejected");
        require(waitFor([&]{return sender.state()==RoomController::State::Ready&&viewer.state()==RoomController::State::Ready;}),"Room join failed");
        auto input=sender.startShare(true);require(bool(input)&&waitFor([&]{return sender.shareState()==SharePublisher::State::Sharing;},8000),"Publisher readiness failed");
        require(waitFor([&]{return catalog.rowCount()==1&&catalog.data(catalog.index(0),ShareModel::Has720Role).toBool()&&catalog.data(catalog.index(0),ShareModel::HasAudioRole).toBool();}),"Complete remote manifest missing");
        const auto id=catalog.data(catalog.index(0),ShareModel::ShareIdRole).toString();const auto generation=sender.shareGeneration();Source source(input,true);
        if(scenario=="split"||scenario=="split-wrong-key"){
            report["split"]=splitSubscriptions(viewer,sender,source,id,generation,scenario=="split-wrong-key");
        }else{
        auto first=std::make_shared<Inspected>(720);auto watch=viewer.watchShare("publisher",id,generation,ShareTrack::Video720,true,first->callbacks(true));require(bool(watch),"720p watch rejected");
        require(!viewer.watchShare("publisher",id,generation,ShareTrack::Video1080,false,first->callbacks(false)),"Duplicate watch allowed two qualities");
        if(scenario=="wrong-key") {
            require(waitFor([&]{return watch->closed();}),"Wrong-key watch did not close");
            require(waitFor([&]{return watch->stopped();},2000),"Wrong-key cleanup stalled");
            const auto inspected=first->report();require(first->count()==0&&inspected["audio"].toObject()["frames"].toDouble()==0,"Wrong-key content reached callbacks");
            selections.append(QJsonObject{{"watch",watch->report()},{"inspection",inspected}});
            source.stop();viewer.leave();sender.leave();require(waitFor([&]{return viewer.state()==RoomController::State::Idle&&sender.state()==RoomController::State::Idle;},2000),"Wrong-key room cleanup stalled");
        } else {
            require(waitFor([&]{return first->count()>=90&&watch->report()["ready"].toBool();}),"Selected 720p/audio did not become ready");
            require(waitFor([&]{return first->report()["audio"].toObject()["stereo_passed"].toBool();}),"Selected stereo not decoded");
            // Continuous, non-discrete source rates must not retire the watch.
            QJsonArray rates;
            for(const int fps:{55,45,30,60}){source.fps=fps;const int before=first->count();const auto until=Clock::now()+std::chrono::seconds(1);waitFor([&]{return Clock::now()>=until;},1500);require(!watch->closed()&&first->count()>before+fps/2,"Variable source rate stalled/disconnected receiver");rates.append(QJsonObject{{"offered_fps",fps},{"inspected",first->count()-before}});}
            report["variable_rates"]=rates;
            require(viewer.unwatchShare("publisher",id)&&watch->closed(),"Unwatch did not revoke immediately");require(waitFor([&]{return watch->stopped();},2000),"Unwatch cleanup exceeded bound");
            selections.append(QJsonObject{{"watch",watch->report()},{"inspection",first->report()}});const int oldCount=first->count(),oldAudio=first->audioCount();
            for(const auto quality:{ShareTrack::Video1080,ShareTrack::Video720}) {
                const bool withAudio=quality==ShareTrack::Video1080;
                auto inspected=std::make_shared<Inspected>(quality==ShareTrack::Video1080?1080:720);auto next=viewer.watchShare("publisher",id,generation,quality,withAudio,inspected->callbacks(withAudio));require(bool(next),"Quality reselection rejected");
                require(waitFor([&]{return inspected->count()>=60&&next->report()["ready"].toBool();}),"Reselected quality did not decode");
                if(withAudio)require(waitFor([&]{return inspected->report()["audio"].toObject()["stereo_passed"].toBool();}),"Reselected stereo did not decode");
                require(first->count()==oldCount&&first->audioCount()==oldAudio,"Stopped receiver delivered more media");
                require(viewer.unwatchShare("publisher",id)&&waitFor([&]{return next->stopped();},2000),"Reselection Stop failed");selections.append(QJsonObject{{"watch",next->report()},{"inspection",inspected->report()}});
            }
            for(int i=0;i<8;i++) {
                auto inspected=std::make_shared<Inspected>(720);auto cancelled=viewer.watchShare("publisher",id,generation,ShareTrack::Video720,false,inspected->callbacks(false));require(bool(cancelled),"Rapid watch rejected");viewer.unwatchShare("publisher",id);require(cancelled->closed()&&waitFor([&]{return cancelled->stopped();},2000),"Rapid watch cancellation stalled");
            }
            report["rapid_cancels"]=8;
            auto retiredFrames=std::make_shared<Inspected>(720);auto retired=viewer.watchShare("publisher",id,generation,ShareTrack::Video720,false,retiredFrames->callbacks(false));require(bool(retired)&&waitFor([&]{return retiredFrames->count()>=30;}),"Pre-restart watch failed");
            source.stop();sender.stopShare();require(waitFor([&]{return sender.shareState()==SharePublisher::State::Stopped&&retired->stopped();},2000),"Outgoing Stop did not retire remote generation");
            const int retiredCount=retiredFrames->count();
            auto replacement=sender.startShare(false);require(bool(replacement)&&waitFor([&]{return sender.shareState()==SharePublisher::State::Sharing;},8000),"Restart failed");const auto nextGeneration=sender.shareGeneration();
            require(nextGeneration>generation&&waitFor([&]{return catalog.data(catalog.index(0),ShareModel::GenerationRole).toULongLong()==nextGeneration&&catalog.data(catalog.index(0),ShareModel::Has720Role).toBool();}),"New remote generation missing");
            auto currentFrames=std::make_shared<Inspected>(720);
            require(!viewer.watchShare("publisher",id,generation,ShareTrack::Video720,false,currentFrames->callbacks(false)),"Old generation watch reopened");
            require(!retired->encryption("publisher",retired->manifest().videoSid,livekit::EncryptionState::Ok),"Old readiness revived retired watch");
            Source newSource(replacement,false);auto current=viewer.watchShare("publisher",id,nextGeneration,ShareTrack::Video720,false,currentFrames->callbacks(false));require(bool(current)&&waitFor([&]{return currentFrames->count()>=60;}),"Replacement generation did not decode");
            require(retiredFrames->count()==retiredCount,"Restart revived old callbacks");
            viewer.leave();require(current->closed()&&waitFor([&]{return viewer.state()==RoomController::State::Idle&&current->stopped();},2000),"Leave did not close receiving media");
            selections.append(QJsonObject{{"watch",current->report()},{"inspection",currentFrames->report()}});
            require(viewer.join(url,qEnvironmentVariable("GOODSHARING_TEST_SUBSCRIBER_TOKEN"),viewKey)&&waitFor([&]{return viewer.state()==RoomController::State::Ready;}),"Viewer rejoin failed");
            auto ownerExitFrames=std::make_shared<Inspected>(720);auto ownerExit=viewer.watchShare("publisher",id,nextGeneration,ShareTrack::Video720,false,ownerExitFrames->callbacks(false));require(bool(ownerExit)&&waitFor([&]{return ownerExitFrames->count()>=30;}),"Owner-disconnect setup failed");
            newSource.stop();sender.leave();require(waitFor([&]{return sender.state()==RoomController::State::Idle&&ownerExit->stopped();},2000),"Owner disconnect did not stop receiving media");
            report["owner_disconnect"]=QJsonObject{{"watch",ownerExit->report()},{"inspection",ownerExitFrames->report()}};
            viewer.leave();require(waitFor([&]{return viewer.state()==RoomController::State::Idle;},2000),"Viewer final cleanup failed");report["generation_restart_passed"]=true;
        }
        }
        report["passed"]=true;
    }catch(const std::exception& e){report["passed"]=false;report["error"]=QString::fromUtf8(e.what());}
    report["selections"]=selections;
    const auto data=QJsonDocument(report).toJson();std::cout<<data.constData();const auto args=app.arguments();const int at=args.indexOf("--report");if(at>=0&&at+1<args.size()){QFile output(args[at+1]);if(output.open(QIODevice::WriteOnly))output.write(data);}return report["passed"].toBool()?0:1;
}
