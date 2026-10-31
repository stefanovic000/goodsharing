"""Cancel late publisher negotiation during shutdown of the pinned engine."""
import difflib
import hashlib
import json
from pathlib import Path
import zipfile
from livekit_protocol_patch import E2EE_ORIGINAL, E2EE_PATCHED
from prepare_frame_cryptor import patched_source
from audio_timing_patch import engine_audio_changes
from prepare_receiver_clock import patched_receiver_sources
from receiver_timing_patch import receiver_video_changes

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / '.deps/sources/rust-a6f93a16/rust-sdks-a6f93a16'


def main():
    marker = SOURCE / '.goodsharing-close-patched'
    archive = ROOT / '.deps/sources/livekit-rust-a6f93a16.zip'
    if hashlib.sha256(archive.read_bytes()).hexdigest() != 'd361ab5562481869f0f1aeaa588bd811ab8a26abeb96fedde5000b9d783c05e0':
        raise RuntimeError('Pinned engine source archive checksum mismatch')
    changes = []
    pending = []
    patch_path = ROOT / 'patches/livekit-rust-a6f93a16-shutdown.patch'
    previous = json.loads(marker.read_text(encoding='utf-8')) if marker.exists() else {}
    previous_valid = patch_path.exists() and previous.get('patch_sha256') == hashlib.sha256(patch_path.read_bytes()).hexdigest()

    def change(relative, pairs):
        path = SOURCE / relative
        with zipfile.ZipFile(archive) as source_zip:
            before = source_zip.read('rust-sdks-a6f93a16/' + relative).decode()
        after = before
        for replacement in pairs:
            old, new, *expected = replacement
            if after.count(old) != (expected[0] if expected else 1):
                raise RuntimeError('Unexpected pinned engine context: ' + old[:80])
            after = after.replace(old, new)
        changes.extend(difflib.unified_diff(before.splitlines(True), after.splitlines(True), fromfile='a/' + relative, tofile='b/' + relative))
        # Allow replacing only our own previous patch or the pristine source.
        current = path.read_text(encoding='utf-8')
        if current != after:
            previous_hash = previous.get('source_sha256', {}).get(relative)
            if current != before and not (previous_valid and hashlib.sha256(current.encode()).hexdigest() == previous_hash):
                raise RuntimeError('Unexpected local edits in engine source: ' + relative)
        pending.append((path, after))

    def create(relative, after):
        path = SOURCE / relative
        current = path.read_text(encoding='utf-8') if path.exists() else ''
        previous_hash = previous.get('source_sha256', {}).get(relative)
        if current and current != after and not (previous_valid and hashlib.sha256(current.encode()).hexdigest() == previous_hash):
            raise RuntimeError('Unexpected local edits in added engine source: ' + relative)
        changes.extend(difflib.unified_diff([], after.splitlines(True), fromfile='/dev/null', tofile='b/' + relative))
        pending.append((path, after))

    def pristine_audio(relative):
        with zipfile.ZipFile(archive) as source_zip:
            return source_zip.read('rust-sdks-a6f93a16/' + relative).decode('utf-8')
    for relative, pairs in engine_audio_changes(pristine_audio):
        change(relative, pairs)
    for relative, pairs in receiver_video_changes(pristine_audio, archive):
        change(relative, pairs)
    create('webrtc-sys/include/livekit/preencoded_chain.h', (ROOT / 'patches/preencoded_chain.h').read_text(encoding='utf-8'))
    create('webrtc-sys/include/livekit/strict_frame_cryptor.h', (ROOT / 'patches/strict_frame_cryptor.h').read_text(encoding='utf-8'))
    create('webrtc-sys/include/livekit/closed_frame_transformer.h', (ROOT / 'patches/closed_frame_transformer.h').read_text(encoding='utf-8'))
    create('webrtc-sys/include/livekit/source_audio_clock.h', (ROOT / 'patches/source_audio_clock.h').read_text(encoding='utf-8'))
    create('webrtc-sys/include/livekit/receiver_media_clock.h', (ROOT / 'patches/receiver_media_clock.h').read_text(encoding='utf-8'))
    for name, content in patched_receiver_sources().items():
        create('webrtc-sys/src/' + name, content)
    repaired_cryptor = patched_source()
    create('webrtc-sys/src/goodsharing_frame_crypto_transformer.cpp', repaired_cryptor)
    # The same generated source is compiled by the isolated native policy test.
    native_source = ROOT / '.deps/sources/webrtc-89d790b/frame_crypto_transformer.fixed.cc'
    if not native_source.exists() or native_source.read_text(encoding='utf-8') != repaired_cryptor:
        native_source.write_text(repaired_cryptor, encoding='utf-8', newline='\n')
    change('webrtc-sys/build.rs', [
        ('        "src/frame_cryptor.cpp",', '        "src/frame_cryptor.cpp",\n        "src/goodsharing_frame_crypto_transformer.cpp",\n        "src/goodsharing_channel_receive.cpp",\n        "src/goodsharing_rtp_video_stream_receiver2.cpp",'),
        ('        webrtc_include.join("third_party/abseil-cpp/"),', '        webrtc_include.join("third_party/abseil-cpp/"),\n        webrtc_include.join("third_party/boringssl/src/include/"),'),
    ])
    # Restore the pristine factory after the rejected pre-decode experiment.
    change('webrtc-sys/src/video_decoder_factory.cpp', [])
    change('webrtc-sys/include/livekit/video_track.h', [
        ('  explicit NativeVideoSink(rust::Box<VideoSinkWrapper> observer);', '''  explicit NativeVideoSink(rust::Box<VideoSinkWrapper> observer);
  ~NativeVideoSink() override;
  void set_remote(bool remote) { remote_.store(remote, std::memory_order_release); }'''),
        ('  rust::Box<VideoSinkWrapper> observer_;', '''  rust::Box<VideoSinkWrapper> observer_;
  std::atomic<bool> remote_{false};
  uint64_t frame_count_ = 0, stale_drops_ = 0, missing_packet_times_ = 0;
  int64_t maximum_seen_us_ = 0, maximum_forward_us_ = 0;'''),
    ])
    change('webrtc-sys/src/video_track.cpp', [
        ('#include <algorithm>', '#include <algorithm>\n#include <cstdio>\n#include <cstdlib>\n#include <chrono>\n#include <thread>\n#include "livekit/preencoded_chain.h"'),
        ('  track()->AddOrUpdateSink(sink.get(),', '''  const auto* source = track()->GetSource();
  sink->set_remote(source && source->remote());
  track()->AddOrUpdateSink(sink.get(),'''),
        ('  observer_->on_frame(std::make_unique<VideoFrame>(frame));', '''  if (remote_.load(std::memory_order_acquire) && std::getenv("GOODSHARING_TEST_AUDIO_TRACE") &&
      (frame_count_ <= 4 || frame_count_ == 60 || frame_count_ == 300)) {
    std::fprintf(stderr, "GoodSharing native video clock: sequence=%llu rtp=%u timestamp_us=%lld ntp_ms=%lld sink_us=%lld\\n",
        static_cast<unsigned long long>(frame_count_), frame.rtp_timestamp(),
        static_cast<long long>(frame.timestamp_us()), static_cast<long long>(frame.ntp_time_ms()),
        static_cast<long long>(webrtc::TimeMicros()));
  }
  observer_->on_frame(std::make_unique<VideoFrame>(frame));'''),
        ('void NativeVideoSink::OnFrame(const webrtc::VideoFrame& frame) {', '''NativeVideoSink::~NativeVideoSink() {
  if (remote_.load(std::memory_order_acquire) && std::getenv("GOODSHARING_TEST_NETWORK_TELEMETRY")) {
    std::fprintf(stderr, "GoodSharing decoded age: stale_drops=%llu missing_packet_times=%llu maximum_seen_us=%lld maximum_forward_us=%lld\\n",
                 static_cast<unsigned long long>(stale_drops_), static_cast<unsigned long long>(missing_packet_times_),
                 static_cast<long long>(maximum_seen_us_), static_cast<long long>(maximum_forward_us_));
  }
}

void NativeVideoSink::OnFrame(const webrtc::VideoFrame& frame) {
  if (remote_.load(std::memory_order_acquire)) {
    // Decode and WebRTC completion already ran. Withhold only obsolete decoded
    // images, before Rust/FFI conversion; never discard compressed references.
    if (++frame_count_ == 60 && std::getenv("GOODSHARING_TEST_NETWORK_TELEMETRY")) {
      const char* value = std::getenv("GOODSHARING_TEST_PRESENT_STALL_MS");
      const long ms = value ? std::strtol(value, nullptr, 10) : 0;
      if (ms > 0 && ms <= 250) std::this_thread::sleep_for(std::chrono::milliseconds(ms));
    }
    auto first = webrtc::Timestamp::PlusInfinity();
    for (const auto& packet : frame.packet_infos()) {
      if (packet.receive_time().IsFinite()) first = std::min(first, packet.receive_time());
    }
    if (!first.IsFinite()) { ++missing_packet_times_; return; }
    const auto age_us = webrtc::TimeMicros() - first.us();
    maximum_seen_us_ = std::max(maximum_seen_us_, age_us);
    if (!goodsharing::sdkInputFresh(age_us)) { ++stale_drops_; return; }
    maximum_forward_us_ = std::max(maximum_forward_us_, age_us);
  }'''),
    ])
    change('webrtc-sys/include/livekit/encoded_video_frame_buffer.h', [
        ('  EncodedRateControlRequest Take();', '  EncodedRateControlRequest Take();\n  uint64_t NextSequence() { return sequence_.fetch_add(1, std::memory_order_relaxed) + 1; }'),
        ('  EncodedRateControlRequest request_;', '  EncodedRateControlRequest request_;\n  std::atomic<uint64_t> sequence_{0};'),
        ('  EncodedFrameType frame_type() const { return frame_type_; }', '''  EncodedFrameType frame_type() const { return frame_type_; }
  uint64_t sequence() const { return sequence_; }
  int64_t queued_age_us() const;
  const std::shared_ptr<EncodedRateControlState>& source_state() const { return rate_control_state_; }'''),
        ('  std::shared_ptr<EncodedRateControlState> rate_control_state_;', '  std::shared_ptr<EncodedRateControlState> rate_control_state_;\n  uint64_t sequence_ = 0;\n  int64_t enqueued_at_us_ = 0;'),
    ])
    change('webrtc-sys/src/encoded_video_frame_buffer.cpp', [
        ('#include "rtc_base/logging.h"', '#include "rtc_base/logging.h"\n#include "rtc_base/time_utils.h"'),
        ('      rate_control_state_(std::move(rate_control_state)) {}', '''      rate_control_state_(std::move(rate_control_state)) {
  if (rate_control_state_) sequence_ = rate_control_state_->NextSequence();
  enqueued_at_us_ = webrtc::TimeMicros();
}'''),
        ('webrtc::VideoFrameBuffer::Type EncodedVideoFrameBuffer::type() const {', '''int64_t EncodedVideoFrameBuffer::queued_age_us() const {
  return webrtc::TimeMicros() - enqueued_at_us_;
}

webrtc::VideoFrameBuffer::Type EncodedVideoFrameBuffer::type() const {'''),
    ])
    change('livekit/src/rtc_engine/peer_transport.rs', [
        ('''        let answer = self.peer_connection().create_answer(options).await?;
        self.peer_connection().set_local_description(answer.clone()).await?;''', '''        let answer = self.peer_connection().create_answer(options).await?;
        // Dual-peer subscribers answer the SFU offer. Advertise stereo receive
        // capability here too, so decoded PCM matches the requested layout.
        let stereo = Self::munge_stereo_for_audio(&answer.to_string());
        let answer = SessionDescription::parse(&stereo, answer.sdp_type())
            .map_err(|_| super::EngineError::Internal("Invalid stereo audio answer".into()))?;
        self.peer_connection().set_local_description(answer.clone()).await?;'''),
        ('''        let mut opus_pts: Vec<&str> = Vec::new();
        for line in &lines {
            let l = line.trim();''', '''        let mut opus_pts: Vec<(usize, &str)> = Vec::new();
        let mut section = 0;
        let mut audio = false;
        for line in &lines {
            let l = line.trim();
            if l.starts_with("m=") {
                section += 1;
                audio = l.starts_with("m=audio ") && !l.starts_with("m=audio 0 ");
            }
            if !audio { continue; }'''),
        ('                    opus_pts.push(pt);', '                    opus_pts.push((section, pt));'),
        ('''        for line in lines {
            let mut rewritten = line.to_string();

            for pt in &opus_pts {''', '''        let mut section = 0;
        for line in lines {
            if line.trim().starts_with("m=") { section += 1; }
            let mut rewritten = line.to_string();

            for (opus_section, pt) in &opus_pts {
                if *opus_section != section { continue; }'''),
        ('                    if !rewritten.contains("stereo=") {', '''                    if !rewritten[prefix.len()..].split(';').any(|param|
                        param.trim().split_once('=').is_some_and(|(key, _)| key.trim() == "stereo")) {'''),
        ('atomic::{AtomicU32, Ordering}', 'atomic::{AtomicBool, AtomicU32, Ordering}'),
        ('    signal_target: proto::SignalTarget,\n    peer_connection:', '    closed: AtomicBool,\n    signal_target: proto::SignalTarget,\n    peer_connection:'),
        ('        Self {\n            signal_target,', '        Self {\n            closed: AtomicBool::new(false),\n            signal_target,'),
        ('    pub fn close(&self) {\n', '''    pub fn close(&self) {
        self.closed.store(true, Ordering::Release);
        self.on_offer_handler.lock().take();
'''),
        ('    pub async fn create_and_send_offer(&self, options: OfferOptions) -> EngineResult<()> {\n        let mut inner = self.inner.lock().await;', '''    pub async fn create_and_send_offer(&self, options: OfferOptions) -> EngineResult<()> {
        if self.closed.load(Ordering::Acquire) {
            return Ok(());
        }
        let result = self.create_and_send_offer_inner(options).await;
        // close() synchronously closes sockets even if an offer future is in flight.
        // Such a future is cancelled, not a failed live-session negotiation.
        if self.closed.load(Ordering::Acquire) { Ok(()) } else { result }
    }

    async fn create_and_send_offer_inner(&self, options: OfferOptions) -> EngineResult<()> {
        let mut inner = self.inner.lock().await;
        if self.closed.load(Ordering::Acquire) { return Ok(()); }'''),
        ('        let mut offer = self.peer_connection.create_offer(options).await?;', '        let mut offer = self.peer_connection.create_offer(options).await?;\n        if self.closed.load(Ordering::Acquire) { return Ok(()); }'),
        ('        // Apply x-google-start-bitrate for all video codecs to improve initial quality.', '''        // GoodSharing publishes from the separate publisher transport. Request
        // stereo Opus there as well; subscriber-answer munging alone does not
        // guarantee that the sender encodes distinct channels.
        let stereo_offer = Self::munge_stereo_for_audio(&sdp);
        if stereo_offer != sdp {
            match SessionDescription::parse(&stereo_offer, offer.sdp_type()) {
                Ok(parsed) => {
                    offer = parsed;
                    sdp = stereo_offer;
                }
                Err(e) => log::warn!("Failed to parse stereo-munged publisher offer: {e}"),
            }
        }

        // Apply x-google-start-bitrate for all video codecs to improve initial quality.'''),
        ('        self.peer_connection.set_local_description(offer.clone()).await?;', '        if self.closed.load(Ordering::Acquire) { return Ok(()); }\n        self.peer_connection.set_local_description(offer.clone()).await?;'),
        ('        if let Some(handler) = self.on_offer_handler.lock().as_mut() {\n            handler(offer);\n        }\n\n        Ok(())\n    }\n}', '''        let mut handler = self.on_offer_handler.lock();
        if !self.closed.load(Ordering::Acquire) {
            if let Some(handler) = handler.as_mut() { handler(offer); }
        }

        Ok(())
    }
}'''),
        ('mod tests {\n    use std::time::Duration;', '''mod tests {
    #[test]
    fn stereo_receive_capability_is_scoped_and_distinct_from_sprop() {
        let sdp = "v=0\\r\\nm=audio 9 UDP/TLS/RTP/SAVPF 111\\r\\na=rtpmap:111 opus/48000/2\\r\\na=fmtp:111 minptime=10;sprop-stereo=1\\r\\nm=video 9 UDP/TLS/RTP/SAVPF 111\\r\\na=rtpmap:111 H264/90000\\r\\na=fmtp:111 profile-level-id=42e01f\\r\\n";
        let out = super::PeerTransport::munge_stereo_for_audio(sdp);
        assert!(out.contains("a=fmtp:111 minptime=10;sprop-stereo=1;stereo=1\\r\\n"));
        assert!(out.contains("a=fmtp:111 profile-level-id=42e01f\\r\\n"));
        assert_eq!(out, super::PeerTransport::munge_stereo_for_audio(&out));
        assert!(!out.contains("\\n") || out.matches('\\n').count() == out.matches("\\r\\n").count());
        let disabled = "v=0\\nm=audio 0 UDP/TLS/RTP/SAVPF 111\\na=rtpmap:111 opus/48000/2\\na=fmtp:111 minptime=10\\n";
        assert_eq!(super::PeerTransport::munge_stereo_for_audio(disabled), disabled);
        let explicit = "v=0\\nm=audio 9 UDP/TLS/RTP/SAVPF 111\\na=rtpmap:111 opus/48000/2\\na=fmtp:111 stereo=0;sprop-stereo=1\\n";
        assert_eq!(super::PeerTransport::munge_stereo_for_audio(explicit), explicit);
    }

    #[tokio::test]
    async fn dual_peer_answer_advertises_stereo() {
        use libwebrtc::prelude::*;
        use livekit_protocol as proto;
        let factory = PeerConnectionFactory::default();
        let offerer = factory.create_peer_connection(RtcConfiguration::default()).unwrap();
        let responder = factory.create_peer_connection(RtcConfiguration::default()).unwrap();
        offerer.add_transceiver_for_media(MediaType::Audio, RtpTransceiverInit {
            direction: RtpTransceiverDirection::RecvOnly, stream_ids: Vec::new(), send_encodings: Vec::new(),
        }).unwrap();
        let transport = super::PeerTransport::new(responder, proto::SignalTarget::Subscriber, false);
        for round in 0..2 {
            if round == 1 {
                offerer.add_transceiver_for_media(MediaType::Video, RtpTransceiverInit {
                    direction: RtpTransceiverDirection::RecvOnly, stream_ids: Vec::new(), send_encodings: Vec::new(),
                }).unwrap();
            }
            let offer = offerer.create_offer(OfferOptions::default()).await.unwrap();
            offerer.set_local_description(offer.clone()).await.unwrap();
            let answer = transport.create_anwser(offer, AnswerOptions::default()).await.unwrap();
            assert!(answer.to_string().lines().any(|line| line.starts_with("a=fmtp:111 ") &&
                line.split_once(' ').unwrap().1.split(';').any(|param| param.trim() == "stereo=1")),
                "Dual-peer audio answer omitted stereo receive capability");
            offerer.set_remote_description(answer).await.unwrap();
        }
        transport.close();offerer.close();
    }

    #[tokio::test]
    async fn shutdown_cancels_offer_and_does_not_wait_for_transport_lock() {
        use libwebrtc::prelude::*;
        use livekit_protocol as proto;
        use std::sync::{Arc, atomic::{AtomicUsize, Ordering}};
        let factory = PeerConnectionFactory::default();
        let pc = factory.create_peer_connection(RtcConfiguration::default()).unwrap();
        let _channel = pc.create_data_channel("shutdown-test", DataChannelInit::default()).unwrap();
        let transport = super::PeerTransport::new(pc, proto::SignalTarget::Publisher, true);
        let offers = Arc::new(AtomicUsize::new(0));
        let emitted = offers.clone();
        transport.on_offer(Some(Box::new(move |_| { emitted.fetch_add(1, Ordering::Relaxed); })));
        let future = transport.create_and_send_offer(OfferOptions::default());
        tokio::pin!(future);
        let pending = std::future::poll_fn(|cx| std::task::Poll::Ready(
            match std::future::Future::poll(future.as_mut(), cx) {
                std::task::Poll::Pending => true,
                std::task::Poll::Ready(result) => { result.unwrap(); false }
            }
        )).await;
        transport.close();
        let at_close = offers.load(Ordering::Relaxed);
        // A new request after closure must not wait behind an in-flight offer.
        // Holding the transport lock makes this assertion deterministic.
        if pending {
            tokio::time::timeout(std::time::Duration::from_secs(2), future.as_mut()).await.unwrap().unwrap();
        }
        let _guard = transport.inner.lock().await;
        tokio::time::timeout(std::time::Duration::from_millis(100), transport.create_and_send_offer(OfferOptions::default())).await.unwrap().unwrap();
        assert_eq!(offers.load(Ordering::Relaxed), at_close, "offer emitted after closure");
        assert_eq!(transport.peer_connection().connection_state(), PeerConnectionState::Closed);
    }

    use std::time::Duration;'''),
    ])
    change('livekit/src/rtc_engine/rtc_session.rs', [
        ('                self.signal_client\n                    .send(proto::signal_request::Message::Answer(proto::SessionDescription {', '''                // Native OnTrack does not refire for a reused receiver. Diff
                // receiver streams after every subscriber offer, as for the
                // single-peer answer path, including removal of retired msids.
                self.process_remote_track_addition(
                    &self.subscriber_pc.as_ref().unwrap().peer_connection());

                self.signal_client
                    .send(proto::signal_request::Message::Answer(proto::SessionDescription {'''),
        ('        self.closed.store(true, Ordering::Release);\n        self.pc_state_notify.notify_waiters();', '''        self.closed.store(true, Ordering::Release);
        // Drop cancels a debounced offer before the peer connection is closed.
        self.negotiation_debouncer.lock().take();
        self.pc_state_notify.notify_waiters();'''),
        ('    fn publisher_negotiation_needed(self: &Arc<Self>) {\n', '    fn publisher_negotiation_needed(self: &Arc<Self>) {\n        if self.closed.load(Ordering::Acquire) { return; }\n'),
        ('    async fn execute_negotiation_with_retry(self: &Arc<Self>) {\n        loop {', '''    async fn wait_negotiation_closed(&self) {
        loop {
            let notified = self.pc_state_notify.notified();
            tokio::pin!(notified);
            notified.as_mut().enable();
            if self.closed.load(Ordering::Acquire) { return; }
            notified.await;
        }
    }

    async fn execute_negotiation_with_retry(self: &Arc<Self>) {
        loop {
            if self.closed.load(Ordering::Acquire) { break; }'''),
        ('                    _ = self.negotiation_queue.waker.notified() => {', '''                    _ = self.wait_negotiation_closed() => {
                        self.negotiation_queue.waiting_for_answer.store(false, Ordering::Release);
                        break;
                    }
                    _ = self.negotiation_queue.waker.notified() => {'''),
        ('                log::debug!("negotiating the publisher (debounced)");', '                if session.closed.load(Ordering::Acquire) { return; }\n                log::debug!("negotiating the publisher (debounced)");'),
    ])
    change('livekit/src/room/participant/local_participant.rs', [
        ('            LocalTrack::Audio(_audio_track) => {\n                // Setup audio encoding', '''            LocalTrack::Audio(_audio_track) => {
                // Advertise the actual native source layout to the SFU so its
                // subscription offer requests stereo Opus instead of a downmix.
                req.stereo = _audio_track.rtc_source().num_channels() > 1;
                // Setup audio encoding'''),
    ])
    change('livekit-signaling/src/lib.rs', [
        ('                if let SignalError::TokenFormat = err {\n                    return Err(err);\n                }', '''                // A failed/timed-out transport has no HTTP status to enrich.
                // Probing that same unreachable endpoint adds a second timeout.
                if !matches!(&err, SignalError::Handshake { .. }) {
                    return Err(err);
                }'''),
    ])
    change('livekit-ffi/src/server/room.rs', [
        ('RoomEvent::E2eeStateChanged { participant, state }', 'RoomEvent::E2eeStateChanged { participant, track_sid, state }'),
        ('                    state: proto::EncryptionState::from(state).into(),', '                    state: proto::EncryptionState::from(state).into(),\n                    track_sid: Some(track_sid.to_string()),'),
        ('impl FfiRoom {\n', '''impl FfiRoom {
    pub(super) fn forget_track_handle(&self, sid: &TrackSid, id: FfiHandleId) {
        let mut tracks = self.inner.track_handle_lookup.lock();
        if tracks.get(sid).copied() == Some(id) { tracks.remove(sid); }
    }

'''),
        ('        for (_, &handle) in self.inner.track_handle_lookup.lock().iter() {', '''        // Take ownership before dropping handles: drop_handle also forgets a
        // released track in this index, and must not recurse into its mutex.
        let tracks: Vec<_> = self.inner.track_handle_lookup.lock().drain().map(|(_, id)| id).collect();
        for handle in tracks {'''),
    ])
    change('livekit-ffi/src/server/mod.rs', [
        ('        let existed = self.ffi_handles.remove(&id).is_some();', '''        let removed = self.ffi_handles.remove(&id);
        let existed = removed.is_some();
        // The room index is non-owning. Forget a client-released remote track
        // so a later room close cannot attempt to release that handle twice.
        if let Some((_, handle)) = &removed {
            if let Some(track) = handle.downcast_ref::<room::FfiTrack>() {
                if let Some(room_id) = track.room_handle {
                    if let Ok(room) = self.retrieve_handle::<room::FfiRoom>(room_id) {
                        room.forget_track_handle(&track.track.sid(), id);
                    }
                }
            }
        }
        drop(removed);'''),
    ])
    change('webrtc-sys/src/passthrough_video_encoder.cpp', [
        ('#include "livekit/passthrough_video_encoder.h"', '#include "livekit/passthrough_video_encoder.h"\n#include "livekit/preencoded_chain.h"\n#include <cstdio>\n#include <cstdlib>\n#include <thread>\n#include <chrono>'),
        ('    codec_ = *codec_settings;', '    codec_ = *codec_settings;\n    chain_ = goodsharing::PreencodedChain{};\n    source_state_.reset();'),
        ('  int32_t InitEncode(const VideoCodec* codec_settings,', '''  ~PassthroughVideoEncoder() override {
    if (std::getenv("GOODSHARING_TEST_NETWORK_TELEMETRY")) {
      std::fprintf(stderr, "GoodSharing SDK age: stale_drops=%llu maximum_seen_us=%lld maximum_forward_us=%lld\\n",
                   static_cast<unsigned long long>(stale_sdk_inputs_),
                   static_cast<long long>(maximum_seen_age_us_), static_cast<long long>(maximum_forward_age_us_));
    }
  }

  int32_t InitEncode(const VideoCodec* codec_settings,'''),
        ('    const bool is_keyframe = IsKeyframe(encoded_buffer->frame_type());', '''    const bool is_keyframe = IsKeyframe(encoded_buffer->frame_type());
    // WebRTC can still replace/skip inputs before Encode (queue pressure,
    // suspension), even with its media-opt dropper disabled. These are
    // externally encoded references: never forward deltas across that gap.
    if (source_state_ != encoded_buffer->source_state()) {
      source_state_ = encoded_buffer->source_state();
      chain_ = goodsharing::PreencodedChain{};
    }
    const auto previous_gaps = chain_.gaps();
    if (!test_stall_checked_ && encoded_buffer->sequence() >= 60) {
      test_stall_checked_ = true;
      const char* test_stall = std::getenv("GOODSHARING_TEST_SDK_STALL_MS");
      const long ms = test_stall ? std::strtol(test_stall, nullptr, 10) : 0;
      if (ms > 0 && ms <= 250 && std::getenv("GOODSHARING_TEST_NETWORK_TELEMETRY")) {
        std::this_thread::sleep_for(std::chrono::milliseconds(ms));
      }
    }
    const auto queued_age_us = encoded_buffer->queued_age_us();
    maximum_seen_age_us_ = std::max(maximum_seen_age_us_, queued_age_us);
    const bool fresh = goodsharing::sdkInputFresh(queued_age_us);
    if (!fresh) ++stale_sdk_inputs_;
    if (!chain_.admit(encoded_buffer->sequence(), is_keyframe, fresh)) {
      if (std::getenv("GOODSHARING_TEST_NETWORK_TELEMETRY") && diagnostic_drops_++ < 8) {
        std::fprintf(stderr, "GoodSharing chain drop: sequence=%llu key=%d gaps=%llu\\n",
                     static_cast<unsigned long long>(encoded_buffer->sequence()), is_keyframe,
                     static_cast<unsigned long long>(chain_.gaps()));
      }
      if (chain_.gaps() != previous_gaps && chain_.gaps() <= 8) {
        RTC_LOG(LS_WARNING) << "GoodSharing preencoded input gap; awaiting fresh keyframe";
      }
      encoded_buffer->request_keyframe();
      encoded_image_callback_->OnFrameDropped(frame.rtp_timestamp(), 0, true);
      return WEBRTC_VIDEO_CODEC_OK;
    }
    maximum_forward_age_us_ = std::max(maximum_forward_age_us_, queued_age_us);'''),
        (' private:\n  void ForwardPendingRateControl(', ''' private:
  goodsharing::PreencodedChain chain_;
  unsigned diagnostic_drops_ = 0;
  uint64_t stale_sdk_inputs_ = 0;
  bool test_stall_checked_ = false;
  int64_t maximum_seen_age_us_ = 0, maximum_forward_age_us_ = 0;
  std::shared_ptr<livekit::EncodedRateControlState> source_state_;
  void ForwardPendingRateControl('''),
        ('    info.supports_native_handle = true;\n', '''    info.supports_native_handle = true;
    // GoodSharing: these inputs are already encoded reference frames. The
    // media-optimization dropper assumes raw input and may discard a P-frame
    // before Encode, breaking the externally encoded prediction chain.
    // Rate feedback is still forwarded to the upstream hardware CBR encoder;
    // raw-frame admission/pacing stays upstream and RTP congestion stays active.
    info.has_trusted_rate_controller = true;
'''),
    ])
    change('webrtc-sys/src/peer_connection_factory.cpp', [
        ('  if (peer_factory_.get() == nullptr) {\n    RTC_LOG_ERR(LS_ERROR) << "Failed to create PeerConnectionFactory";\n    return;\n  }', '''  if (peer_factory_.get() == nullptr) {
    RTC_LOG_ERR(LS_ERROR) << "Failed to create PeerConnectionFactory";
    return;
  }
  // Native WebRTC ignores loopback adapters by default. Local PC fixtures
  // explicitly opt in while the SFU advertises only 127/8 candidates. Keep
  // ordinary Internet-client factory defaults unchanged.
  const char* local_loopback = std::getenv("GOODSHARING_LOCAL_LOOPBACK");
  if (local_loopback && std::string(local_loopback) == "1") {
    webrtc::PeerConnectionFactoryInterface::Options local_options;
    local_options.network_ignore_mask = 0;
    peer_factory_->SetOptions(local_options);
  }'''),
        ('#include "api/field_trials_view.h"', '#include "api/field_trials_view.h"\n#include "rtc_base/experiments/rate_control_settings.h"\n#include "video/video_send_stream_impl.h"\n#include <cstdio>\n#include <cstdlib>'),
        ('class CompositeFieldTrials final', '''// Preserve congestion-window pushback, but express it through rate feedback.
// Dropping externally encoded P-frames here breaks the upstream references.
class PreencodedRateFieldTrials final : public webrtc::FieldTrialsView {
 public:
  std::string Lookup(absl::string_view key) const override {
    if (key == "WebRTC-CongestionWindow") return "QueueSize:350,MinBitrate:30000,DropFrame:false";
    if (key == "WebRTC-VideoRateControl") return "alr_probing:true";
    if (key == "WebRTC-Video-Pacing") return "max_delay:100ms";
    if (key == "WebRTC-Pacer-KeyframeFlushing") return "Enabled";
    if (key == "WebRTC-ForcePlayoutDelay") return "min_ms:0,max_ms:60";
    return "";
  }
  std::unique_ptr<webrtc::FieldTrialsView> CreateCopy() const override {
    return std::make_unique<PreencodedRateFieldTrials>();
  }
};

class CompositeFieldTrials final'''),
        ('  if (enable_warp) {\n    views.push_back(std::make_unique<EnableWarpFieldTrials>());\n  }', '  if (enable_warp) {\n    views.push_back(std::make_unique<EnableWarpFieldTrials>());\n  }\n  views.push_back(std::make_unique<PreencodedRateFieldTrials>());'),
        ('  webrtc::PeerConnectionFactoryDependencies dependencies;', '''  if (std::getenv("GOODSHARING_TEST_NETWORK_TELEMETRY")) {
    const webrtc::RateControlSettings settings(env_.field_trials());
    std::fprintf(stderr, "GoodSharing congestion policy: window=%d pushback=%d drop_frame_only=%d\\n",
                 settings.UseCongestionWindow(), settings.UseCongestionWindowPushback(),
                 settings.UseCongestionWindowDropFrameOnly());
    std::fprintf(stderr, "GoodSharing recovery policy: alr_probing=%d keyframe_flush=%d video_max_ms=60\\n",
                 settings.UseAlrProbing(), env_.field_trials().IsEnabled("WebRTC-Pacer-KeyframeFlushing"));
    const webrtc::internal::PacingConfig pacing(env_.field_trials());
    std::fprintf(stderr, "GoodSharing queue policy: pacing_target_ms=%lld sdk_input_age_ms=100\\n",
                 static_cast<long long>(pacing.max_pacing_delay.Get().ms()));
  }
  webrtc::PeerConnectionFactoryDependencies dependencies;'''),
    ])
    change('webrtc-sys/src/peer_connection.cpp', [
        ('#include "livekit/peer_connection.h"', '#include "livekit/peer_connection.h"\n#include "livekit/closed_frame_transformer.h"\n#include "api/make_ref_counted.h"\n#include "rtc_base/time_utils.h"\n#include <cstdio>\n#include <cstdlib>'),
        ('''    webrtc::scoped_refptr<webrtc::RtpTransceiverInterface> transceiver) {
  observer_->on_track''', '''    webrtc::scoped_refptr<webrtc::RtpTransceiverInterface> transceiver) {
  const auto receiver = transceiver->receiver();
  receiver->SetDepacketizerToDecoderFrameTransformer(webrtc::make_ref_counted<goodsharing::ClosedFrameTransformer>());
  if (std::getenv("GOODSHARING_TEST_NETWORK_TELEMETRY")) std::fprintf(stderr, "GoodSharing startup early-blocked receiver=%p at_us=%lld\\n", static_cast<void*>(receiver.get()), static_cast<long long>(webrtc::TimeMicros()));
  observer_->on_track'''),
        ('  webrtc::PeerConnectionInterface::RTCConfiguration rtc_config{};', '''  webrtc::PeerConnectionInterface::RTCConfiguration rtc_config{};
  // Bound stored audio packets and consume excess backlog faster. This is a
  // packet-count cap, not a hard time bound (packet duration can vary).
  rtc_config.audio_jitter_buffer_max_packets = 10;
  rtc_config.audio_jitter_buffer_fast_accelerate = true;
  // Owned synthetic clock-bootstrap experiment only. Normal app defaults are
  // unchanged; this does not alter receiver validation or media permissions.
  if (std::getenv("GOODSHARING_TEST_AUDIO_TRACE")) {
    const char* value = std::getenv("GOODSHARING_TEST_RTCP_REPORT_MS");
    if (value && std::string(value) == "500") {
      rtc_config.set_audio_rtcp_report_interval_ms(500);
      rtc_config.set_video_rtcp_report_interval_ms(500);
      std::fprintf(stderr, "GoodSharing RTCP report experiment: interval_ms=500\\n");
    }
  }'''),
    ])
    change('webrtc-sys/include/livekit/packet_trailer.h', [
        ('      webrtc::scoped_refptr<webrtc::RtpReceiverInterface> receiver);', '      webrtc::scoped_refptr<webrtc::RtpReceiverInterface> receiver, bool attach, bool enabled);'),
        ('    std::shared_ptr<RtpReceiver> receiver);', '    std::shared_ptr<RtpReceiver> receiver, bool attach, bool enabled);'),
    ])
    change('webrtc-sys/src/packet_trailer.rs', [
        ('            receiver: SharedPtr<RtpReceiver>,\n        ) -> SharedPtr<PacketTrailerHandler>;', '            receiver: SharedPtr<RtpReceiver>,\n            attach: bool,\n            enabled: bool,\n        ) -> SharedPtr<PacketTrailerHandler>;'),
    ])
    change('libwebrtc/src/native/packet_trailer.rs', [
        ('''pub fn create_receiver_handler(
    peer_factory: &PeerConnectionFactory,
    receiver: &RtpReceiver,
) -> PacketTrailerHandler {
    PacketTrailerHandler {''', '''pub fn create_receiver_handler(
    peer_factory: &PeerConnectionFactory,
    receiver: &RtpReceiver,
) -> PacketTrailerHandler {
    create_receiver_handler_with_attachment(peer_factory, receiver, true, true)
}

/// Assemble encrypted metadata processing without installing a plaintext path.
pub fn create_detached_receiver_handler(
    peer_factory: &PeerConnectionFactory,
    receiver: &RtpReceiver,
) -> PacketTrailerHandler {
    create_receiver_handler_with_attachment(peer_factory, receiver, false, true)
}

/// Preserve SDK support for explicitly unencrypted publications. GoodSharing
/// still refuses their presentation because no per-track crypto readiness exists.
pub fn create_unencrypted_receiver_handler(
    peer_factory: &PeerConnectionFactory,
    receiver: &RtpReceiver,
) -> PacketTrailerHandler {
    create_receiver_handler_with_attachment(peer_factory, receiver, true, false)
}

fn create_receiver_handler_with_attachment(
    peer_factory: &PeerConnectionFactory,
    receiver: &RtpReceiver,
    attach: bool,
    enabled: bool,
) -> PacketTrailerHandler {
    PacketTrailerHandler {'''),
        ('            receiver.handle.sys_handle.clone(),\n        ),', '            receiver.handle.sys_handle.clone(),\n            attach,\n            enabled,\n        ),'),
    ])
    change('webrtc-sys/src/packet_trailer.cpp', [
        ('#include <chrono>', '#include <chrono>\n#include <cstdio>\n#include <cstdlib>\n#include <thread>\n#include "rtc_base/time_utils.h"'),
        ('    webrtc::scoped_refptr<webrtc::RtpReceiverInterface> receiver)\n    : rtc_runtime_(rtc_runtime), receiver_(receiver)', '    webrtc::scoped_refptr<webrtc::RtpReceiverInterface> receiver, bool attach, bool enabled)\n    : rtc_runtime_(rtc_runtime), receiver_(receiver)'),
        ('    std::shared_ptr<RtpReceiver> receiver) {', '    std::shared_ptr<RtpReceiver> receiver, bool attach, bool enabled) {'),
        ('      peer_factory->rtc_runtime(), receiver->rtc_receiver());', '      peer_factory->rtc_runtime(), receiver->rtc_receiver(), attach, enabled);'),
        ('  receiver->SetDepacketizerToDecoderFrameTransformer(transformer_);', '''  transformer_->set_enabled(enabled);
  if (attach) receiver->SetDepacketizerToDecoderFrameTransformer(transformer_);
  if (std::getenv("GOODSHARING_TEST_NETWORK_TELEMETRY")) {
    std::fprintf(stderr, "GoodSharing startup metadata-%s receiver=%p at_us=%lld\\n", attach ? "only" : "detached", static_cast<void*>(receiver.get()), static_cast<long long>(webrtc::TimeMicros()));
    const char* value = std::getenv("GOODSHARING_TEST_RECEIVER_SETUP_STALL_MS");
    const long ms = value ? std::strtol(value, nullptr, 10) : 0;
    if (ms > 0 && ms <= 250) std::this_thread::sleep_for(std::chrono::milliseconds(ms));
  }'''),
    ])
    change('webrtc-sys/include/livekit/frame_cryptor.h', [
        ('  void set_enabled(bool enabled) const;', '  void set_enabled(bool enabled) const;\n  // Setup-only: publish the complete configured graph, once.\n  void activate() const;'),
        ('  mutable webrtc::Mutex mutex_;\n  webrtc::scoped_refptr<webrtc::FrameCryptorTransformer>', '  mutable webrtc::Mutex mutex_;\n  mutable bool activated_ = false;\n  webrtc::scoped_refptr<webrtc::FrameCryptorTransformer>'),
    ])
    change('webrtc-sys/src/frame_cryptor.rs', [
        ('        pub fn set_enabled(self: &FrameCryptor, enabled: bool);', '        pub fn set_enabled(self: &FrameCryptor, enabled: bool);\n        pub fn activate(self: &FrameCryptor);'),
    ])
    change('libwebrtc/src/native/frame_cryptor.rs', [
        ('    pub fn set_enabled(self: &FrameCryptor, enabled: bool) {', '''    /// Activate only after metadata and the per-track readiness observer exist.
    pub fn activate(&self) {
        self.sys_handle.activate();
    }

    pub fn set_enabled(self: &FrameCryptor, enabled: bool) {'''),
    ])
    change('webrtc-sys/src/frame_cryptor.cpp', [
        ('#include "livekit/frame_cryptor.h"', '#include "livekit/frame_cryptor.h"\n#include "livekit/strict_frame_cryptor.h"\n#include "livekit/closed_frame_transformer.h"\n#include <cstdio>\n#include <cstdlib>\n#include "rtc_base/time_utils.h"\nnamespace goodsharing { void requirePatchedFrameCryptor(); }'),
        ('KeyProvider::KeyProvider(KeyProviderOptions options) {', 'KeyProvider::KeyProvider(KeyProviderOptions options) {\n  goodsharing::requirePatchedFrameCryptor();'),
        ('new webrtc::RefCountedObject<webrtc::DefaultKeyProviderImpl>(rtc_options)', 'new webrtc::RefCountedObject<goodsharing::StrictKeyProvider>(rtc_options)'),
        ('new webrtc::FrameCryptorTransformer(rtc_runtime->signaling_thread(),', 'new goodsharing::StrictFrameCryptor(rtc_runtime->signaling_thread(),', 2),
        ('  sender->SetEncoderToPacketizerFrameTransformer(e2ee_transformer_);\n  e2ee_transformer_->SetEnabled(false);', '''  // Crypto always starts enabled. Only the closed placeholder is attached
  // until the manager finishes metadata and readiness-observer setup.
  e2ee_transformer_->SetEnabled(true);
  sender->SetEncoderToPacketizerFrameTransformer(webrtc::make_ref_counted<goodsharing::ClosedFrameTransformer>());'''),
        ('  receiver->SetDepacketizerToDecoderFrameTransformer(e2ee_transformer_);\n  e2ee_transformer_->SetEnabled(false);', '''  e2ee_transformer_->SetEnabled(true);
  receiver->SetDepacketizerToDecoderFrameTransformer(webrtc::make_ref_counted<goodsharing::ClosedFrameTransformer>());
  if (std::getenv("GOODSHARING_TEST_NETWORK_TELEMETRY")) std::fprintf(stderr, "GoodSharing startup blocked receiver=%p at_us=%lld\\n", static_cast<void*>(receiver.get()), static_cast<long long>(webrtc::TimeMicros()));'''),
        ('  if (!handler) {\n    return;\n  }', '''  webrtc::MutexLock lock(&mutex_);
  // This setup API cannot replace a graph after activation. Revoke on misuse.
  if (activated_) { e2ee_transformer_->SetEnabled(false); return; }
  if (!handler) {
    return;
  }'''),
        ('''  if (sender_) {
    sender_->SetEncoderToPacketizerFrameTransformer(chained_transformer_);
  }
  if (receiver_) {
    receiver_->SetDepacketizerToDecoderFrameTransformer(chained_transformer_);
  }
}''', '''}

void FrameCryptor::activate() const {
  webrtc::MutexLock lock(&mutex_);
  if (activated_) return;
  auto target = chained_transformer_ ? chained_transformer_
      : webrtc::scoped_refptr<webrtc::FrameTransformerInterface>(e2ee_transformer_);
  if (sender_) sender_->SetEncoderToPacketizerFrameTransformer(target);
  if (receiver_) {
    receiver_->SetDepacketizerToDecoderFrameTransformer(target);
    if (std::getenv("GOODSHARING_TEST_NETWORK_TELEMETRY")) std::fprintf(stderr, "GoodSharing startup crypto-active receiver=%p at_us=%lld\\n", static_cast<void*>(receiver_.get()), static_cast<long long>(webrtc::TimeMicros()));
  }
  activated_ = true;
}'''),
    ])
    change('livekit/src/room/e2ee/manager.rs', [
        ('        let receiver = track.transceiver().unwrap().receiver();', '''        let receiver = track.transceiver().unwrap().receiver();
        // Install a closed path before any standalone metadata transformer.
        let frame_cryptor = if publication.encryption_type() != EncryptionType::None {
            // An encrypted announcement without local crypto configuration
            // leaves the early native guard closed. Never treat it as plain.
            if !self.initialized() { return; }
            Some(self.setup_rtp_receiver(&identity, receiver.clone()))
        } else {
            // The early native guard is lifted only for an explicitly plain
            // SDK publication; encrypted tracks never enter this branch.
            let _plain = packet_trailer::create_unencrypted_receiver_handler(
                LkRuntime::instance().pc_factory(), &receiver);
            None
        };'''),
        ('''                let handler = packet_trailer::create_receiver_handler(
                    LkRuntime::instance().pc_factory(),
                    &receiver,
                );''', '''                let handler = if frame_cryptor.is_some() {
                    packet_trailer::create_detached_receiver_handler(
                        LkRuntime::instance().pc_factory(), &receiver)
                } else {
                    packet_trailer::create_receiver_handler(
                        LkRuntime::instance().pc_factory(), &receiver)
                };'''),
        ('''        if !self.initialized() || publication.encryption_type() == EncryptionType::None {
            return;
        }

        let frame_cryptor = self.setup_rtp_receiver(&identity, receiver);''', '        let Some(frame_cryptor) = frame_cryptor else { return; };'),
        ('        inner.frame_cryptors.insert((identity, publication.sid()), frame_cryptor.clone());', '''        inner.frame_cryptors.insert((identity, publication.sid()), frame_cryptor.clone());
        drop(inner);
        frame_cryptor.activate();''', 2),
        ('type StateChangedHandler = Box<dyn Fn(ParticipantIdentity, EncryptionState) + Send>;', 'type StateChangedHandler = Box<dyn Fn(ParticipantIdentity, TrackSid, EncryptionState) + Send>;'),
        ('handler: impl Fn(ParticipantIdentity, EncryptionState) + Send + \'static,', 'handler: impl Fn(ParticipantIdentity, TrackSid, EncryptionState) + Send + \'static,'),
        ('        self.setup_cryptor(&frame_cryptor);', '        self.setup_cryptor(&frame_cryptor, publication.sid());', 2),
        ('    fn setup_cryptor(&self, frame_cryptor: &FrameCryptor) {', '    fn setup_cryptor(&self, frame_cryptor: &FrameCryptor, track_sid: TrackSid) {'),
        ('                state_changed(participant_identity.into(), state);', '                state_changed(participant_identity.into(), track_sid.clone(), state);'),
    ])
    change('livekit/src/room/mod.rs', [
        ('    E2eeStateChanged {\n        participant: Participant,\n        state: EncryptionState,', '    E2eeStateChanged {\n        participant: Participant,\n        track_sid: TrackSid,\n        state: EncryptionState,'),
        ('            move |participant_identity, state| {', '            move |participant_identity, track_sid, state| {'),
        ('RoomEvent::E2eeStateChanged { participant, state }', 'RoomEvent::E2eeStateChanged { participant, track_sid, state }'),
    ])
    change('livekit-ffi/protocol/room.proto', [(E2EE_ORIGINAL, E2EE_PATCHED)])
    path = ROOT / 'patches/livekit-rust-a6f93a16-shutdown.patch'
    path.parent.mkdir(exist_ok=True)
    for source_file, after in pending:
        if source_file.exists() and source_file.read_text(encoding='utf-8') == after:
            continue
        temporary = source_file.with_suffix(source_file.suffix + '.goodsharing.tmp')
        temporary.write_text(after, encoding='utf-8', newline='\n')
        temporary.replace(source_file)
    patch_bytes = ''.join(changes).encode('utf-8')
    if not path.exists() or path.read_bytes() != patch_bytes:
        path.write_bytes(patch_bytes)
    marker_text = json.dumps({'patch_sha256': hashlib.sha256(path.read_bytes()).hexdigest(), 'source_sha256': {str(p.relative_to(SOURCE)).replace('\\', '/'): hashlib.sha256(after.encode()).hexdigest() for p, after in pending}}, indent=2)
    if not marker.exists() or marker.read_text(encoding='utf-8') != marker_text:
        marker.write_text(marker_text, encoding='utf-8', newline='\n')
    print('Pinned engine shutdown cancellation patch applied')


if __name__ == '__main__':
    main()
