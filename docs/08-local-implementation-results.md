# Local implementation and measured results

Date: 2026-10-20, Europe/Budapest. Status: native feasibility harness and local-preview app implemented; complete screen-sharing application and release readiness remain unfinished. No SSH connection, VPS deployment, reboot, or change to an existing service was made during this implementation session.

Follow-up: [mismatch fixes and verification](09-frame-metadata-fix.md) now records a passing five-minute encrypted stress run: zero mismatches across 17,943 inspected 1080p and 17,988 inspected 720p frames, at 59.81/59.96 decoded-buffer FPS. The historical failures below remain preserved.

## Implemented

- C++20/MSVC build with project-local Qt Quick 6.8.3 and LiveKit C++ 1.12.3. Native LiveKit server 1.13.10 runs on Windows for loopback integration checks.
- Windows Graphics Capture for a selected window or display, with bounded latest-frame handling, D3D11 readback, resize handling, cursor capture, and source-exit detection. No injection/hooks/game-memory access or custom drivers.
- WASAPI application-process-tree loopback and desktop loopback excluding this app's process tree. PCM capture is stereo 48 kHz. No microphone capture is implemented.
- Minimal Qt Quick preview UI with source/audio selection, start/stop, aspect-preserving preview, audio meter, explicit local-only status, and error handling. This UI does not yet join rooms, publish screens, or receive remote shares.
- Hardware-only Media Foundation H.264 adapter, used on this PC through `NVIDIA H.264 Encoder MFT`. It feeds LiveKit's pre-encoded input, processes bitrate/keyframe feedback, and publishes separate exact 1920×1080 and 1280×720 tracks. One associated audio track is published by the synthetic harness.
- SDK AES-GCM E2EE configuration with random client-held test keys, publication-type validation, decoded pixel/metadata checks, correct-key audio-tone validation, wrong-key video/audio rejection, explicit decryption-failure monitoring, bounded runs, and test-owned cleanup.

## Environment and dependency evidence

Windows 10 Pro 22H2 build 19045.6466; NVIDIA GeForce GTX 1070 with approximately 8 GB dedicated memory. MSVC 19.51.36248.0, Windows SDK 10.0.26100.0, Python 3.13.13. [Dependency pins](../dependencies.lock.json) record the tested versions and verified LiveKit release SHA-256 hashes. Qt was fetched through the pinned aqt installer. Full redistribution/license packaging review remains pending; a working local build is not a completed distributable package.

Both `GraphicsCaptureSession::IsSupported()` and runtime process-loopback activation succeeded. Hardware MFT enumeration found the NVIDIA encoder. The SDK's generic `Hardware` setting instead produced **OpenH264 software** in an earlier baseline test, so the application must not treat that preference as proof of hardware encoding. The external hardware path was validated by both successful MFT operation and actual received video. SDK video decode statistics report **FFmpeg software decoding**, not hardware decoding.

## Measured checks

