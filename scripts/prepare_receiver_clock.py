"""Checksum-pinned native RTP/RTCP receiver timing; no WebRTC header ABI changes."""
import difflib
import hashlib
from pathlib import Path
import urllib.request

ROOT = Path(__file__).resolve().parents[1]
SOURCES = {
    'audio/channel_receive.cc': ('4b30d85289fbc98fa9a798385d9170358d04ddbefd9825da24785d4227bf1647', 'b24fe8db4930e2f7ef8f60fa5833890c6ca7266f0205d5c492b9c92f30590c04'),
    'video/rtp_video_stream_receiver2.cc': ('a032c1c3bdeb7bf4afe8a63775b5230253b7f1306037c2040bd97e8b20c28c55', '407af95cc2339c9ea96cfb7c79cfcb6476766b003060420932edd67006306e4b'),
}


def patched_receiver_sources():
    outputs, diff = {}, []
    for relative, (source_hash, header_hash) in SOURCES.items():
        path = ROOT / '.deps/sources/webrtc-89d790b/receiver-clock' / relative
        if not path.exists():
            data = urllib.request.urlopen('https://raw.githubusercontent.com/webrtc-sdk/webrtc/89d790b/' + relative, timeout=30).read()
            if hashlib.sha256(data).hexdigest() != source_hash: raise RuntimeError('Receiver source download checksum mismatch')
            path.parent.mkdir(parents=True, exist_ok=True); path.write_bytes(data)
        if hashlib.sha256(path.read_bytes()).hexdigest() != source_hash: raise RuntimeError('Pinned receiver source mismatch: ' + relative)
        header = ROOT / '.deps/webrtc-89d790b/win-x64-release/include' / relative.replace('.cc', '.h')
        if hashlib.sha256(header.read_text(encoding='utf-8').encode()).hexdigest() != header_hash: raise RuntimeError('Pinned receiver header mismatch: ' + relative)
        before = path.read_text(encoding='utf-8'); after = before
        def replace(old, new):
            nonlocal after
            if after.count(old) != 1: raise RuntimeError('Unexpected receiver context: ' + old[:90])
            after = after.replace(old, new)
        replace('#include "' + relative.replace('.cc', '.h') + '"', '#include "' + relative.replace('.cc', '.h') + '"\n#include "livekit/receiver_media_clock.h"\n#include "rtc_base/time_utils.h"')
        if relative.startswith('audio/'):
            replace('      absolute_capture_time_interpolator_(&env_.clock()) {', '      absolute_capture_time_interpolator_(&env_.clock()) {\n  goodsharing::receiverClocks().add(this, true, remote_ssrc_);')
            replace('ChannelReceive::~ChannelReceive() {', 'ChannelReceive::~ChannelReceive() {\n  goodsharing::receiverClocks().remove(this);')
            replace('      audio_sink_->OnData(data);', '''      const goodsharing::AudioClockContext context{
          data.data, data.sample_rate, data.channels, data.samples_per_channel,
          goodsharing::receiverClocks().audio(this, data.timestamp, webrtc::TimeMicros())};
      const goodsharing::AudioClockScope scope(context);
      audio_sink_->OnData(data);''')
            marker = '  rtp_rtcp_->IncomingRtcpPacket(std::span(data, length));'
        else:
            replace('      absolute_capture_time_interpolator_(&env_.clock()) {', '      absolute_capture_time_interpolator_(&env_.clock()) {\n  goodsharing::receiverClocks().add(this, false, config_.rtp.remote_ssrc);')
            replace('RtpVideoStreamReceiver2::~RtpVideoStreamReceiver2() {', 'RtpVideoStreamReceiver2::~RtpVideoStreamReceiver2() {\n  goodsharing::receiverClocks().remove(this);')
            marker = '  rtp_rtcp_->IncomingRtcpPacket((rtcp_packet));'
        replace(marker, marker + '''
  // Keep the sender-report clock separate from the RTT-dependent local-NTP
  // estimator. A sender NTP/RTP pair does not require a guessed RTT or epoch.
  if (const auto sr = rtp_rtcp_->GetSenderReportStats(); sr &&
      sr->last_remote_ntp_timestamp.Valid() && sr->last_arrival_timestamp.IsFinite()) {
    goodsharing::receiverClocks().update(this,
        sr->last_remote_ntp_timestamp.ToMs() * 1000,
        sr->last_remote_rtp_timestamp, sr->last_arrival_timestamp.us());
  }
''')
        outputs['goodsharing_' + Path(relative).stem + '.cpp'] = after
        diff.extend(difflib.unified_diff(before.splitlines(True), after.splitlines(True), fromfile='a/' + relative, tofile='b/' + relative))
    patch = ROOT / 'patches/webrtc-89d790b-receiver-clock.patch'
    data = ''.join(diff).encode()
    if not patch.exists() or patch.read_bytes() != data: patch.write_bytes(data)
    return outputs
