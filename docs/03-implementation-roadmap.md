# Implementation roadmap

Status: **milestone 0 in progress**, following the user's instruction to continue. Checkboxes remain pending until their evidence is recorded.

The next implementation slice is specified in [the local integration plan](10-next-local-integration-plan.md). It closes stereo/lifecycle gaps and joins capture, transport, playback, and a minimal room UI; this does not mark the remaining roadmap gates complete.

The normal order is milestones 0-5 on the Windows PC, then milestone 6 for final Ubuntu deployment and remote validation. A temporary Ubuntu test is allowed earlier only to answer a concrete question that cannot reasonably be tested locally. Record that question, why the PC cannot cover it, the bounded test, and its cleanup; resume PC development afterward. An unavailable local tool alone does not automatically move development to the VPS.

## 0. Dependency and API feasibility

- [x] Inspect the existing workspace and available tools; preserve the historical benchmark artifacts.
- [x] Choose and build against pinned Windows x64 MSVC, CMake, Qt Quick 6.8.3, and LiveKit SDK 1.12.3. See [dependency pins](../dependencies.lock.json).
- [x] Obtain dependencies into the project area where practical, verify release hashes, and record versions and required redistribution requirements. See [the dependency record](../third_party/README.md); final redistribution clearance remains part of packaging.
- [x] Prove SDK connection, native video reception, and shared-audio publication using synthetic sources. The patched SDK also passes distinct-channel stereo receive/playback tests; see [step-1 results](12-step1-results.md).
- [x] Probe Windows Graphics Capture and WASAPI application/desktop loopback on the actual OS.
- [x] Prove exact 1080p60 and 720p60 publication, actual Windows hardware-encoder selection, and decoded output at both sizes in the synthetic feasibility harness. Actual capture/render and three-share workloads remain later gates.
- [x] Select the paired-track fallback described in [architecture](01-product-and-architecture.md#exact-quality-publication-validate-before-selecting-a-path), with native Media Foundation hardware encoding and pre-encoded LiveKit publication. It is not true simulcast.

Current evidence: [native implementation report](08-local-implementation-results.md), [mismatch fixes](09-frame-metadata-fix.md), and [step-1 results](12-step1-results.md). Following a preserved failed run, the passthrough rate-controller correction passed five minutes at 59.73/59.88 FPS with zero inspected mismatches; saved source recordings passed independent decoding. Stereo/playback and bounded lifecycle checks pass; video remains software FFmpeg decoding. Intermittent contention/startup/shutdown behavior, integrated app operation and release packaging remain later gates.

Exit: a small measured feasibility report identifies the supported publication path, encoder, SDK APIs, audio capabilities, and remaining limitations. Resolve API gaps before polishing the UI. No service deployment is necessary for the initial native capture checks.

## 1. Capture and local preview

- [x] Establish the C++20/Qt Quick application skeleton with separate capture/encoder/preview components; transport currently runs in the native harness.
- [x] Implement source selection, D3D11 capture, aspect-preserving local preview scaling, and a local video preview. Publication scaling remains pending.
- [x] Implement audio mode selection, process/desktop loopback capture, metering, and start/stop handling. Additional device-change tests remain pending.
- [x] Build a controlled source fixture with moving frame IDs and deterministic audio, and validate captured samples automatically.
- [x] Report failures and source exit without enabling microphone recording. Automated captures use owned fixtures; normal capture waits for the user's explicit selection/start.
- [ ] Verify the usable local capture/audio path with Counter-Strike 2 and Rocket League when those environments are available, recording game/build/mode and behavior. Maintain the no-injection/no-hooks/no-game-memory boundary during all implementation work. Repeat relevant checks after integration/packaging changes; do not defer the architecture requirement until the final readiness stage.

Exit: selected window/display capture and both audio modes work locally; automated source-exit, resize, audio-isolation, and cleanup tests pass on available hardware.

## 2. Share and receive locally

- [x] Start an isolated local SFU fixture on free loopback TCP/UDP 8765 for bounded native media tests. Reuse it for the integrated app tests; production service setup remains pending.
- [ ] Publish both video qualities and one associated screen-audio track.
- [ ] Receive, decode, render, and play another client's share.
- [ ] Implement quality changes, keyframe recovery, per-share mute/volume, and reliable source stop/restart.
- [ ] Add a private-room token service for later remote use; avoid embedding server signing secrets in the desktop binary.
- [ ] Automate multi-process publisher/viewer tests and collect decoded-frame/audio results rather than relying on packet counts.

Exit: a local end-to-end test passes at both qualities with audio, bounded latency, and no playback feedback. If local SFU hosting needs an unavailable runtime, first assess supported local alternatives and document the gap. Use a temporary Ubuntu fixture only when necessary to unblock a specific integration test; do not silently install a new VM/container platform or treat that fixture as final deployment. Keep native capture, rendering, audio, and routine testing on the PC.

## 3. Minimal usable UI

- [ ] Implement the room grid, join/share/stop controls, sharing status, and source/audio selection dialog.
- [ ] Add Auto/1080p/720p receive quality, per-share audio controls, enlarge/fullscreen, keyboard navigation, and display-scaling support.
- [ ] Surface connection loss, unavailable capture, missing hardware capability, and incompatible audio routes clearly.
- [ ] Add a local diagnostics/export view with redacted logs and actual capture/encode/decode/render frame rates.

Exit: the app can be operated without entering codec/SDK details in normal use. Automation can join, share a controlled fixture, change quality, and stop cleanly.

## 4. Concurrent load and recovery

- [ ] Exercise three logical shares and the equivalent of 3 then 10 viewers, separating server forwarding tests from Windows decode/render tests.
- [ ] Test a client sharing while watching two other shares with audio.
- [ ] Run latency, drift, sustained-load, reconnect, and device/source-change tests from [the automated test plan](04-automated-test-plan.md).
- [ ] Verify resource cleanup and meaningful errors when hardware encoder sessions are unavailable; never silently represent a reduced rate as 60 FPS.
- [ ] Compare source/game FPS with and without sharing where a repeatable workload is available.

Exit: measured results meet agreed targets on available machines, with clear unsupported cases and no unrelated applications or services stopped.

## 5. Windows packaging and local release readiness

- [ ] Produce a versioned Windows package with the correct runtime dependencies and notices.
- [ ] Smoke-test outside the build directory and verify no developer machine paths or signing secrets are required.
- [ ] Verify share shutdown, room disconnect, app shutdown, and local test-fixture cleanup.
- [ ] Complete local headset/speaker listening and recheck game capture/coexistence with the packaged build, retaining the earlier development-time checks; keep unavailable environments explicitly pending.
- [ ] Document supported Windows/GPU combinations, measured local performance, audio limitations, setup, and diagnostics.
- [ ] Review local capture, both qualities, both audio modes, simultaneous sharing/viewing, sync, recovery, and soak results before moving to final deployment.

Exit: the packaged client is locally usable and tested. Remaining issues are explicitly remote-environment checks or require another person's machine, rather than unresolved basic PC functionality.

## 6. Final Ubuntu port-8765 deployment and Internet test

- [ ] Recheck service listeners and resource headroom on the saved Ubuntu SSH target without disturbing its existing services.
- [ ] Prepare the dedicated service/proxy configuration and exact TCP/UDP bindings for port 8765.
- [ ] Resolve the hostname/certificate and private-group authentication inputs required for a public TLS endpoint.
- [ ] Deploy only this project's isolated components, with rollback and health checks.
- [ ] Validate signaling, UDP media, certificate validation, and actual Internet upload/download from the Windows client.
- [ ] Repeat simultaneous-sharing tests through the VM, keeping Minecraft and other services running.

Exit: remote tests pass through the actual public path, the packaged client passes a remote smoke test, and results from available friends' machines are recorded. Loopback measurements alone do not complete this milestone. Restrictive-network TCP/TURN support is a separate decision if additional listening ports are needed. Update the release documentation with remote findings after these checks.

Voice chat is not a milestone in this roadmap. Retain only the architecture boundary needed to consider it in the far future.
