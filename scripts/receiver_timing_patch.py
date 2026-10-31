"""Expose RTP and RTCP sender clock separately from absent local capture time."""
import re
import zipfile

RECEIVER_AUDIO_FIELDS = '''  // Receiver only: RTCP sender NTP domain; not a local clock or E2EE-authenticated timestamp.
  optional int64 sender_ntp_us = 8;
  optional uint32 rtp_timestamp = 9;
  optional uint32 rtcp_reports = 10;
  optional uint32 sender_clock_epoch = 11;
'''
VIDEO_ORIGINAL = '''message VideoFrameReceived {
  required OwnedVideoBuffer buffer = 1;
  required int64 timestamp_us = 2; // In microseconds
  required VideoRotation rotation = 3;
  optional FrameMetadata metadata = 4;
}'''
VIDEO_PATCHED = VIDEO_ORIGINAL[:-1] + '''  // Receiver RTCP sender NTP domain, separate from timestamp_us.
  optional int64 sender_ntp_us = 5;
  optional uint32 rtp_timestamp = 6;
  optional uint32 rtcp_reports = 7;
  optional uint32 sender_clock_epoch = 8;
}'''


def patch_video_protocol(content):
    text = content.decode()
    if text.count(VIDEO_ORIGINAL) != 1: raise RuntimeError('Unexpected video clock protocol')
    return text.replace(VIDEO_ORIGINAL, VIDEO_PATCHED).encode()


def extend_receiver_audio(result, pristine):
    def pairs(path): return next(p for name, p in result if name == path)
    for path, replacements in result:
        if path == 'libwebrtc/src/audio_frame.rs':
            replacements.extend([
                ('    pub data: Cow', '    pub receiver_clock: Option<crate::ReceiverFrameClock>,\n    pub data: Cow'),
                ('            sender_capture_us: None,', '            receiver_clock: None,\n            sender_capture_us: None,'),
            ])
        elif any(old == 'AudioFrame {\n' for old, *_ in replacements):
            replacements.append(('AudioFrame {\n', 'AudioFrame {\n            receiver_clock: None,\n', pristine(path).count('AudioFrame {\n')))
    pairs('webrtc-sys/src/audio_track.cpp').extend([
        ('#include "livekit/source_audio_clock.h"', '#include "livekit/source_audio_clock.h"\n#include "livekit/receiver_media_clock.h"'),
        ('  const auto sink_time_us = webrtc::TimeMicros();', '''  const auto sink_time_us = webrtc::TimeMicros();
  // An extra format conversion has no verified timestamp association here.
  const auto clock = sample_rate_ == sample_rate && num_channels_ == number_of_channels
      ? goodsharing::decodedAudioClock(audio_data, sample_rate, number_of_channels, number_of_frames)
      : goodsharing::ReceiverClockStamp{};'''),
        ('absolute_capture_timestamp_ms.value_or(0), sink_time_us);', '''absolute_capture_timestamp_ms.value_or(0), sink_time_us,
                       clock.has_rtp, clock.rtp, clock.sender_ntp_us, clock.reports, clock.epoch);''', 2),
    ])
    pairs('webrtc-sys/src/audio_track.rs').extend([
        ('            sink_time_us: i64,', '''            sink_time_us: i64,
            has_rtp: bool, rtp: u32, sender_ntp_us: i64, rtcp_reports: u32, clock_epoch: u32,'''),
        ('sink_time_us: i64);', 'sink_time_us: i64, has_rtp: bool, rtp: u32, sender_ntp_us: i64, rtcp_reports: u32, clock_epoch: u32);'),
        ('has_capture_time: bool, capture_time_ms: i64, sink_time_us: i64) {', 'has_capture_time: bool, capture_time_ms: i64, sink_time_us: i64, has_rtp: bool, rtp: u32, sender_ntp_us: i64, rtcp_reports: u32, clock_epoch: u32) {'),
        ('has_capture_time.then_some(capture_time_ms), sink_time_us);', 'has_capture_time.then_some(capture_time_ms), sink_time_us, has_rtp, rtp, sender_ntp_us, rtcp_reports, clock_epoch);'),
    ])
    pairs('libwebrtc/src/native/audio_stream.rs').extend([
        ('capture_time_ms: Option<i64>, sink_time_us: i64) {', 'capture_time_ms: Option<i64>, sink_time_us: i64, has_rtp: bool, rtp: u32, sender_ntp_us: i64, rtcp_reports: u32, clock_epoch: u32) {'),
        ('self.frame_queue.push(AudioFrame {\n            receiver_clock: None,', '''self.frame_queue.push(AudioFrame {
            receiver_clock: has_rtp.then_some(crate::ReceiverFrameClock {
                rtp_timestamp: rtp, sender_ntp_us: (sender_ntp_us > 0).then_some(sender_ntp_us),
                rtcp_reports, epoch: clock_epoch,
            }),'''),
    ])
    pairs('livekit-ffi/src/conversion/audio_frame.rs').append(
        ('            sender_capture_us: None,', '''            sender_ntp_us: frame.receiver_clock.and_then(|clock| clock.sender_ntp_us),
            rtp_timestamp: frame.receiver_clock.map(|clock| clock.rtp_timestamp),
            rtcp_reports: frame.receiver_clock.map(|clock| clock.rtcp_reports),
            sender_clock_epoch: frame.receiver_clock.map(|clock| clock.epoch),
            sender_capture_us: None,'''))


