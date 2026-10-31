# Persistent audio through GUI quality changes

Date: 2026-10-22, Europe/Budapest. This connects the separate audio/video transport and scheduler foundation to the controlled Qt A/V viewer. Normal launches still receive video only; sound remains behind `--smoke-watch --test-av-playback` while stability and synchronization acceptance continue. Everything in this increment runs locally on the PC.

## What changes during a switch

The viewer keeps one audio subscription, one audio session and one WASAPI worker. Choosing the other quality stages a separate, immutable video watch for the same room, owner, share and generation. The old quality continues playing while the candidate establishes its independent RTP/RTCP clock and encryption permission. Candidate timestamps project onto the existing common timeline; they do not create a new audio timeline.

The candidate becomes selected only when its due picture has actually been painted and Qt reports the frame queued for presentation. This is software presentation evidence, not measured display scanout. The previous video watch is then revoked, its image storage discarded and its reader/subscription retired on the transport worker. Returning to the earlier quality requires a fresh watch; late callbacks cannot reopen the retired one. Cancellation, candidate failure and source closure have explicit controlled test paths. A request can time out after ten seconds while keeping the existing quality when its permission remains valid.

The shared final presentation gate checks the fixed audio watch and selected video, plus candidate video for a candidate output operation. Each watch retains its room, identity, SID, generation and verified encryption checks. A closed or failed scheduler closes the shared gate even if image storage outlives the scheduler. No plaintext fallback is added. Normal video-only viewing keeps its previous path.

## Contention and packet boundaries

A deterministic native test found that the previous exclusive presentation lease allowed a slow authorized paint operation to block the independent audio device commit. The retained `quality-av-permission-before.log` fails that control. Read admission now uses shared leases, allowing authorized audio and video output concurrently; revocation poisons admission before waiting for already-admitted output to finish. Mutations remain exclusive. Native tests still check revocation during preparation, device commit and painting, including each required media role. Private image cloning occurs before the final lease; revocation during copying prevents the queue move.

A second deterministic control found that rejecting a whole device packet because its final samples exceeded the 80 ms A/V horizon also discarded its permitted prefix. The retained `quality-av-prefix-before.log` records that failure. Coordinated output now commits only the prefix already inside the same horizon. A window whose first sample is already too far ahead still waits. The final post-render limit and device reservation checks remain. The test verifies all 433 permitted frames and the exact remaining 479 PCM frames after a fresh picture, without a reset or invented samples.

Adding this control exposed a test executable stack overflow (`0xc00000fd`), recorded separately from a product failure. Its many independent scheduler fixtures now allocate on the heap. The full native suite passes after that repair; none of the product checks was relaxed.

## Buffer the successor before displaying the candidate

Initial hardware attempts still produced audible gaps after adoption despite preserving audio identity. Retiring the old video took microseconds for revocation/unwatching and a few milliseconds to discard images in the instrumented runs; that evidence did not support an approximately 100 ms GUI block at those calls.

One defect in the warming path was independently visible: it retained the first clocked candidate picture but discarded all successors until that picture became due. With a future presentation deadline, that creates a hole immediately after the first new picture. The viewer now ends the single-image warming phase as soon as a valid clocked picture exists, and buffers its successors in the existing six-slot scheduled queue while the old quality remains visible. It attaches the candidate only when due. Native coverage requires a future successor to survive this interval, then queue on its original deadline without resetting audio. The first focused hardware run changes the candidate queue high-water marks from one to three/four and records zero warming drops at adoption.

This adds no PCM queue and no additional video queue. Before usable clock metadata arrives, a candidate retains at most one warming image. Once primed, it uses the existing bounded queue for its short transition; retired/cancelled image storage is explicitly released. The core remains 11,936 bytes, the PCM ring/residence limits remain 60 ms, and the endpoint first-sample and A/V bounds remain 30 ms and 80 ms. These limits do not prove low footprint under three simultaneous sharers; that remains a separate load test.

## Validation and limits

The switch probe measures 720p → 1080p → 720p, cancellation during warmup and source closure during warmup. Reports retain bounded observations and exact audio SID/session/worker identity. Own-process loopback observes stereo output throughout each round trip/cancellation and its post-adoption interval; these intervals require at least 95% observation coverage, no more than 5% quiet samples and no quiet run longer than 40 ms. Source closure instead requires revoked roles and no candidate presentation; silence after closure is expected. Normal switches must preserve clock serial and device-reset counts both at adoption and through the later observations. No repeated acknowledgement of held pixels is accepted as fresh video.

