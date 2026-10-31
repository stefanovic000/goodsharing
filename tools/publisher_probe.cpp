#include "share_publisher.h"
#include "publication_failure.h"
#include "share_receiver.h"
#include "frame_pacer.h"
#include "encoded_recording.h"
#include "audio_test_spectrum.h"
#include "audio_timing_trace.h"
#include <livekit/livekit.h>
#include <livekit/room_delegate.h>
#include <livekit/local_participant.h>
#include <livekit/remote_track_publication.h>
#include <livekit/remote_participant.h>
#include <QCoreApplication>
#include <QFile>
#include <QJsonDocument>
#include <QImage>
#include <windows.h>
#include <mfapi.h>
#include <mmsystem.h>
#include <atomic>
#include <algorithm>
#include <chrono>
#include <iostream>
#include <map>
#include <set>
#include <thread>
#include <cmath>
#include <variant>
#include <deque>
using namespace goodsharing;
using Clock=std::chrono::steady_clock;
constexpr double pi=3.141592653589793;
std::uint64_t nowUs(){return std::chrono::duration_cast<std::chrono::microseconds>(Clock::now().time_since_epoch()).count();}
void require(bool value,const char* message){if(!value)throw std::runtime_error(message);}
QJsonObject formatTransportStats(livekit::SessionStats session) {
    QJsonArray outgoing,incoming,audio,incomingAudio,selected;
    auto all=std::move(session.publisher_stats);all.insert(all.end(),session.subscriber_stats.begin(),session.subscriber_stats.end());
    std::set<std::string> selectedIds;std::map<std::string,livekit::IceCandidateStats> remotes;
    for(const auto& value:all) {
        if(const auto* r=std::get_if<livekit::RtcTransportStats>(&value.stats))selectedIds.insert(r->transport.selected_candidate_pair_id);
        if(const auto* r=std::get_if<livekit::RtcRemoteCandidateStats>(&value.stats))remotes[r->rtc.id]=r->candidate;
    }
    for(const auto& value:all) {
        if(const auto* record=std::get_if<livekit::RtcOutboundRtpStats>(&value.stats);record&&record->stream.kind=="video") {
            const auto& s=record->outbound;
            outgoing.append(QJsonObject{{"width",static_cast<int>(s.frame_width)},{"height",static_cast<int>(s.frame_height)},
                {"frames_encoded",static_cast<double>(s.frames_encoded)},{"frames_sent",static_cast<double>(s.frames_sent)},
                {"encoder",QString::fromStdString(s.encoder_implementation)},{"total_encode_seconds",s.total_encode_time},
                {"packet_send_delay_seconds",s.total_packet_send_delay},{"packets_sent",static_cast<double>(record->sent.packets_sent)},{"bytes_sent",static_cast<double>(record->sent.bytes_sent)},{"quality_limitation",static_cast<int>(s.quality_limitation_reason)},
                {"nack",static_cast<double>(s.nack_count)},{"pli",static_cast<double>(s.pli_count)}});
        }
        if(const auto* record=std::get_if<livekit::RtcInboundRtpStats>(&value.stats);record&&record->stream.kind=="video") {
            const auto& s=record->inbound;
            incoming.append(QJsonObject{{"width",static_cast<int>(s.frame_width)},{"height",static_cast<int>(s.frame_height)},
                {"frames_received",static_cast<double>(s.frames_received)},{"frames_decoded",static_cast<double>(s.frames_decoded)},
                {"frames_dropped",static_cast<double>(s.frames_dropped)},{"decoder",QString::fromStdString(s.decoder_implementation)},
                {"total_decode_seconds",s.total_decode_time},{"packets_lost",static_cast<double>(record->received.packets_lost)},
                {"packets_discarded",static_cast<double>(s.packets_discarded)},{"nack",static_cast<double>(s.nack_count)},
                {"pli",static_cast<double>(s.pli_count)},{"jitter_buffer_seconds",s.jitter_buffer_delay},
                {"jitter_buffer_frames",static_cast<double>(s.jitter_buffer_emitted_count)}});
        }
        if(const auto* r=std::get_if<livekit::RtcOutboundRtpStats>(&value.stats);r&&r->stream.kind=="audio")audio.append(QJsonObject{{"packets_sent",static_cast<double>(r->sent.packets_sent)},{"bytes_sent",static_cast<double>(r->sent.bytes_sent)}});
        if(const auto* r=std::get_if<livekit::RtcInboundRtpStats>(&value.stats);r&&r->stream.kind=="audio")incomingAudio.append(QJsonObject{{"jitter_buffer_seconds",r->inbound.jitter_buffer_delay},{"jitter_buffer_samples",static_cast<double>(r->inbound.jitter_buffer_emitted_count)},{"total_samples_received",static_cast<double>(r->inbound.total_samples_received)},{"concealed_samples",static_cast<double>(r->inbound.concealed_samples)},{"packets_lost",static_cast<double>(r->received.packets_lost)}});
        if(const auto* r=std::get_if<livekit::RtcCandidatePairStats>(&value.stats);r&&selectedIds.contains(r->rtc.id)) {
            const auto remote=remotes.find(r->candidate_pair.remote_candidate_id);
            selected.append(QJsonObject{{"remote_address",remote==remotes.end()?QString{}:QString::fromStdString(remote->second.address)},{"remote_port",remote==remotes.end()?0:remote->second.port},
                {"protocol",remote==remotes.end()?QString{}:QString::fromStdString(remote->second.protocol)},{"available_outgoing_bitrate_bps",r->candidate_pair.available_outgoing_bitrate},{"rtt_ms",r->candidate_pair.current_round_trip_time*1000}});
        }
    }
    return {{"outgoing_video",outgoing},{"incoming_video",incoming},{"outgoing_audio",audio},{"incoming_audio",incomingAudio},{"selected_ice",selected}};
}
QJsonObject transportStats(livekit::Room& room){return formatTransportStats(room.getStats().get());}
struct Receiver final:livekit::RoomDelegate {
    struct Video {int height=0,width=0,frames=0,priming=0,nonblackPriming=0,metadataErrors=0,dimensionErrors=0,pixelErrors=0,lastPixelErrorFrame=0;Clock::time_point first,last;QJsonArray errors,missingMetadata,timings;std::vector<double> latencies;};
    const bool networkTelemetry=qEnvironmentVariableIntValue("GOODSHARING_TEST_NETWORK_TELEMETRY")!=0;
    goodsharing_test::AudioTimingTrace audioTrace{qEnvironmentVariable("GOODSHARING_TEST_AUDIO_TRACE")};
    std::mutex mutex,audioMutex;
    bool accepting=true;int audioTracks=0;
    std::map<std::string,std::shared_ptr<Video>> videos;
    ShareReceiver readers;
    std::vector<std::int16_t> samples;
    std::vector<std::int16_t> startupSamples,activeSamples,tailSamples;
    std::atomic_bool contentStarted=false,sourceStopped=false;
    const int readerStallMs=qEnvironmentVariableIntValue("GOODSHARING_TEST_READER_STALL_MS");
    int channels=0,rate=0;
    bool allEncrypted=true;
    std::atomic<int> decryptionFailures=0;
    void onE2eeStateChanged(livekit::Room&,const livekit::E2eeStateChangedEvent& e)override{if(e.participant)readers.encryption(e.participant->identity(),e.track_sid,e.state);if(e.state==livekit::EncryptionState::DecryptionFailed||e.state==livekit::EncryptionState::MissingKey)decryptionFailures++;}
    void onTrackSubscribed(livekit::Room&,const livekit::TrackSubscribedEvent& e)override {
        std::lock_guard lock(mutex);if(!accepting||!e.track||!e.publication||!e.participant)return;
        const auto identity=parseShareTrackName(QString::fromStdString(e.track->name()));if(!identity)return;
        allEncrypted&=e.publication->encryptionType()==livekit::EncryptionType::GCM;
        if(identity->track==ShareTrack::Audio) {
            audioTracks++;
            readers.audio(e.track,[this](livekit::AudioFrameEvent& event){const auto receivedUs=nowUs();std::lock_guard audioLock(audioMutex);channels=event.frame.numChannels();rate=event.frame.sampleRate();audioTrace.received(event.frame.data(),channels,rate,receivedUs,event.capture_time_ms,event.sink_time_us,event.rtp_timestamp,event.sender_ntp_us,event.rtcp_reports,event.sender_clock_epoch);if(samples.size()<48000*2*6)samples.insert(samples.end(),event.frame.data().begin(),event.frame.data().end());auto& phase=sourceStopped?tailSamples:contentStarted?activeSamples:startupSamples;if(phase.size()<48000*2*6)phase.insert(phase.end(),event.frame.data().begin(),event.frame.data().end());},2,e.participant->identity());return;
        }
        auto counter=std::make_shared<Video>();counter->height=identity->track==ShareTrack::Video1080?1080:720;counter->width=counter->height==1080?1920:1280;videos[e.track->sid()]=counter;
        readers.video(e.track,[this,counter](livekit::VideoFrameEvent& event){
            auto identity=event.metadata&&event.metadata->user_data?decodeFrameIdentity(*event.metadata->user_data):std::nullopt;
            if(!identity){
                counter->metadataErrors++;
                if(counter->missingMetadata.size()<10){int maximum=0;for(std::size_t p=0;p<static_cast<std::size_t>(event.frame.width())*event.frame.height()*4;p+=4)for(int channel=0;channel<3;channel++)maximum=std::max(maximum,static_cast<int>(event.frame.data()[p+channel]));
                    QString screenshot;const auto folder=qEnvironmentVariable("GOODSHARING_TEST_DIAGNOSTICS");if(!folder.isEmpty()){screenshot=folder+"/missing-metadata-"+QString::number(counter->height)+"-"+QString::number(counter->metadataErrors)+".png";QImage(event.frame.data(),event.frame.width(),event.frame.height(),event.frame.width()*4,QImage::Format_RGBA8888).save(screenshot);}
                    counter->missingMetadata.append(QJsonObject{{"maximum_rgb",maximum},{"source_content_started",contentStarted.load()},{"source_stopped",sourceStopped.load()},{"width",event.frame.width()},{"height",event.frame.height()},{"metadata_present",event.metadata.has_value()},{"user_data_bytes",event.metadata&&event.metadata->user_data?static_cast<int>(event.metadata->user_data->size()):0},{"screenshot",screenshot}});
                }return;
            }
            if(event.frame.width()!=counter->width||event.frame.height()!=counter->height){counter->dimensionErrors++;return;}
            if(identity->priming){counter->priming++;for(int y:{20,counter->height/2})for(int x:{20,counter->width/2})if(event.frame.data()[4*(y*counter->width+x)]>32)counter->nonblackPriming++;return;}
            if(!counter->frames)counter->first=Clock::now();counter->last=Clock::now();counter->frames++;
            if(counter->frames==60&&readerStallMs>0)std::this_thread::sleep_for(std::chrono::milliseconds(readerStallMs));
            unsigned decoded=0;for(int bit=0;bit<16;bit++)if(event.frame.data()[4*(20*counter->width+bit*40+20)]>128)decoded|=1u<<bit;
            if(decoded!=(identity->frameId&0xffff)) {
                counter->pixelErrors++;counter->lastPixelErrorFrame=counter->frames;
                if(counter->errors.size()<10) {
                QString screenshot;const auto folder=qEnvironmentVariable("GOODSHARING_TEST_DIAGNOSTICS");
                if(counter->errors.size()<2&&!folder.isEmpty()){screenshot=folder+"/pixel-mismatch-"+QString::number(counter->height)+"-"+QString::number(counter->errors.size())+".png";QImage(event.frame.data(),event.frame.width(),event.frame.height(),event.frame.width()*4,QImage::Format_RGBA8888).save(screenshot);}
                counter->errors.append(QJsonObject{{"expected",static_cast<int>(identity->frameId&0xffff)},{"decoded",static_cast<int>(decoded)},{"frame_number",counter->frames},{"capture_us",static_cast<double>(identity->captureTimestampUs)},{"received_us",static_cast<double>(nowUs())},{"screenshot",screenshot}});
                }
            }
            counter->latencies.push_back((static_cast<double>(nowUs())-identity->captureTimestampUs)/1000.0);
            if(networkTelemetry&&counter->timings.size()<20000) {
                QJsonObject timing{{"capture_us",static_cast<double>(identity->captureTimestampUs)},{"received_us",static_cast<double>(nowUs())},{"sdk_timestamp_us",static_cast<double>(event.timestamp_us)},{"frame_id",static_cast<int>(identity->frameId)}};
                timing["rtp_timestamp"]=event.rtp_timestamp?QJsonValue(static_cast<double>(*event.rtp_timestamp)):QJsonValue(QJsonValue::Null);
                timing["sender_ntp_us"]=event.sender_ntp_us?QJsonValue(static_cast<double>(*event.sender_ntp_us)):QJsonValue(QJsonValue::Null);
                timing["rtcp_reports"]=event.rtcp_reports?QJsonValue(static_cast<double>(*event.rtcp_reports)):QJsonValue(QJsonValue::Null);
                timing["sender_clock_epoch"]=event.sender_clock_epoch?QJsonValue(static_cast<double>(*event.sender_clock_epoch)):QJsonValue(QJsonValue::Null);
                counter->timings.append(timing);
            }
        },e.participant->identity());
    }
    void onTrackUnsubscribed(livekit::Room&,const livekit::TrackUnsubscribedEvent& e)override{if(e.publication)readers.remove(e.publication->sid());}
    void stop(){ {std::lock_guard lock(mutex);accepting=false;}readers.stop();}
    ~Receiver()override{stop();}
    QJsonObject report() {
        QJsonArray video;
        for(const auto& [sid,c]:videos){std::sort(c->latencies.begin(),c->latencies.end());const auto duration=std::chrono::duration<double>(c->last-c->first).count();video.append(QJsonObject{{"height",c->height},{"width",c->width},{"frames",c->frames},{"priming_frames",c->priming},{"nonblack_priming",c->nonblackPriming},{"marker_errors",c->errors},{"pixel_error_frames",c->pixelErrors},{"last_pixel_error_frame",c->lastPixelErrorFrame},{"metadata_errors",c->metadataErrors},{"missing_metadata",c->missingMetadata},{"dimension_errors",c->dimensionErrors},{"fps",c->frames>1?(c->frames-1)/duration:0},{"p95_ms",c->latencies.empty()?0:c->latencies[static_cast<std::size_t>((c->latencies.size()-1)*.95)]},{"timings",c->timings}});}
        return {{"verification",readers.report()},{"videos",video},{"audio_tracks",audioTracks},{"audio",goodsharing_test::spectrum(samples,channels)},{"startup_audio",goodsharing_test::spectrum(startupSamples,channels)},{"active_audio",goodsharing_test::spectrum(activeSamples,channels)},{"tail_audio",goodsharing_test::spectrum(tailSamples,channels)},{"sample_rate",rate},{"all_encrypted",allEncrypted},{"decryption_failures",decryptionFailures.load()},{"reader_error",QString::fromStdString(readers.error())}};
    }
};
struct Monitor final:livekit::RoomDelegate {
    std::shared_ptr<EncryptionGate> gate;
    std::mutex mutex;std::set<std::string> readySids;int missingSid=0;
    explicit Monitor(std::shared_ptr<EncryptionGate> g):gate(std::move(g)){}
    void onE2eeStateChanged(livekit::Room& room,const livekit::E2eeStateChangedEvent& e)override {
        const auto local=room.localParticipant().lock();if(!local||e.participant!=local.get())return;
        gate->observe(local->identity(),e.track_sid,e.state);
        if(e.state==livekit::EncryptionState::Ok){std::lock_guard lock(mutex);if(e.track_sid.empty())missingSid++;else readySids.insert(e.track_sid);}
    }
    QJsonObject report(){std::lock_guard lock(mutex);QJsonArray sids;for(const auto& sid:readySids)sids.append(QString::fromStdString(sid));return {{"ok_sids",sids},{"missing_sid_events",missingSid}};}
};
struct Pair {
    Receiver receive;Monitor monitor;livekit::Room subscriber,publisher;bool closed=false;QString closeError;
    explicit Pair(std::shared_ptr<EncryptionGate> gate):monitor(std::move(gate)){subscriber.setDelegate(&receive);publisher.setDelegate(&monitor);}
    void close()noexcept {
        if(closed)return;closed=true;
        const auto attempt=[&](auto operation){try{operation();}catch(...){closeError="Owned room cleanup failed";}};
        attempt([&]{receive.stop();});attempt([&]{subscriber.setDelegate(nullptr);publisher.setDelegate(nullptr);});
        attempt([&]{const bool connected=publisher.connectionState()!=livekit::ConnectionState::Disconnected;if(!publisher.disconnect()&&connected)throw std::runtime_error("disconnect failed");});
        attempt([&]{const bool connected=subscriber.connectionState()!=livekit::ConnectionState::Disconnected;if(!subscriber.disconnect()&&connected)throw std::runtime_error("disconnect failed");});
    }
    ~Pair(){close();}
};
ShareVideoInput fixture(std::uint64_t index) {
    ShareVideoInput frame;frame.identity={nowUs(),static_cast<std::uint32_t>((index*40503+7)&0xffff),false};
    for(std::size_t i=0;i<2;i++){const int width=i?1280:1920,height=i?720:1080;frame.nv12[i].assign(static_cast<std::size_t>(width)*height*3/2,128);std::fill_n(frame.nv12[i].begin(),static_cast<std::size_t>(width)*height,90);for(int y=0;y<40;y++)for(int x=0;x<width;x++)frame.nv12[i][y*width+x]=((frame.identity.frameId>>(x/40%16))&1)?235:16;}
    return frame;
}
QJsonObject run(const QString& scenario,int seconds) {
    auto gate=std::make_shared<EncryptionGate>(1);Pair pair(gate);
    livekit::RoomOptions options;options.auto_subscribe=true;options.adaptive_stream=false;options.dynacast=false;options.join_retries=0;options.connect_timeout=std::chrono::milliseconds(1000);
    const auto key=QByteArray::fromHex(qgetenv("GOODSHARING_TEST_E2EE_KEY"));require(key.size()==32,"Test key missing");livekit::E2EEOptions encryption;encryption.key_provider_options.shared_key=std::vector<std::uint8_t>(key.begin(),key.end());encryption.key_provider_options.failure_tolerance=0;options.encryption=encryption;
    auto receiverOptions=options;if(scenario=="wrong-key")(*receiverOptions.encryption->key_provider_options.shared_key)[0]^=0xff;
    if(scenario=="receiver-unconfigured")receiverOptions.encryption.reset();
    const auto url=qEnvironmentVariable("GOODSHARING_TEST_URL").toStdString();
    require(pair.subscriber.connect(url,qEnvironmentVariable("GOODSHARING_TEST_SUBSCRIBER_TOKEN").toStdString(),receiverOptions),"Subscriber join failed");
    require(pair.publisher.connect(url,qEnvironmentVariable("GOODSHARING_TEST_PUBLISHER_TOKEN").toStdString(),options),"Publisher join failed");
    if(receiverOptions.encryption)pair.subscriber.e2eeManager().lock()->setEnabled(true);pair.publisher.e2eeManager().lock()->setEnabled(true);
    const bool audio=scenario!="audio-off";SharePublisher::Config config{"0123456789abcdef0123456789abcdef",1,audio};
    if(scenario=="missing-handler")config.readinessTimeout=std::chrono::milliseconds(1000);
    auto input=std::make_shared<ShareInput>(config.id,audio,gate);
    std::array<std::unique_ptr<EncodedRecording>,2> recordings;
    const auto folder=qEnvironmentVariable("GOODSHARING_TEST_RECORD_ENCODED");if(!folder.isEmpty())for(int i=0;i<2;i++)recordings[i]=std::make_unique<EncodedRecording>(folder+"/quality-"+QString::number(i?720:1080)+".gsv",i?1280:1920,i?720:1080);
    std::uint64_t published=0,realOutputs=0,outputsAfterRevocation=0;bool revoked=false,disablePerformed=false,managerDisabled=false,reenableBlocked=false;
    SharePublisher::Observers observers;
    const int audioWorkerStall=qEnvironmentVariableIntValue("GOODSHARING_TEST_AUDIO_WORKER_STALL_MS");
    require(audioWorkerStall>=0&&audioWorkerStall<=250,"Invalid controlled audio worker stall");
    std::atomic_bool audioWorkerStalled=false;std::atomic<std::uint64_t> stallBegin=0,stallEnd=0;unsigned audioHandoffs=0;
    observers.beforeAudioHandoff=[&](std::uint64_t){if(audioWorkerStall&&++audioHandoffs==100){
        audioWorkerStalled=true;stallBegin=nowUs();std::this_thread::sleep_for(std::chrono::milliseconds(audioWorkerStall));stallEnd=nowUs();}};
    const int nativeAudioStall=qEnvironmentVariableIntValue("GOODSHARING_TEST_AUDIO_NATIVE_STALL_MS");
    require(nativeAudioStall>=0&&nativeAudioStall<=250,"Invalid controlled SDK-entry delay");
    std::atomic_bool nativeAudioStalled=false;unsigned nativeAttempts=0;
    if(nativeAudioStall)observers.beforeAudioCapture=[&](std::uint64_t){if(++nativeAttempts==100){nativeAudioStalled=true;std::this_thread::sleep_for(std::chrono::milliseconds(nativeAudioStall));}};
    std::atomic<std::uint64_t> firstResume=0,audioAfterStall=0,maximumAudioAge=0;
    observers.audioHandoff=[&](std::uint64_t source,std::uint64_t began){
        const auto age=began>=source?began-source:0;maximumAudioAge=std::max(maximumAudioAge.load(),age);
        if(stallEnd.load()){if(!firstResume.load())firstResume=began;audioAfterStall++;}};
    const int encodedStall=qEnvironmentVariableIntValue("GOODSHARING_TEST_ENCODED_STALL_MS");int observedOutput=0;
    observers.beforeVideoHandoff=[&](int height,FrameIdentity identity){
        if(height!=1080||identity.priming||++observedOutput!=60)return;
        if(encodedStall>0)std::this_thread::sleep_for(std::chrono::milliseconds(encodedStall));
        if(scenario=="revoke-in-output"){
            revoked=true;const auto manifest=gate->report()["tracks"].toArray();
            const auto sid=manifest[0].toObject()["sid"].toString().toStdString();
            gate->observe("publisher",sid,livekit::EncryptionState::New);
            gate->observe("publisher",sid,livekit::EncryptionState::Ok);
        }
    };
    observers.published=[&](std::size_t count){published=count;if(scenario=="partial-failure"&&count==1)throw std::runtime_error("Controlled failure after first publication");if(scenario=="cancel-start"&&count==1)gate->close();};
    observers.video=[&](int height,const EncodedFrame& frame,FrameIdentity identity){if(!identity.priming){realOutputs++;if(revoked)outputsAfterRevocation++;}if(recordings[height==720?1:0])recordings[height==720?1:0]->retain(frame,identity.frameId);};
    SharePublisher publisher(pair.publisher,config,input,std::move(observers));
    std::atomic<std::uint64_t> offered=0,accepted=0,audioAccepted=0;
    pair.receive.audioTrace.setSourceSampleClock();
    std::string videoError,audioError;
    std::jthread video([&](std::stop_token stop){try{FramePacer pacing(60);std::uint64_t frame=0;while(!stop.stop_requested()){pacing.wait();if(stop.stop_requested())break;auto next=fixture(frame++);offered++;if(input->video(std::move(next)))accepted++;}}catch(const std::exception& e){videoError=e.what();}});
    std::jthread sound;
    const int audioDelayMs=qEnvironmentVariableIntValue("GOODSHARING_TEST_AUDIO_DELAY_MS");
    require(audioDelayMs>=0&&audioDelayMs<=70&&(!audioDelayMs||(pair.receive.audioTrace.enabled()&&scenario=="stereo")),"Invalid owned synthetic audio delay");
    if(audio)sound=std::jthread([&](std::stop_token stop) {
        try {
            struct Pending {ShareAudioInput block; std::uint64_t offset,capture;};
            std::deque<Pending> pending;
            // Generated PCM has an exact 48 kHz sample clock. Timer wakeup is
            // submission time, not the first sample's source time. Preserve
            // this grid through both normal and deliberately delayed handoff.
            std::uint64_t offset=0;const auto sourceOriginUs=nowUs();
            while(!stop.stop_requested()) {
                const auto captureUs=sourceOriginUs+offset*1000000/48000;
                std::this_thread::sleep_until(Clock::time_point(std::chrono::microseconds(captureUs)));
                if(stop.stop_requested())break;
                ShareAudioInput next{captureUs,std::vector<std::int16_t>(960)};
                for(int i=0;i<480;i++) {
                    const int amplitude=pair.receive.audioTrace.enabled()?goodsharing_test::timingAmplitude(offset+i):4000;
                    next.stereo[i*2]=static_cast<std::int16_t>(amplitude*std::sin((offset+i)*440*2*pi/48000));
                    next.stereo[i*2+1]=static_cast<std::int16_t>(amplitude*std::sin((offset+i)*880*2*pi/48000));
                }
                if(!audioDelayMs) {
                    if(input->audio(std::move(next))) {audioAccepted++;pair.receive.audioTrace.source(offset,captureUs);}
                    offset+=480;continue;
                }
                pending.push_back({std::move(next),offset,captureUs});offset+=480;
                require(pending.size()<=9,"Owned delay queue exceeded its fixed diagnostic bound");
                while(!pending.empty()&&nowUs()-pending.front().capture>=static_cast<std::uint64_t>(audioDelayMs)*1000) {
                    auto ready=std::move(pending.front());pending.pop_front();
                    if(input->audio(std::move(ready.block))) {
                        audioAccepted++;pair.receive.audioTrace.source(ready.offset,ready.capture);
                    }
                }
            }
        }catch(const std::exception& e){audioError=e.what();}
    });
    if(scenario=="cancel-before-publish")gate->close();
    publisher.start();
    const auto startupDeadline=Clock::now()+std::chrono::seconds(7);
    while(publisher.state()==SharePublisher::State::Starting&&Clock::now()<startupDeadline){publisher.tick(scenario=="missing-handler"?std::array<bool,3>{true,false,true}:std::array<bool,3>{true,true,true});std::this_thread::sleep_for(std::chrono::milliseconds(1));}
    const auto readyReport=publisher.report();
    if(publisher.state()==SharePublisher::State::Sharing)pair.receive.contentStarted=true;
    const bool normal=scenario=="stereo"||scenario=="audio-off"||scenario=="wrong-key"||scenario=="receiver-unconfigured"||scenario=="revoke"||scenario=="revoke-in-output"||scenario=="disable-encryption";
    std::optional<Clock::time_point> contentStart;
    QJsonArray networkStats,receiverNetworkStats;std::optional<std::future<livekit::SessionStats>> pendingStats,pendingReceiverStats;auto nextStats=Clock::now();
    const int ownerStall=qEnvironmentVariableIntValue("GOODSHARING_TEST_OWNER_STALL_MS");bool ownerStalled=false;
    if(normal&&publisher.state()==SharePublisher::State::Sharing){contentStart=Clock::now();while(Clock::now()-*contentStart<std::chrono::seconds(seconds)&&publisher.state()==SharePublisher::State::Sharing){
        if(pair.receive.networkTelemetry&&networkStats.size()<650) {
            if(pendingStats&&pendingStats->wait_for(std::chrono::seconds(0))==std::future_status::ready){networkStats.append(QJsonObject{{"sample_us",static_cast<double>(nowUs())},{"transport",formatTransportStats(pendingStats->get())},{"publisher",publisher.report()}});pendingStats.reset();}
            if(pendingReceiverStats&&pendingReceiverStats->wait_for(std::chrono::seconds(0))==std::future_status::ready){receiverNetworkStats.append(QJsonObject{{"sample_us",static_cast<double>(nowUs())},{"transport",formatTransportStats(pendingReceiverStats->get())}});pendingReceiverStats.reset();}
            if(!pendingStats&&!pendingReceiverStats&&Clock::now()>=nextStats){pendingStats.emplace(pair.publisher.getStats());pendingReceiverStats.emplace(pair.subscriber.getStats());nextStats=Clock::now()+std::chrono::milliseconds(500);}
        }
        if(ownerStall>0&&!ownerStalled&&Clock::now()-*contentStart>std::chrono::seconds(1)){ownerStalled=true;std::this_thread::sleep_for(std::chrono::milliseconds(ownerStall));}
        publisher.tick();if(scenario=="disable-encryption"&&Clock::now()-*contentStart>std::chrono::milliseconds(1500)&&!disablePerformed){disablePerformed=true;revoked=true;const auto manager=pair.publisher.e2eeManager().lock();manager->setEnabled(false);managerDisabled=!manager->enabled();}if(scenario=="revoke"&&Clock::now()-*contentStart>std::chrono::seconds(1)&&!revoked){revoked=true;const auto manifest=gate->report()["tracks"].toArray();gate->observe("publisher",manifest[0].toObject()["sid"].toString().toStdString(),livekit::EncryptionState::New);gate->observe("publisher",manifest[0].toObject()["sid"].toString().toStdString(),livekit::EncryptionState::Ok);}std::this_thread::sleep_for(std::chrono::milliseconds(1));}}
    video.request_stop();video.join();if(sound.joinable()){sound.request_stop();sound.join();}
    if(publisher.state()==SharePublisher::State::Sharing){const auto until=Clock::now()+std::chrono::milliseconds(150);while(Clock::now()<until){publisher.tick();std::this_thread::sleep_for(std::chrono::milliseconds(1));}}
    QJsonObject senderStats,receiverStats;
    if(normal&&publisher.state()==SharePublisher::State::Sharing){senderStats=transportStats(pair.publisher);receiverStats=transportStats(pair.subscriber);}
    if(disablePerformed){pair.publisher.e2eeManager().lock()->setEnabled(true);publisher.tick();reenableBlocked=publisher.state()==SharePublisher::State::Failed&&gate->closed()&&gate->failed();}
    const auto beforeStop=publisher.report();pair.receive.sourceStopped=true;const auto stopStart=Clock::now();publisher.stop();const auto stopMs=std::chrono::duration<double,std::milli>(Clock::now()-stopStart).count();
    const auto stopped=publisher.report();const auto remaining=pair.publisher.localParticipant().lock()->trackPublications().size();
    for(auto& recording:recordings)if(recording)recording->finish();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));pair.receive.stop();
    QJsonObject result{{"scenario",scenario},{"seconds",seconds},{"ready",readyReport},{"before_stop",beforeStop},{"stopped",stopped},{"stop_ms",stopMs},{"remaining_local_publications",static_cast<int>(remaining)},
      {"receive",pair.receive.report()},{"sender_events",pair.monitor.report()},{"sender_rtc",senderStats},{"receiver_rtc",receiverStats},{"network_stats",networkStats},{"video_offered",static_cast<double>(offered.load())},{"video_admitted",static_cast<double>(accepted.load())},{"audio_admitted",static_cast<double>(audioAccepted.load())},
      {"real_video_handoffs",static_cast<double>(realOutputs)},{"handoffs_after_revocation",static_cast<double>(outputsAfterRevocation)},{"video_producer_error",QString::fromStdString(videoError)},{"audio_producer_error",QString::fromStdString(audioError)},{"published_before_cleanup",static_cast<double>(published)}};
    result["disable_performed"]=disablePerformed;result["manager_disabled_observed"]=managerDisabled;result["reenable_did_not_resume_share"]=reenableBlocked;
    result["old_input_rejected"]=!input->video(fixture(offered.load()+1));result["old_input_closed"]=input->report()["closed"].toBool();
    result["receiver_network_stats"]=receiverNetworkStats;
    result["audio_worker_stall"]=QJsonObject{{"performed",audioWorkerStalled.load()},{"requested_ms",audioWorkerStall},
        {"begin_us",static_cast<double>(stallBegin.load())},{"end_us",static_cast<double>(stallEnd.load())},
        {"first_successful_resume_us",static_cast<double>(firstResume.load())},{"successful_handoffs_after_stall",static_cast<double>(audioAfterStall.load())},
        {"maximum_handoff_start_age_us",static_cast<double>(maximumAudioAge.load())}};
    result["audio_timing_trace"]=pair.receive.audioTrace.finish();
    result["native_audio_stall_performed"]=nativeAudioStalled.load();
    pair.close();result["close_error"]=pair.closeError;return result;
}
int main(int argc,char**argv) {
    QCoreApplication app(argc,argv);QJsonObject report;
    try {
        struct Media {HRESULT com=CoInitializeEx(nullptr,COINIT_MULTITHREADED);Media(){require(SUCCEEDED(com)&&SUCCEEDED(MFStartup(MF_VERSION)),"Media initialization failed");timeBeginPeriod(1);}~Media(){timeEndPeriod(1);MFShutdown();if(SUCCEEDED(com))CoUninitialize();}}media;
        struct SDK{SDK(){livekit::initialize(livekit::LogLevel::Warn);}~SDK(){livekit::shutdown();}}sdk;
        if(app.arguments().contains("--encoder-idle")){
            // No screen/audio capture or room: reproduce a paced encoder with
            // one input followed by a raw-admission pause longer than its age.
            QJsonArray cases;
            for(int height:{720,1080}){
                const int width=height==720?1280:1920;int handed=0;
                EncodedVideoOutput output(width,height,100000,"idle-probe",{},[&](const EncodedFrame&,FrameIdentity){handed++;},false,{},std::chrono::milliseconds(150));
                auto source=fixture(0);const auto start=nowUs();
                output.submit(source.nv12[height==720?1:0],{start,1,false});
                const auto until=Clock::now()+std::chrono::milliseconds(350);
                QJsonObject row{{"height",height}};
                try{while(Clock::now()<until){output.poll();std::this_thread::sleep_for(std::chrono::milliseconds(1));}}
                catch(const std::exception& e){row["failure_code"]=publicationFailureCode(e.what());}
                row["output"]=output.report();row["handed"]=handed;cases.append(row);
            }
            report={{"encoder_idle",cases}};
        }else{
            const int seconds=qEnvironmentVariableIntValue("GOODSHARING_TEST_SECONDS");require(seconds>=3&&seconds<=300,"Use 3 to 300 seconds");
            report=run(qEnvironmentVariable("GOODSHARING_PUBLISHER_CASE","stereo"),seconds);
        }
    }catch(const std::exception& e){report["error"]=QString::fromUtf8(e.what());}
    const auto data=QJsonDocument(report).toJson();std::cout<<data.constData();const auto args=app.arguments();const int at=args.indexOf("--report");if(at>=0&&at+1<args.size()){QFile file(args[at+1]);if(file.open(QIODevice::WriteOnly))file.write(data);}return report.contains("error")?1:0;
}
