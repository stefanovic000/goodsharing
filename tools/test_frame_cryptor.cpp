// Exercises the exact pinned WebRTC library, without RTP or decoded PCM/PLC.
#include "../patches/strict_frame_cryptor.h"
#include "../patches/closed_frame_transformer.h"
#include "api/make_ref_counted.h"
#include "rtc_base/thread.h"
#include <chrono>
#include <condition_variable>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <vector>

uint32_t cryptoTestRtpBase=0x43215678;
uint32_t cryptoTestSsrc=12345;
bool cryptoTestLongHeader=false;
namespace webrtc {
// The SDK explicitly permits this test-fake class name to construct frames.
class MockTransformableFrame : public TransformableFrameInterface {
public:
    MockTransformableFrame(std::vector<uint8_t> bytes, Direction direction, uint32_t timestamp)
        : TransformableFrameInterface(Passkey()), bytes_(std::move(bytes)), direction_(direction), timestamp_(timestamp) {}
    std::span<const uint8_t> GetData() const override { return bytes_; }
    void SetData(std::span<const uint8_t> data) override { bytes_.assign(data.begin(), data.end()); }
    uint8_t GetPayloadType() const override { return 111; }
    uint32_t GetSsrc() const override { return cryptoTestSsrc; }
    uint32_t GetTimestamp() const override { return cryptoTestRtpBase+timestamp_; }
    void SetRTPTimestamp(uint32_t timestamp) override { timestamp_ = timestamp-cryptoTestRtpBase; }
    Direction GetDirection() const override { return direction_; }
    std::string GetMimeType() const override { return "audio/opus"; }
    std::optional<Timestamp> ReceiveTime() const override { return std::nullopt; }
    std::optional<Timestamp> CaptureTime() const override { return std::nullopt; }
    std::optional<TimeDelta> SenderCaptureTimeOffset() const override { return std::nullopt; }
private:
    std::vector<uint8_t> bytes_;
    Direction direction_;
    uint32_t timestamp_;
};
class MockTransformableVideoFrame : public TransformableVideoFrameInterface {
public:
    MockTransformableVideoFrame(std::vector<uint8_t> bytes, Direction direction, uint32_t timestamp)
        : TransformableVideoFrameInterface(Passkey()), bytes_(std::move(bytes)), direction_(direction), timestamp_(timestamp) {
        header_.codec=kVideoCodecH264;header_.video_type_header=RTPVideoHeaderH264{};
    }
    std::span<const uint8_t> GetData() const override { return bytes_; }
    void SetData(std::span<const uint8_t> data) override { bytes_.assign(data.begin(),data.end()); }
    uint8_t GetPayloadType() const override { return 96; }
    uint32_t GetSsrc() const override { return cryptoTestSsrc; }
    uint32_t GetTimestamp() const override { return cryptoTestRtpBase+timestamp_; }
    void SetRTPTimestamp(uint32_t timestamp) override { timestamp_=timestamp-cryptoTestRtpBase; }
    Direction GetDirection() const override { return direction_; }
    std::string GetMimeType() const override { return "video/H264"; }
    std::optional<Timestamp> ReceiveTime() const override { return std::nullopt; }
    std::optional<Timestamp> CaptureTime() const override { return std::nullopt; }
    std::optional<TimeDelta> SenderCaptureTimeOffset() const override { return std::nullopt; }
    bool IsKeyFrame() const override { return true; }
    std::optional<std::string> Rid() const override { return std::nullopt; }
    VideoFrameMetadata Metadata() const override { return {}; }
    void SetMetadata(const VideoFrameMetadata&) override {}
    const RTPVideoHeader& header() const override { return header_; }
private:
    std::vector<uint8_t> bytes_;Direction direction_;uint32_t timestamp_;RTPVideoHeader header_;
};
}
using W = webrtc::FrameCryptorTransformer;
using Direction = webrtc::TransformableFrameInterface::Direction;
bool videoMode=false;
struct Failure {std::string message;};
void require(bool value, const char* message) { if (!value) throw Failure{message}; }
class Sink : public webrtc::TransformedFrameCallback {
public:
    explicit Sink(std::string label):label_(std::move(label)){}
    struct Result { uint32_t timestamp; std::vector<uint8_t> bytes; };
    void OnTransformedFrame(std::unique_ptr<webrtc::TransformableFrameInterface> frame) override {
        std::lock_guard lock(mutex_); const auto bytes=frame->GetData();
        frames_.push_back({frame->GetTimestamp()-cryptoTestRtpBase,{bytes.begin(),bytes.end()}}); ready_.notify_all();
    }
    std::vector<Result> until(uint32_t timestamp) {
        std::unique_lock lock(mutex_);
        if(!ready_.wait_for(lock,std::chrono::seconds(3),[&]{for(const auto& r:frames_)if(r.timestamp==timestamp)return true;return false;}))
            throw Failure{"Cryptor completion sentinel timed out: "+label_+" timestamp="+std::to_string(timestamp)+" callbacks="+std::to_string(frames_.size())};
        return frames_;
    }
    std::vector<Result> snapshot() { std::lock_guard lock(mutex_);return frames_; }
private:
    std::mutex mutex_;std::condition_variable ready_;std::vector<Result> frames_;std::string label_;
};
class States : public webrtc::FrameCryptorTransformerObserver {
public:
    void OnFrameCryptionStateChanged(const std::string, webrtc::FrameCryptionState state) override {
        std::lock_guard lock(mutex_);states_.push_back(state);ready_.notify_all();
    }
    void until(webrtc::FrameCryptionState state) {
        std::unique_lock lock(mutex_);
        require(ready_.wait_for(lock,std::chrono::seconds(3),[&]{for(auto value:states_)if(value==state)return true;return false;}),"Cryptor state sentinel timed out");
    }
private:
    std::mutex mutex_;std::condition_variable ready_;std::vector<webrtc::FrameCryptionState> states_;
};
webrtc::scoped_refptr<webrtc::KeyProvider> provider(bool strict, uint8_t keyByte=7) {
    webrtc::KeyProviderOptions options;options.shared_key=true;options.failure_tolerance=-1;
    webrtc::scoped_refptr<webrtc::KeyProvider> p;
    if(strict)p=webrtc::make_ref_counted<goodsharing::StrictKeyProvider>(options);
    else { p=webrtc::make_ref_counted<webrtc::DefaultKeyProviderImpl>(options);p->options().discard_frame_when_cryptor_not_ready=false; }
    p->SetSharedKey(0,std::vector<uint8_t>(32,keyByte));return p;
}
webrtc::scoped_refptr<W> cryptor(webrtc::Thread* signaling, bool strict, webrtc::scoped_refptr<webrtc::KeyProvider> p, webrtc::scoped_refptr<Sink> sink) {
    webrtc::scoped_refptr<W> c;
    const auto type=videoMode?W::MediaType::kVideoFrame:W::MediaType::kAudioFrame;
    if(strict)c=new goodsharing::StrictFrameCryptor(signaling,"sender",type,W::Algorithm::kAesGcm,p);
    else c=new W(signaling,"sender",type,W::Algorithm::kAesGcm,p);
    c->SetEnabled(true);
    webrtc::scoped_refptr<webrtc::FrameTransformerInterface> iface=c;
    if(videoMode)iface->RegisterTransformedFrameSinkCallback(sink,cryptoTestSsrc);else iface->RegisterTransformedFrameCallback(sink);return c;
}
void submit(webrtc::scoped_refptr<W> c,std::vector<uint8_t> bytes,Direction direction,uint32_t timestamp) {
    webrtc::scoped_refptr<webrtc::FrameTransformerInterface> iface=c;
    if(videoMode)iface->Transform(std::make_unique<webrtc::MockTransformableVideoFrame>(std::move(bytes),direction,timestamp));
    else iface->Transform(std::make_unique<webrtc::MockTransformableFrame>(std::move(bytes),direction,timestamp));
}
int main(int argc,char** argv) {
    unsigned malformedSamples=0;
    bool strict=true;for(int i=1;i<argc;i++){if(std::string(argv[i])=="--upstream-control")strict=false;else if(std::string(argv[i])=="--video")videoMode=true;else if(std::string(argv[i])=="--rtp-edge")cryptoTestRtpBase=0;else if(std::string(argv[i])=="--rtp-wrap")cryptoTestRtpBase=0xffffff80;else if(std::string(argv[i])=="--ssrc-edge")cryptoTestSsrc=0x00000101;else if(std::string(argv[i])=="--long-header")cryptoTestLongHeader=true;else return 2;}
    try {
        auto signaling=webrtc::Thread::Create();require(signaling->Start(),"Signaling thread failed");
        auto setupSink=webrtc::make_ref_counted<Sink>("closed setup");
        auto setupGate=webrtc::make_ref_counted<goodsharing::ClosedFrameTransformer>();
        setupGate->RegisterTransformedFrameCallback(setupSink);
        setupGate->RegisterTransformedFrameSinkCallback(setupSink,cryptoTestSsrc);
        for(unsigned i=0;i<16;i++) {
            if(videoMode)setupGate->Transform(std::make_unique<webrtc::MockTransformableVideoFrame>(std::vector<uint8_t>(64,0x37),Direction::kReceiver,i));
            else setupGate->Transform(std::make_unique<webrtc::MockTransformableFrame>(std::vector<uint8_t>(64,0x37),Direction::kReceiver,i));
        }
        setupGate=nullptr;
        require(setupSink->snapshot().empty(),"Closed setup transformer forwarded media");
        std::vector<uint8_t> plain(64,0x37);plain[0]=0xf8;
        if(videoMode){plain[0]=plain[1]=plain[2]=0;plain[3]=1;plain[4]=0x65;plain[5]=0x88;}
        if(videoMode&&cryptoTestLongHeader){auto slice=plain;plain.assign(300,0x37);plain[0]=plain[1]=plain[2]=0;plain[3]=1;plain[4]=0x67;plain.insert(plain.end(),slice.begin(),slice.end());}
        auto encodeSink=webrtc::make_ref_counted<Sink>("encoder");auto enc=cryptor(signaling.get(),strict,provider(strict),encodeSink);
        submit(enc,plain,Direction::kSender,100);auto encrypted=encodeSink->until(100).back().bytes;
        require(encrypted!=plain&&encrypted.size()>plain.size(),"Encryption forwarded plaintext");
        auto otherSink=webrtc::make_ref_counted<Sink>("other-key encoder");auto other=cryptor(signaling.get(),strict,provider(strict,9),otherSink);
        submit(other,plain,Direction::kSender,101);auto wrong=otherSink->until(101).back().bytes;

        auto missingSink=webrtc::make_ref_counted<Sink>("missing-key encoder");auto states=webrtc::make_ref_counted<States>();
        auto missingSender=cryptor(signaling.get(),strict,provider(strict),missingSink);
        missingSender->RegisterFrameCryptorTransformerObserver(states);missingSender->SetKeyIndex(5);
        submit(missingSender,plain,Direction::kSender,300);states->until(webrtc::kMissingKey);
        missingSender->SetKeyIndex(0);submit(missingSender,plain,Direction::kSender,301);
        auto afterMissing=missingSink->until(301);require(afterMissing.size()==1&&afterMissing[0].bytes!=plain,"Missing sender key exposed media");
        missingSender=nullptr;

        auto receiveProvider=provider(strict);receiveProvider->SetSifTrailer({0xa1,0xb2,0xc3,0xd4});
        auto sink=webrtc::make_ref_counted<Sink>("receiver");auto dec=cryptor(signaling.get(),strict,receiveProvider,sink);
        submit(dec,encrypted,Direction::kReceiver,99);require(sink->until(99).back().bytes==plain,"Initial valid authentication corrupted payload");
        auto tagged=plain;tagged.insert(tagged.end(),{0xa1,0xb2,0xc3,0xd4});
        submit(dec,tagged,Direction::kReceiver,1);
        submit(dec,plain,Direction::kReceiver,2);
        submit(dec,wrong,Direction::kReceiver,3);
        auto tampered=encrypted;tampered[20]^=0x55;submit(dec,tampered,Direction::kReceiver,4);
        auto missing=encrypted;missing.back()=5;submit(dec,missing,Direction::kReceiver,5);
        auto truncated=encrypted;truncated.resize(truncated.size()-4);submit(dec,truncated,Direction::kReceiver,6);
        submit(dec,{},Direction::kReceiver,7);
#ifdef GOODSHARING_FIXED_PARSER_TEST
        // Incomplete headers/trailers must fail closed without indexing before
        // the buffer, allocating from underflow or crashing a reader worker.
        submit(dec,{0},Direction::kReceiver,8);
        submit(dec,{0,0},Direction::kReceiver,9);
        submit(dec,{0,0,1,0x65},Direction::kReceiver,10);
        submit(dec,{0,0,0,1,0x65,0},Direction::kReceiver,11);
        if(strict) {
            uint32_t random=0x71a53f29;
            for(uint32_t n=0;n<128;n++) {
                std::vector<uint8_t> bytes(n%97+1);
                for(auto& byte:bytes){random^=random<<13;random^=random>>17;random^=random<<5;byte=static_cast<uint8_t>(random);}
                if(bytes.size()>=32){bytes[bytes.size()-2]=12;bytes.back()=0;}
                if(videoMode&&bytes.size()>=6&&n%2==0){bytes[0]=bytes[1]=bytes[2]=0;bytes[3]=1;bytes[4]=0x65;bytes[5]=0x88;}
                submit(dec,std::move(bytes),Direction::kReceiver,1000+n);
                ++malformedSamples;
            }
        }
#endif
        submit(dec,encrypted,Direction::kReceiver,100);
        const auto received=sink->until(100);unsigned bypassed=0;bool taggedPlaintext=false;
        for(const auto& r:received) {if(r.timestamp!=99&&r.timestamp!=100){++bypassed;if(r.timestamp==1&&r.bytes==plain)taggedPlaintext=true;}else require(r.bytes==plain,"Valid authentication corrupted payload");}
        if(strict)require(bypassed==0,"Unauthenticated input reached decoded payload callback");

        // Disabled upstream cryptors deliberately passthrough. The strict
        // variant must never enter that mode, including a later enable call.
        dec->SetEnabled(false);submit(dec,plain,Direction::kReceiver,200);
        enc->SetEnabled(false);submit(enc,plain,Direction::kSender,201);
        bool disabledSenderPlaintext=false,disabledReceiverPlaintext=false;
        if(!strict){disabledReceiverPlaintext=sink->until(200).back().bytes==plain;disabledSenderPlaintext=encodeSink->until(201).back().bytes==plain;require(disabledReceiverPlaintext&&disabledSenderPlaintext,"Upstream disabled-mode control did not forward plaintext");}
        else {
            dec->SetEnabled(true);enc->SetEnabled(true);
            require(!dec->enabled()&&!enc->enabled(),"Revoked cryptor reopened");
            submit(dec,encrypted,Direction::kReceiver,202);submit(enc,plain,Direction::kSender,203);
        }
        // Destroying the transformer joins its worker; no assertion depends on
        // sleeping or mistaking a delayed callback for a rejected frame.
        dec=nullptr;enc=nullptr;other=nullptr;
        if(strict){require(sink->snapshot().size()==2,"Revoked receiver forwarded media");require(encodeSink->snapshot().size()==1,"Revoked sender forwarded media");}
        std::cout<<"{\"passed\":true,\"video\":"<<(videoMode?"true":"false")<<",\"strict\":"<<(strict?"true":"false")<<",\"malformed_samples\":"<<malformedSamples<<",\"unauthenticated_forwarded\":"<<bypassed<<",\"tagged_plaintext_forwarded\":"<<(taggedPlaintext?"true":"false")<<",\"disabled_sender_plaintext_forwarded\":"<<(disabledSenderPlaintext?"true":"false")<<",\"disabled_receiver_plaintext_forwarded\":"<<(disabledReceiverPlaintext?"true":"false")<<",\"missing_sender_key_rejected\":true,\"valid_roundtrip\":true,\"revocation_checked\":true}\n";
        return 0;
    }catch(const Failure& e){std::cerr<<e.message<<'\n';return 1;}
}
