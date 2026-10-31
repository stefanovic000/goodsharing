"""Preserve original sender PCM capture time through the pinned native SDK."""


def extend_sender_timing(result, pristine):
    def pairs(path):
        return next(replacements for name, replacements in result if name == path)

    # Append the sender-only field to all owned PCM constructors. It is not a
    # receiver capture timestamp, and is never populated by a decoded callback.
    for path, replacements in result:
        if path == 'libwebrtc/src/audio_frame.rs':
            replacements.append(('    pub data: Cow', '''    /// Sender MSVC steady_clock microseconds of the first captured sample.
    /// Only timed, unbuffered source submission consumes this field.
    pub sender_capture_us: Option<i64>,
    pub data: Cow'''))
            replacements.append(('            capture_time_ms: None,', '            sender_capture_us: None,\n            capture_time_ms: None,'))
        elif any(old == 'AudioFrame {\n' for old, *_ in replacements):
            replacements.append(('AudioFrame {\n', 'AudioFrame {\n            sender_capture_us: None,\n', pristine(path).count('AudioFrame {\n')))
    pairs('livekit-ffi/src/server/audio_source.rs').append(
        ('sender_capture_us: None,', 'sender_capture_us: buffer.sender_capture_us,'))
    pairs('livekit-ffi/src/conversion/audio_frame.rs').append(
        ('            capture_time_ms: frame.capture_time_ms,', '            sender_capture_us: None,\n            capture_time_ms: frame.capture_time_ms,'))
    pairs('webrtc-sys/include/livekit/audio_track.h').extend([
        ('\n                       const SourceContext* ctx,', '''
                       bool has_sender_capture_time,
                       int64_t sender_capture_us,
                       const SourceContext* ctx,'''),
        ('\n                     const SourceContext* ctx,', '''
                     bool has_sender_capture_time,
                     int64_t sender_capture_us,
                     const SourceContext* ctx,'''),
        ('    int sample_rate_ = 0;', '    uint64_t timed_capture_count_ = 0;\n    int sample_rate_ = 0;'),
    ])
    pairs('webrtc-sys/src/audio_track.rs').append(
        ('            userdata: *const SourceContext,', '''            has_sender_capture_time: bool,
            sender_capture_us: i64,
            userdata: *const SourceContext,'''))
    pairs('webrtc-sys/src/audio_track.cpp').extend([
        ('#include <cstdlib>', '#include <cstdlib>\n#include <chrono>\n#include <cstdio>\n#include "livekit/source_audio_clock.h"'),
        ('    const SourceContext* ctx,', '''    bool has_sender_capture_time,
    int64_t sender_capture_us,
    const SourceContext* ctx,''', 2),
        ('  if (queue_size_samples_) {', '''  std::optional<int64_t> capture_ms;
  if (has_sender_capture_time) {
    // Buffered/reframed timestamps have no verified association. Refuse them.
    if (queue_size_samples_ || sample_rate != sample_rate_ ||
        number_of_channels != num_channels_ || number_of_frames != sample_rate / 100 ||
        data.size() != number_of_frames * number_of_channels) return false;
    const auto steady_us = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    const auto native_us = webrtc::TimeMicros();
    capture_ms = goodsharing::sourceAudioCaptureMs(sender_capture_us, steady_us, native_us);
    if (!capture_ms) return false;
    ++timed_capture_count_;
    if (std::getenv("GOODSHARING_TEST_AUDIO_TRACE") &&
        (timed_capture_count_ <= 4 || timed_capture_count_ == 60 || timed_capture_count_ == 300)) {
      std::fprintf(stderr, "GoodSharing sender audio clock: sequence=%llu source_us=%lld steady_us=%lld native_us=%lld capture_ms=%lld age_us=%lld\\n",
          static_cast<unsigned long long>(timed_capture_count_),
          static_cast<long long>(sender_capture_us), static_cast<long long>(steady_us),
          static_cast<long long>(native_us), static_cast<long long>(*capture_ms),
          static_cast<long long>(steady_us - sender_capture_us));
    }
  }

  if (queue_size_samples_) {'''),
        ('      if (std::getenv("GOODSHARING_TEST_AUDIO_CAPTURE_CLOCK") &&', '''      if (capture_ms) {
        sink->OnData(data.data(), sizeof(int16_t) * 8, sample_rate,
                     number_of_channels, number_of_frames, capture_ms);
      } else if (std::getenv("GOODSHARING_TEST_AUDIO_CAPTURE_CLOCK") &&'''),
        ('number_of_frames, ctx, on_complete);', 'number_of_frames, has_sender_capture_time, sender_capture_us, ctx, on_complete);'),
    ])
    result.append(('libwebrtc/src/native/audio_source.rs', [
        ('        // Fast path: no buffering', '''        if frame.sender_capture_us.is_some() && self.queue_size_samples != 0 {
            return Err(RtcError {
                error_type: RtcErrorType::InvalidState,
                message: "timed capture requires unbuffered audio source".to_owned(),
            });
        }

        // Fast path: no buffering'''),
        ('                    nb_frames,\n', '''                    nb_frames,
                    frame.sender_capture_us.is_some(),
                    frame.sender_capture_us.unwrap_or(0),
''', 2),
    ]))
