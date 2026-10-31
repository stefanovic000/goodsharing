#include <windows.h>
#include <dxgi1_2.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mmsystem.h>
#include <winrt/base.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <wrl/client.h>
#include <QCoreApplication>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <livekit/livekit.h>
#include <livekit/local_participant.h>
#include <livekit/room_delegate.h>
#include <livekit/video_frame.h>
#include <livekit/video_stream.h>
#include <livekit/remote_track_publication.h>
#include <atomic>
#include <chrono>
#include <iostream>
#include <map>
#include <limits>
#include <mutex>
#include <thread>
#include <tuple>
#include "audio_capture.h"
#include "hardware_encoder.h"
#include "mf_marker_decoder.h"
#include "encoded_recording.h"
#include "frame_metadata_codec.h"
#include "frame_pacer.h"
#include "encoded_video_output.h"
#include "diagnostic_track_readers.h"
#include <livekit/encoded_video_source.h>
#include <algorithm>
#include <cmath>
#include <QImage>
#include <QDir>
#include <livekit/audio_stream.h>
#include <livekit/local_audio_track.h>

using Microsoft::WRL::ComPtr;
using Clock = std::chrono::steady_clock;
bool safeMetadata() { return qEnvironmentVariable("GOODSHARING_TEST_METADATA_CODEC","safe")!="legacy"; }
livekit::VideoFrameMetadata wireMetadata(livekit::VideoFrameMetadata original,bool priming=false) {
    if(!safeMetadata()) return original;
    livekit::VideoFrameMetadata wire;
    wire.user_data=goodsharing::encodeFrameIdentity({original.user_timestamp_us.value_or(0),original.frame_id.value_or(0),priming});
    return wire;
}
class Receiver final : public livekit::RoomDelegate {
public:
    struct Counter { std::atomic<int> frames{0}; std::atomic<int> validMarkers{0}; int pixelMarkers=0,warmupFrames=0; bool encrypted=false; int width=0,height=0; Clock::time_point first,last; std::vector<double> latencies; QJsonArray markerErrors; };
    std::mutex mutex;
    std::map<std::string, std::shared_ptr<Counter>> counters;
    goodsharing_test::DiagnosticTrackReaders reader;
    std::vector<std::int16_t> audioSamples;
    int audioChannels=0,audioRate=0;
    bool encryptedAudio=false;
    bool accepting=true;
    std::atomic<int> decryptionFailures{0};
    std::atomic<int> encryptionOk{0};
    std::atomic_bool contentStarted{false};
    void onE2eeStateChanged(livekit::Room&,const livekit::E2eeStateChangedEvent& e) override { if(e.state==livekit::EncryptionState::Ok) encryptionOk++; if(e.state==livekit::EncryptionState::DecryptionFailed||e.state==livekit::EncryptionState::MissingKey) decryptionFailures++; }
    void onTrackSubscribed(livekit::Room&, const livekit::TrackSubscribedEvent& e) override {
        if(e.track->kind()==livekit::TrackKind::KIND_AUDIO) {
            std::lock_guard lock(mutex); if(!accepting) return;
            encryptedAudio=e.publication->encryptionType()==livekit::EncryptionType::GCM;
            reader.audio(e.track,[this](livekit::AudioFrameEvent& ev) {
                    audioChannels=ev.frame.numChannels(); audioRate=ev.frame.sampleRate();
                    if(!contentStarted.load()) return;
                    if(audioSamples.size()<48000*5) for(std::size_t i=0;i<ev.frame.data().size();i+=ev.frame.numChannels()) audioSamples.push_back(ev.frame.data()[i]);
            },1); // Retain the existing mono tone analyzer; stereo has its own suite.
            return;
        }
        if (e.track->kind() != livekit::TrackKind::KIND_VIDEO) return;
        std::lock_guard lock(mutex); if(!accepting) return;
        auto counter=std::make_shared<Counter>();
        counter->encrypted=e.publication->encryptionType()==livekit::EncryptionType::GCM;
        counters[e.track->name()]=counter;
        const auto name=QString::fromStdString(e.track->name());
        reader.video(e.track,[this,counter,name](livekit::VideoFrameEvent& ev) {
                bool priming=!contentStarted.load();
                if(safeMetadata()) {
                    std::optional<goodsharing::FrameIdentity> identity;
                    if(ev.metadata&&ev.metadata->user_data) identity=goodsharing::decodeFrameIdentity(*ev.metadata->user_data);
                    if(identity) { ev.metadata->frame_id=identity->frameId; ev.metadata->user_timestamp_us=identity->captureTimestampUs; priming=priming||identity->priming; }
                    else ev.metadata.reset();
                } else if(ev.metadata&&ev.metadata->frame_id==std::numeric_limits<std::uint32_t>::max()) priming=true;
                if(priming) { counter->warmupFrames++; return; }
                if(counter->frames==0) counter->first=Clock::now();
                counter->last=Clock::now();
                counter->width=ev.frame.width(); counter->height=ev.frame.height();
                counter->frames++;
                if (ev.metadata && ev.metadata->frame_id) {
                    counter->validMarkers++;
                    unsigned decoded=0;
                    for(int bit=0;bit<16;bit++) { auto* pixel=ev.frame.data()+4*(20*ev.frame.width()+bit*40+20); if(pixel[0]>128) decoded|=1u<<bit; }
                    if(decoded==(*ev.metadata->frame_id&0xffff)) counter->pixelMarkers++;
                    else if(counter->markerErrors.size()<10) {
                        const auto folder=qEnvironmentVariable("GOODSHARING_TEST_DIAGNOSTICS");
                        QString screenshot;
                        if(!folder.isEmpty()) { screenshot=folder+"/"+name+"-"+QString::number(*ev.metadata->frame_id)+".png"; QImage(ev.frame.data(),ev.frame.width(),ev.frame.height(),ev.frame.width()*4,QImage::Format_RGBA8888).save(screenshot); }
                        counter->markerErrors.append(QJsonObject{{"expected",static_cast<int>(*ev.metadata->frame_id&0xffff)},{"decoded",static_cast<int>(decoded)},{"screenshot",screenshot}});
                    }
                }
                if(ev.metadata&&ev.metadata->user_timestamp_us&&!qEnvironmentVariableIntValue("GOODSHARING_TEST_UNSAFE_TIMESTAMP")) { const auto now=std::chrono::duration_cast<std::chrono::microseconds>(Clock::now().time_since_epoch()).count(); counter->latencies.push_back((now-static_cast<long long>(*ev.metadata->user_timestamp_us))/1000.0); }
        });
    }
    void stop() {
        { std::lock_guard lock(mutex); accepting=false; }
        reader.stop();
    }
    ~Receiver() override { stop(); }
};

