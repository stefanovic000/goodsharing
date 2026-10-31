# Budget-aware refresh and fixed-cap diagnostics

Date: 2026-10-21, Europe/Budapest. Local synthetic media only. This follows [the parser repair](21-cryptor-parser-repair.md). Congestion recovery and intermittent startup decoding remain **open**, and the UI remains a local capture preview. No SSH/VPS deployment or unrelated application changes were made.

## Change

The old one-second refresh rule could repeatedly request an IDR immediately after raw admission paid off a large previous IDR. In the earlier shallow restriction trace, a roughly 7 KiB frame at the 30 kbps feedback floor took about two seconds of budget to drain. A new forced keyframe then prevented useful delta-frame cadence. This identifies an avoidable scheduling behavior, not the complete cause of the network freezes.

`PeriodicKeyframe` now estimates the last accepted keyframe's wire cost using the same conservative payload/fragmentation allowance as raw admission. Its optional refresh interval is the greater of one second and four times that frame's estimated upload time at the current feedback bitrate. A 7,000-byte payload estimates 7,576 wire bytes: at 30,000 bps, optional refresh spacing is about 8.08 seconds. Requests coalesce until a keyframe is accepted. Normal high-bandwidth refresh spacing remains one second.

This is an estimate for scheduled refreshes, **not a strict total keyframe/wire-rate bound**. The hardware encoder's GOP, receiver keyframe requests, bitrate-cut requests and explicit reference-chain recovery retain priority. The default capture/encoder rate remains 60 FPS, and sparse or variable-rate input remains valid. No worker or media queue is added; encryption and per-track admission checks are unchanged. Automatic 1080p pause is still unimplemented.

The output report includes dimensions and a periodic-request count. `test_network_media.py --mode pressure --cap-bps 650000` applies a repeatable explicit total upload cap; the original baseline-ratio mode remains available. Existing latency, pixel, independent-decode, encryption, route, cleanup and freeze gates are retained.

`scripts/analyze_network_media.py` reads an existing fixture and emits phase-specific counter deltas plus the sender feedback timeline. Unlike the earlier exploratory helper, it excludes sampling intervals crossing a cap/restore boundary. Weighted queue means are not p95 values or an exact latency-stage decomposition. Its audio concealment fraction uses the received-sample counter as denominator; it is not an audible-continuity score or source-to-playback latency measurement.

The synthetic receiver now records the total number of pixel-error frames, the last error's frame ordinal and timestamps for the first ten errors. The network runner enables its existing bounded screenshot capture for the first two mismatches. It records only the owned synthetic fixture.

## Verification and failures

[All twelve native checks](../artifacts/congestion-refresh-diagnostic-native-run.log) pass, including sparse-rate refresh cost, request coalescing, capacity restoration and normal refresh spacing. The SDK DLL is unchanged: `030f0247fb315fedd2c41097c3071f0a4690f45b6403dd319e9f40949bde4fe8`. The final diagnostic publisher is `60bef7671dc51de23f77c0f67c413c847a7576f1f7ee776b18b25c07faf59df1`.

