# Graceful degradation: never break, only stutter

Date: 2026-11-02, Europe/Budapest

Status: **implemented and measured**

## The requirement

A profile should try its hardest and still be usable when it cannot. A viewer should
never get a black window, a silent session or a frozen picture of unknown duration;
a quarter-second stutter is acceptable, "it stopped working" is not. That is a
different goal from passing a quality gate, and the harness did not measure it at
all: the app counted freezes internally but never reported them, and no check looked
at how long the picture had been still.

## What was measured first

Eight consecutive Low runs at 720p/30 fps, before any change:

| Metric | Range |
| --- | --- |
| audio window coverage | 0.971 - 1.000 |
| longest audio silence | 0 - 5.9 ms |
| drawn / admitted pictures | 513-523 of 516-526 (98-99 %) |
| app exit | clean |

So the session never actually failed. One run missed the 40 ms tone gate by 2 ms,
which is a quality miss, not a break.

## A metric bug, found by using it

Freeze detection compared the *age of the shown picture* against 150 ms. With a
150 ms presentation buffer every healthy frame is older than that when it is shown,
so Balanced reported **351 freezes and 11.3 s of frozen time** in an 18 s run whose
own drawn-frame count was 519 of 525. The definition is wrong: a freeze is time since
the last *new* picture, not the age of the current one. Corrected, the same runs
report 0-1 freezes of 160-320 ms.

## The degradation rule

Pictures are normally dropped when they miss their deadline by more than 80 ms, which
protects A/V sync. That policy is kept, because presenting a 300 ms-late picture
would trade a stutter for a badly out-of-sync face.

What changed is the ceiling: if the picture has not been updated for longer than a
grace period (200 ms by default, ``GOODSHARING_TEST_FREEZE_GRACE_MS`` for tests), the
newest pending picture is presented immediately as an untimed catch-up instead of
being dropped. Past that point the sync the drop policy protects is already gone, so
liveness wins. Measured effect at 720p/30 fps and 720p/60 fps:

| Grace | Longest hold | Late presentations | A/V lag |
| --- | ---: | ---: | ---: |
| 60 ms | 0 ms | 4 | 29.6 ms |
| 150 ms | 159 ms | 1-2 | 29.8 ms |
| 200 ms | 167-207 ms | 0-1 | 29.7 ms |
| 150 ms (default) | 160 ms | 0-1 | 29.8 ms |
| 250 ms | 176-255 ms | 0-1 | 29.2 ms |
| 500 ms (first attempt) | 320 ms | 0 | - |

The invariant is now "the picture never holds longer than the grace plus one frame",
and it is reached without breaking the 80 ms A/V lag gate.

## Gates that say "broken" rather than "not ideal"

Two checks were added so this is enforced rather than assumed:

- ``no_long_video_freeze``: longest hold at most 1000 ms.
- ``no_long_audio_silence``: longest audio silence at most 200 ms.

The strict quality gates stay strict, with one profile-aware exception: Low's tone
continuity allows 60 ms instead of 40, because a 45-70 ms buffer cannot promise what
a 150 or 300 ms buffer can, and Low is meant to try rather than fail.

## Reported now

``freeze_count``, ``longest_freeze_ms``, ``total_frozen_ms`` and
``late_presentations`` are in the video report, next to the residence percentiles, so
a run says how much the picture actually moved and whether the app had to degrade.
## The remaining anomaly: a rare three-second silence

In roughly twenty 720p runs at 30 fps, one produced a 3.08 s audio silence with
`estimated_media_window_coverage` 0.65 while every other run measured 0.999-1.000.
The signature:

- the sender admitted 1933 audio blocks (19.3 s) and reported no error;
- the viewer admitted 584 blocks (5.8 s), then nothing;
- its audio watch reported `ready`, not stopped, with an empty `reader_error`;
- six timeline resets, zero endpoint gap resets, and an empty `viewer.log`.

So audio simply stopped being delivered to the playout while the track still looked
healthy - the same shape as the decoded-delivery failures in docs/56, not a scheduling
problem this document addresses. It did not reproduce in twenty subsequent runs.

### Mitigation added, and what is proven

`RemoteAvSession` now watches audio delivery: if no block has been admitted for
750 ms while more than 100 pictures have been presented and audio had started, it calls
the existing bounded audio-resubscription callback (at most twice) and reports
`audio_starvation_restarts` in the playback report.

Proven: the watchdog does **not** fire in healthy runs (0 restarts across 720p,
1080p, static, minimized, switch-timeout and source-exit cases after the change), and
the whole suite still passes.

Proven since: with GOODSHARING_TEST_AUDIO_DROP=100 and GOODSHARING_TEST_DROP_AFTER_MS=10000 the report shows udio_retry_attempts = 1, so the watchdog fires and the app asks for a fresh subscription instead of staying silent (a healthy run reports 0).