QJsonObject capabilities() {
    QJsonObject result;
    try { result["windows_graphics_capture"] = winrt::Windows::Graphics::Capture::GraphicsCaptureSession::IsSupported(); }
    catch(const winrt::hresult_error& e) { result["windows_graphics_capture_error"]=QString::fromWCharArray(e.message().c_str()); }
    result["process_loopback_audio"]=goodsharing::AudioCapture::probeProcessLoopback();
    ComPtr<IDXGIFactory1> factory;
    QJsonArray adapters;
    if(SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) {
        for(UINT i=0;;i++) { ComPtr<IDXGIAdapter1> a; if(factory->EnumAdapters1(i,&a)==DXGI_ERROR_NOT_FOUND) break; DXGI_ADAPTER_DESC1 d{}; a->GetDesc1(&d); adapters.append(QJsonObject{{"name",QString::fromWCharArray(d.Description)},{"dedicated_memory_mb",static_cast<double>(d.DedicatedVideoMemory/1048576)}}); }
    }
    result["gpus"]=adapters;
    QJsonArray encoders;
    IMFActivate** list=nullptr; UINT count=0;
    MFT_REGISTER_TYPE_INFO output{MFMediaType_Video,MFVideoFormat_H264};
    if(SUCCEEDED(MFTEnumEx(MFT_CATEGORY_VIDEO_ENCODER,MFT_ENUM_FLAG_HARDWARE|MFT_ENUM_FLAG_SORTANDFILTER,nullptr,&output,&list,&count))) {
        for(UINT i=0;i<count;i++) { wchar_t* name=nullptr; UINT length=0; if(SUCCEEDED(list[i]->GetAllocatedString(MFT_FRIENDLY_NAME_Attribute,&name,&length))) { encoders.append(QString::fromWCharArray(name)); CoTaskMemFree(name); } list[i]->Release(); }
        CoTaskMemFree(list);
    }
    result["windows_h264_hardware_encoders"]=encoders;
    return result;
}