Offline negative controls reject missing intervals/samples, a gap above 40 ms, a changed audio session, a second output worker, a post-commit clock/device reset, missing counters, an unqueued candidate, excessive warming images and disagreeing commit counters. These verify the assertions, not physical lip sync or encrypted transport by themselves.

Earlier attempts remain failures under their original checks, including one 980 ms quiet interval and a subsequent clock realignment loop. A successful later finite test does not erase those results or establish that every historical clock, coverage, congestion or crypto anomaly has the same cause. Physical speaker/display synchronization, long-duration drift, sparse/no-new-frame video, device changes, recovery/load, multiple simultaneous shares and whole-app security acceptance remain open. Anti-cheat testing still requires actual representative games; the app continues using ordinary Windows capture and loopback APIs without game injection, hooks, drivers or game-memory access. No VPS deployment or unrelated process cleanup is part of this increment.

All 22 native tests pass. The final local matrix has **27 passes out of 28 cases**:

| Suite | Passed / attempted |
| --- | ---: |
| Encrypted A/V, including all three handover paths | 12 / 13 |
| Normal video-only viewing | 6 / 6 |
| Standalone WASAPI output | 5 / 5 |
| Managed transport, including independent media watches | 4 / 4 |

The matrix's static-to-motion case fails several real output/continuity checks. Output stops after approximately 4.3 seconds of committed media; the sender ends with a reported 30 kbps target for each quality. A later timing-enabled repeat passes without that collapse. The failure remains recorded; its approximately 99.96% first-to-last-media-window metric only covers the interval that actually played and must not be read as whole-test continuity. The checkpoint, recent stereo and final pixel assertions correctly reject the run.

Five additional current-build cases have **four passes and one failure**: the repeated round trip and warmup source closure pass; cancellation loses readiness before staging any candidate, so the probe correctly fails instead of skipping the exercise. It has zero switch observations, 112 presented pictures, 84,575 committed media frames, serial 967 and repeated startup-alignment rejection. Its sender bitrate remains around 2.6 Mbps per quality, so the static case's bitrate collapse does not explain this failure. Both the subsequent timing-enabled cancellation diagnostic and the static recovery diagnostic pass. This is intermittent baseline clock/recovery evidence, not proof of a cancellation-specific defect or a resolved recovery problem. Across the matrix and these follow-ups, **31 of 33 cases pass**. Earlier diagnostic attempts are preserved separately and are not counted as final passes.

Three post-buffering round trips pass, including the focused pre-final-build check and two current-build runs. Their two switch intervals have longest observed quiet runs of 0.021/4.458 ms, 0.021/3.833 ms and 0.146/0.021 ms respectively. In the final matrix's round trip, the audio session/SID and single worker remain unchanged; reset serial and successful device reset count remain three throughout all five live observations. These are finite own-process loopback results, not a guarantee of gap-free long sessions.

The same matrix records resource use. During the round-trip middle window, the viewer plus test-only audio observer averages about 0.99 CPU cores (8.2% of this 12-logical-processor PC), with a 214.7 MiB private-memory peak. The two-quality capture sender averages 2.26 cores (18.9%) with a 287.4 MiB private-memory peak. This sender uses CPU conversion in this run. These are combined local test measurements with temporary dual decoding during switching, not an accepted three-sharer footprint or a before/after performance improvement. Further CPU/memory work remains necessary.

The [validation record](../artifacts/20261022-gui-handover-validation.json), [source snapshot](../artifacts/20261022-gui-handover-source.zip) and [binary snapshot](../artifacts/20261022-gui-handover-binaries.zip) preserve the build, passes and failures. The snapshot verifier checks current file hashes, both current archives and the preceding PCM snapshot's evidence/archives without rewriting historical results. Final auditing finds no executable running from this workspace and stops nothing. Tests clean up only their own children. Normal UI audio stays disabled; next investigate the intermittent baseline clock/alignment and bitrate recovery failures before longer sync, load and device-change acceptance.
