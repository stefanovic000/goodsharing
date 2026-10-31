# Step 1: stereo receive, playback and SDK lifecycle

Date: 2026-10-20, Europe/Budapest. **Step 1's API investigation, implementation and bounded local verification are complete.** Stereo/playback, normal lifecycle, a controlled decoder-load lifecycle run, sustained video and independent recording checks pass. Earlier intermittent contention failures remain recorded below and must be addressed in later load/startup hardening. This does not complete UI integration or a redistributable release.

[Combined verification index](../artifacts/20261020-step1-validation.json) links the final raw evidence, exact runtime hashes and preserved failed checks. All owned test/server/compiler processes had exited at the final process check.

## What changed

The active SDK is a project-local build based on LiveKit C++ 1.12.3 / C++ commit 80130a2 and Rust a6f93a16 / FFI 0.12.82. Original release files are retained separately. [The dependency lock](../dependencies.lock.json), [C++ patch](../patches/livekit-cpp-1.12.3-stereo-build.patch), [engine patch](../patches/livekit-rust-a6f93a16-shutdown.patch) and build scripts record the exact inputs and modifications.

- `AudioStream::Options` now forwards sample rate and channel count through both track and participant receive paths. Existing callers retain the original mono default; the stereo probe explicitly requests 48 kHz and two channels.
- Audio publication now advertises a native stereo source to the SFU through `AddTrackRequest.stereo`. Exposing only the receive option passed once but intermittently produced a duplicated mono mix, including through actual playback. That failed report remains preserved in [the pause handoff](11-step1-handoff.md).
- Native peer-transport close cancels new/late offer results, clears its offer callback, cancels debounced session negotiation and wakes a fast-negotiation waiter. Socket closure remains synchronous. A native Rust test covers an offer started before close and a post-close offer request while the transport mutex is held.
- The FFI room's non-owning track index now forgets a handle when the client releases it. Room close drains the index before releasing remaining tracks. This removes the repeated-release cause of the unknown-handle warning while retaining the warning for other invalid releases.
- Signaling no longer issues a second diagnostic HTTP request after a transport error/timeout. HTTP handshake failures still retain status validation. In the stalled-handshake scenario, cleanup now waits for the configured one-second connection attempt instead of another three-second diagnostic request.

## Ownership and shutdown contract

`RoomConnection` owns its connector thread and room. Start/Stop belong to the owner thread; a delegate must outlive the connection. Stop requests cooperative cancellation, joins the blocking SDK connect, clears the delegate and disconnects. It never disconnects concurrently with a connect that could publish room state afterwards. This is **not** an instantaneous SDK connect-cancel API: the measured stalled attempt uses `connect_timeout=1000 ms` and no join retries. Other timeout/retry settings and later setup stages may take longer; broader setup/reconnect cancellation remains an integration test requirement.

The stereo pair rejects late stream creation, closes its stream and joins its reader, clears both delegates, and attempts both room disconnects even if one cleanup fails. Reader and room teardown precede playback/capture buffer destruction. Room/source/track objects die before SDK shutdown. The original video receiver uses the same late-callback admission rule. No arbitrary shutdown sleep was added to hide negotiation errors.

`AudioPlayback` uses shared-mode WASAPI, stereo 48 kHz signed 16-bit PCM, a 100 ms queue bound, validated volume, an owned COM/worker lifetime, and device/error/queue/underrun counters. The observed device buffer is 22 ms. Device changes, multiple-share mixing, live volume controls and sustained drift correction remain later integration work.

## Evidence

All audio below is controlled synthetic content: 440 Hz on the left, 880 Hz on the right, amplitude 4000. The wanted-channel amplitude must exceed 100 and crosstalk must remain below 3%; reporting two channels alone cannot pass. Tests save measurements, not audio recordings.