void fill(livekit::VideoFrame& f, unsigned id) {
    auto* bytes=f.data(); const int width=f.width(),height=f.height();
    if(!id) for(int y=0;y<height;y++) for(int x=0;x<width;x++) { auto p=bytes+4*(y*width+x); p[0]=static_cast<unsigned char>(x*255/width); p[1]=static_cast<unsigned char>(y*255/height); p[2]=70; p[3]=255; }
    for(int y=0;y<40;y++) for(int x=0;x<width;x++) { auto p=bytes+4*(y*width+x); auto v=((id>>(x/40%16))&1)?240:16; p[0]=p[1]=p[2]=static_cast<unsigned char>(v); p[3]=255; }
}

QJsonObject mediaTest(const QString& url,const QString& pubToken,const QString& subToken,int seconds,const QString& encryptionMode) {
    Receiver delegate;
    Receiver publisherMonitor;
    livekit::Room subscriber,publisher;
    subscriber.setDelegate(&delegate);
    publisher.setDelegate(&publisherMonitor);
    livekit::RoomOptions options; options.auto_subscribe=true; options.adaptive_stream=false; options.dynacast=false;
    const bool encrypted=encryptionMode!="off";
    if(encrypted) {
        const auto key=QByteArray::fromHex(qgetenv("GOODSHARING_TEST_E2EE_KEY"));
        if(key.size()!=32) throw std::runtime_error("A random 32-byte test encryption key is required");
        livekit::E2EEOptions e2ee;
        e2ee.key_provider_options.shared_key=std::vector<std::uint8_t>(key.begin(),key.end());
        e2ee.key_provider_options.failure_tolerance=0;
        options.encryption=e2ee;
    }
    auto subscriberOptions=options;
    if(encryptionMode=="wrong-key") (*subscriberOptions.encryption->key_provider_options.shared_key)[0]^=0xff;
    if(!subscriber.connect(url.toStdString(),subToken.toStdString(),subscriberOptions)) throw std::runtime_error("Subscriber connection failed");
    if(!publisher.connect(url.toStdString(),pubToken.toStdString(),options)) throw std::runtime_error("Publisher connection failed");
    if(encrypted) {
        auto publisherEncryption=publisher.e2eeManager().lock();
        auto subscriberEncryption=subscriber.e2eeManager().lock();
        if(!publisherEncryption||!subscriberEncryption) throw std::runtime_error("E2EE manager missing");
        publisherEncryption->setEnabled(true); subscriberEncryption->setEnabled(true);
        if(!publisherEncryption->enabled()||!subscriberEncryption->enabled()) throw std::runtime_error("E2EE activation failed");
    }
    std::vector<std::shared_ptr<livekit::VideoSource>> sources;
    std::vector<std::shared_ptr<livekit::LocalVideoTrack>> tracks;
    std::vector<livekit::VideoFrame> frames;
    const bool external=qEnvironmentVariable("GOODSHARING_TEST_ENCODER")=="mf";
    const bool localDecode=qEnvironmentVariableIntValue("GOODSHARING_TEST_LOCAL_DECODE")!=0;
    const bool paced=!qEnvironmentVariableIntValue("GOODSHARING_TEST_LEGACY_PACING");
    std::vector<std::unique_ptr<MfMarkerDecoder>> localDecoders;
    std::vector<std::unique_ptr<EncodedRecording>> recordings;
    std::vector<std::unique_ptr<goodsharing::EncodedVideoOutput>> encoders;
    std::vector<std::vector<std::uint8_t>> nv12Frames;
    std::vector<QJsonArray> feedbackHistory(2);
    std::vector<std::uint64_t> lastBitrate(2,0);
    for(auto [width,height,bitrate]:{std::tuple{1920,1080,6000000},std::tuple{1280,720,3000000}}) {
        const auto index=sources.size();
        frames.push_back(livekit::VideoFrame::create(width,height,livekit::VideoBufferType::RGBA));fill(frames.back(),0);
        if(external) {
            if(localDecode) localDecoders.push_back(std::make_unique<MfMarkerDecoder>(width,height));
            const auto recordingFolder=qEnvironmentVariable("GOODSHARING_TEST_RECORD_ENCODED");
            if(!recordingFolder.isEmpty())recordings.push_back(std::make_unique<EncodedRecording>(recordingFolder+"/quality-"+QString::number(height)+".gsv",width,height));
            auto transform=[](livekit::VideoFrameMetadata metadata,bool priming) {
                const auto unsafeAt=qEnvironmentVariable("GOODSHARING_TEST_UNSAFE_TIMESTAMP_AT","-1").toLongLong();
                if(!priming&&qEnvironmentVariableIntValue("GOODSHARING_TEST_UNSAFE_TIMESTAMP")&&(unsafeAt<0||metadata.frame_id.value_or(0)==static_cast<unsigned>(unsafeAt))) metadata.user_timestamp_us=0x000001fffffebe00ULL|(metadata.frame_id.value_or(0)&0xffu);
                const auto tamperAt=qEnvironmentVariable("GOODSHARING_TEST_TAMPER_AT","-1").toLongLong();
                if(tamperAt>=0&&metadata.frame_id&&*metadata.frame_id==static_cast<unsigned>(tamperAt))metadata.frame_id=*metadata.frame_id^1u;
                return wireMetadata(metadata,priming);
            };
            auto observer=[&,index](const goodsharing::EncodedFrame& frame,goodsharing::FrameIdentity identity) {
                if(localDecode)localDecoders[index]->submit(frame,identity.frameId);
                if(!recordings.empty())recordings[index]->retain(frame,identity.frameId);
            };
            encoders.push_back(std::make_unique<goodsharing::EncodedVideoOutput>(width,height,bitrate,"quality-"+std::to_string(height),transform,observer,paced));
            auto publish=encoders.back()->publishOptions();
            livekit::FrameMetadataFeatures metadata;metadata.frame_id=!safeMetadata();metadata.user_timestamp=!safeMetadata();metadata.user_data=safeMetadata();publish.frame_metadata_features=metadata;
            publisher.localParticipant().lock()->publishTrack(encoders.back()->track(),publish);
            sources.push_back(nullptr);tracks.push_back(encoders.back()->track());
            auto& pixels=nv12Frames.emplace_back(static_cast<std::size_t>(width)*height*3/2,128);
            for(int y=0;y<height;y++) for(int x=0;x<width;x++) pixels[y*width+x]=static_cast<std::uint8_t>(16+x*180/width);
        } else {
            auto source=std::make_shared<livekit::VideoSource>(width,height);
            auto track=livekit::LocalVideoTrack::createLocalVideoTrack("quality-"+std::to_string(height),source);
            livekit::TrackPublishOptions publish;
            publish.video_codec=livekit::VideoCodec::H264;publish.simulcast=false;
            publish.video_encoding=livekit::VideoEncodingOptions{static_cast<std::uint64_t>(bitrate),60};
            publish.video_encoder=livekit::VideoEncoderBackend::Hardware;
            publish.source=livekit::TrackSource::SOURCE_SCREENSHARE;
            publish.degradation_preference=livekit::DegradationPreference::MaintainFramerateAndResolution;
            livekit::FrameMetadataFeatures metadata;metadata.frame_id=!safeMetadata();metadata.user_timestamp=!safeMetadata();metadata.user_data=safeMetadata();publish.frame_metadata_features=metadata;
            publisher.localParticipant().lock()->publishTrack(track,publish);
            sources.push_back(source);tracks.push_back(track);
        }
    }
    auto audioSource=std::make_shared<livekit::AudioSource>(48000,2);
    auto audioTrack=livekit::LocalAudioTrack::createLocalAudioTrack("share-audio",audioSource);
    livekit::TrackPublishOptions audioOptions; audioOptions.source=livekit::TrackSource::SOURCE_SCREENSHARE_AUDIO; audioOptions.dtx=false;
    publisher.localParticipant().lock()->publishTrack(audioTrack,audioOptions);
    if(encrypted) {
        publisher.e2eeManager().lock()->setEnabled(true);
        subscriber.e2eeManager().lock()->setEnabled(true);
        auto black=nv12Frames;
        for(std::size_t i=0;i<black.size();i++) std::fill_n(black[i].begin(),static_cast<std::size_t>(frames[i].width())*frames[i].height(),16);
        if(!external) for(auto& frame:frames) std::fill_n(frame.data(),frame.dataSize(),0);
        goodsharing::FramePacer primingPacer(60);
        const auto deadline=Clock::now()+std::chrono::seconds(5);
        while(publisherMonitor.encryptionOk<3) {
            if(Clock::now()>=deadline) throw std::runtime_error("Encrypted media priming failed; no test content was sent");
            primingPacer.wait();
            const auto timestamp=std::chrono::duration_cast<std::chrono::microseconds>(Clock::now().time_since_epoch()).count();
            for(std::size_t i=0;i<sources.size();i++) {
                if(external) encoders[i]->submit(black[i],{static_cast<std::uint64_t>(timestamp),std::numeric_limits<std::uint32_t>::max(),true});
                else { livekit::VideoCaptureOptions capture; capture.timestamp_us=timestamp; livekit::VideoFrameMetadata identity; identity.user_timestamp_us=timestamp; identity.frame_id=std::numeric_limits<std::uint32_t>::max(); capture.metadata=wireMetadata(identity,true); sources[i]->captureFrame(frames[i],capture); }
            }
            audioSource->captureFrame(livekit::AudioFrame::create(48000,2,480),100);
        }
        for(auto& encoder:encoders) encoder->requestKeyframe();
        if(!external) for(auto& frame:frames) fill(frame,0);
    }
    delegate.contentStarted=true;
    std::string audioError;
    std::jthread audioProducer([audioSource,&audioError](std::stop_token stop) {
        try {
            std::uint64_t offset=0; const auto origin=Clock::now();
            while(!stop.stop_requested()) {
                std::vector<std::int16_t> samples(960);
                for(int i=0;i<480;i++) samples[i*2]=samples[i*2+1]=static_cast<std::int16_t>(std::sin((offset+i)*440*2*3.141592653589793/48000)*4000);
                audioSource->captureFrame(livekit::AudioFrame(std::move(samples),48000,2,480),100);
                offset+=480; std::this_thread::sleep_until(origin+std::chrono::microseconds(offset*1000000/48000));
            }
        } catch(const std::exception& e) { audioError=e.what(); }
    });
    timeBeginPeriod(1);
    const auto start=Clock::now();
    goodsharing::FramePacer capturePacer(60);
    unsigned sent=0;
    while(Clock::now()-start<std::chrono::seconds(seconds)) {
        if(paced) {
            capturePacer.wait();
            if(Clock::now()-start>=std::chrono::seconds(seconds)) break;
        }
        if(sent==static_cast<unsigned>(qEnvironmentVariableIntValue("GOODSHARING_TEST_STALL_AT"))) {
            const int stall=qEnvironmentVariableIntValue("GOODSHARING_TEST_STALL_MS");
            if(stall>0) std::this_thread::sleep_for(std::chrono::milliseconds(stall));
        }
        const unsigned marker=qEnvironmentVariable("GOODSHARING_TEST_MARKER")=="stress"?(sent*40503u+7u)&0xffffu:sent;
        livekit::VideoCaptureOptions capture;
        capture.timestamp_us=std::chrono::duration_cast<std::chrono::microseconds>(Clock::now().time_since_epoch()).count();
        livekit::VideoFrameMetadata metadata; metadata.frame_id=marker; metadata.user_timestamp_us=capture.timestamp_us; capture.metadata=metadata;
        for(std::size_t i=0;i<sources.size();i++) {
            if(external) {
                const auto bitrateStep=qEnvironmentVariable("GOODSHARING_TEST_BITRATE_STEP_AT","-1").toLongLong();
                const auto feedback=encoders[i]->updateFeedback(bitrateStep>=0&&sent>=bitrateStep&&sent<bitrateStep+120?1000000:0);
                const auto bitrate=feedback.rate_control?feedback.rate_control->target_bitrate_bps:0;
                if(feedback.keyframe_requested||(bitrate&&bitrate!=lastBitrate[i])) {
                    feedbackHistory[i].append(QJsonObject{{"source_frame",static_cast<int>(sent)},{"target_bitrate_bps",static_cast<double>(bitrate)},{"keyframe_requested",feedback.keyframe_requested}});
                    if(bitrate) lastBitrate[i]=bitrate;
                }
                const int width=frames[i].width();
                for(int y=0;y<40;y++) for(int x=0;x<width;x++) nv12Frames[i][y*width+x]=((marker>>(x/40%16))&1)?235:16;
                encoders[i]->submit(nv12Frames[i],{static_cast<std::uint64_t>(capture.timestamp_us),marker,false});
            } else { fill(frames[i],marker); auto rawCapture=capture; rawCapture.metadata=wireMetadata(*capture.metadata); sources[i]->captureFrame(frames[i],rawCapture); }
        }
        sent++;
        const auto burstStart=static_cast<unsigned>(qEnvironmentVariableIntValue("GOODSHARING_TEST_STALL_AT"));
        const auto burstFrames=static_cast<unsigned>(qEnvironmentVariableIntValue("GOODSHARING_TEST_BURST_FRAMES"));
        if(!paced&&(!burstFrames||sent<burstStart||sent>=burstStart+burstFrames)) std::this_thread::sleep_until(start+std::chrono::microseconds(static_cast<long long>(sent)*1000000/60));
    }
    timeEndPeriod(1);
    if(external) { const auto end=Clock::now()+std::chrono::milliseconds(150); while(Clock::now()<end) { for(auto& e:encoders) e->poll(); std::this_thread::sleep_for(std::chrono::milliseconds(1)); } }
    QJsonObject result{{"publication_mode","paired exact-resolution tracks"},{"encryption",encryptionMode},{"publisher_encryption_ok_events",publisherMonitor.encryptionOk.load()},{"synthetic_preroll",encrypted},{"metadata_codec",safeMetadata()?"GS02 ASCII envelope":"legacy binary fields"},{"pacing",paced?"bounded capture and encoded handoff pacing":"legacy catch-up"},{"seconds",seconds},{"source_frames_per_quality",static_cast<int>(sent)}};
    QJsonArray outbound;
    auto stats=publisher.getStats().get();
    for(auto& value:stats.publisher_stats) if(auto* record=std::get_if<livekit::RtcOutboundRtpStats>(&value.stats);record&&record->stream.kind=="video") {
        auto& s=record->outbound;
        outbound.append(QJsonObject{{"width",static_cast<int>(s.frame_width)},{"height",static_cast<int>(s.frame_height)},{"fps",s.frames_per_second},{"frames_encoded",static_cast<int>(s.frames_encoded)},{"encoder",QString::fromStdString(s.encoder_implementation)},{"power_efficient",s.power_efficient_encoder}});
    }
    result["outbound"]=outbound;
    QJsonArray hardware;
    for(auto& encoder:encoders)hardware.append(encoder->report());
    result["external_hardware_encoders"]=hardware;
    QJsonArray localChecks,feedbackReports;
    for(auto& decoder:localDecoders) localChecks.append(decoder->report());
    for(std::size_t i=0;i<feedbackHistory.size();i++) feedbackReports.append(QJsonObject{{"height",frames[i].height()},{"events",feedbackHistory[i]}});
    result["before_transport_decode"]=localChecks;
    result["encoder_feedback"]=feedbackReports;
    QJsonArray decodedStats;
    auto receivedStats=subscriber.getStats().get();
    auto allReceivedStats=std::move(receivedStats.publisher_stats);
    allReceivedStats.insert(allReceivedStats.end(),receivedStats.subscriber_stats.begin(),receivedStats.subscriber_stats.end());
    for(auto& value:allReceivedStats) if(auto* record=std::get_if<livekit::RtcInboundRtpStats>(&value.stats)) {
        auto& s=record->inbound; decodedStats.append(QJsonObject{{"decoder",QString::fromStdString(s.decoder_implementation)},{"frames_decoded",static_cast<int>(s.frames_decoded)},{"width",static_cast<int>(s.frame_width)},{"height",static_cast<int>(s.frame_height)},{"packets_lost",static_cast<double>(record->received.packets_lost)},{"nack_count",static_cast<int>(s.nack_count)},{"pli_count",static_cast<int>(s.pli_count)},{"frames_dropped",static_cast<int>(s.frames_dropped)},{"packets_discarded",static_cast<double>(s.packets_discarded)}});
    }
    result["receiver_rtc_stats"]=decodedStats;
    audioProducer.request_stop(); audioProducer.join();
    subscriber.setDelegate(nullptr); delegate.stop();
    QJsonArray inbound;
    { std::lock_guard lock(delegate.mutex); for(auto& [name,c]:delegate.counters) { const double elapsed=std::chrono::duration<double>(c->last-c->first).count(); std::sort(c->latencies.begin(),c->latencies.end()); const double p95=c->latencies.empty()?0:c->latencies[static_cast<std::size_t>((c->latencies.size()-1)*.95)]; inbound.append(QJsonObject{{"track",QString::fromStdString(name)},{"encrypted_publication",c->encrypted},{"synthetic_preroll_frames",c->warmupFrames},{"width",c->width},{"height",c->height},{"frames",c->frames.load()},{"metadata_frames",c->validMarkers.load()},{"pixel_markers",c->pixelMarkers},{"marker_errors",c->markerErrors},{"capture_to_decoded_p95_ms",p95},{"decoded_fps",elapsed>0?(c->frames.load()-1)/elapsed:0}}); } }
    result["inbound"]=inbound;
    if(!delegate.reader.error().empty())throw std::runtime_error(delegate.reader.error());
    double real=0,imag=0,energy=0;
    for(std::size_t i=0;i<delegate.audioSamples.size();i++) { const double phase=i*440*2*3.141592653589793/48000; const auto sample=delegate.audioSamples[i]; real+=sample*std::cos(phase); imag+=sample*std::sin(phase); energy+=static_cast<double>(sample)*sample; }
    double maxWindowTone=0;
    constexpr std::size_t windowSamples=4800;
    for(std::size_t windowStart=0;windowStart+windowSamples<=delegate.audioSamples.size();windowStart+=windowSamples/2) {
        double windowReal=0,windowImag=0;
        for(std::size_t i=0;i<windowSamples;i++) { const double phase=i*440*2*3.141592653589793/48000; windowReal+=delegate.audioSamples[windowStart+i]*std::cos(phase); windowImag+=delegate.audioSamples[windowStart+i]*std::sin(phase); }
        maxWindowTone=std::max(maxWindowTone,2*std::hypot(windowReal,windowImag)/windowSamples);
    }
    result["decryption_failures"]=delegate.decryptionFailures.load();
    result["audio"]=QJsonObject{{"captured_samples",static_cast<int>(delegate.audioSamples.size())},{"sample_rate",delegate.audioRate},{"channels",delegate.audioChannels},{"encrypted_publication",delegate.encryptedAudio},{"tone_440_amplitude",delegate.audioSamples.empty()?0:2*std::hypot(real,imag)/delegate.audioSamples.size()},{"max_100ms_tone_440_amplitude",maxWindowTone},{"rms",delegate.audioSamples.empty()?0:std::sqrt(energy/delegate.audioSamples.size())},{"producer_error",QString::fromStdString(audioError)}};
    for(auto& recording:recordings) recording->finish();
    subscriber.disconnect(); publisher.disconnect();
    return result;
}

