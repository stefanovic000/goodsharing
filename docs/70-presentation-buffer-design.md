# Presentation buffer: design for real latency profiles

Status: **design; step 1 implemented** (see Progress). Five tuning attempts mapped
the caps (see `docs/70` history below); the remaining work is a model change, not a
parameter.

## Problem

Audio is scheduled *relative to pictures already on screen*. In
`RemoteAudioPlayout::pump` a block is committed only when its mapped sender time
sits inside `horizonUs_` of `presentedSender_`. A probe block cannot exist ahead of
the presented picture, so a lead larger than ~100 ms has no valid position:

- `bounded_common_video_lead` / `bounded_video_presentation_wait` invert,
- the wait path calls `resetTiming()`, which flushes video as well,
- the result, measured: at a 250 ms lead the client presents **no video and no
  audio** (all gates fail), at 55 ms everything passes.

Caps found and removed while investigating (each verified by the harness):

| Cap | Where | Now |
| --- | --- | --- |
| A/V horizon ±80 ms | `remote_audio_playout.cpp` | follows `leadUs_` |
| Anchor horizon 100 ms | `audio_playout.cpp` | follows `leadUs_` |
| Residency 60 ms | `audio_playout.h` | constant (blocks a >60 ms lead) |
| Staleness 150 ms | `remote_video.h` | constant (blocks a >150 ms lead) |
| Lead validation 200 ms | `audio_playout.cpp` (`configureTiming`) | throws `Invalid playout timing` — the hard reason every >200 ms attempt died instantly |
| No timeline before the first picture | `remote_audio_playout.cpp` (`pump`) | audio cannot start; video presentation waits on the audio anchor, so the two deadlock |
| **Frame queue depth 6** | `remote_video.h` (`pending_`) | **fixed: 24 makes video present at a 250 ms lead (173 frames vs 0)** |
| **Audio admission at large lead** | `AudioPlayout::admit`/`alignAudio_` | still open: with video presenting correctly, **every audio block is denied** (`queue_high_water_blocks = 0`, `first_audio_ms = 0`) |
| Presented-reference coupling | `pump()` vs `presentedSender_` | **not addressed** |

## Model

One session clock. Let `D` be the profile's presentation delay (e.g. 45 / 150 /
300 ms). Everything is scheduled on `senderClock + D`:

```
presentation(time) = sender(time) + D

video:  a frame captured at t is due at  t + D
audio:  a block captured at  t is due at  t + D
```

Consequences:

1. `presentedSender_` stops being "the last picture on screen at wall-clock now".
   It becomes the sender time of the **delayed timeline**:
   `presentedTimeline = now - D` (mapped through the established clock).
2. The A/V horizon is measured against that timeline, not against the paint
   moment, so a 300 ms lead has a valid position and no reset is triggered.
3. Video and audio are held back **together**, so sync is preserved by
   construction rather than corrected afterwards.

## Changes

**`stream_profile.h`** — replace `leadUs`/`videoWaitLimitUs` with a single
`bufferUs` per profile (45 / 150 / 300) and derive both from it
(`lead = buffer`, `wait = buffer + headroom`).

**`AudioPlayout`** — residency becomes `bufferUs + margin` (member, not constant);
queue capacity sized for the largest profile; expiry uses the same number.

**`RemoteVideo`** — staleness budget becomes `bufferUs + margin` (the fix that was
written and reverted); `Pending` depth must cover `bufferUs` at 60 fps.

**`RemoteAudioPlayout::pump`** — the anchor and `SourceWindow` use the delayed
timeline: `presentedTimeline = localFor(now) - bufferUs` instead of the last
painted sender time. This is the core change; everything else follows from it.

**Harness** — every gate expressed in milliseconds of allowed buffering
(`queue_high_water_blocks * 10 <= bufferUs + 20`), and the lead/wait gates take
`bufferUs + headroom`.

## Tests before it may be called done

1. Matrix: {Low, Balanced, Smooth} × {720p, 1080p} × {30, 60 fps} — all green.
2. A/V sync: `bounded_av_lag` under 80 ms in every cell (the buffer must not
   desynchronise the picture from the sound).
3. Latency: measure glass-to-glass for each profile; it must grow by roughly
   `bufferUs` and not more.
4. Loss case: replay a run with induced packet loss (`GOODSHARING_TEST_DROP` hook
   to be added) — Smooth must show no audio gap where Low is allowed to.

## Risks

