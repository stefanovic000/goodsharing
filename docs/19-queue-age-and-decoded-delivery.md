# SDK queue age and decoded delivery

This increment adds a 100 ms local age check at the SDK pass-through encoder and at the remote decoded-image sink. The sender check uses enqueue time from the same local clock, preserving access-unit ordering and fresh-keyframe recovery. The receiver keeps normal decoding/reference completion intact and withholds only old decoded images before Rust/FFI conversion. Local source/preview sinks bypass the receiver filter. Missing remote packet timestamps are rejected and counted. No image queue or worker is added.

The video pacer requests a 100 ms maximum expected queue delay instead of its default two seconds. This is a desired draining target, not a hard expiry: bandwidth and congestion can still prevent prompt transmission. Native parser diagnostics verify the actual pinned binary accepts this setting.

The receiver age covers earliest locally received RTP packet through decoded-image delivery entry. It cannot detect the full time a packet spent waiting upstream, cannot measure another PC's raw monotonic timestamp, and does not include subsequent conversion/rendering. It is not a capture-to-display ceiling. Decoding prediction dependencies still costs CPU even when the resulting image is withheld.

## Rejected experiments

The earlier pre-decode H.264 filter is removed: it coupled age rejection to decoder errors and keyframe recovery, and its constrained-upload tests failed continuous delivery. The shallow-pressure run [20261021-043028-network-media](../artifacts/20261021-043028-network-media/summary.json) had 3.0/6.3 second gaps; the deep run [20261021-043051-network-media](../artifacts/20261021-043051-network-media/summary.json) also failed and encountered a UDP teardown reset. Their reports and original runtime hashes remain preserved. Its inactive header is archived at `artifacts/rejected-age-bounded-decoder.h`; it is no longer included in the production patch. A passing isolated 200 ms decoder-stall test did not establish continuous pressured playback.

The preceding sender-age-only engine also failed both restricted upload cases despite encoder-entry age staying near one millisecond. These are intermediate results, not reassigned to the final engine.

## Tests

The network runner now requires delivered video to reach the observation tail and have no inter-frame gap above two seconds, alongside the unchanged 150 ms p95 age target. A few fresh frames can no longer hide a long freeze. This is a test acceptance condition, not a runtime FPS cutoff. The intentional stalls remain synthetic diagnostic seams, bounded to 250 ms and requiring the test telemetry flag.

Only test-owned local UDP traffic is restricted. No Windows global network rule, unrelated application, game, SSH connection or VPS service is modified. Winsock 10054 reset events from closed owned UDP endpoints are counted separately; other socket errors still fail the relay. Missing packets are not counted as delivered data.

## Final runtime measurements

The engine SHA-256 is `599c43e1937d5c4a98acdda379ee6582351d14babb2a7ed0ef1824b3c7765d66`; the reproducible Rust patch is `8f0a73d33f570375cebd3ae0e4adcc98fe6bf8b4e16a70e7fb0c151d5e6da98b`. The publisher executable remains `b27b5b1a69affdb99c9dec98963c1c5dd20a1ef34f2106c25faa7eaff12a38aa`, and the wrapper is unchanged. Every run records its actual loaded-file lineage in `runtime-info.json`; executable equality alone does not establish the same engine.

| Synthetic local scenario | 1080p | 720p | Outcome |
| --- | --- | --- | --- |
| [Clean relay](../artifacts/20261021-044239-network-media/summary.json), decoded p95 | 29.27 ms | 24.97 ms | Pass, with 1,042/1,043 inspected live frames, stereo and independently decoded source recordings. |
| [SDK input stall, 200 ms](../artifacts/20261021-044319-network-media/summary.json), longest frame gap | 233 ms | 234 ms | Pass. Each encoder rejects one 201 ms-old input; maximum forwarded input age is at most 2 ms. |
| [Decoded delivery stall, 200 ms](../artifacts/20261021-044346-network-media/summary.json), longest frame gap | 217 ms | 215 ms | Pass. Each sink withholds seven old images, sees age up to 202 ms and forwards only ages at most 84 ms. |
| [50 ms FIFO, restricted upload](../artifacts/20261021-044432-network-media/summary.json), pressure p95 | 43 ms, only **2** frames | 2,972 ms, only **7** frames | **Fail**. Longest gaps are 5,994/2,346 ms. |
| Same shallow case, recovery p95 | 78 ms | 55 ms | These few recovered frames do not erase the failed continuous-delivery gate. First fresh frame after restoration takes 2,037/816 ms. |
| [1,000 ms FIFO, restricted upload](../artifacts/20261021-044455-network-media/summary.json), pressure p95 | 905 ms | 960 ms | **Fail**. Longest gaps are 2,543/1,992 ms; 1080p does not reach the observation tail. |
| Same deep case, recovery p95 | 77 ms, **5** frames | 100 ms, **20** frames | First fresh frame takes 1,478/564 ms. Brief recovery does not prove sustained playback. |

