# Actual capture publication in the Windows app

Date: 2026-10-21, Europe/Budapest. This increment connects the app's selected screen/window and audio source to the reusable encrypted publisher. It follows [audio timing/sync diagnostics](24-audio-content-timing-and-sync.md). No VPS deployment, game interaction or unrelated application shutdown is involved.

## User-visible change

The Qt Quick app now provides room join/leave, a connection status, Share/Stop and the existing local preview. The development room form accepts an address, access token and mandatory 32-byte client-held key; secret fields are masked and cleared after submission. This is a development connection form, not the production invitation/rekey protocol.

The final badge reports preparing/stopping separately and requires actual capture plus encryption readiness before saying "ENCRYPTED SHARE". Review caught an earlier badge using the broader active-session flag during preparation; media was already blocked, but that label was premature. The final smoke cases assert the preparation badge immediately after Share and the encrypted badge only during ready capture. The [pre-badge snapshot](../artifacts/20261021-capture-input-validation.json) remains preserved with its own sources/binaries and does not validate this UI correction.

Share publishes the selected source as paired 1920x1080 and 1280x720 H.264 tracks, with application audio, desktop audio or audio off. Local preview remains available separately. Controls distinguish preview from an active encrypted share; the source/audio selection is fixed while sharing. Mumble remains the voice client.

The normal app still has no remote-share tiles, remote rendering or audio playback. An independent native test viewer exercises receiving. This is a working app **publisher**, not the completed two-app sharing demo or a release.

## Capture adapter and timing

`CapturePublisherInput` is a one-generation adapter around WGC and WASAPI. It starts capture only after all required publisher handlers are ready. It never primes handlers with captured content. Conversion and audio processing run off the Qt and transport owners. Stop closes publication permission before stopping/joining the capture workers; callbacks cannot revive a closed input. Capture failure stops both media paths and fails the generation without selecting another source.

There is one replaceable raw image awaiting conversion, plus at most one in conversion. The existing publisher raw mailbox, encoded reference-chain protection, age checks and mandatory per-track E2EE remain in use. Audio packets are split/assembled into 480-frame, 48 kHz stereo blocks with a maximum partial block below 10 ms. Timestamp discontinuities discard a partial old block rather than concatenate it with unrelated new samples. Invalid formats/timestamps, excessive packet size or an unrecoverable handoff rejection terminate the share. The existing ten-block audio input queue remains bounded.

