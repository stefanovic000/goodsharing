# Clock bootstrap experiments

Date: 2026-10-22, Europe/Budapest. Synthetic local work authorized by “continue”. The final opt-in experiment establishes usable sender-clock timing in **1.2 seconds**, compared with approximately eight seconds using the existing report intervals. Normal app/server defaults are unchanged, and remote sound playback remains disabled.

No real screen/window capture, screenshot, real audio capture or physical playback was performed. Capture/GUI capture regressions remain pending the user's explicit approval under [AGENTS.md](../AGENTS.md). No unrelated application was closed and no VPS service was deployed.

## Implementation

The client experiment requests 500 ms audio/video RTCP report intervals using the pinned WebRTC configuration setters. It is active only when both the synthetic trace and explicit test flag are present. Normal operation retains upstream defaults. Receiver mapping still requires two consistent reports; its rate validation, epochs, expiry, fixed-capacity storage and media permission checks are unchanged.

The [pinned SFU report worker](https://raw.githubusercontent.com/livekit/livekit/v1.13.10/pkg/rtc/participant.go) has a separate three-second timer. A separate local server build adds an explicit opt-in 500 ms interval while retaining that default. It uses the existing Go 1.26.1 compiler, project-local caches, read-only module files and verified module sums. The exact source archive, patch, build scripts and binary provenance are pinned and checked. The existing release server binary is preserved. This is an isolated experiment rather than a deployed or redistributable server release; Go dependency notice collection remains pending as described in [third-party requirements](../third_party/README.md).

The initial comparison used the release server for default reporting and the rebuilt server for faster reporting. To remove that build difference, a second matrix holds both client and server binaries constant while varying only the report settings. All later matrix cases also use the same server build. Each run owns its loopback TCP/UDP 8765 service, refuses occupied listeners, uses generated H.264/stereo PCM and stops its own processes.

RTCP sender NTP remains infrastructure-supplied timing, not E2EE-authenticated capture metadata. It grants no decryption or playback permission. The repaired cryptor source and strict/closed policy headers are unchanged from increment 29. Production invitations, rekey/reconnect and whole-app E2EE release acceptance remain open.

## Diagnostic corrections

The old synthetic audio generator reused video pacing and stamped each generated 480-sample block at timer wakeup. Its waveform/content code advanced on an exact 48 kHz sample counter. One delayed experiment accumulated **54.94 ms** difference between those timelines in twenty seconds. The generator now assigns each first-sample timestamp from a fixed source origin plus its exact sample position and waits until that time before producing the block. Submission jitter remains measurable capture age; original timestamps survive the deliberate 50 ms holding queue. This changes the generated fixture only. Real WASAPI capture and its original timestamps are unchanged.

The sender analyzer now verifies the sample grid for every admitted generated block when explicitly marked with this model. It still checks only six native submissions for source-to-native timestamp propagation; it does not claim native measurements for every block. Two added controls distinguish these checks and reject wakeup drift in an otherwise untraced source block. All 2,001 admitted timestamps follow the grid exactly in each final confirmation.

Another failed experiment exposed an observation-duration bug: one trailing content match jumped backward to early content, making `last_position - first_position` negative despite over eighteen seconds of prior valid observation. Observation duration now uses the longest ordered source-content interval. Backward matches or forward jumps larger than the elapsed window interval plus 200 ms break that interval, so an isolated jump cannot fabricate a long observation. All offsets and matches remain in coverage/percentile metrics; no outlier is removed to force a pass. Three new controls cover a backward tail, a large false forward jump and unmatched-window gaps. The five-second startup, eight-second observation, 90% coverage, 80 ms clock-offset and 150 ms decoded-content-age targets are unchanged.

The [original failed report](../artifacts/20261022-033228-media-clock/summary.json) retains its negative duration. A [separate reanalysis](../artifacts/clock-bootstrap-span-reanalysis.json) measures an 18.3-second ordered interval and passes the clock check. Its backward-match count and original large content-age outlier remain visible. This diagnostic does not establish a strict maximum end-to-end age or resolve historical content-matching anomalies.

## Results

The corrected sample-clock comparison and final confirmations show:

| Client reports | SFU reports | First fully mapped audio/video window | Startup target |
| --- | --- | ---: | --- |
| Upstream defaults | 3 seconds | 8.2 s | Fail |
| 500 ms | 3 seconds | 5.2 s | Fail |
| Upstream defaults | 500 ms | 3.1 s | Pass |
| 500 ms | 500 ms | 1.2 s in both final confirmations | Pass |

These are sync-metadata startup times, not a media buffer or a measurement of screen-to-screen latency. The test continuously drains media. No startup backlog or new application playback queue was added.

The [final clean confirmation](../artifacts/20261022-033503-media-clock/summary.json) and [50 ms source-delay confirmation](../artifacts/20261022-033528-media-clock/summary.json) both pass native aggregate media checks, source timing, common-clock startup/continuity and independent decoded-audio content-age checks:

| Run | Quality | Median signed clock offset | p95 absolute clock offset | Mapped windows after training |
| --- | ---: | ---: | ---: | ---: |
| Clean | 1080p | −10.65 ms | 11.72 ms | 184/186 (98.92%) |
| Clean | 720p | −10.50 ms | 11.77 ms | 184/186 (98.92%) |
| Original PCM held 50 ms | 1080p | −12.22 ms | 13.32 ms | 184/186 (98.92%) |
| Original PCM held 50 ms | 720p | −12.74 ms | 13.78 ms | 184/186 (98.92%) |

The median changes by −1.57/−2.24 ms, within the unchanged 10 ms delay-invariance diagnostic bound. Each final run observes more than eighteen seconds of ordered content. Whole-run mapped-window coverage is 92.93%; initial untrained timing remains absent rather than being replaced by callback arrival. These remain local, pre-presentation, short generated-media measurements; they do not verify physical A/V sync, presented 60 FPS or a ten-minute drift limit.

Across all **18 live clock experiments**, eight original failed snapshots are retained. One early client-only run stalled media delivery: audio content matching fell to 37%, video stopped at 474 inspected frames per quality, and its last clock mapping expired. Aggregate tone/pixel probe checks alone did not detect continuous delivery failure; the independent content/clock gates rejected it. A later same-build client-only run delivered continuous content but still missed startup at 5.2 seconds. The original stall's cause remains unresolved.

The second matrix's delayed run also failed coverage/age (84.29% mapped coverage and 163.88 ms p95 decoded-audio window age). The later sample-clock correction removes the proven generator drift; it does not establish that every earlier failure had that cause. These failures are retained and normal settings have not been changed. See the [complete comparison record](../artifacts/clock-bootstrap-final-comparison.json), including original group statuses, source-grid errors and clock-state transitions.

## Validation and next work

All **17 native tests**, **29 offline controls** (ten RTCP, nine audio-content, four prior local-clock, six source-clock), and the SDK shutdown regression pass. The separately built SFU passes all **three upstream RTP statistics tests**. The first Go test selection matched zero tests; that attempt and its build inputs are retained. The corrected build requires exactly three top-level passes before recording provenance. No test pass is claimed for the empty selection.

All **11 normal publisher regression cases** pass on the final probe with the existing release SFU, including independent H.264 decoding. Four additional cases pass with both fast-report settings: wrong key, unconfigured receiving, revocation during encoded output and encryption disable. Wrong-key and unconfigured receivers admit zero audio/video; valid RTCP timing does not open the media gate. Historical raw-SDK crypto anomalies are not declared resolved.

The [frozen validation record](../artifacts/20261022-clock-bootstrap-validation.json) records exact sources, binaries, SDK/server provenance, original failures, final confirmations and pending gates. [Source](../artifacts/20261022-clock-bootstrap-source.zip) and [binary](../artifacts/20261022-clock-bootstrap-binaries.zip) archives retain this increment; they are development snapshots requiring the pinned Qt runtime. The preceding SDK is saved in `artifacts/20261022-pre-clock-bootstrap-sdk.zip`. Increment 29's frozen archives remain unchanged.

Next implement a production bootstrap policy and bounded synchronized WASAPI playback with final owner/SID/generation permission checks, coordinated revoke/flush/reset and audio continuity through video-quality changes. Measure report overhead before changing normal defaults. Physical speaker/display sync, ten-minute drift, capture regressions, congestion recovery, sustained captured 60 FPS, three-share CPU/memory budgets, game coexistence, packaging and VPS/Internet tests remain open. Real capture requires approval first.
