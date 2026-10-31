# Original audio capture time

Date: 2026-10-22, Europe/Budapest. Local work authorized by “continue”. Remote sound playback remains disabled. This increment fixes the sender's lost PCM capture timestamp; it does not establish remote synchronization.

## Change

The captured first-sample timestamp already exists in `ShareAudioInput`, on the sender application's MSVC `steady_clock`. Previously the publisher checked its age, then submitted only PCM. The new SDK `AudioSource::captureFrameAt` carries that timestamp through the C++ request, protobuf, Rust frame and native unbuffered source. At the native handoff, elapsed capture age is translated into WebRTC's `TimeMicros` domain. Runtime/FFI scheduling delay remains part of that age; there is no assumed epoch offset and no comparison between PCs.

The native source rejects timestamps older than 100 ms, future/invalid timestamps, a mismatched PCM format, non-10-ms frames and timestamped buffered submissions. It never retries timed media as untimed PCM. An SDK error stops the publication through the existing audio-worker/encryption gate. Untimed priming silence and unrelated existing SDK callers remain untimed. Capture callback timeout now throws as documented instead of merely logging and returning success.

The conversion helper is shared by the SDK build and its native arithmetic test. The direct native source test sends distinguishable PCM blocks captured 25 and 70 ms earlier, checks their exact PCM/timestamp association at a local sink, rejects seven invalid calls, and verifies that untimed PCM has no invented capture timestamp. This local sink test bypasses Opus, the SFU and playback.

The live synthetic diagnostic samples only six native source submissions, checks that each exact timestamp belongs to an admitted source block, and checks the translated age including millisecond rounding. It does not measure every submitted block or claim that timing survives encoding, network forwarding or remote playback. Normal application operation emits no timing trace.

## Receiver boundary

The pinned upstream [remote audio source](https://raw.githubusercontent.com/webrtc-sdk/webrtc/89d790b/pc/remote_audio_source.cc) deliberately passes absent absolute capture time to audio-track sinks. Its raw input includes an RTP timestamp, which the generic track callback does not expose. The pinned [audio receive implementation](https://raw.githubusercontent.com/webrtc-sdk/webrtc/89d790b/audio/channel_receive.cc) invokes that raw callback before filling the frame's estimated NTP time. Merely overriding the optional local-capture-time callback cannot provide the missing remote clock.

The next receiver change must expose genuine decoded-media timing with its clock domain and provenance. RTP counters alone have independent origins for audio and video. They need a verified sender-report/capture mapping or an authenticated per-share media mapping, with wraparound, discontinuity, packet loss and time-stretch behavior tested. Callback arrival must never substitute for source timing. Retain audio through video-quality changes, then implement bounded WASAPI scheduling with a final permission check and coordinated revoke/flush before enabling sound.

## Validation

The rebuilt SDK passes its native shutdown regression. All 16 application/native tests pass, including the delayed PCM source test: the requested 25/70 ms capture ages arrive as 26/71 ms, consistent with elapsed submission time and millisecond rounding. Seven invalid API calls are rejected and untimed PCM stays untimed. The four new sender diagnostic controls, four existing media-clock controls and nine audio-content controls pass (17 offline controls).

The [18-second synthetic loopback experiment](../artifacts/20261022-014446-media-clock/summary.json) passes native media correctness, stereo and all six sampled sender-capture checks. The sampled original timestamps exactly match admitted source blocks; their source-to-native submission ages are 96–162 microseconds. These six samples are not an all-block latency percentile.

The same experiment **fails the remote common-clock gate**: all 1,831 received audio blocks lack capture timestamps; all 1,076 decoded 1080p frames and 1,077 decoded 720p frames have zero public SDK timestamps. The independent content check matches 171/179 audio windows (95.53%), with 75.35 ms p95 synthetic source-to-decoded-callback window age. This excludes actual capture, presentation, speaker delay and Internet transport and does not verify sync. The failure remains recorded as a failed clock experiment.

All 11 [publisher regression cases](../artifacts/20261022-014527-share-publisher/summary.json) pass, including wrong-key/unconfigured receiving, cancellation, revocation, encryption disable and independent H.264 decoding. All four [actual capture cases](../artifacts/20261022-014635-capture-sharing/summary.json) pass: selected-app stereo, audio off, wrong key and source closure. The known source-closure error is required by that case. Stereo/isolation, zero wrong-key admitted content and controlled cleanup remain verified by those bounded cases.

The current short actual-capture samples are:

| Selected video | Inspected decoded frames | Inspected FPS | Capture-to-decoded p95 | Pixel marker errors |
|---|---:|---:|---:|---:|
| 720p with app audio | 293 | 59.91 | 43.90 ms | 0 |
| 1080p, audio off | 606 | 58.98 | 71.56 ms | 0 |

These are inspected callbacks from a controlled 960×540 fixture scaled into the paired outputs, not native 1080p detail, physical display cadence or a five-minute acceptance test. The 1080p short sample is below 59 FPS and does not close the sustained-60-FPS gate. Sender profiling in approximately 5.2-second middle windows records 16.04%/19.26% of this 12-logical-processor PC and 286.19/283.93 MiB peak private memory respectively, including WGC, conversion, both hardware encoders and local preview. These short samples do not isolate the timing change or establish a resource budget. The independent viewer is an inspection probe without normal UI rendering or sound playback; its measurements are not the finished viewer's footprint.

All six [GUI viewing cases](../artifacts/20261022-014800-remote-viewing/summary.json) pass: 720p, 1080p, quality switching, wrong key, source closure and static content. The final regression suites therefore pass 16 native tests, 17 offline controls and 21 integration cases. The separate live common-clock experiment still fails as described above.

The [frozen validation record](../artifacts/20261022-source-audio-validation.json) records exact source, executable, SDK and evidence hashes. The [source archive](../artifacts/20261022-source-audio-source.zip) contains the reproducible recipes and tests. The [binary archive](../artifacts/20261022-source-audio-binaries.zip) requires the pinned Qt runtime and is not a release package. The preceding SDK is retained in `artifacts/20261022-pre-source-audio-sdk.zip`. The cryptor source/policy is unchanged. Test-owned clients and SFUs are stopped; no unrelated process was stopped and no VPS service was deployed.

The common-clock gate, speaker/display sync, ten-minute drift, congestion recovery, sustained captured 60 FPS, three-share workload, resource budgets, game coexistence, E2EE release acceptance and VPS/Internet tests remain open. Earlier failures remain recorded in the preceding reports.