| Check | Result and evidence |
| --- | --- |
| Six stereo cases | [Complete suite](../artifacts/20261020-151952-stereo/summary.json) passed: mono negative control, unencrypted stereo, encrypted stereo by track, encrypted stereo by participant, encrypted WASAPI playback measured by this process's loopback, and wrong-key rejection. Every room emptied and its owned SFU exited; no negotiation-after-close, panic, or unknown-handle warning. |
| Repeated fresh playback | Suite playback plus [second](../artifacts/20261020-152132-stereo/summary.json) and [third](../artifacts/20261020-152158-stereo/summary.json) fresh sessions passed both received PCM and process-loopback separation. No queue rejection. The first suite's worst crosstalk was below 0.4% at playback loopback, versus the unchanged 3% limit. |
| Wrong-key stereo | Explicit GCM publication/readiness and SDK decryption failures; maximum full-interval tone below 0.056 and maximum overlapping 100 ms tone 2.78, below limits 40/200. PCM concealment callbacks are not mistaken for decrypted content. |
| Lifecycle | [Twenty cycles](../artifacts/20261020-152111-lifecycle/summary.json) passed, including rejected credentials, exception-driven RAII cleanup and a stalled join. Maximum explicit Stop 8.82 ms; stalled-join cleanup 1002.97 ms; no server participants remained. After warmup, handles/threads grew by 0/0 and private memory by 1,044,480 bytes within the defined bound. This is a bounded regression, not a long soak. |
| Native shutdown regression | `engine-native-shutdown-test2.log`: 1 passed, 0 failed, approximately 0.02 s. Initial test-only polling compilation failed and was corrected; the failed log is retained. |
| CTest | Metadata codec and pacing tests both passed. |
| Metadata controls | [Regression](../artifacts/20261020-152249-metadata-regression.json) passed. Unsafe legacy framing failed as expected, safe framing passed, and deliberately wrong IDs were rejected. Independent decoding also passed the retained synthetic access units. |
| Video/audio encryption modes | [15-second suite](../artifacts/20261020-152308-local-media/summary.json) passed off, shared-key and wrong-key modes with NVIDIA MFT encoding and preserved encoded handoffs. Correct-key qualities exceeded 59 FPS; wrong-key decoded no video and no usable tone. |

The initial checks above ran before the final wrapper rebuild and the final test-only engine-source adjustment. A final installed-artifact regression is recorded below when finished. Historical failures remain unchanged.

### Historical final-artifact regression before the passthrough correction

- [Final six-case stereo suite](../artifacts/20261020-153912-stereo/summary.json) passed all receive/playback/key/lifecycle checks against the final SDK.
- [Final lifecycle suite](../artifacts/20261020-153811-lifecycle/summary.json) passed 20 explicit cycles, all three explicitly counted exception cleanups, rejected credentials, and stalled join. Maximum Stop 20.32 ms; stalled cleanup 999.45 ms; after warmup 0 extra handles/threads and 1,273,856 extra private bytes; room and owned-process cleanup passed.
- **[Final five-minute video stress run](../artifacts/20261020-153120-local-media/summary.json) failed.** 1080p/720p averaged 58.39/58.22 inspected FPS. Each quality had one pixel mismatch at source frame 2530 (expected 39829, decoded 40949), coincident with both bitrate targets dropping from about 2.56 Mbps to 1.02 Mbps. This is a correlation, not yet a proven cause. P95 submission-to-decoded latency remained 50.42/54.28 ms; all hardware-produced access units were handed off. Receiver statistics included packet loss, decoder drops and two native stream-queue overflow warnings. Preserve this failure and its diagnostic images; do not claim the earlier passing baseline establishes this build's sustained correctness.

This failure triggered further investigation before finishing step 1. A bounded bitrate-transition diagnostic was added to retain encoded access units for independent decoding; it does not alter normal/default behavior or relax verification thresholds.

[A forced two-second 1 Mbps step](../artifacts/20261020-154052-local-media/summary.json) passed, and [a separate 60-second recorded stress run](../artifacts/20261020-154133-local-media/summary.json) passed live and independent decoding. Neither reproduced the failure, so a bitrate change alone is not established as its cause.