The [earlier build's nine publisher cases](../artifacts/20261021-161121-share-publisher/summary.json) pass. Its subsequent [clean-network failure](../artifacts/20261021-161159-network-media/summary.json) has 1080p pixel mismatches despite zero relay packet drops and correct independent sender recordings. This failure stopped that batch. It is preserved with its original executable hash `d93072e43ea9a3a9e720fd74872ff9ee764e79fd0943c31b4e80076400973669`; it is not assigned to the final diagnostic executable.

With the final diagnostics, [eight of nine publisher cases pass](../artifacts/20261021-161513-share-publisher/summary.json). The `disable-encryption` scenario fails its whole-media gate because **43 early 1080p frames are corrupt**, beginning with its first inspected live frame and ending at frame 51. A [synthetic screenshot](../artifacts/20261021-161513-share-publisher/disable-encryption-recordings/pixel-mismatch-1080-0.png) shows damaged decoded content. The observed encryption-disable action still reaches Failed, closes the gate, rejects the old input, records zero video handoffs after revocation and refuses to resume after re-enabling the manager. Those passing stop assertions do not make the whole scenario pass. Actual in-flight audio/presentation revocation remains separately unproven.

The same final run's wrong-key case admits **zero live/preroll video frames, zero audio sample frames and zero verified tracks**. The native authentication checks remain passing. Startup pixel corruption is not evidence of plaintext leakage or an explained cryptor failure; its root cause is undetermined. Historical raw-audio anomalies and whole-app E2EE release validation remain open.

Two clean diagnostic runs were scheduled in advance; both pass, with independently decoded recordings. Their passing results do not resolve the intermittent failure:

| Final diagnostic run | 1080p | 720p | Result |
| --- | --- | --- | --- |
| [Clean A](../artifacts/20261021-161638-network-media/summary.json), decoded-callback p95 | 45.97 ms | 38.52 ms | Pass; 1,072 inspected frames each. |
| [Clean B](../artifacts/20261021-161704-network-media/summary.json), decoded-callback p95 | 48.07 ms | 38.06 ms | Pass; 1,064 / 1,065 inspected frames. |
| [650 kbps, 50 ms FIFO](../artifacts/20261021-161731-network-media/summary.json), pressure p95 | 304.33 ms, 4 frames | 65.01 ms, 3 frames | **Fail**; maximum gaps 4,826 / 2,723 ms. 720p maximum pressure age is 2,771 ms despite its small-sample p95. |
| Same shallow run, first fresh frame after restoration | 1,110 ms | 73 ms | Fresh recovery passes, but pressure latency/continuity fail. |
| [650 kbps, 1,000 ms FIFO](../artifacts/20261021-161755-network-media/summary.json), pressure p95 | 819.34 ms | 842.13 ms | **Fail**; maximum gaps 1,678 / 672 ms. |
| Same deep run, first fresh frame after restoration | 52 ms | 97 ms | Recovery/continuity pass; pressure latency fails. |

Both fixed-cap pressure fixtures verify the actual cap, sender/receiver relay routes, inspected media correctness, independent source decoding, sender/decoded local-stage age checks and owned cleanup. Both still fail the unchanged total gate. These are one-PC source-to-decoded-callback measurements, not Internet or display latency, a sustained 60 FPS benchmark or a controlled performance comparison against the old scheduler.

The [shallow phase analysis](../artifacts/20261021-161731-network-media/phase-analysis.json) has roughly 18/29 ms sender packet-queue weighted means at 1080p/720p during pressure; its 720p receiver-jitter mean is 780 ms over only five emitted frames. Audio jitter weighted mean is 122 ms during pressure and 146 ms during recovery, with a 0.173 pressure concealment-counter fraction. The [deep analysis](../artifacts/20261021-161755-network-media/phase-analysis.json) reports audio jitter means of 174 ms during pressure and 87 ms during recovery, with a 0.224 pressure concealment-counter fraction. Full-run stereo detection passes, but audio protection under congestion is **not verified**.

The [final read-only process audit](../artifacts/congestion-refresh-final-process-check.json) finds zero owned probes/SFUs and stops no process. Ninja reports no pending build work. [Frozen source/runtime/results](../artifacts/20261021-congestion-refresh-validation.json) preserve passing and failing reports separately. No five-minute or resource-soak result is assigned to this build.

## Next work

First isolate startup corruption using the new timing/images and an explicit trace from cryptor installation/readiness through the first authenticated keyframe to decoder delivery. Check the hypothesis that the decoder has already processed invalid startup references; do not declare it the cause or hide damaged frames in the test. A passing repeat is insufficient.

Then establish pressure-time audio continuity/age and reference recovery with the explicit cap. Define and test the last-resort 1080p pause only after sustained measured insufficiency, preserving audio/720p, exact encryption permissions, bounded pending work and fresh-keyframe restoration with headroom/hysteresis. Viewer quality selection alone does not save the sender's two-track upload. Production rendering/playback admission, actual capture publication, three-share capacity, hardware decode, long resource stability and game coexistence remain open.