def receiver_video_changes(pristine, archive):
    result = [
        ('libwebrtc/src/lib.rs', [('pub mod audio_frame;', '''/// Decoded first-sample/frame timing from native RTP/RTCP metadata.
/// Sender NTP is infrastructure supplied, not local time or E2EE-authenticated.
#[derive(Debug, Clone, Copy)]
pub struct ReceiverFrameClock {
    pub rtp_timestamp: u32,
    pub sender_ntp_us: Option<i64>,
    pub rtcp_reports: u32,
    pub epoch: u32,
}

pub mod audio_frame;''')]),
        ('livekit-ffi/protocol/video_frame.proto', [(VIDEO_ORIGINAL, VIDEO_PATCHED)]),
        ('webrtc-sys/include/livekit/video_frame.h', [
            ('#include "api/video/video_frame.h"', '#include "api/video/video_frame.h"\n#include "livekit/receiver_media_clock.h"'),
            ('  uint32_t timestamp() const;', '''  uint32_t timestamp() const;
  bool has_receiver_rtp() const { return receiver_clock_.has_rtp; }
  int64_t sender_ntp_us() const { return receiver_clock_.sender_ntp_us; }
  uint32_t rtcp_reports() const { return receiver_clock_.reports; }
  uint32_t sender_clock_epoch() const { return receiver_clock_.epoch; }'''),
            ('  webrtc::VideoFrame frame_;', '  webrtc::VideoFrame frame_;\n  goodsharing::ReceiverClockStamp receiver_clock_;'),
        ]),
        ('webrtc-sys/src/video_frame.cpp', [
            ('#include <memory>', '#include <memory>\n#include "rtc_base/time_utils.h"'),
            ('    : frame_(std::move(frame)) {}', '''    : frame_(std::move(frame)) {
  if (!frame_.packet_infos().empty()) {
    const auto ssrc = frame_.packet_infos().begin()->ssrc();
    bool consistent = true;
    for (const auto& packet : frame_.packet_infos()) {
      if (packet.ssrc() != ssrc || packet.rtp_timestamp() != frame_.rtp_timestamp()) consistent = false;
    }
    if (consistent) receiver_clock_ = goodsharing::receiverClocks().video(
        ssrc, frame_.rtp_timestamp(), webrtc::TimeMicros());
  }
}'''),
        ]),
        ('webrtc-sys/src/video_frame.rs', [
            ('        fn timestamp(self: &VideoFrame) -> u32;', '''        fn timestamp(self: &VideoFrame) -> u32;
        fn has_receiver_rtp(self: &VideoFrame) -> bool;
        fn sender_ntp_us(self: &VideoFrame) -> i64;
        fn rtcp_reports(self: &VideoFrame) -> u32;
        fn sender_clock_epoch(self: &VideoFrame) -> u32;'''),
        ]),
        ('libwebrtc/src/video_frame.rs', [
            ('    pub timestamp_us: i64, // When the frame was captured in microseconds', '    pub timestamp_us: i64, // When the frame was captured in microseconds\n    pub receiver_clock: Option<crate::ReceiverFrameClock>,'),
            ('Self { rotation, timestamp_us: 0, frame_metadata: None, buffer }', 'Self { rotation, timestamp_us: 0, frame_metadata: None, receiver_clock: None, buffer }'),
        ]),
    ]
    # Every owned video constructor explicitly has no timing unless its native
    # frame supplies it. Enumerate exact pristine literal lines, not substrings
    # of EncodedVideoFrame or FrameMetadata.
    with zipfile.ZipFile(archive) as z:
        for name in z.namelist():
            if not name.endswith('.rs'): continue
            relative = name.removeprefix('rust-sdks-a6f93a16/')
            if relative.startswith('livekit-capture/'): continue  # Distinct capture crate frame type.
            before = z.read(name).decode()
            matches = list(re.finditer(r'(?m)^([^\n]*?)(?<![A-Za-z0-9_])VideoFrame \{\n', before))
            if not matches: continue
            replacements = []
            for old in dict.fromkeys(m.group(0) for m in matches):
                replacements.append((old, old + '            receiver_clock: None,\n', before.count(old)))
            if relative == 'libwebrtc/src/native/video_stream.rs':
                replacements.append(('self.frame_queue.push(VideoFrame {\n            receiver_clock: None,', '''self.frame_queue.push(VideoFrame {
            receiver_clock: frame.has_receiver_rtp().then_some(crate::ReceiverFrameClock {
                rtp_timestamp: frame.timestamp(),
                sender_ntp_us: (frame.sender_ntp_us() > 0).then_some(frame.sender_ntp_us()),
                rtcp_reports: frame.rtcp_reports(), epoch: frame.sender_clock_epoch(),
            }),'''))
            if relative == 'livekit-ffi/src/server/video_stream.rs': raise RuntimeError('Unexpected video literal in stream adapter')
            result.append((relative, replacements))
    result.append(('livekit-ffi/src/server/video_stream.rs', [
        ('                                    timestamp_us,', '''                                    sender_ntp_us: frame.receiver_clock.and_then(|clock| clock.sender_ntp_us),
                                    rtp_timestamp: frame.receiver_clock.map(|clock| clock.rtp_timestamp),
                                    rtcp_reports: frame.receiver_clock.map(|clock| clock.rtcp_reports),
                                    sender_clock_epoch: frame.receiver_clock.map(|clock| clock.epoch),
                                    timestamp_us,'''),
    ]))
    return result
