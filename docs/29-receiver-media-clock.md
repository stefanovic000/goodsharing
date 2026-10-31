# Receiver RTP/RTCP media clock

Date: 2026-10-22, Europe/Budapest. Local synthetic work authorized by “continue”. Receiver timing now provides genuine decoded RTP timestamps and a separate RTCP sender-report mapping. Both short live runs validate the mapping after training, but approximately eight-second startup fails the five-second target. Remote sound playback remains disabled.

The user requires an explicit question and approval before any real window/display capture, screenshot, or capture-based fixture test. This increment uses generated frames/PCM only. Current-build capture and GUI capture regressions remain pending approval. The rule is recorded in [AGENTS.md](../AGENTS.md); older capture reports remain evidence for their older builds only.

## Timing and security boundary

The pinned [audio receive implementation](https://raw.githubusercontent.com/webrtc-sdk/webrtc/89d790b/audio/channel_receive.cc) has the decoded first-sample RTP timestamp, which the [generic remote audio source](https://raw.githubusercontent.com/webrtc-sdk/webrtc/89d790b/pc/remote_audio_source.cc) drops when forwarding PCM. Its old NTP estimator and the [video receiver's estimator](https://raw.githubusercontent.com/webrtc-sdk/webrtc/89d790b/video/rtp_video_stream_receiver2.cc) wait for an RTT measurement before training. The existing optional capture timestamp and public video timestamp remain absent/zero in these live runs. They are preserved as such; callback arrival is never substituted for capture time.

Two additional native source replacements observe actual RTCP sender-report NTP/RTP pairs before that RTT gate. Their shared helper maps the known negotiated audio 48 kHz and video 90 kHz counters, with independent RTP origins, into the common sender-report NTP domain. No assumed RTT, receiver/sender clock offset or comparison between PCs is used. Source capture timing from increment 28 is preserved.

The registry has 32 fixed slots scoped to native receiver lifetimes, requires two consistent reports, handles RTP wraparound, expires mappings after five seconds without a fresh report, and limits extrapolation to five seconds. Duplicate reports cannot refresh age. Invalid timing, discontinuities or inconsistent rate reset the mapping and advance its epoch. Audio association requires the exact PCM pointer, rate, channel count and sample count within a thread-local callback scope; converted/rebuffered local frames receive no invented timing. Video requires consistent packet RTP/SSRC metadata and a unique active receiver SSRC; ambiguity supplies no clock. Native class layouts and virtual interfaces are unchanged.

Four new optional SDK fields carry decoded RTP, mapped sender NTP in microseconds, report count and clock epoch through C++, protobuf, Rust and native callbacks. They are separate from the existing timestamp fields. Missing timing stays missing. Future playback must also bind timing to the admitted owner/SID/generation, drop stale epochs and reset on revoke/reconnect.

**RTCP timing is infrastructure-supplied and is not E2EE-authenticated capture metadata.** It grants no media/decryption permission. Mandatory media encryption and the existing exact owner/SID admission gate remain in force. The repaired cryptor source and strict/closed policy headers are unchanged from increment 28. This increment does not close historical raw-SDK audio anomalies or whole-app E2EE release acceptance.

The engine build removes exactly the two original receiver objects and the already-replaced cryptor object from the pinned WebRTC archive, then links the replacements. Preparation verifies original source/header hashes; the SDK manifest verifies patch inputs, removed object names and installed DLL hashes. No duplicate-symbol suppression or unpinned source fetch is used. The added upstream files retain their BSD headers and existing WebRTC notice requirements.

## Synthetic measurements

Two owned local 20-second runs publish generated stereo PCM and hardware H.264 video through the loopback SFU. The second deliberately holds original PCM for 50 ms before submission, preserving its source timestamp. Independent content matching identifies the audio's position in the generated video timeline, then compares their mapped sender NTP values. The calculation uses 480 ms audio windows, does not extrapolate across missing video intervals/epochs, and is a clock/content diagnostic before presentation.

| Run | Quality | Median signed clock offset | p95 absolute offset | Mapped windows after training | First fully mapped window |
| --- | ---: | ---: | ---: | ---: | ---: |
| Baseline | 1080p | −17.51 ms | 19.16 ms | 113/116 (97.41%) | 8.3 s |
| Baseline | 720p | −18.00 ms | 19.53 ms | 113/116 (97.41%) | 8.3 s |
| Original PCM held 50 ms | 1080p | −13.98 ms | 14.95 ms | 116/118 (98.31%) | 8.0 s |
| Original PCM held 50 ms | 720p | −13.67 ms | 14.70 ms | 116/118 (98.31%) | 8.0 s |

The median changes by 3.53/4.33 ms, within the diagnostic's 10 ms delay-invariance bound, rather than shifting by the inserted 50 ms. Both runs pass native media correctness and all six sampled source-capture checks. Every received audio block has its real RTP timestamp, but the initial 821/2,029 baseline and 800/2,026 delayed blocks lack a trained sender-NTP mapping. Baseline video has 888/1,201 mapped frames per quality; the delayed run has 899/1,164 at 1080p and 899/1,201 at 720p. These are short inspected callbacks, not sustained presented-60-FPS results.

After training, at least eight seconds of measured content and at least 90% mapped coverage pass the p95 absolute-offset limit of 80 ms. **Both complete common-clock experiments still fail**, because startup exceeds five seconds. Whole-run mapped-window coverage is only 56.78%/58.59%. The [original baseline](../artifacts/20261022-021437-media-clock/summary.json) and [delayed experiment](../artifacts/20261022-021632-media-clock/summary.json) retain their failure status and original analyses. Additional [baseline](../artifacts/receiver-clock-baseline-analysis.json), [delayed](../artifacts/receiver-clock-delayed-analysis.json) and [comparison](../artifacts/receiver-clock-delay-comparison.json) records separate steady mapping from startup without changing any threshold.

The pinned SFU's [subscriber report worker](https://raw.githubusercontent.com/livekit/livekit/v1.13.10/pkg/rtc/participant.go) sends reports at three-second intervals, while the pinned native audio sender defaults to a five-second report interval. These intervals are plausible contributors to the observed bootstrap delay; this increment does not prove every contributor or alter either interval. No eight-second playback buffer was added. The probes continuously drain decoded media. Future playback must discard unclocked/stale input instead of accumulating a startup backlog.

This is not a speaker/display synchronization measurement, a ten-minute drift test, an Internet/congestion result, or evidence that all callback-age outliers are acceptable. The earlier independent content-matching and pressure failures remain open.

## Validation and retained evidence

The patched engine's native shutdown regression passes. All **17 native tests** pass, including two-report training, RTP wraparound, independent RTP origins/rates, stale and duplicate reports, malformed large timestamps, discontinuity/retraining, SSRC ambiguity, receiver address reuse, fixed-capacity exhaustion, exact PCM association, nested callbacks and thread isolation. The existing timed-source test also verifies that local PCM cannot acquire remote receiver timing.

All **24 offline controls** pass: seven RTCP clock controls, nine audio-content controls, four prior local-clock controls and four source-clock controls. They include absent/untrained timing, unrelated clock domains, a deliberately shifted clock, and valid steady mapping that still fails startup. All **11 synthetic publisher regression cases** pass, including wrong key, unconfigured crypto, cancellation, revocation during output, encryption disable and independent H.264 decoding. Every test-owned client/SFU exits and the final process audit finds zero project processes; no unrelated application is closed.

The first engine build failed to compile the fixed-slot helper because MSVC required an explicit default member initializer. That compiler failure is retained in `artifacts/receiver-clock-engine-first-build-failed.log`; the corrected engine, wrapper and app build successfully. Final SDK provenance verification passes and the final app build reports no remaining work. Actual-capture/GUI regression suites were not run for this build and remain pending the user's explicit capture approval.

The [frozen validation record](../artifacts/20261022-receiver-clock-validation.json) records exact sources, binaries, runtime manifests, evidence and explicit failed/pending gates. [Source](../artifacts/20261022-receiver-clock-source.zip) and [binary](../artifacts/20261022-receiver-clock-binaries.zip) archives preserve this increment; the binaries require the pinned Qt runtime and are not a release package. The previous installed SDK is retained in `artifacts/20261022-pre-receiver-clock-sdk.zip`. Increment 28's frozen evidence is unchanged. No VPS service was deployed.

## Next work

Reduce real clock bootstrap time with a bounded synthetic experiment, preserving report provenance and the existing startup gate. Then implement bounded WASAPI scheduling around the receiver clock, with no stale backlog, final permission checks, coordinated stop/revoke/flush/reset, and measured speaker/display sync. Preserve audio through video-quality changes before enabling sound in the normal UI.

Ten-minute drift, congestion recovery, sustained captured 60 FPS, three shares, hardware decoding, combined CPU/memory budgets, game coexistence, E2EE release acceptance, packaging and VPS/Internet validation remain open. Capture-dependent work requires approval first.
