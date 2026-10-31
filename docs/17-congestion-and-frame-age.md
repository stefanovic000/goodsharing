# Sender frame age and congestion diagnostics

Date: 2026-10-21, Europe/Budapest. Sender-local recovery and the existing bounded regressions pass. **The upload-pressure gate fails** and step 2 remains open. The UI still provides local capture preview only. All media in these checks is synthetic; no SSH or VPS work was performed.

## Implemented sender behavior

The coordinated publisher now checks capture age before encoding and immediately before synchronous SDK handoff starts. Its sender-local budget is 150 ms. An obsolete raw frame can be discarded before it creates H.264 references. If an already encoded frame becomes obsolete, delivery closes until a fresh keyframe is accepted: dependent frames cannot silently resume the discarded reference chain. Failure to produce a keyframe after 32 fresh delta outputs fails explicitly. Encoder pending identities remain bounded at 16; pending output older than the local budget fails explicitly after polling.

This budget ends at **SDK call entry**. The call's duration is measured, not forcibly interrupted. It does not bound later SDK queues, network, receiver jitter buffers, decoding or presentation. Source timestamps use the sender's monotonic clock; no synchronized clocks across PCs are assumed.

Positive WebRTC bitrate feedback updates the hardware encoder first. Its configured minimum is 100,000 bps, but WebRTC requested as little as 30,000 bps during pressure. Diagnostics now separate the requested rate and actual clamped setting. A constant-size byte-debt controller limits raw admission using measured encoded access-unit sizes plus an estimated packet/envelope allowance. It skips raw frames before encoding when necessary, without another video queue. Debt drains with the requested rate; idle time accumulates no burst credit. This is average-rate backpressure with an unknown next-frame/pipeline burst, **not** an exact wire-rate or packet-age limit. Zero-rate feedback is still ignored; suspension semantics need separate validation.

Large bitrate cuts request a keyframe with a 500 ms cooldown. Capture-time keyframe requests also prevent a low admitted frame rate from stretching the normal 60-frame GOP over many seconds. Admission may fall below 60 FPS under pressure; 60 remains the normal source target. Planned last-resort 1080p pause/restoration hysteresis is not implemented. The independent audio worker is unchanged. No new media storage, polling worker or SDK binary patch was added for this policy.

## Test isolation

`test_network_media.py` starts only its own SFU and client. Signaling and the SFU backend bind to 127.0.0.1:8765. An owned UDP relay binds an existing host-local IPv4 address on port 8765; the SFU advertises that address and filters backend candidates to loopback. The relay admits locally bindable client source IPs, limits flows/storage, and cleans up its own sockets/thread. No firewall, adapter, route, driver or system traffic rule changes. All media stays on this PC.

The runner verifies both clients' final selected UDP candidate and every sampled sender candidate against the relay endpoint. It measures bytes actually forwarded under the cap. Transport stats are sampled asynchronously every 500 ms; bounded decoded-frame timing is enabled only for this diagnostic. Normal publisher runs omit that overhead.

Pressure lasts four seconds at 60% of the preceding three-second measured upstream rate. The shared FIFO uplink has a **one-second maximum queue age**, 2 MiB byte bound and packet-count bound; downstream is unrestricted. This exercises bufferbloat. Clients cannot remove packets already waiting in that simulated router; shallow-buffer and loss scenarios remain needed. The existing 150 ms decoded-latency and two-second fresh-recovery targets are not weakened for this scenario. Latency runs from synthetic input creation to decoded callback on one PC, excluding capture/rendering. Audio latency and playback gaps are not measured.

The relay's [known UDP workload](../artifacts/20261021-030223-relay-self-check.json) passes flow isolation, measured rate, byte/age bounds, exercised drops and restoration. The earlier [passing self-check](../artifacts/20261021-024558-relay-self-check.json) is preserved.

## Final-build evidence

Publisher executable SHA-256: `6e7943d3bf5ef929d4a90d282fc4be57a01bd0f405cd3577d89f7f82f754e3bb`. The C++ wrapper, Rust engine and both SDK patches are unchanged from the managed-receiving snapshot. [Validation and source hashes](../artifacts/20261021-congestion-validation.json) distinguish passing regressions from the failed pressure gate.