int main(int argc,char** argv) {
    QCoreApplication app(argc,argv);
    winrt::init_apartment(winrt::apartment_type::multi_threaded);
    MFStartup(MF_VERSION);
    QJsonObject report=capabilities();
    auto args=app.arguments();
    try {
        if(args.contains("--decode-recording")) {
            const int index=args.indexOf("--decode-recording");
            if(index+1>=args.size()) throw std::runtime_error("Encoded recording path missing");
            report["encoded_recording"]=verifyEncodedRecording(args[index+1]);
        }
        if(args.contains("--media")) {
            const auto url=qEnvironmentVariable("GOODSHARING_TEST_URL","http://127.0.0.1:8765");
            const auto pub=qEnvironmentVariable("GOODSHARING_TEST_PUBLISHER_TOKEN");
            const auto sub=qEnvironmentVariable("GOODSHARING_TEST_SUBSCRIBER_TOKEN");
            if(pub.isEmpty()||sub.isEmpty()) throw std::runtime_error("Test tokens missing");
            struct SdkScope { SdkScope(){livekit::initialize(livekit::LogLevel::Warn);} ~SdkScope(){livekit::shutdown();} } sdkScope;
            report["media"]=mediaTest(url,pub,sub,qEnvironmentVariableIntValue("GOODSHARING_TEST_SECONDS")>0?qEnvironmentVariableIntValue("GOODSHARING_TEST_SECONDS"):15,qEnvironmentVariable("GOODSHARING_TEST_ENCRYPTION","off"));
        }
    } catch(const std::exception& e) { report["error"]=QString::fromUtf8(e.what()); }
    const auto data=QJsonDocument(report).toJson(QJsonDocument::Indented);
    std::cout<<data.constData();
    if(args.contains("--report")) { const int i=args.indexOf("--report"); if(i+1<args.size()) { QFile file(args[i+1]); if(file.open(QIODevice::WriteOnly)) file.write(data); } }
    MFShutdown();
    return report.contains("error")?1:0;
}