Both WGC compositor timestamps and WASAPI sample timestamps use the Windows QPC clock. The adapter maps that clock into the publisher's steady-clock microseconds once per capture session; it retains the capture timestamp rather than substituting conversion-completion time. Audio split blocks advance by sample duration. See Microsoft's [WGC timestamp definition](https://learn.microsoft.com/en-us/uwp/api/windows.graphics.capture.direct3d11captureframe.systemrelativetime) and [WASAPI timestamp units](https://learn.microsoft.com/en-us/windows/win32/api/audioclient/nf-audioclient-iaudiocaptureclient-getbuffer). These same-PC times are not clocks that can be compared across different PCs. Preserving input timestamps also does not implement a shared receiver playback clock.

## Conversion and footprint

The correctness-first path uses CPU images. It bounds conversion to at most a 1920x1080 source canvas, converts BGRA to I420 once through the pinned SDK, scales its planes and interleaves chroma into the two exact NV12 outputs. Even-aligned letterboxing/pillarboxing preserves aspect ratio within pixel rounding. Transparent content is composited over black; this is an SDR path, not HDR/color-management acceptance. GPU readback still copies to CPU, and hardware encoding downstream does not make conversion zero-copy.

The initial implementation converted two full RGB output canvases separately. Its [first actual-capture run](../artifacts/20261021-213141-capture-sharing/summary.json) passed pixels, selected-app audio isolation, Stop and cleanup, but received only 44.01 FPS at 720p. Conversion work totaled 10.30 seconds across 456 admitted pairs. That functional pass is retained as a performance shortfall.

Shared source conversion and bounded conversion pacing subsequently reached 59.84 inspected FPS with 37.10 ms capture-to-decoded p95 at 720p in [the intermediate run](../artifacts/20261021-213609-capture-sharing/summary.json). Its 1080p audio-off run reached 58.34 FPS and 62.04 ms p95. Neither short sample is a five-minute 60 FPS acceptance result. The sampled GUI sender still used about 1.92 / 2.21 logical-core equivalents and approximately 284 / 283 MiB peak private memory in the reported middle windows. These figures include capture, two encoders, transport, local preview and Qt UI, but exclude the independent viewer and SFU. They establish remaining footprint work rather than a low-CPU claim.

The final path additionally limits the owned WGC readback worker to a 60 FPS schedule **before** texture pulling/copying. WGC's frame-arrival callback only signals an event; waiting is confined to the owned reader. This avoids reading every 165/240 Hz desktop update. Lower/irregular source rates remain valid. Existing preview callers keep their previous uncapped behavior unless they request a cap. Final measurements and exact runtime/source hashes are retained in the frozen evidence below; they do not establish an Internet, three-share or game-performance budget.

## Automated validation

`scripts/test_capture_sharing.py` starts only its own local SFU, moving source window, unwanted-tone process, GoodSharing GUI and independent viewer. It refuses an occupied service port. Test tokens and media keys are passed through child environments, not command-line arguments or server media-key configuration. It captures only its owned source, saves only fixture media/screenshots, and cleans up only those child processes.

The normal GoodSharing publisher is invoked through the same backend entry points as the GUI controls. The independent viewer uses authenticated owner/share/generation grouping and verified receiving. It checks decoded dimensions, publication metadata and two complementary visible marker rows, counts distinct captured source IDs separately from transport IDs, and measures compositor-capture-to-decoded-callback latency on this PC. The final fixture emits 440 Hz left / 880 Hz right; both wanted tones must arrive with cross-channel leakage below 3%. A separate unwanted process emits 660 Hz and must also stay below the 3% limit. Earlier intermediate fixtures used the same 440 Hz tone in both channels and 880 Hz in the unwanted process; those earlier isolation checks alone were not distinct-channel stereo proof.

The controlled source window is configured at **960x540**, scaled into the exact output canvases. Thus the 1080p case verifies a 1080p output track, not native-1920x1080 capture throughput or additional source detail. Native full-screen/high-resolution capture performance remains a separate gate.

Final cases cover application audio at selected 720p, audio-off at selected 1080p, wrong-key viewing, and source-process exit. Wrong-key viewing must admit zero video frames and zero audio sample frames. Audio-off must admit no audio blocks. Source exit is an expected terminal failure: the smoke app's exit code 3 is accepted only with the explicit source-closed error, a closed failed publisher, prior correctly received media and completed cleanup. Ordinary Stop must finish with a stopped, closed generation. These assertions do not claim that all previously authorized in-flight media can be recalled.

The native suite adds color/UV-order checks, portrait/ultrawide/odd-dimension letterboxing, transparent pixels over black, invalid-image rejection and refusal to start capture before encryption readiness. The complementary marker row changes only the controlled source fixture; existing preview checks still use its original top marker row.

Run locally:

```powershell
.\scripts\build.ps1 -SkipConfigure
python scripts/test_capture_sharing.py --case all --profile-resources
```

Desktop audio selection is wired to the existing process-excluding loopback implementation, but this increment's end-to-end audio tests use the owned application to avoid recording arbitrary desktop sound. Controlled distinct-channel stereo and application exclusion pass; desktop self-playback exclusion and actual speaker/display synchronization remain integration gates. The independent viewer inspects PCM; it does not play received audio or render a remote Qt tile.

## Evidence and remaining work

[Frozen source/runtime/results](../artifacts/20261021-capture-publication-validation.json) preserve the first functional/performance shortfall, the conversion-only intermediate, final readback-limited cases, native checks, preview regressions and owned-process cleanup. No earlier long-run or congestion pass is assigned to the new app. The LiveKit wrapper/engine and crypto patches are unchanged.

All four [final captured-media cases](../artifacts/20261021-215233-capture-sharing/summary.json), thirteen [native checks](../artifacts/capture-input-readback-native-run.log), and the [normal](../artifacts/20261021-215347-preview-normal/summary.json), [restart](../artifacts/20261021-215354-preview-restart/summary.json) and [source-exit](../artifacts/20261021-215401-preview-source-exit/summary.json) preview checks pass within their scopes. The final app is `cd6f22c3ad9842de03995df1b2d891c8346ead71fb12eebd2ce4bddca8d14872`; its SDK engine remains `7de2c8fa388825b73cf1e6999b8c32f2cab56c64cc4075261e65be180a1ebc43`.

| Final selected receive path | Inspected frames | Inspected FPS | Capture-to-decoded p95 | Audio result |
| --- | --- | --- | --- | --- |
| Application audio, 720p | 606 | 58.75 | 61.29 ms | 507,360 sample frames; distinct L/R and exclusion pass. |
| Audio off, 1080p | 613 | 58.76 | 68.89 ms | Zero sample frames and zero admitted capture blocks. |
| Wrong key, 720p | 0 | N/A | N/A | Zero admitted sample frames; measured wanted tones zero. |
| Source exits, 720p | 290 | 59.25 before exit | 50.74 ms | Prior stereo passes; source closure fails/closes the publisher. |

All inspected marker/metadata/dimension checks pass. The final normal 720p capture reads 626 frames during the short sharing interval, versus 1,721 desktop reads in the conversion-only intermediate. The native 60-FPS readback schedule reduces redundant capture work, but these are individual runs, not a controlled statistical CPU comparison.

The final normal GUI sender's sampled middle window uses 2.00 core equivalents (16.69% of this 12-thread PC) for the stereo/720p receive case, and 1.97 cores (16.41%) for audio-off/1080p. The sender always publishes **both** output qualities; the independent viewer selects one. Sender private peaks are 286.25 / 285.48 MiB, working-set peaks 166.56 / 167.47 MiB. The viewer and SFU are excluded from those sender numbers. This remains too costly to call the low-footprint goal complete; GPU conversion/readback/rendering and sustained budgets still need work. The source-exit middle window contains idle time after the source disappears and is not an active-sharing CPU figure.

Next connect a managed remote share to the normal Qt window and bounded stereo playback with final permission checks and a common presentation clock. Then exercise actual flash/click A/V sync, quality changes, resize/minimize/static sources, restart/device loss, sustained FPS/resources and concurrent shares. Existing congestion/audio matching failures and whole-app E2EE release gates remain open. No automatic 1080p fallback, production invitation/rekey protocol, anti-cheat compatibility certification or VPS deployment is claimed.
