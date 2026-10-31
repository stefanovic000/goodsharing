#include "wasapi_playout_sink.h"
#include "remote_audio_playout.h"
#include "audio_capture.h"
#include "audio_playback.h"
#include "audio_test_spectrum.h"
#include <QCoreApplication>
#include <QCommandLineParser>
#include <QFile>
#include <QJsonDocument>
#include <windows.h>
#include <objbase.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <iostream>
#include <mutex>
#include <thread>

using namespace goodsharing;
using Clock=std::chrono::steady_clock;
std::int64_t nowUs(){return std::chrono::duration_cast<std::chrono::microseconds>(Clock::now().time_since_epoch()).count();}
std::int64_t qpcUs(){LARGE_INTEGER q{},f{};QueryPerformanceCounter(&q);QueryPerformanceFrequency(&f);return static_cast<std::int64_t>(static_cast<long double>(q.QuadPart)*1000000/f.QuadPart);}
void require(bool value,const char* message){if(!value)throw std::runtime_error(message);}
struct Recording {
    std::mutex mutex;std::vector<std::int16_t> before,after,muted;QString error;std::int64_t cut=0,muteBegin=0,muteEnd=0;std::size_t tailPeak=0;
    void consume(AudioBlock block){std::lock_guard lock(mutex);
        auto& out=cut&&block.timestampUs>cut+80000?after:before;
        if(out.size()+block.samples.size()<=48000*2*8)out.insert(out.end(),block.samples.begin(),block.samples.end());
        if(muteBegin&&block.timestampUs>muteBegin+80000&&(!muteEnd||block.timestampUs<muteEnd)&&muted.size()+block.samples.size()<=48000*2*8)
            muted.insert(muted.end(),block.samples.begin(),block.samples.end());
        if(cut&&block.timestampUs>cut+80000)for(auto s:block.samples)tailPeak=std::max(tailPeak,static_cast<std::size_t>(std::abs(static_cast<int>(s))));}
};
struct GateSink final:AudioPlayoutSink {
    WasapiPlayoutSink device;std::shared_ptr<RemoteShare> watch;bool revokePrepared=false,revoked=false;
    std::optional<Window> prepare(std::int64_t now)override{auto window=device.prepare(now);
        if(window&&revokePrepared&&!revoked){watch->revoke();revoked=true;}return window;}
    bool commit(std::span<const std::int16_t> pcm)override{return device.commit(pcm);}
    void cancel()noexcept override{device.cancel();}
    bool reset()noexcept override{return device.reset();}
    bool hardwareEndpoint()const noexcept override{return true;}
};
int main(int argc,char** argv){
    QCoreApplication app(argc,argv);QCommandLineParser parser;parser.addOption({"case","normal, mute, unverified, revoke-in-prepare or stall","name","normal"});
    parser.addOption({"report","JSON report path","file"});parser.process(app);
    const auto name=parser.value("case");QJsonObject result{{"case",name},{"passed",false},{"actual_wasapi",true},
        {"process_loopback_tested",true},{"physical_speaker_display_sync_measured",false},{"real_screen_capture",false},
        {"synthetic_permission_events",true},{"network_or_decryption_test",false},{"legacy_player_supplies_only_silence",true}};
    const auto initialized=CoInitializeEx(nullptr,COINIT_MULTITHREADED);int exit=1;
    try{
        require(SUCCEEDED(initialized),"COM initialization failed");
        require(name=="normal"||name=="mute"||name=="unverified"||name=="revoke-in-prepare"||name=="stall","Unknown test case");
        Recording recording;AudioCapture capture;
        // Own-process silence keeps loopback observations alive after our
        // tested endpoint stops. This legacy player never receives media.
        AudioPlayback observationSilence;observationSilence.start(0);
        require(capture.start(AudioMode::Application,GetCurrentProcessId(),[&](AudioBlock b){recording.consume(std::move(b));},
            [&](QString error){std::lock_guard lock(recording.mutex);recording.error=error;}),"Process-loopback start failed");
        // Declare the endpoint after recording so it is destroyed first on error.
        GateSink sink;
        auto watch=std::make_shared<RemoteShare>(RemoteShare::Manifest{"fixture-owner","0123456789abcdef0123456789abcdef",1,
            ShareTrack::Video720,"fixture-video","fixture-audio"},RemoteShare::Callbacks{[](livekit::VideoFrameEvent&){},[](livekit::AudioFrameEvent&){}});
        sink.watch=watch;sink.revokePrepared=name=="revoke-in-prepare";
        RemoteAudioPlayout playout(sink);playout.bind(watch);
        if(name!="unverified"){watch->encryption("fixture-owner","fixture-video",livekit::EncryptionState::Ok);watch->encryption("fixture-owner","fixture-audio",livekit::EncryptionState::Ok);}
        std::this_thread::sleep_for(std::chrono::milliseconds(120));
        const auto origin=nowUs();std::int64_t nextVideo=0,nextAudio=10000;bool stopped=false,stalled=false,failed=false;
        std::uint64_t commitsAtStop=0;std::int64_t revokeDuration=0;QJsonObject beforeStop,endpointBeforeStop;
        bool muted=false,unmuted=false;double mediaAtUnmute=0;
        while(nowUs()-origin<4200000){
            auto elapsed=nowUs()-origin;
            if(name=="mute"&&!muted&&elapsed>=1000000){playout.setMuted(true);playout.pump();muted=true;
                std::lock_guard lock(recording.mutex);recording.muteBegin=qpcUs();}
            if(name=="mute"&&!unmuted&&elapsed>=1800000){playout.setMuted(false);playout.pump();unmuted=true;mediaAtUnmute=playout.report()["committed_media_frames"].toDouble();
                std::lock_guard lock(recording.mutex);recording.muteEnd=qpcUs();}
            if(name=="stall"&&!stalled&&elapsed>=1800000){
                require(!failed&&sink.device.report()["commits"].toInt()>50,"Endpoint failed before the deliberate stall");
                stalled=true;std::this_thread::sleep_for(std::chrono::milliseconds(110));elapsed=nowUs()-origin;}
            if(!stopped&&elapsed>=3000000){
                beforeStop=playout.report();endpointBeforeStop=sink.device.report();
                {std::lock_guard lock(recording.mutex);recording.cut=qpcUs();}
                const auto start=nowUs();watch->revoke();playout.pump();revokeDuration=nowUs()-start;
                stopped=true;commitsAtStop=static_cast<std::uint64_t>(sink.device.report()["commits"].toDouble());
            }
            if(!stopped){
                // Skip missed source times after a stall, never generate a FIFO.
                if(elapsed>=nextVideo){const auto stamp=elapsed;playout.video({4000000000000000ll+stamp,static_cast<std::uint32_t>(stamp*9/100),1,2,true});nextVideo=elapsed+16667;}
                // A capture block becomes available after all 480 samples,
                // with its original first-sample time, not ten future ms.
                if(elapsed>=nextAudio){const auto stamp=(elapsed/10000-1)*10000;std::array<std::int16_t,960> pcm;
                    for(int i=0;i<480;i++){const auto sample=stamp*48/1000+i;pcm[i*2]=static_cast<std::int16_t>(1600*std::sin(sample*440*2*goodsharing_test::pi/48000));
                        pcm[i*2+1]=static_cast<std::int16_t>(1600*std::sin(sample*880*2*goodsharing_test::pi/48000));}
                    playout.audio({4000000000000000ll+stamp,static_cast<std::uint32_t>(stamp*48/1000),1,2,true},pcm);nextAudio=stamp+20000;}
            }
            const auto pumped=playout.pump();failed|=pumped==RemoteAudioPlayout::Pump::Failed;
            sink.device.wait(2);
        }
        playout.stop();capture.stop();observationSilence.stop();
        const auto endpoint=sink.device.report(),output=playout.report();
        const auto spectrum=goodsharing_test::spectrum(recording.before,2);
        result["before_revoke"]=beforeStop;result["endpoint_before_revoke"]=endpointBeforeStop;
        result["playout"]=output;result["endpoint"]=endpoint;result["loopback_before_revoke"]=spectrum;
        result["post_revoke_frames"]=static_cast<double>(recording.after.size()/2);result["post_revoke_peak"]=static_cast<double>(recording.tailPeak);
        result["revoke_and_pump_us"]=static_cast<double>(revokeDuration);result["observed_failure"]=failed;
        const auto committed=endpoint["submitted_frames_including_silence"].toDouble();
        const auto coverage=committed?output["committed_media_frames"].toDouble()/committed:0;
        result["committed_media_fraction"]=coverage;result["minimum_committed_media_fraction"]=.95;
        result["source_blocks_available_after_last_sample"]=true;
        int mutePeak=0;for(auto s:recording.muted)mutePeak=std::max(mutePeak,std::abs(static_cast<int>(s)));
        result["muted_observation_frames"]=static_cast<double>(recording.muted.size()/2);result["muted_peak"]=mutePeak;
        require(recording.error.isEmpty(),"Process-loopback recording failed");
        require(recording.after.size()>=48000/5*2&&recording.tailPeak<40,"Post-revoke loopback was missing or contained audio");
        require(!endpoint["running"].toBool()&&endpoint["padding_frames"].toInt()==0&&endpoint["reserved_frames"].toInt()==0,"Stop/reset retained endpoint data");
        require(endpoint["commits"].toDouble()==commitsAtStop,"Endpoint accepted more commits after revocation");
        if(name=="normal"||name=="mute")require(!failed&&spectrum["stereo_passed"].toBool()&&endpoint["clock_readings"].toInt()>50&&
            output["committed_media_frames"].toDouble()>48000,"Scheduled stereo did not reach the actual process-loopback path");
        if(name=="normal"||name=="mute")require(coverage>=.95,"Timed PCM continuity below 95 percent of committed device frames");
        if(name=="mute")require(unmuted&&recording.muted.size()>=19200&&mutePeak<40&&
            output["committed_media_frames"].toDouble()>mediaAtUnmute+24000,"Mute/unmute did not flush, stay silent and resume fresh media");
        if(name=="unverified"||name=="revoke-in-prepare")require(!failed&&endpoint["commits"].toInt()==0&&
            spectrum["max_100ms_tone_amplitude"].toDouble()<40,"Unverified/revoked content reached the device");
        if(name=="revoke-in-prepare")require(sink.revoked&&endpoint["cancels"].toInt()>0,"Reservation revoke case was not exercised");
        if(name=="stall")require(stalled&&spectrum["stereo_passed"].toBool()&&
            endpoint["short_discontinuities"].toInt()>0&&
            endpoint["first_discontinuity"].toObject()["reason"].toString()=="deadline_too_soon"&&
            endpoint["commits"].toInt()>50,"Exhausted endpoint clock was not detected and fenced");
        result["passed"]=true;exit=0;
    }catch(const std::exception& e){result["error"]=QString::fromUtf8(e.what());std::cerr<<e.what()<<'\n';}
    if(SUCCEEDED(initialized))CoUninitialize();
    const auto bytes=QJsonDocument(result).toJson(QJsonDocument::Indented);QFile file(parser.value("report"));
    if(!file.open(QIODevice::WriteOnly)||file.write(bytes)!=bytes.size()){std::cerr<<"Could not save report\n";return 2;}
    std::cout<<QJsonDocument(result).toJson(QJsonDocument::Compact).constData()<<'\n';return exit;
}
