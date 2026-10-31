"""Pinned optional receiver audio timing; missing timestamps stay missing."""
from receiver_timing_patch import RECEIVER_AUDIO_FIELDS
AUDIO_ORIGINAL = '''message AudioFrameBufferInfo {
  required uint64 data_ptr = 1; // *const i16
  required uint32 num_channels = 2;
  required uint32 sample_rate = 3;
  required uint32 samples_per_channel = 4;
}'''
AUDIO_PATCHED = AUDIO_ORIGINAL[:-1] + '''  // GoodSharing: receiver WebRTC clock, not sender app monotonic time.
  optional int64 capture_time_ms = 5;
  optional int64 sink_time_us = 6;
  // Sender only: original first-sample MSVC steady_clock time, microseconds.
  optional int64 sender_capture_us = 7;
''' + RECEIVER_AUDIO_FIELDS + '}'

def patch_audio_protocol(content):
    text = content.decode('utf-8')
    if text.count(AUDIO_ORIGINAL) != 1:
        raise RuntimeError('Unexpected pinned audio buffer protocol')
    return text.replace(AUDIO_ORIGINAL, AUDIO_PATCHED).encode('utf-8')

def engine_audio_changes(pristine):
    """Use pristine, checksum-pinned sources, never patch generated build files."""
    result = [
        ('livekit-ffi/protocol/audio_frame.proto', [(AUDIO_ORIGINAL, AUDIO_PATCHED)]),
        ('webrtc-sys/include/livekit/audio_track.h', [
            ('              size_t number_of_frames) override;', '''              size_t number_of_frames) override;
  void OnData(const void* audio_data, int bits_per_sample, int sample_rate,
              size_t number_of_channels, size_t number_of_frames,
              std::optional<int64_t> absolute_capture_timestamp_ms) override;'''),
        ]),
        ('webrtc-sys/src/audio_track.cpp', [
            ('#include <algorithm>', '#include <algorithm>\n#include <cstdlib>'),
            ('#include "rtc_base/logging.h"', '#include "rtc_base/logging.h"\n#include "rtc_base/time_utils.h"'),
            ('                             size_t number_of_frames) {\n  RTC_CHECK_EQ(16, bits_per_sample);', '''                             size_t number_of_frames) {
  OnData(audio_data, bits_per_sample, sample_rate, number_of_channels,
         number_of_frames, std::nullopt);
}

void NativeAudioSink::OnData(const void* audio_data, int bits_per_sample,
                             int sample_rate, size_t number_of_channels,
                             size_t number_of_frames,
                             std::optional<int64_t> absolute_capture_timestamp_ms) {
  const auto sink_time_us = webrtc::TimeMicros();
  RTC_CHECK_EQ(16, bits_per_sample);'''),
            ('frame_.num_channels(), frame_.samples_per_channel());', '''frame_.num_channels(), frame_.samples_per_channel(),
                       absolute_capture_timestamp_ms.has_value(),
                       absolute_capture_timestamp_ms.value_or(0), sink_time_us);'''),
            ('                       number_of_frames);', '''                       number_of_frames,
                       absolute_capture_timestamp_ms.has_value(),
                       absolute_capture_timestamp_ms.value_or(0), sink_time_us);'''),
            ('''    for (auto sink : sinks_)
      sink->OnData(data.data(), sizeof(int16_t) * 8, sample_rate,
                   number_of_channels, number_of_frames);''', '''    for (auto sink : sinks_) {
      // Diagnostic only: the owned synthetic unbuffered producer asks whether
      // explicit native capture time survives negotiation/forwarding. This is
      // NOT a timestamp for application/WASAPI capture and is never its fallback.
      if (std::getenv("GOODSHARING_TEST_AUDIO_CAPTURE_CLOCK") &&
          std::getenv("GOODSHARING_TEST_AUDIO_TRACE")) {
        sink->OnData(data.data(), sizeof(int16_t) * 8, sample_rate,
                     number_of_channels, number_of_frames, webrtc::TimeMillis());
      } else {
        sink->OnData(data.data(), sizeof(int16_t) * 8, sample_rate,
                     number_of_channels, number_of_frames);
      }
    }'''),
        ]),
        ('webrtc-sys/src/audio_track.rs', [
            ('            nb_frames: usize,\n        );', '''            nb_frames: usize,
            has_capture_time: bool,
            capture_time_ms: i64,
            sink_time_us: i64,
        );'''),
            ('fn on_data(&self, data: &[i16], sample_rate: i32, nb_channels: usize, nb_frames: usize);', 'fn on_data(&self, data: &[i16], sample_rate: i32, nb_channels: usize, nb_frames: usize, capture_time_ms: Option<i64>, sink_time_us: i64);'),
            ('''    fn on_data(&self, data: &[i16], sample_rate: i32, nb_channels: usize, nb_frames: usize) {
        self.observer.on_data(data, sample_rate, nb_channels, nb_frames);
    }''', '''    fn on_data(&self, data: &[i16], sample_rate: i32, nb_channels: usize, nb_frames: usize,
               has_capture_time: bool, capture_time_ms: i64, sink_time_us: i64) {
        self.observer.on_data(data, sample_rate, nb_channels, nb_frames,
                              has_capture_time.then_some(capture_time_ms), sink_time_us);
    }'''),
        ]),
        ('libwebrtc/src/audio_frame.rs', [
            ('    pub data: Cow', '''    /// Optional original capture time translated to the receiver WebRTC clock.
    pub capture_time_ms: Option<i64>,
    /// WebRTC clock sampled at the native sink, before FFI/reader queues.
    pub sink_time_us: Option<i64>,
    pub data: Cow'''),
            ('        Self {\n            data:', '        Self {\n            capture_time_ms: None,\n            sink_time_us: None,\n            data:'),
        ]),
        ('livekit-ffi/src/conversion/audio_frame.rs', [
            ('            data_ptr: frame.data.as_ptr() as u64,', '''            data_ptr: frame.data.as_ptr() as u64,
            capture_time_ms: frame.capture_time_ms,
            sink_time_us: frame.sink_time_us,'''),
        ]),
    ]
    # Generated/rebuffered PCM has no proven original timestamp. Preserve None
    # for all such constructors instead of silently assigning arrival time.
    literals = (
        'libwebrtc/src/native/audio_stream.rs',
        'livekit-ffi/src/server/audio_source.rs',
        'livekit-ffi/src/server/audio_stream.rs',
        'livekit-ffi/src/server/requests.rs',
        'livekit/src/plugin.rs',
        'livekit/tests/common/e2e/audio.rs',
        'examples/play_from_disk/src/main.rs',
        'examples/mobile/src/lib.rs',
        'examples/basic_room/src/main.rs',
        'examples/local_audio/src/main.rs',
    )
    for path in literals:
        text = pristine(path)
        count = text.count('AudioFrame {\n')
        if not count:
            raise RuntimeError('Missing pinned PCM constructor: ' + path)
        pairs = [('AudioFrame {\n', 'AudioFrame {\n            capture_time_ms: None,\n            sink_time_us: None,\n', count)]
        if path == 'libwebrtc/src/native/audio_stream.rs':
            pairs += [
                ('fn on_data(&self, data: &[i16], sample_rate: i32, nb_channels: usize, nb_frames: usize) {', 'fn on_data(&self, data: &[i16], sample_rate: i32, nb_channels: usize, nb_frames: usize, capture_time_ms: Option<i64>, sink_time_us: i64) {'),
                ('''self.frame_queue.push(AudioFrame {
            capture_time_ms: None,
            sink_time_us: None,''', '''self.frame_queue.push(AudioFrame {
            capture_time_ms,
            sink_time_us: Some(sink_time_us),'''),
            ]
        result.append((path, pairs))
    from sender_audio_timing_patch import extend_sender_timing
    extend_sender_timing(result, pristine)
    from receiver_timing_patch import extend_receiver_audio
    extend_receiver_audio(result, pristine)
    return result