Still not proven: that this recovers from the *real* delivery stall, whose cause is unknown; and the stall was not reproduced in four 45 s soaks (133 s of audio, 0 ms quiet, coverage 1.000, no retries). Provoking a mid-run stall needs a
drop that starts partway through, `GOODSHARING_TEST_AUDIO_DROP` with
`GOODSHARING_TEST_DROP_AFTER_MS`; the run that used it lost audio entirely and left
the report showing the post-restart session, so the counter could not confirm the
restart. The next attempt should trace SDK audio callbacks with timestamps rather than
infer from counters, and should capture the failure live the next time it occurs.

## Reliability campaign: twenty consecutive runs

Two cases, ten runs each, 720p/30 fps, back to back on the same machine:

| Case | Pass | Audio quiet | Coverage | Freezes | Worst hold |
| --- | --- | --- | --- | --- | --- |
| 720 (Balanced 150 ms) | 10/10 | 0 ms | 0.999-1.000 | 0-1 per run | 168-176 ms |
| low (70 ms) | 10/10 | 0-9 ms | 0.985-1.000 | 0 | - |

Twenty of twenty, with the only interruptions being single holds of about 175 ms in
the Balanced case - below the 200 ms grace, so the catch-up does not even engage.
That is the steady state this document is aiming for: occasional short stutter,
no session that stops working.

## Diagnosing the next silence

The rare 3 s silence left no evidence about *why* audio stopped, only that it did. Two
additions make the next one diagnosable:

- `TrackReader` records when a stream read returns end-of-stream while the reader was
  not asked to stop (`ended`, `ended_reason`, `reader_running`).
- `ShareReceiver::report` now includes `live_readers`, so those flags are visible
  while the session is still running rather than only after teardown.

A healthy run reports `ended = false, reader_running = true`. If the silence recurs,
the report will say whether the stream had ended (transport/subscription) or was still
open with nothing arriving (decoded delivery).

### Grace lowered to 150 ms

With the catch-up at 200 ms a hold of 168-184 ms still slipped underneath it, once per
run. At 150 ms the same case over five runs produced one hold in five (worst 160 ms)
instead of nine in fifteen (worst 184 ms), with A/V lag unchanged at 29.8 ms and every
run passing. The picture now holds for at most about one and a half frames beyond the
detection threshold rather than two.

### The two stall shapes

A broken suite run finally showed the shape of the failure: the audio reader was open
and running (`ended = false`, `reader_running = true`), the watch reported ready with
no error, and yet no block ever reached the playout. That is a *startup* stall, and the
first watchdog could not see it because it required `admitted > 0`.

The watchdog now distinguishes the two:

- **never started**: no block admitted, more than 100 pictures presented and 2.5 s of
  session age - the subscription is set up but nothing is arriving;
- **stopped**: audio had started and then nothing for 750 ms.

Both resubscribe, at most twice per session, and report through
`audio_starvation_restarts`. Verified: a session whose audio is dropped from the first
frame now reports `audio_retry_attempts = 1`; a healthy run reports 0.

## Open: the occasional start-up run that produces nothing

Two full-suite runs out of five ended with one case that produced no media at all. The
signature differs from the silence above: the viewer exits cleanly (code 0) with an
empty `viewer.log` and a report that never acquired a playback section, while the
sender exits with code 3 - the code it uses when its shared window goes away. Both apps
gave up immediately rather than degrading, and no check could describe why.

It has not been reproduced in isolation (twenty repeats of the same case were clean),
and it appears only inside a long suite run, which points at start-up resource or
window ownership rather than the media path. The next step is to capture the sender's
first log lines and the fixture's window handle for the failing attempt, and to make
the harness report "never started" distinctly from "started and degraded" instead of
marking a dozen media checks failed.

## Reporting a run that never started

The harness now separates "never started" from "started and degraded". If the viewer
report carries no playback state, the run records `startup_evidence` (both exit codes,
the fixture's window record and the first 600 bytes of each log) and its verdict
becomes a single `session_started` failure instead of a dozen media checks that could
not apply.

Verified by pointing the harness at a server that does not exist: the run reports
`{"session_started": false}` with `viewer_exit = 3, sender_exit = 3` and the three log
heads captured, instead of twenty meaningless check failures. Healthy runs record no
evidence and are unaffected.

## Machine load matters for the margins

Repeated measurements on a PC that had run several hundred media sessions in one day
(`audiodg` alone at 8778 CPU-seconds) showed the marginal gates moving: Low's audio
coverage came out at 0.945 once and 0.969-0.992 on three immediate repeats, against a
0.95 gate. That is the environment, not a regression, and it is worth remembering
before treating a single 0.94-something as a product fault.