- [Six native tests](../artifacts/raw-budget-ctest.log) pass, including variable cadence, exact age boundary, reference isolation, keyframes expiring while pacing, bounded failed recovery, byte-rate admission and idle burst prevention.
- [All eight publisher cases](../artifacts/20261021-025905-share-publisher/summary.json) pass with independent decoding of recorded positive outputs. Readiness, wrong-key rejection, audio-off, startup cancellation, rollback and revocation retain their checks.
- [A 200 ms encoded-output stall](../artifacts/20261021-025956-share-publisher/summary.json) passes. The 1080p output drops one stale encoded frame and completes one keyframe recovery; both outputs finish with zero pending frames and no open recovery gate. Recorded outputs independently decode without pixel errors. Audio submits 502 ten-millisecond blocks over five seconds; mailbox high-water is 10 ms. Maximum SDK handoff-start ages are 26.22/26.27 ms. The earlier age-only build recovered both qualities in [its separate stall run](../artifacts/20261021-024620-share-publisher/summary.json); those results are not reassigned to this build.
- [A 250 ms video-owner stall](../artifacts/20261021-030006-share-publisher/summary.json) passes: 14 raw frames replaced, 502 audio blocks submitted, 10 ms audio high-water and correct independent recordings. Intentional-stall checks are not sustained-60-FPS benchmarks.
- [Managed receiving](../artifacts/20261021-030045-remote-share/summary.json) passes normal and wrong-key cases, including variable cadence, quality selection, generation replacement, cancellation and teardown. [Controller regression](../artifacts/20261021-030103-room-controller/summary.json) passes eight cycles and owned publisher checks. Joined-idle CPU averages 0.013% of 12 logical processors over about ten seconds; the next command completes in 1.14 ms. This short synthetic measurement is not an integrated-client footprint budget.
- [The final clean relay run](../artifacts/20261021-030150-network-media/summary.json) passes media, stereo, independent decoding and all 36 sampled routes. Decoded-callback p95 is **48.68 ms at 1080p / 43.45 ms at 720p**. Independent recordings inspect 1,080 content frames per quality without errors. Relay drops are zero, queue high-water is one packet, and maximum added queue age is 0.075 ms.

## Failed pressure gate

[The recorded final pressure run](../artifacts/20261021-030017-network-media/summary.json) verifies a **637,562 bps** cap for 4.03 seconds, all 36 sampled relay routes, executable consistency and complete owned cleanup. **It fails live pixel correctness and latency.** Pressure p95 is **1,012.90/957.03 ms** at 1080p/720p. For frames captured at least two seconds after restoration, p95 is **807.12/681.30 ms**. The 720p stream first meets the fresh-frame condition after 7.18 seconds; 1080p never meets it in the observation window. Relay maximum age is 999.83 ms with three expired packet drops. Stereo tone analysis passes; this does not prove pressure-time audio latency or continuity.

Independent decoding of the exact sender recordings inspects **438/583 correct content frames**, with no pending frames or pixel errors. Live receiver inspection has pixel errors while metadata/dimensions remain valid. This narrows the defect to transport/receive handling in this scenario; it does not identify a specific SDK, SFU, depacketizer or decoder cause. Fresh-keyframe/dependency recovery after late or omitted frames needs investigation. Dropping arbitrary encoded/RTP packets would not be a safe fix.

The pre-admission [valid pressure failure](../artifacts/20261021-024736-network-media/summary.json) showed 2.26/2.76-second pressure p95 and multi-second recovery. Cumulative sender packet-send delay reached hundreds of seconds across many packets. The first admission build's [separate failure](../artifacts/20261021-025807-network-media/summary.json) reduced those sender-queue symptoms but still failed correctness/recovery. These different attempts are not a controlled performance-improvement estimate or a completed network fix.

Preserved diagnostic attempts:

| Attempt | Interpretation |
| --- | --- |
| [023138](../artifacts/20261021-023138-network-media/summary.json) | Invalid path: ICE bypassed the relay over Radmin IPv6; zero relay packets. |
| [023321](../artifacts/20261021-023321-network-media/summary.json) | Join failed with a loopback-only advertised candidate; no media evidence. |
| [023801](../artifacts/20261021-023801-network-media/summary.json) | Valid host-local relay baseline before sender policy changes. |
| [023946](../artifacts/20261021-023946-network-media/summary.json) | **Invalid restriction measurement despite historical `passed: true`**: resetting the send deadline on an empty queue let isolated packets escape the cap. The relay was corrected and a measured rate check added. Old JSON is preserved and excluded from passing evidence. |

## Remaining work

Resolve late-frame/reference recovery and measure queue stages, including receiver jitter residence. Add shallow-buffer, loss, varying RTT and sustained-insufficiency cases; measure audio gaps and A/V alignment. Define last-resort 1080p pause/restoration thresholds from that evidence. The pressure gate stays failing until correctness, latency and recovery all pass.

Actual WGC/WASAPI publication, rendering/playback UI, three-share workload, verified hardware decoding, long resource soak, production keys/reconnects and game coexistence remain open. Earlier startup-pixel and resource-growth failures remain tracked in [the publisher report](14-share-publisher.md). No previous five-minute or soak result applies to this build. Existing applications/services were left running; no game injection, hooks, overlays or driver changes were introduced.
