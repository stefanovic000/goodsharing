# 30 and 60 fps

Date: 2026-10-30, Europe/Budapest

Status: **30 fps healthy; 60 fps publishes and presents correctly, but the paint path on a loaded PC caps what reaches the screen**

The share popup offers a 30/60 rate. Three things were changed so the two rates
behave as rates rather than as a bandwidth accident.

## What changed

1. **The bitrate follows the rate.** 60 fps needs a bigger budget for the same
   text sharpness, so the 1080p budget becomes 1.4x (10 -> 14 Mbps) and the 720p
   budget 5 -> 7 Mbps. 30 fps keeps the tuned reference numbers, and
   `GOODSHARING_TEST_SHARE_BITRATE` still overrides both.
2. **Presentation floor 10 ms -> 4 ms.** A 60 fps cadence is 16.7 ms; a 10 ms
   minimum spacing dropped every picture whose deadline bunched with the previous
   one. Measured on a 60 fps share: `presentation_catchup_drops` fell from **104
   to 1** and the queue stopped sitting at its high-water mark.
3. **A guarded 60 -> 30 sender fallback.** At 60 fps, if more than 20 % of frames
   miss the handoff age budget across a 2 s window, the publisher halves its input
   cadence; it restores 60 fps after a window with under 5 % late frames. Reported
   as `input_stride` / `fps_fallbacks`.

## Measurements

Both rates, local owned SFU, 18 s watch, 720p:

| Rate | Sent (capture admitted) | Presented | Catch-up drops | Longest audio quiet |
| --- | ---: | ---: | ---: | ---: |
| 30 fps | ~560 | 596-610 | 1 | 40.0-58.8 ms |
| 60 fps | 1102 | 447-458 | 1 | 663 ms (loaded PC) |

Reading: at 60 fps the sender really does hand off 60 fps and the viewer's
scheduler now keeps up (one catch-up drop), but only about a quarter of the
offered pictures reach the screen because the CPU paint path
(`QQuickPaintedItem`, 1280x720 scaled into the tile) spikes past a 16.7 ms frame
under load (`maximum_paint_ms` 18 ms). The sender-side fallback does not help
here because the frames die in the viewer, not at the encoder.

The 30 fps audio number is the known 40-50 ms endpoint flake escalating to 58.8 ms
while unrelated applications held the CPU around 70 %; the same build measured
40.0 ms and passed coverage on a quieter machine.

## Live counters (the report used to be frozen)

publicationReport_ was only refreshed on state transitions, so every counter in
a finished run (encoded, handed off, keyframes, fallbacks) was the first snapshot -
which is why 60 fps could not be diagnosed from a report. The controller now
re-emits the publication report every second while a share runs.

With live counters a 60 fps share shows captured=889, encoded=1754, handed=1754
over five seconds: both tracks encode and hand off at 60/s with no replacements,
no rate limiting and no stale frames. The shortfall is on the receiving side, where
about 25 of the 60 pictures per second are presented while verage_paint_ms stays
near 2 ms - the remaining work is on the viewer's presentation path, not the
publisher.

## Receiver accounting with a pinned rate

The harness pinned the profile and volume but not the share rate, and the app had
`shareFps=60` saved from an earlier session - so several "30 fps" runs were really
60 fps. `--share-fps 30|60` now pins it for every run, and the receiver reports
`submitted_frames` and `pending_drops` next to admitted/drawn.

Pinned measurements, 720p, 18 s watch:

| Rate | Sent | Submitted (SDK) | Admitted | Drawn | Unclocked | Catch-up | Quiet |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 30 fps | 573 | 509 | 356 | 351 | 153 | 0 | 0.0 ms |
| 60 fps | 1137 | 1004 | 699 | 680 | 305 | 3 | 50.0 ms |

Both rates lose the same ~30 % of delivered pictures, and `invalid_frames`,
`denied_frames_or_draws` and `pending_drops` are all zero or near zero: the loss is
`unclocked_frames` - pictures for which the presentation scheduler
(`RemoteAudioPlayout::scheduleVideo`) cannot issue a ticket, which the receiver
discards instead of presenting.