Both final restrictions last four seconds at 60% of the preceding measured traffic (about 633/613 kbps), with verified sender and receiver UDP routes and measured rate enforcement. They preserve inspected pixel/metadata/dimension correctness, stereo tone identification, independent decoding, zero remaining participants and owned-server cleanup. They have no relay infrastructure error. The tests require no missing remote packet timestamps; all delivered receiver-local stage ages stay below 100 ms, while **total source-to-decoded age still exceeds seconds** in some cases. This directly demonstrates the limit of local stage checks. Network loss, encoded dependency waiting, sender/SFU queues and upstream packet age still need broader recovery work; these results do not isolate a single cause.

The two stall checks submit 1,801 audio blocks each with a 10 ms sender mailbox high-water mark. That verifies independent input draining under those bounded stalls. Tone identification and a bounded sender mailbox do not prove pressure-time receiver audio continuity, A/V synchronization or a total audio latency bound.

There is no controlled before/after performance comparison here: restriction rates derive from each run's observed traffic and feedback histories differ. Removing the pre-decode error gate avoids intentionally damaging decoder reference readiness, but does not resolve the already-open transport recovery gate. No sustained 60 FPS, hardware-decode, three-share capacity, CPU/RAM improvement or leak-free claim is made from these short runs.

## Regression and provenance checks

[All eight publisher cases](../artifacts/20261021-044537-share-publisher/summary.json) pass with independent source decoding, including wrong-key rejection, cancellation, rollback and revocation. [The 200 ms encoded-output stall](../artifacts/20261021-044719-share-publisher/summary.json) rejects one stale frame per quality and completes fresh-keyframe recovery with no open delivery gate. The independent audio worker submits 502 blocks with 10 ms mailbox high-water. [The 250 ms owner-stall test](../artifacts/20261021-044728-share-publisher/summary.json) also passes independent decoding and resource sampling over ten seconds. Intentional stalls are not sustained-60-FPS acceptance tests.

[Managed receiving](../artifacts/20261021-044800-remote-share/summary.json) passes normal and wrong-key scenarios, variable cadence, generation/quality selection and cleanup. [Room control](../artifacts/20261021-044817-room-controller/summary.json) passes eight cycles, cancellation and owned publication restart. Joined idle uses 0.013% of 12 logical processors over about ten seconds and handles the next command in 1.28 ms; owned publisher Stop peaks at 209.24 ms. These short tests do not resolve historical lifecycle-growth failures.

The [owner-stall resource trace](../artifacts/20261021-044728-share-publisher/stereo-footprint.json) includes synthetic generation, hardware encoding, software decoding of **both** qualities, content inspection and in-memory recordings. Sampled lifetime CPU averages 11.36% of 12 logical processors / 1.363 core equivalents; peak working set is 146.14 MiB and peak private committed memory is 235.30 MiB. This is an instrumented fault workload, not a steady-state app budget or evidence of a controlled improvement. The new age filters add scalar counters and scan existing packet metadata; they allocate no picture queue or worker. The test does not prove three-share capacity or hardware decoding.

The [six native tests](../artifacts/decoded-age-ctest.log) pass, including variable cadence, SDK age boundaries, stale keyframe rejection, encoded sequence gaps and fresh-keyframe recovery. The [owned relay self-check](../artifacts/20261021-043930-relay-self-check.json) passes flow isolation, rate and storage bounds. The source-level Rust shutdown test was not rerun for this final engine; its build manifest records that fact. Final runtime lifecycle tests are separate evidence. [Provenance verification](../artifacts/decoded-age-provenance-check.json) confirms recipe idempotence, a pristine decoder factory, removal of the inactive decoder header, dependency-lock agreement and matching installed/runtime SDK hashes. Ninja reports no pending work. The [final process audit](../artifacts/decoded-age-final-process-check.json) records cleanup without stopping unrelated processes.

## Remaining work

The [frozen validation and source archive](../artifacts/20261021-decoded-age-validation.json) preserve this final engine separately from intermediate attempts. **Upload-pressure latency/continuous recovery and step 2 remain open.** Consider reference-safe transport expiry/recovery and mapped/authenticated capture timing rather than treating another PC's monotonic clock as synchronized. Protect and measure receiver audio through each pressure phase. Implement the planned last-resort 1080p pause/restoration hysteresis only with evidence and tests showing sustained insufficiency; it is not present in this increment.

Actual WGC/WASAPI publication, rendering/playback UI, remote rekey/reconnect and integrated resource/capacity work still follow. The UI remains a local preview. Earlier intermittent corruption and resource-growth failures retain their failed status. No historical long-duration success is reassigned to the new DLL, and no anti-cheat acceptance or public Internet result is claimed.