Code inspection found a separate unsafe configuration in the SDK passthrough adapter: its `EncoderInfo` left `has_trusted_rate_controller` at the default false. [WebRTC documents](https://webrtc.googlesource.com/src/+/refs/heads/main/api/video_codecs/video_encoder.h) that this enables a media-optimization input dropper. Dropping a raw frame before encoding is valid; doing so after our external encoder produced a reference frame can break subsequent decoding. The patch now declares external rate control for the passthrough adapter, with hardware CBR feedback and RTP congestion handling retained. Its subsequent sustained verification is recorded below. This correction does not yet conclusively identify every cause of the earlier loss/mismatch.

The failed run handed 17,980/17,977 hardware access units to the SDK, while its outbound encoder counters reached only 17,934/17,931. The earlier passing five-minute baseline differed by just one access unit per quality. These asynchronous counters help identify an internal-drop risk but are not a complete per-access-unit transport trace.

After the adapter change, [eight-second mode/bitrate-step checks](../artifacts/20261020-155052-local-media/summary.json) passed in all encryption modes, with independent decoding of every recorded content frame. The native shutdown regression passed again. The current engine DLL SHA-256 is `288132e619e0e30b7ad273d49cdf2441c8c63eca789a52cfb1a52662d24c7924`; new fixture runs verify and retain both runtime DLL hashes and the build manifests in `runtime-info.json`, and reject stale probe DLLs.

### Passthrough correction: sustained result

[The final 300-second live report](../artifacts/20261020-155136-local-media/shared-key.json) passed the unchanged correctness/rate/latency criteria with the four-thread load burst: 17,919/17,919 inspected 1080p frames matched, 17,964/17,964 inspected 720p frames matched, at 59.73/59.88 FPS and p95 submission-to-decoded delay 48.17/52.87 ms. Hardware produced and handed off 17,995 access units per quality; the outbound encoder counters reached 17,994 per quality, returning to the expected one-unit asynchronous difference. Decoder/RTP statistics reported zero decoder drops, packet loss, NACK and PLI. Inspection counts still differ from decoder totals; they are not interchangeable.

The original aggregate runner then failed its fixed 60-second **offline** decode deadline and retained an empty `runs` summary. That failed wrapper result is preserved. [A separate bounded verification](../artifacts/20261020-155857-recording-verification/summary.json), without recapturing or modifying the live report, independently decoded all 17,990 content frames in each source recording with zero mismatches and no pending frames. Offline analysis now uses a limit scaled to recording duration, up to 300 seconds per quality; no live media threshold changed. Use `scripts/verify_recordings.py` to inspect saved synthetic recordings independently.

While that offline decode was running, [an overlapping lifecycle test](../artifacts/20261020-155912-lifecycle/summary.json) failed its shutdown deadline/error gate before recording its first cycle, and [an overlapping audio suite](../artifacts/20261020-160020-stereo/summary.json) failed playback startup at the unchanged encryption-readiness timeout. Both fixtures still cleaned up and left no participants. The old lifecycle report did not retain its failing Stop duration/error, so that exact failure cannot be classified from its report; the probe now retains the failing cycle. Resource contention is a possible explanation, not a proven cause. These stress failures remain recorded; serial final checks follow below, and broader startup/shutdown behavior under sustained contention remains an integration/load requirement.

### Final serial and controlled-load checks

- [All six stereo cases](../artifacts/20261020-160401-stereo/summary.json) passed against the final runtime: track/participant receive, actual WASAPI playback, mono negative control, and key rejection. Final playback had no rejected or underrun frames, a 10 ms queue high-water mark and crosstalk below 0.018% in process loopback; the unchanged limit is 3%. Wrong-key tone amplitudes were zero. Rooms emptied and the owned SFU exited.
- [Serial lifecycle](../artifacts/20261020-160347-lifecycle/summary.json) passed 20 cycles, three exception cleanups, invalid credentials and stalled join. Maximum Stop 19.16 ms; stalled join cleanup 1007.88 ms. After warmup, +2 handles, +0 threads and +1,093,632 private bytes remained within the defined bounds.
- [Bounded decoder-load reproduction](../artifacts/20261020-160601-lifecycle-decode-load/summary.json) also passed. The owned decoder was active throughout the lifecycle suite, consuming approximately 14.27 CPU seconds during it. [Its lifecycle run](../artifacts/20261020-160602-lifecycle/summary.json) passed all 20 cycles with maximum Stop 8.13 ms, cancellation 1000.64 ms, no extra handles/threads after warmup and empty room membership. The decoder was deliberately terminated as an owned load fixture after the test; its full recording had already passed separate verification. This attempt did not reproduce or explain the earlier intermittent failure.

The final checks establish their bounded scenarios. The earlier contention timeouts remain failed historical checks; a later passing test is not proof that their cause is fixed. Keep them as explicit startup/shutdown/load regression cases before release.

[Final metadata regression](../artifacts/20261020-160801-metadata-regression.json) also passed on this runtime: safe framing succeeds, legacy framing and deliberately wrong IDs fail as expected, and independent source decoding remains clean. CTest's metadata/pacing checks and the native Rust shutdown regression passed. Step 2 and VPS deployment were not started.

## Build and redistribution record

`build-patched-sdk.ps1` builds the wrapper followed by the engine. Reinstalling the wrapper restores the separately staged patched engine, preventing the old release FFI from silently replacing it. `build.ps1` verifies current patch and installed binary/header hashes. Preparation checks archive hashes and actual file contents on every run, preflights edits, writes changed files atomically, and avoids rewriting unchanged source files. C++ vendored dependencies use URL_HASH; the engine uses Cargo.lock. Rustup/Cargo 1.97.1, protoc and libclang remain under `.tools/`; no system default toolchain or PATH was changed.

[The redistribution record](../third_party/README.md) and [inventory](../third_party/inventory.json) retain upstream notices, installed Qt SBOMs, WebRTC's aggregate license and build configuration, Rust dependency declarations, and native resampler source-header attribution. They explicitly record LGPL/MPL and codec/source/relinking work still required for a release package. Recording these requirements completes the dependency investigation scope; it does not clear distribution.

## Remaining scope

The normal app still provides a local capture preview. Stereo playback and connection ownership currently live in reusable native components and probes. Actual captured-media publication, receiving/rendering/playback in the room UI, quality switching, multiple simultaneous shares, A/V sync/drift, complete reconnect/cancel/device-loss behavior, packaging, actual game capture/coexistence checks and Internet testing remain later work. The no-injection/game-hook boundary is a core requirement throughout implementation. Video reception still uses software FFmpeg decoding. No SSH/VPS changes were made and no unrelated application/service was stopped.