- The delayed timeline touches the same code that keeps audio and video in sync;
  a mistake there produces drift, which is worse than a stall. Implement behind a
  profile value of 0 meaning "old behaviour" so the two paths can be A/B tested.
- `presentedSender_` is read from the paint path and the playout thread; the
  delayed form must be computed atomically, not by two threads subtracting from
  different clocks.

## Order of work

1. `bufferUs` in the profile + derived values (no behaviour change at 45 ms).
2. Delayed timeline in `pump` behind `bufferUs == 0` switch.
3. Residency, staleness, queue depth, gates.
4. Matrix, then latency measurement, then the loss case.
5. Only then raise Balanced to 150 and Smooth to 300 in the defaults.

## Progress

Step 1 is done. `StreamTiming` now holds `bufferUs` and `headroomUs` and derives
`leadUs()` and `videoWaitLimitUs()` from them; nothing reads an independently
tuned lead any more, and `preview_backend` passes the derived pair to the playout
core. The shipped values keep every number the profiles were tuned with, so this
step is a no-behaviour-change refactor:

| Profile | buffer | headroom | lead | wait | capture age |
| --- | --- | --- | --- | --- | --- |
| Low | 45 | 35 | 45 | 80 | 120 |
| Balanced | 55 | 35 | 55 | 90 | 150 |
| Smooth | 55 | 55 | 55 | 110 | 250 |

`tools/test_stream_profile.cpp` (`stream_profile_timing`) pins that table, checks
that the wait is always buffer plus headroom, and runs every profile through
`AudioPlayout::configureTiming` so a buffer that outgrows the current 200 ms lead
bound fails the suite instead of throwing `Invalid playout timing` on the first
watch. The Smooth headroom stays wider than the other two only because that is the
wait it was tuned with; step 5 is expected to collapse all three onto one headroom
as the buffers grow.

## Native-suite baseline

At this commit the native suite is **not** green: 17 of 23 ctest cases pass and six
fail. Those six are not caused by step 1 (the affected binaries do not include
`stream_profile.h` and were not relinked by it); they predate it, and at least one
is a test left behind by an intentional behaviour change:

- `encoded_delivery_budget` — `test_encoded_delivery.cpp` still expects the ~3 s
  periodic-keyframe interval. `PeriodicKeyframe::request` now requires
  `max(5 s, 30 s at 1 MB/10 Mbps)` (`periodic_keyframe.h`, docs/64), so the
  9 s/30 kbps request the test makes is correctly refused. The test needs updating
  to the current interval, not the other way round.
- `bounded_audio_playout`, `remote_audio_commit_admission` — both assert the older
  fixed 80 ms bounded-advance gate (`now+80000`, "slightly early"/"modestly early"
  pictures) that the configurable lead/wait model replaced. Deciding their new
  expectations means deciding what the gates should be under `bufferUs`, so they
  belong with step 3.
- `remote_video_presentation` — expects a submitted frame to be refused after
  165 ms; docs/68 deliberately made untimed pictures presentable at arrival, which
  changes that boundary.
- `encryption_gate_and_input` — expects the bounded backlog to report
  `pending_audio_ms == 100` with one overflow drop.
- `remote_share_ownership` — expects a history bound that `RemoteShare` no longer
  produces.

The last two are diagnosed only as far as their failing assertion; neither has been
traced to a specific code change yet. Recording them here is bookkeeping, not a gate:
the acceptance path for this design is the A/V matrix under "Tests before it may be
called done", not the unit suite.

## Observed failure (2026-11-02)

The 720p case at the current 55 ms lead is intermittent, and the failures are not
near misses — they are reset storms:

| Run | Result | drawn / admitted | audio blocks admitted | first audio | resets |
| --- | --- | --- | --- | --- | --- |
| `20261102-032345` | fail | 162 / 519 | 0 | — | 715 |
| `20261102-032443` | pass | 527 / 531 | 1190 | 15.2 ms | 3 |

Six consecutive 720p runs that night gave pass, fail, fail, fail, fail, pass; three
fresh runs on the step-1 build gave three passes. A run either settles (3 resets, all
media flowing) or never converges (hundreds of resets, no audio block ever admitted),
which is the shape this document's model is meant to remove: each reset flushes video
as well as audio, so the two validate each other into a livelock once the lead has no
valid position relative to the last painted picture.

Acceptance numbers for the model change, taken from the passing run: roughly 3
resets, ~527 drawn of ~531 admitted, over 1000 admitted audio blocks and a first
audible block near 15 ms — repeated across runs, not a single green one.
