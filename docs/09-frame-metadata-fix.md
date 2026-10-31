# Video mismatch and encryption-startup fixes

Date: 2026-10-20, Europe/Budapest. Status: focused build, negative controls, and five-minute encrypted local stress verification passed. Full application and remote validation remain separate roadmap work.

## Metadata framing

The pinned LiveKit SDK appends frame metadata to H.264 access units. Its [trailer implementation](https://github.com/livekit/rust-sdks/blob/a6f93a16/webrtc-sys/src/packet_trailer.cpp) inverts each metadata byte with XOR `0xff` to avoid H.264 NAL start codes. Inverting arbitrary binary values does not provide that guarantee: input bytes `ff ff fe be` become `00 00 01 41`, which resembles a new H.264 slice.

A controlled timestamp containing that sequence reproduced a failure reliably. The Windows hardware encoder produced valid 1080p/720p access units: an independent Microsoft decoder verified every recorded marker. The old metadata framing nevertheless resulted in zero decoded video on both encrypted remote tracks. The problem therefore occurs after encoding for this reproduction, rather than in the synthetic source pixels.

[The new metadata codec](../src/frame_metadata_codec.cpp) places the original 64-bit capture timestamp, 32-bit frame ID, and a synthetic-preroll flag in a fixed 22-byte `GS02` envelope, using canonical Base64url over explicitly ordered binary fields. It uses the SDK's user-data feature and leaves the unsafe built-in binary timestamp/ID fields unset. Every envelope byte is ASCII below `0x80`; after the SDK's XOR, every byte is at least `0x80`. Such values cannot introduce an H.264 start code. The receiver validates the version, exact size, flags, and canonical encoding before restoring the original identity. Unsupported or invalid metadata fails verification; there is no automatic downgrade to the old format.

LiveKit's media encryption remains enabled and unchanged. This wrapper provides codec-safe framing for identity metadata. The original full pixel comparison, exact-resolution checks, and wrong-key requirements remain in place.

## Capture and encoded-output pacing

Metadata framing alone was insufficient: a [five-minute run with safe framing but the original pacing](../artifacts/20261020-015448-local-media/shared-key.json) still produced one 1080p and two 720p marker mismatches under a controlled CPU burst. No RTP packet loss was reported; decoder drops and keyframe requests did occur. That evidence is retained and is not counted as a successful fix.

The original producer caught up after scheduler stalls by submitting frames immediately, and the asynchronous hardware transform could hand several access units to the SDK in a burst. [FramePacer](../src/frame_pacer.cpp) now uses a Windows high-resolution waitable timer for capture and each encoded output. It preserves the nominal 60 FPS clock with limited jitter compensation, while spacing consecutive handoffs by at least 15 ms. It forwards every accepted encoded access unit in order; it does not discard predicted frames, replace the encoder with all-intra output, or exclude mismatched decoded content from verification. Raw input may still be rejected by a busy hardware encoder and is reported separately.

## Encryption startup

A later wrong-key check detected part of the transmitted tone, despite encrypted publication labels. Merely enabling the E2EE manager after publication was insufficient evidence that its frame cryptors were ready. The harness now feeds only synthetic black video and silence during startup, waits for all three local encryption-OK events, and requests fresh video keyframes before starting test content. A five-second readiness timeout fails closed. This startup protocol must also be applied when actual capture is connected to publishing.

Preroll frames carry a separate explicit flag and are counted separately. All subsequent decoded content frames still undergo the original exact pixel/metadata check for the full requested test duration. Wrong-key audio keeps the original five-second tone-amplitude limit of 40 and additionally requires every overlapping 100 ms window to remain below 200, preventing a brief tone from being hidden by averaging. PCM concealment/noise alone is not treated as decrypted audio. No real user audio or screen content is used by these checks.

The pinned C++ SDK's public `frameCryptors()` query failed with an empty/unsupported FFI request during investigation. It is not used as a readiness mechanism. The SDK remains responsible for media cryptography; no custom cipher was added.

## Verification

- CTest passed 100,032 identity round trips, including both preroll flags, zero/max values and the problematic timestamp, the byte-safety invariant, and rejection of malformed/truncated/version-mismatched envelopes. Its second test verifies pacing after a stall and invalid-rate/1 FPS boundaries.
- [The reproducible regression suite](../artifacts/20261020-022314-metadata-regression.json) passed all three cases: old framing fails the constructed byte pattern, new framing preserves every decoded marker for the same pattern, and deliberately sending one wrong frame ID per quality is detected as exactly one mismatch per track. Independent Microsoft decoding verified the hardware output in all three cases.
- [Eight-second mode checks](../artifacts/20261020-022323-local-media/summary.json) passed unencrypted video/audio, matching-key encrypted video/audio, and wrong-key rejection. Wrong-key video decoded zero content frames; the global/maximum-window tone amplitudes were 0.202/10.10, with three SDK decryption failures.
- Three additional fresh eight-second wrong-key connections all passed strict video/audio rejection: [run 1](../artifacts/20261020-022917-local-media/summary.json), [run 2](../artifacts/20261020-022926-local-media/summary.json), [run 3](../artifacts/20261020-022937-local-media/summary.json). Each temporary server was stopped by its owning runner.
- [Independent before-transport decoding](../artifacts/20261020-014109-local-media/summary.json) verified all hardware output in a five-minute investigation run. That run happened before the framing change and was also clean at the receiver, so a clean test alone is not treated as proof that a code fix caused the improvement.
- [Final five-minute stress run](../artifacts/20261020-022405-local-media/summary.json): safe framing, paced hardware output, rapidly changing markers, and four owned CPU-load threads active for a three-second burst. No extra live decoder or recording was enabled, preserving the workload that previously reproduced the intermittent failure. Full media, numerical rate, latency, audio, and encoded-handoff gates passed.

| Final encrypted stress measurement | 1080p | 720p |
| --- | --- | --- |
| Decoded-buffer average FPS | 59.81 | 59.96 |
| Content frames inspected | 17,943 | 17,988 |
| Correct pixel/metadata identities | 17,943 | 17,988 |
| Mismatches | 0 | 0 |
| Synthetic submission-to-decoded p95 | 47.79 ms | 51.65 ms |
| Hardware access units / SDK handoffs, including preroll | 18,003 / 18,003 | 18,002 / 18,002 |
| SDK decoder drops / RTP loss / NACK / PLI | 0 / 0 / 0 / 0 | 0 / 0 / 0 / 0 |

The first five seconds of received audio contained the intended 440 Hz tone at amplitude 3896 with no producer error. These are local synthetic submission/decoded-buffer measurements with one publisher and one subscriber room in one process, through a loopback SFU. Bounded receiver queues mean inspected-buffer counts differ from codec decode and offered-frame counts; zero mismatches applies to every inspected content frame, not a claim that every offered frame was presented. No actual screen-capture, display-presentation, Internet, or three-PC capacity claim follows from this test. Stereo receive remains unresolved.

The exact timestamp values and encoded access units from the earliest intermittent failures were not retained. The framing defect is proven; the internal SDK stage responsible for every historical load-related mismatch is not conclusively isolated. Optional recording and independent-decoder diagnostics preserve additional evidence if a failure recurs. Historical failing reports remain unchanged. A clean local run establishes its measured workload, not every future GPU/network workload.

## Repeatable checks

```powershell
.\scripts\build.ps1
ctest --test-dir build --output-on-failure
python .\scripts\test_metadata_regression.py
python .\scripts\test_local_media.py --seconds 15 --encryption all --encoder mf
python .\scripts\test_local_media.py --seconds 300 --encryption shared-key --encoder mf --marker stress --cpu-burst 4
```

Use Visual Studio's bundled `ctest.exe` if CTest is not on PATH. Native media checks require the actual Windows user desktop/audio session. The regression suite intentionally expects failure from its old-framing and wrong-ID child runs; the suite's own result passes only when those failures are detected and its fixed-framing case passes.

`--record-encoded` optionally saves only controlled synthetic access units and independently decodes them after the live run. `--local-decode` decodes immediately before RTP for stage isolation, at the cost of extra live CPU work. `--stall-ms`, `--burst-frames`, and `--cpu-burst` affect only test-owned producers/threads. `--metadata-codec legacy`, `--unsafe-timestamp`, and `--tamper-frame-id-at` are explicit diagnostic/negative-control options; the default is safe framing.

The runner now returns failure for a failed five-minute media/rate gate or failed independent decode. All testing remains local. No VM change, deployment, or unrelated process shutdown is part of this fix.
