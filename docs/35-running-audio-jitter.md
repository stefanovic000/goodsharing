# Late audio without resetting an established presentation

Date: 2026-10-22, Europe/Budapest. This increment fixes two reproduced scheduling problems: one late decoded audio block could invalidate an established timeline, and a borderline future audio window could reset it before giving the next video presentation a chance to arrive. It does not claim to resolve all historical GUI pauses, clock changes, bandwidth collapses or ICE failures. Normal UI audio remains disabled.

## Reproduction and change

A deterministic fake-endpoint regression establishes a permitted video presentation and commits its matching audio. The next 10 ms audio block arrives 5 ms after its original deadline. Before this change, the scheduler's under-10-ms-headroom rule realigned the common clock and invalidated the original video ticket. The retained [before-change output](../artifacts/jitter-av-before.log) records that failure.

Startup alignment is still allowed within the existing 80 ms video-lead cap. After the first successful final-permission endpoint commit containing media, that timeline becomes established. An isolated late audio block then stays on its original timeline; the existing renderer trims late samples and keeps the still-valid video ticket. No extra PCM or video queue is added, and late samples are not delayed to make them fit. This limits the interruption; it does not reconstruct lost samples or promise inaudible recovery.

Three consecutive valid audio blocks with less than 10 ms of first-sample deadline headroom trigger a bounded timing reset. A block with at least 10 ms clears that consecutive count. This also covers blocks whose deadlines are still in the future but are consistently too close for the device write-ahead requirement; simply waiting until they are past due could otherwise produce repeated partial drops. The count is over 10 ms blocks, not a wall-clock guarantee: several decoded blocks can arrive in one burst. On reset, queued PCM is discarded and the device is flushed. Playback requires a new video clock/ticket and a new queued presentation before fresh audio can play. Existing clock-epoch, stale-video, endpoint-failure, permission and revoke paths retain their existing behavior. Any invalidated timeline loses its established state, allowing bounded startup alignment again.

The established flag is set only after `watch.presentAudio` permits the actual device commit and the sink reports success. Failed or denied writes cannot establish a timeline. The non-coordinated endpoint path never enables adaptive alignment and retains its previous behavior. New counters report low-headroom blocks during established playback and sustained low-headroom resets. The scheduler grows by 24 bytes, from 11,904 to 11,928 bytes, for its state and counters; PCM storage is unchanged.

## Waiting for a due video presentation

An intermediate real 1080p static run still failed its two-second audio-coverage check: 90.52% coverage during that interval, despite 98.39% whole-window coverage. It had a presentation reset but no sustained-low-headroom reset. That result remains a failure and does not establish the exact cause of every presentation pause.

A second deterministic regression independently reproduced an avoidable reset when the last sample of a future device window would be just over 80 ms ahead of the last queued picture, while another video ticket was due shortly. The old path rendered/consumed PCM and then reset the timeline. The corrected path checks the reserved window before rendering. If audio would be too far ahead, it cancels the reservation and waits without consuming PCM, changing the timeline or acknowledging old pixels as fresh. A newly queued permitted video frame can then release that window.

The existing 80 ms age limit on the last queued presentation still bounds the wait; an expired presentation flushes rather than waiting indefinitely. Device discontinuities and the 60 ms PCM residence/capacity also retain their existing behavior. Audio behind the permitted A/V range or invalid clock mappings still reset; the actual post-render admission check remains in place. Preflight uses the whole reserved window, so it can wait conservatively even when rendering would have shortened a packet. Separate counters now distinguish waiting for video, expired-presentation resets and invalid/out-of-range lag resets. They add 24 bytes to the adapter and no media queue.

## Verification and scope

The new native regressions reproduce both unnecessary resets. They then check exact remaining PCM after trimming, continuation without a fresh video ticket, sufficient headroom clearing a late run, recovery after three subsequent late blocks, cleared hardware data, rejection of an old ticket and successful fresh presentation/PCM. Separate controls verify reset after three future-dated blocks with only 5 ms headroom, future-window waiting without consuming PCM, subsequent delivery of the untouched samples after fresh video, and expiry without an extra audio commit. Extreme endpoint timestamps are rejected before packet-duration arithmetic and cannot commit audio. Existing native tests still cover encryption readiness, final-commit/revoke races, stale presentation, endpoint gaps/failures, queue limits and variable frame rates.

Real-PC tests use only the owned window and generated stereo tones through local E2EE transport, queued Qt presentation and WASAPI. Moving and static 720p/1080p, motion resume, wrong keys, source closure and intentional GUI-thread interruption remain separate cases. Standalone endpoint and normal video-only GUI checks are recorded separately. Original failures remain in the preceding frozen reports; passing this increment does not retroactively turn them into passes.

The final native suite passes all 21 tests. The final encrypted A/V matrix passes all ten cases on the reviewed binary. It observes **43 preflight waits for video**, no out-of-range A/V-lag resets, and one expired-presentation reset in the intentionally stalled GUI case. The final hardware matrix observes no low-headroom blocks or sustained-low-headroom resets; those new audio-lateness branches are verified by native fault controls, not claimed as hardware fault-injection coverage.

The five standalone endpoint cases and six normal video-only GUI cases also pass on the final binaries, for **21 native tests and 21 final local integration passes**. The endpoint cases retain mute, unverified admission, revocation during preparation and output-stall coverage. Normal UI receiving remains video-only, including its quality-switch and static-picture checks. Snapshot verification checks the new files and archives and the unchanged preceding evidence; cleanup leaves no project test executables running.

| Final case | Active software-window media coverage | Recent p95 estimated A/V lag |
| --- | ---: | ---: |
| Moving 720p | 100.00% | 56.95 ms |
| Moving 1080p | 98.96% | 56.51 ms |
| Static 720p | 100.00% | 53.07 ms |
| Static 1080p | 99.92% | 57.38 ms |
| Intentional GUI stall | 98.29% | 54.28 ms |

Both wrong-key cases commit zero media and queue zero pictures. These finite software measurements pass the existing checks; they do not prove gap-free long-running playback, physical sync or that every historical failure is resolved. The intermediate failing 1080p interval and all preliminary reports remain retained separately from the final build.

The [validation record](../artifacts/20261022-jitter-av-validation.json), [source snapshot](../artifacts/20261022-jitter-av-source.zip) and [binary snapshot](../artifacts/20261022-jitter-av-binaries.zip) retain exact outcomes and provenance. PCM capacity/residence stay at 60 ms, pending video remains six images plus the displayed image, estimated A/V admission remains 80 ms and the endpoint first-sample lead remains capped at 30 ms. RTCP timing is still infrastructure supplied and is not encryption permission or a physical speaker/display sync measurement.

Unresolved work includes other continuity failures, true sparse-video audio, continuous audio across quality changes, device loss, physical sync and drift, long-load/resource budgets, three simultaneous shares, game coexistence, invitations/rekey/reconnect and Internet/VPS acceptance. No VM deployment or unrelated capture/cleanup is part of this increment.
