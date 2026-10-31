"""Apply the pinned, reviewable C++-only patch; retain the release FFI engine."""
import difflib
import hashlib
import json
from pathlib import Path
import shutil
import zipfile
from livekit_protocol_patch import patch_room_protocol
from audio_timing_patch import patch_audio_protocol
from receiver_timing_patch import patch_video_protocol, RECEIVER_AUDIO_FIELDS

ROOT = Path(__file__).resolve().parents[1]
CPP = ROOT / '.deps/sources/cpp-80130a2/client-sdk-cpp-80130a2'
RUST = ROOT / '.deps/sources/rust-a6f93a16/rust-sdks-a6f93a16'
RELEASE = ROOT / '.deps/livekit-1.12.3/livekit-sdk-windows-x64-1.12.3'


def main():
    marker = CPP / '.goodsharing-wrapper-patched'
    changes = []
    pending = []
    previous = json.loads(marker.read_text(encoding='utf-8')) if marker.exists() else {}
    patch = ROOT / 'patches/livekit-cpp-1.12.3-stereo-build.patch'
    previous_valid = patch.exists() and previous.get('patch_sha256') == hashlib.sha256(patch.read_bytes()).hexdigest()
    archive = ROOT / '.deps/sources/livekit-cpp-80130a2.zip'
    if hashlib.sha256(archive.read_bytes()).hexdigest() != 'afd41a64247fb6faf76e6d4e4e63e7697eb998061dbe46bfb521796706e45bf4':
        raise RuntimeError('C++ source archive checksum mismatch')
    def pristine(relative):
        with zipfile.ZipFile(archive) as source_zip:
            return source_zip.read('client-sdk-cpp-80130a2/' + relative).decode('utf-8')

    def change(relative, replacements):
        path = CPP / relative
        before = pristine(relative)
        after = before
        for replacement in replacements:
            old, new, *expected = replacement
            if after.count(old) != (expected[0] if expected else 1):
                raise RuntimeError(f'Unexpected pinned source context: {relative}: {old[:60]}')
            after = after.replace(old, new)
        changes.extend(difflib.unified_diff(before.splitlines(True), after.splitlines(True), fromfile='a/' + relative, tofile='b/' + relative))
        current = path.read_text(encoding='utf-8')
        previous_hash = previous.get('source_sha256', {}).get(relative)
        if current not in (before, after) and not (previous_valid and hashlib.sha256(current.encode()).hexdigest() == previous_hash):
            raise RuntimeError('Unexpected local source edits: ' + relative)
        pending.append((path, after))

    change('include/livekit/audio_source.h', [
        ('#include <cstdint>', '#include <cstdint>\n#include <optional>'),
        ('  void captureFrame(const AudioFrame& frame, int timeout_ms = 20);', '''  void captureFrame(const AudioFrame& frame, int timeout_ms = 20);
  /// Original first-sample MSVC steady_clock microseconds on this sender PC.
  /// Requires an unbuffered source and a fresh, non-future 10 ms frame.
  void captureFrameAt(const AudioFrame& frame, std::int64_t capture_timestamp_us,
                      int timeout_ms = 100);'''),
        ('  // Internal helper to reset', '''  void captureFrameImpl(const AudioFrame& frame, int timeout_ms,
                        std::optional<std::int64_t> capture_timestamp_us);
  // Internal helper to reset'''),
    ])
    change('src/audio_source.cpp', [
        ('void AudioSource::captureFrame(const AudioFrame& frame, int timeout_ms) {', '''void AudioSource::captureFrame(const AudioFrame& frame, int timeout_ms) {
  captureFrameImpl(frame, timeout_ms, std::nullopt);
}

void AudioSource::captureFrameAt(const AudioFrame& frame, std::int64_t capture_timestamp_us,
                                 int timeout_ms) {
  if (queue_size_ms_ != 0 || capture_timestamp_us <= 0 ||
      frame.sampleRate() != sample_rate_ || frame.numChannels() != num_channels_ ||
      frame.samplesPerChannel() != static_cast<std::size_t>(sample_rate_ / 100)) {
    throw std::invalid_argument("Timed capture requires unbuffered matching 10 ms PCM and a positive source timestamp");
  }
  captureFrameImpl(frame, timeout_ms, capture_timestamp_us);
}

void AudioSource::captureFrameImpl(const AudioFrame& frame, int timeout_ms,
                                  std::optional<std::int64_t> capture_timestamp_us) {'''),
        ('  const proto::AudioFrameBufferInfo buf = frame.toProto();', '''  proto::AudioFrameBufferInfo buf = frame.toProto();
  if (capture_timestamp_us) buf.set_sender_capture_us(*capture_timestamp_us);'''),
        ('    LK_LOG_WARN("captureAudioFrameAsync timed out after {} ms", timeout_ms);', '    throw std::runtime_error("Audio capture callback timed out");'),
    ])
    change('include/livekit/audio_stream.h', [
        ('  AudioFrame frame; ///< The decoded PCM audio frame.', '''  AudioFrame frame; ///< The decoded PCM audio frame.
  /// Optional receiver WebRTC clock timing. Absence never means zero.
  std::optional<std::int64_t> capture_time_ms;
  std::optional<std::int64_t> sink_time_us;
  /// Actual decoded RTP timing, in the separate RTCP sender NTP domain.
  /// Infrastructure supplied; not an E2EE-authenticated timestamp.
  std::optional<std::int64_t> sender_ntp_us;
  std::optional<std::uint32_t> rtp_timestamp, rtcp_reports, sender_clock_epoch;'''),
        ('    std::size_t capacity{0};', '''    std::size_t capacity{0};

    /// Requested decoded PCM format. Defaults preserve the original mono API.
    std::uint32_t sample_rate{48000};
    std::uint32_t num_channels{1};''')])
    change('src/audio_stream.cpp', [
        ('    AudioFrameEvent ev{AudioFrame::fromOwnedInfo(fr.frame())};', '''    AudioFrameEvent ev{AudioFrame::fromOwnedInfo(fr.frame()), std::nullopt, std::nullopt};
    const auto& timing = fr.frame().info();
    if (timing.has_capture_time_ms()) ev.capture_time_ms = timing.capture_time_ms();
    if (timing.has_sink_time_us()) ev.sink_time_us = timing.sink_time_us();
    if (timing.has_sender_ntp_us()) ev.sender_ntp_us = timing.sender_ntp_us();
    if (timing.has_rtp_timestamp()) ev.rtp_timestamp = timing.rtp_timestamp();
    if (timing.has_rtcp_reports()) ev.rtcp_reports = timing.rtcp_reports();
    if (timing.has_sender_clock_epoch()) ev.sender_clock_epoch = timing.sender_clock_epoch();'''),
        ('  // TODO, sample_rate and num_channels are not useful in AudioStream, remove it\n  // from FFI.\n  //  new_audio_stream->set_sample_rate(options_.sample_rate);\n  //  new_audio_stream->set_num_channels(options.num_channels);', '  new_audio_stream->set_sample_rate(options_.sample_rate);\n  new_audio_stream->set_num_channels(options_.num_channels);'),
        ('  // TODO, sample_rate and num_channels are not useful in AudioStream, remove it\n  // from FFI.\n  // as->set_sample_rate(options_.sample_rate);\n  // as->set_num_channels(options_.num_channels);', '  as->set_sample_rate(options_.sample_rate);\n  as->set_num_channels(options_.num_channels);'),
    ])
    change('include/livekit/video_stream.h', [
        ('  std::optional<VideoFrameMetadata> metadata;', '''  std::optional<VideoFrameMetadata> metadata;
  /// RTCP sender NTP domain; infrastructure supplied, not local capture time.
  std::optional<std::int64_t> sender_ntp_us;
  std::optional<std::uint32_t> rtp_timestamp, rtcp_reports, sender_clock_epoch;'''),
        ('    /// Maximum number of VideoFrameEvent items buffered in the internal queue.', '    /// GoodSharing: maximum items at each native and C++ decoded queue stage.'),
        ('  /// Factory: create a VideoStream bound to a specific Track', '''  struct Diagnostics {
    std::uint64_t frames_received{0}, frames_read{0}, frames_dropped{0};
    std::size_t queue_high_water{0};
  };
  /// GoodSharing: snapshot C++ admission counters under the stream mutex.
  Diagnostics diagnostics() const;

  /// Factory: create a VideoStream bound to a specific Track'''),
        ('  mutable std::mutex mutex_;', '  mutable std::mutex mutex_;\n  Diagnostics diagnostics_;'),
    ])
    change('src/video_stream.cpp', [
        ('    ev.timestamp_us = fr.timestamp_us();', '''    ev.timestamp_us = fr.timestamp_us();
    if (fr.has_sender_ntp_us()) ev.sender_ntp_us = fr.sender_ntp_us();
    if (fr.has_rtp_timestamp()) ev.rtp_timestamp = fr.rtp_timestamp();
    if (fr.has_rtcp_reports()) ev.rtcp_reports = fr.rtcp_reports();
    if (fr.has_sender_clock_epoch()) ev.sender_clock_epoch = fr.sender_clock_epoch();'''),
        ('#include <utility>', '#include <utility>\n#include <algorithm>\n#include <limits>\n#include <stdexcept>'),
        ('  capacity_ = other.capacity_;', '  capacity_ = other.capacity_;\n  diagnostics_ = other.diagnostics_;', 2),
        ('  capacity_ = options.capacity;', '''  if (options.capacity > std::numeric_limits<std::uint32_t>::max()) {
    throw std::invalid_argument("Video queue capacity exceeds the FFI range");
  }
  capacity_ = options.capacity;''', 2),
        ('  new_video_stream->set_format(toProto(options.format));', '  new_video_stream->set_format(toProto(options.format));\n  new_video_stream->set_queue_size_frames(static_cast<std::uint32_t>(options.capacity));'),
        ('  vs->set_format(toProto(options.format));', '  vs->set_format(toProto(options.format));\n  vs->set_queue_size_frames(static_cast<std::uint32_t>(options.capacity));'),
        ('  out = std::move(queue_.front());', '  ++diagnostics_.frames_read;\n  out = std::move(queue_.front());'),
        ('void VideoStream::pushFrame(VideoFrameEvent&& ev) {', '''VideoStream::Diagnostics VideoStream::diagnostics() const {
  const std::scoped_lock<std::mutex> lock(mutex_);
  return diagnostics_;
}

void VideoStream::pushFrame(VideoFrameEvent&& ev) {'''),
        ('    if (capacity_ > 0 && queue_.size() >= capacity_) {', '    ++diagnostics_.frames_received;\n    if (capacity_ > 0 && queue_.size() >= capacity_) {'),
        ('      // Ring behavior: drop oldest frame.', '      ++diagnostics_.frames_dropped;\n      // Ring behavior: drop oldest frame.'),
        ('    queue_.push_back(std::move(ev));', '    queue_.push_back(std::move(ev));\n    diagnostics_.queue_high_water = std::max(diagnostics_.queue_high_water, queue_.size());'),
    ])
    change('include/livekit/room_event_types.h', [
        ('struct E2eeStateChangedEvent {', 'struct E2eeStateChangedEvent {\n  /// GoodSharing: empty on an incompatible engine; never grant readiness then.\n  std::string track_sid;'),
    ])
    change('include/livekit/room.h', [
        ('class RemoteParticipant;', 'class RemoteParticipant;\nclass RemoteTrackPublication;'),
        ('  std::vector<std::weak_ptr<RemoteParticipant>> remoteParticipants() const;', '''  std::vector<std::weak_ptr<RemoteParticipant>> remoteParticipants() const;

  /// GoodSharing: copy initial publications under the same room lock used by
  /// room events. Avoid iterating RemoteParticipant's mutable map concurrently.
  std::vector<std::pair<std::string, std::shared_ptr<RemoteTrackPublication>>>
  remotePublicationSnapshot() const;'''),
    ])
    change('include/livekit/remote_track_publication.h', [
        ('private:\n', '''private:
  friend class Room;
  // An incoming event acknowledges state; it must not send another request.
  void observeSubscribed(bool subscribed) noexcept { subscribed_ = subscribed; }
'''),
    ])
    change('src/room.cpp', [
        ('rpublication->setSubscribed(true);', 'rpublication->observeSubscribed(true);'),
        ('publication->setSubscribed(false);', 'publication->observeSubscribed(false);'),
        ('ev.state = static_cast<EncryptionState>(es.state());', 'ev.state = static_cast<EncryptionState>(es.state());\n            ev.track_sid = es.track_sid();'),
        ('rparticipant->mutableTrackPublications().emplace(rpublication->sid(), std::move(rpublication));', 'rparticipant->mutableTrackPublications().emplace(rpublication->sid(), rpublication);'),
        ('ConnectionState Room::connectionState() const {', '''std::vector<std::pair<std::string, std::shared_ptr<RemoteTrackPublication>>>
Room::remotePublicationSnapshot() const {
  const std::scoped_lock<std::mutex> guard(lock_);
  std::vector<std::pair<std::string, std::shared_ptr<RemoteTrackPublication>>> out;
  for (const auto& [identity, participant] : remote_participants_) {
    for (const auto& [sid, publication] : participant->trackPublications()) {
      out.emplace_back(identity, publication);
    }
  }
  return out;
}

ConnectionState Room::connectionState() const {'''),
    ])
    cmake = pristine('CMakeLists.txt')
    first = cmake.index('find_program(CARGO_EXECUTABLE NAMES cargo REQUIRED)')
    last = cmake.index('# Note: protozero_plugin.o removal', first)
    ffi = '''# GoodSharing: reuse the exact official release engine; build only C++.
set(RUST_ROOT "${LIVEKIT_ROOT_DIR}/client-sdk-rust")
add_library(livekit_ffi SHARED IMPORTED GLOBAL)
set_target_properties(livekit_ffi PROPERTIES
  IMPORTED_LOCATION "${RUST_ROOT}/target/release/livekit_ffi.dll"
  IMPORTED_IMPLIB "${RUST_ROOT}/target/release/livekit_ffi.dll.lib"
  INTERFACE_INCLUDE_DIRECTORIES "${RUST_ROOT}/livekit-ffi/include")
add_custom_target(build_rust_ffi)
# Only upstream's internal build-version adapter uses generated UniFFI C++.
# It is not part of the public client API; retain all room/media/token APIs.
set(LIVEKIT_UNIFFI_CPP_GENERATED_DIR "${LIVEKIT_BINARY_DIR}/generated")

'''
    change('CMakeLists.txt', [
        (cmake[first:last], ffi),
        ('  set(LIVEKIT_USE_SYSTEM_PROTOBUF ON CACHE BOOL "Use system protobuf (vcpkg) on Windows" FORCE)', '  set(LIVEKIT_USE_SYSTEM_PROTOBUF OFF CACHE BOOL "Use pinned vendored protobuf" FORCE)'),
        ('  src/uniffi_bindgen_adapter.cpp\n', ''),
        ('  $<TARGET_OBJECTS:livekit_uniffi_cpp>\n', ''),
        ('set_source_files_properties(\n  ${LIVEKIT_ROOT_DIR}/src/uniffi_bindgen_adapter.cpp\n  PROPERTIES OBJECT_DEPENDS ${LIVEKIT_UNIFFI_CPP_HEADER}\n)\n', ''),
    ])
    change('cmake/protobuf.cmake', [
        ('if(WIN32 AND NOT LIVEKIT_USE_SYSTEM_PROTOBUF)', 'if(WIN32 AND LIVEKIT_USE_VCPKG)'),
        ('  URL "https://github.com/abseil/abseil-cpp/archive/refs/tags/${LIVEKIT_ABSEIL_VERSION}.tar.gz"', '  URL "https://github.com/abseil/abseil-cpp/archive/refs/tags/${LIVEKIT_ABSEIL_VERSION}.tar.gz"\n  URL_HASH SHA256=733726b8c3a6d39a4120d7e45ea8b41a434cdacde401cba500f14236c49b39dc'),
        ('  URL "https://github.com/protocolbuffers/protobuf/releases/download/v${LIVEKIT_PROTOBUF_VERSION}/protobuf-${LIVEKIT_PROTOBUF_VERSION}.tar.gz"', '  URL "https://github.com/protocolbuffers/protobuf/releases/download/v${LIVEKIT_PROTOBUF_VERSION}/protobuf-${LIVEKIT_PROTOBUF_VERSION}.tar.gz"\n  URL_HASH SHA256=d19643d265b978383352b3143f04c0641eea75a75235c111cc01a1350173180e'),
    ])
    change('cmake/spdlog.cmake', [('  URL "https://github.com/gabime/spdlog/archive/refs/tags/v${LIVEKIT_SPDLOG_VERSION}.tar.gz"', '  URL "https://github.com/gabime/spdlog/archive/refs/tags/v${LIVEKIT_SPDLOG_VERSION}.tar.gz"\n  URL_HASH SHA256=25c843860f039a1600f232c6eb9e01e6627f7d030a2ae5e232bdd3c9205d26cc')])
    change('cmake/nlohmann_json.cmake', [('  URL "https://github.com/nlohmann/json/releases/download/v${LIVEKIT_NLOHMANN_JSON_VERSION}/json.tar.xz"', '  URL "https://github.com/nlohmann/json/releases/download/v${LIVEKIT_NLOHMANN_JSON_VERSION}/json.tar.xz"\n  URL_HASH SHA256=42f6e95cad6ec532fd372391373363b62a14af6d771056dbfc86160e6dfff7aa')])
    # Validate every source edit before mutating any file. A marker is never
    # trusted in place of checking the actual pinned sources on each run.
    rust_archive = ROOT / '.deps/sources/livekit-rust-a6f93a16.zip'
    if hashlib.sha256(rust_archive.read_bytes()).hexdigest() != 'd361ab5562481869f0f1aeaa588bd811ab8a26abeb96fedde5000b9d783c05e0':
        raise RuntimeError('Rust protocol archive checksum mismatch')
    protocol_files = []
    with zipfile.ZipFile(rust_archive) as source_zip:
        prefix = 'rust-sdks-a6f93a16/'
        for name in source_zip.namelist():
            relative = name.removeprefix(prefix)
            if name.endswith('/') or not (relative.startswith(('livekit-ffi/protocol/', 'livekit-ffi/include/')) or relative in ('Cargo.toml', 'livekit-ffi/Cargo.toml')):
                continue
            target_file = CPP / 'client-sdk-rust' / relative
            target_file.parent.mkdir(parents=True, exist_ok=True)
            content = source_zip.read(name)
            original_content = content
            if relative == 'livekit-ffi/protocol/room.proto':
                content = patch_room_protocol(content)
            if relative == 'livekit-ffi/protocol/audio_frame.proto':
                content = patch_audio_protocol(content)
            if relative == 'livekit-ffi/protocol/video_frame.proto':
                content = patch_video_protocol(content)
            # Exact migration from increment 27; do not accept arbitrary edits.
            previous_audio = content.replace(RECEIVER_AUDIO_FIELDS.encode(), b'') if relative == 'livekit-ffi/protocol/audio_frame.proto' else content
            legacy_audio = previous_audio.replace(b'  // Sender only: original first-sample MSVC steady_clock time, microseconds.\n  optional int64 sender_capture_us = 7;\n', b'')
            if target_file.exists() and target_file.read_bytes() not in (original_content, content, previous_audio, legacy_audio):
                raise RuntimeError('Unexpected local protocol/header edits: ' + relative)
            protocol_files.append((target_file, content))
    for path, after in pending:
        if path.read_text(encoding='utf-8') == after:
            continue
        temporary = path.with_suffix(path.suffix + '.goodsharing.tmp')
        temporary.write_text(after, encoding='utf-8', newline='\n')
        temporary.replace(path)
    for path, content in protocol_files:
        if not path.exists() or path.read_bytes() != content:
            path.write_bytes(content)
    target = CPP / 'client-sdk-rust/target/release'
    target.mkdir(parents=True, exist_ok=True)
    if hashlib.sha256((RELEASE / 'bin/livekit_ffi.dll').read_bytes()).hexdigest() != 'cbd88551afab6dd6e0f93be1a47202a7fbe841097982dbe5965e54d77ac2573b':
        raise RuntimeError('Original release engine checksum mismatch')
    shutil.copy2(RELEASE / 'bin/livekit_ffi.dll', target / 'livekit_ffi.dll')
    shutil.copy2(RELEASE / 'lib/livekit_ffi.dll.lib', target / 'livekit_ffi.dll.lib')
    patch.parent.mkdir(exist_ok=True)
    patch_bytes = ''.join(changes).encode('utf-8')
    if not patch.exists() or patch.read_bytes() != patch_bytes:
        patch.write_bytes(patch_bytes)
    info = {'base_cpp': '80130a2', 'protocol': 'a6f93a16', 'ffi_engine_unchanged_sha256': hashlib.sha256((target / 'livekit_ffi.dll').read_bytes()).hexdigest(), 'patch_sha256': hashlib.sha256(patch.read_bytes()).hexdigest(), 'source_sha256': {str(path.relative_to(CPP)).replace('\\', '/'): hashlib.sha256(after.encode()).hexdigest() for path, after in pending}}
    marker_text = json.dumps(info, indent=2)
    if not marker.exists() or marker.read_text(encoding='utf-8') != marker_text:
        marker.write_text(marker_text, encoding='utf-8', newline='\n')
    print(json.dumps(info, indent=2))


if __name__ == '__main__':
    main()
