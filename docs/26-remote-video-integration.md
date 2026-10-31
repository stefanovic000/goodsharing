# Remote video in the Windows app

Date: 2026-10-21, Europe/Budapest. Local implementation authorized by the user's “continue”. No SSH/VPS work or unrelated process shutdown.

The normal Qt application now watches another client's encrypted video. A grouped share selector offers manual 720p or 1080p selection, Apply selection and Stop viewing. One selected remote video is supported in this increment. Local preview/publication can remain active in its separate surface; combined-workload acceptance is still pending. Connected-room controls collapse, and capture controls can expand while watching, leaving most of the window for video.

Remote sound playback is deliberately disabled. Sending selected-app/desktop audio remains available, and independent native probes retain stereo reception. The app does not yet provide a synchronized audio/video viewing demo.

## Media ownership and drawing

`PreviewBackend` groups authenticated announcements through `ShareModel`, then requests an immutable `RemoteShare` manifest with exact owner, share ID, generation, selected quality and video SID. It requests no audio subscription for this video-only increment. Old selections are revoked immediately; a replacement starts only after the old watch reports complete reader/subscription teardown. This prevents overlapping normal video decoders during quality replacement. Reconnect, room failure/leave, source departure and encryption revocation stop viewing.

`RemoteVideo` owns one latest RGBA image copied from the borrowed SDK callback. It rejects wrong dimensions/format/rotation, missing GS02 identity and synthetic priming. There are no per-frame Qt queued invocations accumulating images. The UI checks for changes on its existing 16 ms timer. The final `VideoItem::paint` call fills the background and invokes `RemoteShare::presentVideo` immediately before drawing content. This checks room admission, sticky watch permission, all selected encryption states, and the receiver's current exact owner/SID permission and failure state. Drawing and explicit watch revocation share a mutex. A retained callback/image cannot grant permission to a replacement generation.

The **150 ms bound is receiver-local arrival-to-first-paint admission**, not capture-to-display latency or a network TTL. A late first presentation is discarded. An image already presented while permitted can remain when the source becomes static; its permission is still checked on redraw and by the UI poll. After 150 ms without a new image, the status describes the picture as possibly static or stalled. That state is not treated as successful A/V synchronization. Revocation clears the surface. These checks do not recall an already submitted compositor frame or prove physical display timing.

The renderer uses a CPU `QImage` and `QQuickPaintedItem`. Hardware decode, GPU texture import/zero-copy, sustained 60 FPS presentation and the final low-footprint target are not established. A paint revision count identifies distinct received images drawn by the CPU; it is not a count of unique source content or physical refreshes.

## Why audio synchronization remains next

Inspection of the pinned SDK found that `livekit::AudioFrameEvent` exposes samples, rate and channel count but no presentation/capture timestamp. The Rust `libwebrtc::AudioFrame` and FFI audio buffer information likewise omit timing. Comparing that callback's arrival with the sender's GS02 monotonic timestamp would mix unrelated clocks across PCs.

The pinned WebRTC `AudioTrackSinkInterface` has an overload with optional `absolute_capture_timestamp_ms`, documented on the receiver's `TimeMillis()` clock. The current native SDK sink overrides the older overload and drops that information. Its availability and relationship to delivered video timing still need proof. This is a concrete next investigation, not evidence that a usable common timeline already exists. No SDK or cryptor binaries changed in this increment.

Next expose optional audio timing through the reproducible SDK patch, verify it against video on a common receiver clock, then connect bounded stereo playback with a device clock and final render-worker permission checks. Test actual flash/click alignment, variable FPS, continuous audio during quality replacement and ten-minute drift before enabling playback. Missing/unusable timing must have an explicit policy; callback-arrival guesses do not satisfy the sync gate.

## Verification and retained limits

The frozen evidence record is [remote-video validation](../artifacts/20261021-remote-video-validation.json). It records source/binary archive hashes, the unchanged SDK runtime, native tests, GUI runs, capture/preview regressions and cleanup. Executables in the archive still require the pinned Qt/LiveKit runtime; it is not a redistributable package.

`scripts/test_remote_viewing.py` starts only its own loopback SFU, controlled source, GUI publisher and GUI viewer. It refuses an occupied port and passes credentials through the environment. The source is a 960×540 controlled window scaled into exact 1080p/720p tracks, not a native-1080 capture-capacity test. Each normal GUI case verifies the complementary fixture bars in a screenshot of the composited Qt surface. It does not inspect every presented frame, measure photons or prove five-minute FPS.

The final suite covers both selected qualities, 720→1080 replacement, zero drawn content with a wrong key, source closure and a frozen source whose exact final pixel ID is recorded independently. All native checks include unverified/malformed/priming input, held static images, delayed first paint, queued-image revocation, sticky closure, exact owner/SID permission and room invalidation. No speaker playback or audio sync is claimed by these tests.

[All six final GUI cases pass](../artifacts/20261021-222458-remote-viewing/summary.json); [all 14 native checks pass](../artifacts/remote-view-final-native.log). The tested app SHA-256 is `179c438a4e9b424ac4c5760685c8094b77db9bac5a9b1068424be11507e6a75d`. The fixture/SDK/native-test hashes and subsequent capture/preview regression paths are pinned in the frozen evidence record.

| GUI scenario | Images admitted | Distinct received images CPU-painted | Final screenshot |
| --- | ---: | ---: | --- |
| 720p | 482 | 436 | Correct fixture bars. |
| 1080p | 485 | 437 | Correct fixture bars. |
| 720→1080 switch | 221 after replacement | 204 after replacement | Correct bars on the 1080p watch; earlier 720p also painted over 100 images. |
| Wrong key | 0 | 0 | No fixture image. |
| Source closes | 286 before closure | 268 before closure | No retained visible share after closure. |
| Frozen source | 478 | 437 | Correct recorded final source ID. WGC continued delivering unchanged pixels. |

Counts come from short runs whose viewer attempts a screenshot at nine seconds; startup, teardown and the quality-replacement gap are included differently. They are not a sustained FPS benchmark. The viewer samples only one composited fixture marker per scenario; earlier independent capture tests check decoded content across many frames.

The short active middle sample for the **receiver process alone** uses 0.72 core equivalents / 6.01% of this 12-thread PC for 720p and 1.17 cores / 9.74% for 1080p. Whole-run private-memory peaks are 143.72 / 167.88 MiB; working-set peaks are 114.57 / 136.82 MiB. The sender, SFU and fixture are separate processes. These figures include software decoding, image copying and Qt presentation; they do not establish the low-footprint target. Source-exit and wrong-key windows contain inactive time and are not normal viewing CPU estimates.

The intermediate [five-case UI suite](../artifacts/20261021-221747-remote-viewing/summary.json) passed before the compact-layout/final-permission changes. The following [six-case attempt](../artifacts/20261021-222207-remote-viewing/summary.json) retains a failed **test assumption**: it required WGC callbacks to stop when the source stopped drawing. This PC continued delivering unchanged images, while the frozen fixture was correctly visible. The corrected test records the fixture's last source ID and compares the composited pixels to it. Native tests separately verify behavior when frame delivery genuinely stops. That retained failing result is not relabeled as a pass.

Congestion failures, historical raw audio/resource anomalies, production invitation/rekey handling, simultaneous three-share load, device-loss/reconnect recovery, sustained FPS/resources, game coexistence and packaging remain open. The anti-cheat design boundary stays Windows-managed capture/audio with no injection, hooks, overlay, driver or game-memory access; this increment does not claim game acceptance.
