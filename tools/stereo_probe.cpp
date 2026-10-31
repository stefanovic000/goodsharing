#include "audio_capture.h"
#include "audio_playback.h"
#include "diagnostic_track_readers.h"
#include "audio_test_spectrum.h"
#include <livekit/livekit.h>
#include <livekit/room_delegate.h>
#include <livekit/audio_stream.h>
#include <livekit/local_participant.h>
#include <livekit/local_audio_track.h>
#include <livekit/remote_track_publication.h>
#include <QCoreApplication>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <windows.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <iostream>
#include <mutex>
#include <thread>

using Clock=std::chrono::steady_clock;
constexpr double pi=3.141592653589793;
using goodsharing_test::spectrum;
class StereoReceiver final:public livekit::RoomDelegate {
public:
    std::mutex mutex;
    bool accepting=true;
    std::atomic_bool content{false};
    std::atomic<int> encryptionOk{0},decryptionFailures{0};
    std::shared_ptr<livekit::AudioStream> stream;
    goodsharing_test::DiagnosticTrackReaders reusable;
    bool attached=false;
    std::thread reader;
    std::vector<std::int16_t> samples;
    int channels=0,rate=0;
    bool encrypted=false;
    QString error;
    goodsharing::AudioPlayback* playback=nullptr;
    void consume(livekit::AudioFrameEvent& ev) {
        channels=ev.frame.numChannels();rate=ev.frame.sampleRate();
        if(!content)return;
        if(samples.size()<48000*2*6)samples.insert(samples.end(),ev.frame.data().begin(),ev.frame.data().end());
        if(playback&&channels==2&&rate==48000)playback->submit(ev.frame.data());
    }
    void onE2eeStateChanged(livekit::Room&,const livekit::E2eeStateChangedEvent& e) override {
        if(e.state==livekit::EncryptionState::Ok) encryptionOk++;
        if(e.state==livekit::EncryptionState::DecryptionFailed||e.state==livekit::EncryptionState::MissingKey) decryptionFailures++;
    }
    void onTrackSubscribed(livekit::Room&,const livekit::TrackSubscribedEvent& e) override {
        if(e.track->kind()!=livekit::TrackKind::KIND_AUDIO) return;
        std::lock_guard lock(mutex);
        if(!accepting||attached) return;
        attached=true;
        livekit::AudioStream::Options options; options.capacity=16; options.sample_rate=48000; options.num_channels=qEnvironmentVariableIntValue("GOODSHARING_MONO_CONTROL")?1:2;
        encrypted=e.publication->encryptionType()==livekit::EncryptionType::GCM;
        if(!qEnvironmentVariableIntValue("GOODSHARING_PARTICIPANT_STREAM")) {
            reusable.audio(e.track,[this](livekit::AudioFrameEvent& event){consume(event);},options.num_channels);
            return;
        }
        // Retain direct fromParticipant as an independent SDK API regression.
        stream=livekit::AudioStream::fromParticipant(*e.participant,livekit::TrackSource::SOURCE_SCREENSHARE_AUDIO,options);
        reader=std::thread([this,source=stream] {
            try {
                livekit::AudioFrameEvent ev;
                while(source->read(ev)) {
                    consume(ev);
                }
            } catch(const std::exception& e) { error=QString::fromUtf8(e.what()); }
        });
    }
    void stop() {
        std::shared_ptr<livekit::AudioStream> source;
        std::thread worker;
        { std::lock_guard lock(mutex); accepting=false; source=stream; worker=std::move(reader); }
        if(source) source->close();
        if(worker.joinable()) worker.join();
        reusable.stop();
        if(error.isEmpty())error=QString::fromStdString(reusable.error());
    }
    ~StereoReceiver() override {stop();}
};
struct Pair {
    StereoReceiver receive,monitor;
    livekit::Room subscriber,publisher;
    bool closed=false;
    QString closeError;
    Pair(){subscriber.setDelegate(&receive);publisher.setDelegate(&monitor);}
    void close() noexcept {
        if(closed) return; closed=true;
        const auto attempt=[&](auto action){try{action();}catch(const std::exception& e){if(!closeError.isEmpty())closeError+="; ";closeError+=QString::fromUtf8(e.what());}};
        attempt([&]{receive.stop();});
        attempt([&]{subscriber.setDelegate(nullptr);});
        attempt([&]{publisher.setDelegate(nullptr);});
        const auto disconnect=[](livekit::Room& room){const bool connected=room.connectionState()!=livekit::ConnectionState::Disconnected;if(!room.disconnect()&&connected)throw std::runtime_error("SDK disconnect did not complete");};
        // Every cleanup is attempted even if an earlier stage failed.
        attempt([&]{disconnect(publisher);});
        attempt([&]{disconnect(subscriber);});
    }
    ~Pair(){close();}
};
int main(int argc,char** argv) {
    QCoreApplication app(argc,argv);
    QJsonObject report;
    try {
        struct Sdk {Sdk(){livekit::initialize(livekit::LogLevel::Warn);} ~Sdk(){livekit::shutdown();}} sdk;
        std::mutex loopMutex;
        std::vector<std::int16_t> loopSamples;
        QString loopError;
        goodsharing::AudioPlayback playback;
        goodsharing::AudioCapture loopback;
        Pair pair; // Readers stop before playback/capture and their buffers die.
        const bool play=qEnvironmentVariableIntValue("GOODSHARING_PLAYBACK")!=0;
        if(play) {
            playback.start(.15f); pair.receive.playback=&playback;
            loopback.start(goodsharing::AudioMode::Application,GetCurrentProcessId(),[&](goodsharing::AudioBlock block){ std::lock_guard lock(loopMutex); if(loopSamples.size()<48000*2*8) loopSamples.insert(loopSamples.end(),block.samples.begin(),block.samples.end()); },[&](QString error){std::lock_guard lock(loopMutex);loopError=error;});
        }
        livekit::RoomOptions opts; opts.auto_subscribe=true;
        const auto mode=qEnvironmentVariable("GOODSHARING_TEST_ENCRYPTION","shared-key");
        if(mode!="off") {
            const auto key=QByteArray::fromHex(qgetenv("GOODSHARING_TEST_E2EE_KEY")); if(key.size()!=32) throw std::runtime_error("Test key missing");
            livekit::E2EEOptions e; e.key_provider_options.shared_key=std::vector<std::uint8_t>(key.begin(),key.end()); e.key_provider_options.failure_tolerance=0; opts.encryption=e;
        }
        auto receiveOpts=opts;
        if(mode=="wrong-key") (*receiveOpts.encryption->key_provider_options.shared_key)[0]^=0xff;
        const auto url=qEnvironmentVariable("GOODSHARING_TEST_URL").toStdString();
        if(!pair.subscriber.connect(url,qEnvironmentVariable("GOODSHARING_TEST_SUBSCRIBER_TOKEN").toStdString(),receiveOpts)) throw std::runtime_error("Subscriber connect failed");
        if(!pair.publisher.connect(url,qEnvironmentVariable("GOODSHARING_TEST_PUBLISHER_TOKEN").toStdString(),opts)) throw std::runtime_error("Publisher connect failed");
        auto source=std::make_shared<livekit::AudioSource>(48000,2);
        auto track=livekit::LocalAudioTrack::createLocalAudioTrack("stereo-test",source);
        livekit::TrackPublishOptions publish; publish.source=livekit::TrackSource::SOURCE_SCREENSHARE_AUDIO; publish.dtx=false; publish.audio_encoding=livekit::AudioEncodingOptions{128000};
        pair.publisher.localParticipant().lock()->publishTrack(track,publish);
        const bool timedCapture=qEnvironmentVariableIntValue("GOODSHARING_TEST_TIMED_CAPTURE")!=0;
        std::int64_t nextCaptureUs=std::chrono::duration_cast<std::chrono::microseconds>(Clock::now().time_since_epoch()).count();
        const auto submit=[&](livekit::AudioFrame frame) {
            if(timedCapture)source->captureFrameAt(frame,nextCaptureUs,100);
            else source->captureFrame(frame,100);
            nextCaptureUs+=10000;
        };
        if(opts.encryption) {
            const auto deadline=Clock::now()+std::chrono::seconds(5);
            while(pair.monitor.encryptionOk<1) {
                if(Clock::now()>deadline) throw std::runtime_error("Encryption readiness timed out before test tones");
                submit(livekit::AudioFrame::create(48000,2,480));
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
        }
        pair.receive.content=true;
        const auto start=Clock::now();
        for(std::uint64_t offset=0;offset<48000*5;offset+=480) {
            std::vector<std::int16_t> pcm(960);
            for(int i=0;i<480;i++) {pcm[i*2]=static_cast<std::int16_t>(4000*std::sin((offset+i)*440*2*pi/48000));pcm[i*2+1]=static_cast<std::int16_t>(4000*std::sin((offset+i)*880*2*pi/48000));}
            submit(livekit::AudioFrame(std::move(pcm),48000,2,480));
            std::this_thread::sleep_until(start+std::chrono::microseconds((offset+480)*1000000/48000));
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        const auto readerStopStart=Clock::now(); pair.receive.stop();
        report["reader_stop_ms"]=std::chrono::duration<double,std::milli>(Clock::now()-readerStopStart).count();
        if(play){playback.stop();loopback.stop();}
        report["receive"]=spectrum(pair.receive.samples,pair.receive.channels);
        report["receive_error"]=pair.receive.error;
        report["sample_rate"]=pair.receive.rate;
        report["encrypted_publication"]=pair.receive.encrypted;
        report["publisher_encryption_ok_events"]=pair.monitor.encryptionOk.load();
        report["decryption_failures"]=pair.receive.decryptionFailures.load();
        report["mode"]=mode;
        report["timed_capture"]=timedCapture;
        if(play){report["playback"]=playback.report();report["process_loopback"]=spectrum(loopSamples,2);report["loopback_error"]=loopError;}
        const auto stopStart=Clock::now(); pair.close();
        report["close_ms"]=std::chrono::duration<double,std::milli>(Clock::now()-stopStart).count(); report["close_error"]=pair.closeError;
    } catch(const std::exception& e) {report["error"]=QString::fromUtf8(e.what());}
    const auto data=QJsonDocument(report).toJson(QJsonDocument::Indented);std::cout<<data.constData();
    const auto args=app.arguments();const int index=args.indexOf("--report"); if(index>=0&&index+1<args.size()){QFile f(args[index+1]);if(f.open(QIODevice::WriteOnly))f.write(data);}
    return report.contains("error")?1:0;
}