| Check | Observed result | Evidence |
| --- | --- | --- |
| Native controlled window capture, 5 seconds | 960×540, 782 capture callbacks, 297 distinct moving frame IDs, no reported error. Capture callbacks can exceed unique source frames on this high-refresh display. | [Screen report](../artifacts/20261020-011039-screen/screen.json) |
| Application audio isolation, 5 seconds | Wanted 440 Hz amplitude 1589; unrelated 880 Hz amplitude 0.18. Passed. | [Application report](../artifacts/20261020-010718-audio/application.json) |
| Desktop audio exclusion, 5 seconds | External 440/880 Hz tones included at amplitudes 1575/1589; own receiver-process-tree 660 Hz tone at 14.48, below the 3% leakage threshold. Passed. | [Desktop report](../artifacts/20261020-010718-audio/desktop.json) |
| Qt Quick preview and controlled capture | App rendered its selected test window and saved a screenshot. Initial run captured 806 frames. | [Preview report](../artifacts/20261020-011905-preview/preview.json), [controlled screenshot](../artifacts/20261020-011905-preview/preview.png) |
| Preview stop/restart | Passed. First stop 6 ms; final stop 3 ms; restarted capture delivered 347 callbacks. | [Restart report](../artifacts/20261020-012359-preview-restart/preview.json) |
| Source exits while previewing | Initial test found a missing capture-close event. Added explicit window/process validation; rerun passed and cleared the preview. | [Passing source-exit report](../artifacts/20261020-012530-preview-source-exit/preview.json) |
| Correct-key encrypted media, 15 seconds | Both exact sizes decoded; pixel markers all matched. Approximately 59.97 / 59.68 decoded FPS; synthetic capture-to-decode p95 54.03 / 51.66 ms. Known audio tone received. | [Short encrypted report](../artifacts/20261020-011225-local-media/shared-key.json) |
| Wrong key, hardware video + audio, 5 seconds | Both video publications encrypted; zero decoded video frames. Audio tone absent (0.064 amplitude versus roughly 4000 when correct); three SDK decryption failures. Passed. WebRTC still emitted low-level concealment noise. | [Wrong-key report](../artifacts/20261020-011515-local-media/wrong-key.json) |
| Five-minute encrypted video + audio | 59.18 FPS at 1080p and 59.42 FPS at 720p; synthetic capture-to-decode p95 52.93 / 48.54 ms. Hardware encoder emitted 17,988 / 17,996 of 18,000 offered frames. Audio tone passed. **Full correctness failed:** 2 of 17,752 received 1080p pixel markers mismatched; 720p markers all matched. | [Five-minute report](../artifacts/20261020-011528-local-media/shared-key.json) |
| Rapid-changing marker stress, 60 seconds | Reproduced 2 marker mismatches at each size, clustered around source frames 1009–1017. Saved controlled decoded images for diagnosis. Rates 59.22 / 59.06 FPS. **Correctness failed.** | [Stress report](../artifacts/20261020-012237-local-media/shared-key.json) |

Rates above are decoded-buffer callback measurements on this PC, with one publisher and one subscriber connected through a loopback-only SFU. The synthetic media publisher/subscriber are independent SDK room instances in one native process, not three independent Windows PCs. Both qualities are subscribed simultaneously to verify them; the eventual UI should subscribe to one quality per logical share. Compilation and a controlled preview smoke test also ran during parts of the five-minute measurement, so it is not a fully isolated idle-machine benchmark.

The earlier five-minute summary coupled the rate gate to pixel correctness and therefore marks its gate false despite both numerical rates exceeding 59. The runner now reports numerical rate and complete media-correctness gates separately. Historical reports are preserved. No failing correctness result has been changed to a pass.

The latency measurements begin at synthetic-frame submission, not actual Windows screen capture. They do not include display presentation or the Internet. No audio/video alignment or long-term drift claim follows from these tests. The short unencrypted/encrypted comparison does not isolate encryption CPU overhead.

## Remaining gates and next work

1. Carry the verified metadata framing, pacing, and encryption-readiness behavior from the harness into actual app publication. [The focused fix report](09-frame-metadata-fix.md) records a passing full local stress gate and unchanged pixel thresholds. The exact internal cause of every historical load-related mismatch is not conclusively isolated; retain diagnostics and rerun the gate after integration changes.
2. Historical stereo gap: this original harness supplied stereo but received mono because the stock C++ receive options omitted channel-count selection. The subsequent patched SDK and separate distinct-channel/playback probe resolve that gap; see [step-1 results](12-step1-results.md). The old video harness retains its mono receive default.
3. Connect actual capture/scaling/audio to room publication and implement remote video rendering/audio playout, quality switching, private room access, reconnects, and observable encryption errors. The current preview UI and synthetic network harness are separate.
4. Implement and review client-held invitation keys, authenticated distribution, missing-key behavior, late joins, reconnects, and fresh-key membership changes. The random test shared secret is not a production group-key protocol. See [encryption design](07-encryption.md).
5. Complete real capture-to-receive tests, three-share workloads, source/device changes, sync/drift, resource/soak checks, game/headset acceptance, redistribution notices, packaging, and multi-PC testing. Windows hardware decoding is not proven by the current SDK path.
6. Only then perform the final Ubuntu port-8765 deployment and public Internet tests. Existing Oracle benchmark evidence remains historical and unchanged.

Historical harness shutdown logs reported a pending negotiation using an already closed peer connection. Those logs remain preserved. The later engine patch and bounded native/lifecycle regressions are recorded in [step-1 results](12-step1-results.md); SDK shutdown occurs after room/media object destruction. Broader reconnect/setup cancellation remains a later integration requirement.

Test runners bound their duration, retain failure evidence, and terminate only processes they start. No real user audio content was saved. Image diagnostics contain synthetic patterns or the app displaying its own controlled capture fixture.
