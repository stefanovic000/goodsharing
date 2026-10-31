# Keeping a future audio window available for PCM

Date: 2026-10-22, Europe/Budapest. This increment fixes a deterministic premature-silence bug and records what happens during an actual blocked viewer. It does not establish the exact cause of every historical audio-coverage failure, including the preceding 78% run. Normal UI audio and GUI quality-handover integration remain disabled/unconnected respectively.

## Reproduction and scheduling change

After a permitted picture and successful PCM commit, the decoded queue can briefly empty while the endpoint requests its next, still-future window. Previously, the scheduler could commit a whole silent packet immediately. A block arriving later but still before that window's original deadline could then lose its sample positions. The existing shortening logic protected a request crossing a known PCM tail, but not a window starting at or beyond the end of all known PCM.

The new native control reproduces this without hardware: after playing the first block, the endpoint requests the next block ten milliseconds before its deadline; that block arrives two milliseconds later, still eight milliseconds before it is due. The retained [before-change result](../artifacts/pcm-arrival-before.log) fails because the endpoint already committed silence. The corrected path cancels that reservation and waits; when the block arrives, its exact samples are committed at the original deadline without reanchoring or flushing.

Waiting is enabled only for coordinated playback after a successful media commit has established alignment. If no PCM is known for the start of the reserved future window, the scheduler waits while more than four milliseconds remain before its first audible sample. At the four-millisecond write guard it renders silence if content is still missing. It never moves that window or the common timeline later. Native tests verify the missing-block fallback as well as successful timely arrival. The non-coordinated endpoint policy is unchanged.

Waiting does not bypass presentation-age, final permission, endpoint or clock checks. It does not acknowledge a held image as a fresh picture. The six-block PCM ring, 60 ms residence, 80 ms A/V admission and 30 ms maximum endpoint lead remain unchanged. Expired queued PCM is removed before waiting, so waiting cannot extend its lifetime. Superseded PCM remains subject to the existing late-sample drop rules. A new diagnostic counter adds eight bytes: the core is now 11,936 bytes, with no extra PCM or video queue.

## Observing an actual viewer stall

The controlled paint-stall test now keeps five metadata-only observations: immediately before the 220 ms GUI block, immediately after, then 100, 500 and 2,000 ms into recovery. Each includes current presentation/media timestamps, reset serial, successful device-reset count, PCM and picture counts. No intermediate screenshot or synchronous file write is introduced. A GUI heartbeat now runs for this case as well as the static/checkpoint tests. These observations are bounded and exist only in the controlled smoke-test mode.

The initial diagnostic confirms a second issue in the old test: an endpoint-gap reset can flush/fence playback before the presentation-age guard runs. That run records a 236 ms GUI gap, the same 124 queued presentations before/after the block, serial/device resets increasing from six to nine, and the last media deadline stopping well before the end of the block. Fresh pictures and PCM resume afterward. It still fails the old specific-counter assertion; that failed result is retained, not reclassified as a passing historical run.

The revised check requires the actual effects instead of one reset-reason counter: a measured GUI block of at least 200 ms on the same ready watch; live aligned media beforehand; a new timeline serial and successful device flush; at most one already-pending presentation during the block; a stale final media deadline and a quiet tail in the blocked interval; and fresh pictures plus at least 90% media coverage over the two-second recovery interval. Existing overall audio-coverage, stereo, output-gap, bounded lag/queue, wrong-key and post-stop silence checks remain. Offline negative controls reject missing observations, absent GUI delay/flush, continued media, invented presentations and missing recovery. Native tests retain direct presentation-age expiry and fresh-presentation fencing coverage independently of endpoint recovery.

This is software/device-scheduling evidence, not acoustic speaker/display synchronization. A different valid reset path can satisfy the observable invariants; a counter increment alone can no longer satisfy them. A short bounded trace is retained in the diagnostic snapshots, but no complete historical root-cause reconstruction is claimed.

## Verification and next work

All 22 native tests pass after the change. Added cases cover empty/known PCM tails, timely arrival, unchanged sample ordering/reset count, the four-millisecond fallback, expiry while waiting, revocation while waiting and unchanged standalone behavior. The existing two-quality handover, final-admission/revoke races, render stalls and clock/queue controls also pass.

All ten current-build encrypted A/V cases pass, including moving/static 720p/1080p, motion resume, source closure, wrong keys and the richer stall effects check. Both wrong-key cases commit zero media and queue zero pictures. All five standalone output cases pass. The initial diagnostic uses the corrected scheduler but the original specific-counter assertion and remains a failed attempt; it is not counted as a final pass. Historical frozen artifacts remain unchanged.

| Current-build A/V case | Media-window coverage | Recent p95 estimated A/V lag |
| --- | ---: | ---: |
| Moving 720p | 100.00% | 54.26 ms |
| Moving 1080p | 99.96% | 55.97 ms |
| Static 720p | 100.00% | 55.59 ms |
| Static 1080p | 100.00% | 55.33 ms |
| Intentional GUI stall | 98.15% | 56.37 ms |

The stall run observes nine new PCM waits. Its measured GUI gap is 237 ms; queued presentation count stays at 126 through the block, while reset serial and successful device resets both increase from three to six. Only 1,440 further media frames are submitted during the block before output stops. After two seconds of recovery, presentation count reaches 233 and 94,636 additional media frames have been committed. The presentation-age-reset counter remains zero because endpoint recovery already fences the old timeline. These finite observations satisfy the new effects checks; they do not prove lossless long-running output or identify the exact cause of the historical 78% run.

All six normal video-only cases also pass, including its existing quality switch, for **22 native tests and 21 final integration passes**. The earlier diagnostic remains a separate failure under its original counter-specific check. The core and test changes preserve the existing E2EE admission implementation, transport/subscription ownership, native SDK, device code and video renderer.

The [validation record](../artifacts/20261022-pcm-arrival-validation.json), [source snapshot](../artifacts/20261022-pcm-arrival-source.zip) and [binary snapshot](../artifacts/20261022-pcm-arrival-binaries.zip) retain exact outcomes, the initial diagnostic and offline observation controls. Snapshot verification checks all new file/archive hashes and the preceding frozen evidence/archives. Final process auditing is read-only; each test stops only its own children and unrelated applications/services remain running.

Next connect the independently verified persistent-audio handover foundation to actual queued Qt presentation and measure complete switches. Before enabling normal UI audio, continue recovery/load, sparse-video, sync/drift, multi-share and device-loss validation. Historical 78% coverage, bandwidth collapse, connection and crypto anomalies are not declared resolved by fixing this independently reproduced race. VPS deployment and unrelated capture/cleanup are outside this increment.