## The unclocked fallback (implemented)

Pending now carries an untimed flag and submit synthesises an arrival-paced
ticket when the scheduler has none, instead of discarding the picture:
draw/changed treat those entries as valid and shownUntimed_ keeps the shown
picture from being wiped by the ticket check.

Measured, 720p, 18 s, rule rate pinned:

| Rate | Sent | Submitted | Admitted | Drawn | Quiet |
| --- | ---: | ---: | ---: | ---: | ---: |
| 30 fps | 568 | 513 | 513 | 508 (~28 fps) | 5.7 ms |
| 60 fps | 1144 | 1003 | 1003 | 968 (~54 fps) | 3.3 ms |

Both cases pass; admitted now equals submitted, so no delivered picture is
discarded when the audio anchor is missing.

## Earlier design note

RemoteVideo::submit drops a picture when 	iming_.schedule returns nothing -
there is no ticket, so no deadline, so it is counted as unclocked_ and thrown
away. The fix is to present those pictures at arrival time instead:

- in the !ticket branch, take the same admission and copy steps as the untimed
  path and publish the image directly, marking selectedRevision_ so the draw
  path sees it as the current picture and does not fall back to the previous one;
- keep counting them separately (unclocked_) so a rise is still visible in the
  report;
- verify that a scheduled picture in the queue is never displaced by an untimed
  one more than one frame, and re-run 30 and 60 fps with --share-fps pinned.

That was deliberately not squeezed into this pass: the change touches the
scheduled-presentation state machine (shownTicket_, selectedRevision_,
drawnRevision_), and a partial edit there would silently regress the rate that
works today.

## Next

A viewer-driven fallback: when presentation sustains less than ~80 % of the
offered rate for a few seconds, ask the sender (or the track subscription) to drop
to 30 fps. That keeps 60 fps where the machine can render it and degrades to a
steady 30 elsewhere, instead of publishing 60 into a paint path that cannot use
it.

## Endpoint starvation hold

A starved endpoint cycle now repeats the last committed block instead of reading
silence: commit writes through a shared writeReserved, the sink keeps the last
block and offers 
epeatLast() (virtual on AudioPlayoutSink, default false), and
the pump holds for at most one window with a cap of three holds per second. The cap
matters: an uncapped hold repeated 83 windows in an 18 s 1080p60 run, which pushed
the endpoint's non-media share to 0.906 and failed the fill and coverage gates.

With the cap, 1080p60 passed: quiet 32.02 ms, fill 0.98, coverage 0.976, 24 holds.
720p runs oscillate with machine load on the same build (quiet 0.02 ms when idle,
1602 ms under unrelated CPU load), so the remaining quiet failures are load, not
the hold.

## Decoupling the per-frame callbacks

udio(), ideo(), alid() and presented() used to take the session mutex,
so 60 lazy per-second video callbacks competed with the pump and its 5 ms
reservation lease. They now read an atomic playout_ pointer instead. Effect at
60 fps: starvation holds 20 -> 4 -> 0, quiet 672 -> 42 -> 0.02 ms, coverage
0.903 -> 1.000. The capped hold stays as a safety net (27 engagements at 30 fps
without harming coverage).

## The harness records system CPU

Gate failures used to flap with whatever else ran on the machine, so each case now
samples system CPU through GetSystemTimes (no third-party package) and stores
system_cpu: {average_percent, peak_percent, samples} in the summary.

Measured with that context:

| Case | Result | Quiet | Coverage | Drawn | CPU avg / peak |
| --- | --- | ---: | ---: | ---: | --- |
| 720p @ 30 | pass | 5.71 ms | 0.969 | 511 | 61.6 % / 74.3 % |
| 720p @ 60 | pass | 0.35 ms | 0.972 | 1019 | 67.5 % / 86.5 % |
| 1080p @ 60 | pass | 15.54 ms | 1.000 | 906 | 77.9 % / 93.2 % |
| 1080p @ 30 | fail | 70.02 ms | 0.961 | 505 | 68.3 % / 85.0 % |
