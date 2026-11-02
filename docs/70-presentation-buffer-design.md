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

1. Matrix: {Low, Balanced, Smooth} Ã— {720p, 1080p} Ã— {30, 60 fps} — all green.
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

Step 2 is done. `RemoteAudioPlayout::pump` no longer measures audio against the
last painted picture. When a buffer is configured, the reference is the delayed
timeline — `senderAt(now)`, the sender position that should be audible now, which
the common clock already places one lead after capture. Three consequences:

- The A/V horizon, the trimmed commit margin, the render `SourceWindow` and the
  lag reset all use that reference instead of `presentedSender_`.
- A presented ticket is no longer required before audio may start, and the
  last-paint age reset only applies once something has actually been painted. This
  is the deadlock in the table above: with no picture ever presented the reference
  stayed at 0, every block was "ahead of the picture", and nothing was admitted.
- `GOODSHARING_TEST_OLD_TIMELINE=1` sets the buffer to 0 and restores the pre-model
  source, so one binary can A/B both paths.

Measured on the 720p case, same binary, delayed arm extended to seven runs:

| Path | Result | drawn | audio blocks | resets |
| --- | --- | --- | --- | --- |
| delayed timeline | 7/7 pass | 507–525 of 512–528 | 888–1189 | 2–3 |
| old timeline | 1/3 fail (tone gaps / coverage) | 511–515 of 515–519 | 1169–1186 | 2–3 |

No delayed run reset more than three times, against 715 in the storm run. Two of the
seven admitted fewer audio blocks (888, 889) while still passing every gate, so the
delayed path is not uniform yet — worth watching in the step 4 matrix rather than
declaring the model finished.

The old path's failure this time was a tone gap rather than a reset storm, so its
weakness is intermittent in more than one way, and three runs per arm is thin
evidence. Step 4's repeated matrix is what should decide.

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

## Step 5 attempt: the large buffers still storm (2026-11-02)

The lifetime and ticket-lead bounds now follow the buffer (`videoLifetime` =
max(150 ms, buffer+50 ms), ticket lead = max(80 ms, buffer+25 ms), lead
validation raised to 600 ms), so nothing rejects a 300 ms profile outright any
more. Balanced was then set to 150 ms and Smooth to 300 ms and the 8-case matrix
was run on that build:

| Case | Result | drawn | audio blocks | resets |
| --- | --- | --- | --- | --- |
| low 30 | pass | 509/513 | 889 | 3 |
| 720 30 | fail | 172/528 | 4 | 711 |
| 1080 30 | fail | 489/512 | 848 | 28 |
| smooth 30 | fail | 162/519 | 0 | 715 |
| 720 60 | fail | 318/1012 | 5 | 1383 |
| 1080 60 | fail | 316/1037 | 3 | 1429 |
| smooth 60 | fail | 318/1026 | 0 | 1415 |

Only the 45 ms profile survives. So the delayed timeline removed the deadlock at
55 ms but does **not** by itself make 150/300 ms work: at those delays the
presented-reference coupling still livelocks, and the harness's lead/wait gates
(90/100/120 ms) also describe the old, small leads. The profile values were
reverted; the bounds plumbing stayed, because it is inert at 45/55 ms and is what
any future attempt needs.

Next: find what still couples to the paint moment above ~100 ms. The remaining
candidates are the ticket path in `RemoteAudioPlayout::presentedLocked`
(80 ms deadline age), the A/V horizon `max(80 ms, lead+40 ms)` and the
first-picture wait, in that order.

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

### Second attempt: gates follow the profile, buffers still not stable

Three more couplings to the old lead were removed: the ticket-age and last-paint
age gates used a literal 80 ms (they now use the profile wait bound and
buffer+wait), the first-picture head start stopped at 300 ms (now buffer+300 ms),
and the harness lead/wait gates now read `target_video_lead_ms` and
`video_presentation_wait_limit_ms` from the run report instead of a table fixed
at 90/100/120 ms. `GOODSHARING_TEST_BUFFER_US` exercises a buffer without
changing the shipped profiles.

Measured with that build, still not shippable:

| Buffer | Result | drawn | audio blocks | first audio | resets |
| --- | --- | --- | --- | --- | --- |
| 150 ms | fail | 519/529 | 1177 | 4.8 s | 8 |
| 150 ms (repeat) | fail | storm | — | — | hundreds |
| 300 ms | fail | 161/517 | 0 | — | 713 |
| 45/55 ms defaults | pass | 513-996 of 517-1008 | 889-1173 | 13-30 ms | 2-3 |

So 150 ms is now close - video and audio both flow, nothing storms on the good
run - but its audio needs 4.8 s to start and it still storms on repeat. 300 ms
still livelocks. The shipped profiles therefore stay at 45/55 ms, where the full
matrix and the native suite are green.
### Third attempt: the large buffers work (2026-11-02)

Three fixed-size assumptions, not one, kept the buffers from ever committing. All
are now derived from the profile:

- `AudioPlayout` rejected any block whose deadline was more than **100 ms**
  ahead (`deadline>now+100000`). A 300 ms buffer makes every block exactly that
  far ahead, so nothing was ever admitted and the timeline reset on every frame.
  The limit is now `max(100 ms, lead+40 ms)`.
- `projectVideo` accepted a projection only 10-80 ms ahead, so no ticket could
  validate past an 80 ms lead. It now uses the configured ticket lead.
- The PCM queue was `std::array<Block,6>` - 60 ms - while the buffer needs the
  whole presentation delay. Depth is now `6 + buffer/10 ms` blocks (capped at
  40) with heap storage, because 40 blocks inline overflowed test stacks.

The ticket lead limit and the wait bound are now the same value, which is what the
`deadline == received + leadLimit` equality in `validLocked` assumes. The
harness gates became durations as this document's plan required: queue depth is
`high_water * frameMs <= lead + wait + 2 frames`, PCM depth is
`blocks * 10 ms <= lead + 60 ms`, and the lead/wait gates read the values the run
reports.

Full 8-case matrix at the *shipped* profiles, which are now Low 45 / Balanced 150 /
Smooth 300 ms:

| Case | Result | drawn | audio blocks | first audio | max lead | queue |
| --- | --- | --- | --- | --- | --- | --- |
| 720/30 | pass | 509/515 | 890 | 72 ms | 156 | 6 |
| 1080/30 | pass | 507/515 | 1173 | 80 ms | 150 | 6 |
| low/30 | pass | 509/515 | 889 | 21 ms | 74 | 3 |
| smooth/30 | pass | 509/519 | 1187 | 230 ms | 302 | 10 |
| 720/60 | pass | 1001/1035 | 1188 | 80 ms | 153 | 11 |
| 1080/60 | pass | 1006/1040 | 1190 | 79 ms | 157 | 11 |
| low/60 | pass | 1025/1034 | 1190 | 32 ms | 73 | 5 |
| smooth/60 | pass | 1006/1036 | 1175 | 239 | 313 | 20 |

Resets are 2-3 in every cell and the native suite is 23/23. The trade the profiles
make is now visible in the numbers: first audio rises from 21 ms (Low) to 230-239 ms
(Smooth), which is the presentation delay being paid for jitter tolerance.
### Repeatability

The matrix was run twice, in opposite order of cases, plus the scenario cases:

- pass 1: 8/8 (720, 1080, low, smooth x 30, 60 fps)
- pass 2: 8/8 (same cases, reordered)
- scenarios at 60 fps: `static`, `static-1080`, `minimized`, `switch` and
  `wrong-key` all pass

Twenty-one consecutive green runs, every one at 2-3 resets. For comparison, the
same 720p case at 55 ms before this work failed 4 of 6 runs with 715 resets and no
audio. The profile trade is now the only real cost: first audio is 21-32 ms on
Low, 72-80 ms on Balanced and 230-239 ms on Smooth, which is the buffer being paid
for jitter tolerance as designed.
## Loss case: the buffer does not absorb erasure (2026-11-02)

The step 4 loss case now exists: `--loss PERCENT` makes the receiver drop that
share of arriving frames before the scheduler sees them (`GOODSHARING_TEST_DROP`,
read with `getenv` because this app's Qt accessor reports 0 for the variable).

720p, 30 fps, same build, all three profiles:

| Loss | Low 45 ms | Balanced 150 ms | Smooth 300 ms |
| --- | --- | --- | --- |
| 0% | pass, cov 0.97 | pass, cov 0.99 | pass, cov 0.99 |
| 5% | pass, cov 1.00 | pass, cov 1.00 | pass, cov 1.00 |
| 10% | fail, cov 0.886 | fail, cov 0.892 | fail, cov 0.886 |
| 20% | fail, cov 0.740 | fail, cov 0.799 | fail, cov 0.809 |

The profiles do not separate: at 10% and 20% every profile loses the same share of
media and fails the same gates, and the failures are `media_filled_device_frames`,
`live_window_coverage` and `continuous_tone_without_gap` — missing samples, not
late ones. That is the expected result and it refutes part of this document's own
premise: a presentation buffer converts *jitter* into tolerance, but it cannot
invent an erased 10 ms block. Erasure needs recovery (FEC, retransmission), which is
the same conclusion docs/67 reached from the internet path.

A jitter test would be the one that separates the profiles, and the naive version
(sleeping in the ingress callback) kills the viewer because it blocks the SDK
callback thread, so it needs a queued, timer-driven delay rather than a sleep. That
is the open piece of the loss case.

Next levers, in order: FEC/retransmission for erasure, then a non-blocking jitter
impairment to demonstrate the buffer's actual benefit.
## Jitter case: the buffer does buy tolerance (2026-11-02)

The jitter impairment is `--delay-ms N`: each arriving audio block is queued and
released N ms late by a worker thread (`GOODSHARING_TEST_DELAY_MS`). Sleeping in
the ingress callback instead kills the viewer, because it blocks the SDK callback
thread; and enabling the delay from the first frame perturbs *startup*, which is
what the first attempt measured — non-monotonic results. It now engages only after a
3 s warm-up, so the measurement is steady state.

The `painted` gate needed tightening for this: it asked for `timed_draws > 100`,
but a halted presentation still produced ~110 timed pictures before the impairment
engaged, so a broken run passed. It now requires >200, and every normal case
(350 at 30 fps, 676 at 60, 559 minimized) clears it comfortably.

720p, 30 fps, same build, timed pictures out of ~350:

| Delayed arrival | Low 45 ms | Balanced 150 ms | Smooth 300 ms |
| --- | --- | --- | --- |
| 60 ms | **fail**, 19 | pass, 350 | pass, 346 |
| 120 ms | **fail**, 111 | **fail**, 108 | pass, 346 |
| 200 ms | **fail**, 114 | **fail**, 20 | pass, 346 |

The threshold tracks the buffer: Low fails from 60 ms (its buffer is 45), Balanced
survives 60 but not 120, Smooth still passes at 200 and failed at 250 in the earlier
run. That is the trade the profiles exist for, finally visible: the buffer buys
tolerated lateness roughly proportional to its size, and costs first-audio delay of
the same order.

Note what does *not* move: media coverage stays at 0.999 and admitted audio blocks
stay in the 800-1180 range in every cell, including failures. The runs fail because
timed presentation stops, not because audio is lost - the audio path re-anchors and
keeps playing while the picture timeline stalls.

With that, the loss and jitter cases in "Tests before it may be called done" both
exist, and they say opposite things: jitter scales with the buffer, erasure does not
scale with anything available here and needs FEC.
## Why there is no network-level loss test yet

The `--loss` impairment drops frames *after* decode, at the playout boundary. That
matters for how far its result can be pushed: RED is already enabled on the audio
publication (`options.red=true` in share_publisher.cpp) and NACK/FEC live below the
decoder, so nothing in the transport layer can rescue a frame that was dropped after
it. The 10% result therefore says "these profiles do not survive losing a tenth of
their decoded media", not "FEC would not help".

Reaching real packet loss needs the media to traverse an impairing hop. Two attempts
failed on this host:

- A UDP relay on `127.0.0.2:8765` (media forwarded to the SFU on `127.0.0.1:8765`)
  coexists with the SFU's socket - verified directly - but the SFU only puts the
  relay in the path when it advertises that address, and LiveKit's candidate filter
  cannot advertise `127.0.0.2` because it is a loopback alias rather than a real
  interface. With the filter set to `127.0.0.2/32` the offer has no usable
  candidate and the session never starts (0 packets through the relay); without it,
  media takes the direct `127.0.0.1` path (6 packets seen, all handshake).
- Impairing inside the client instead would sit above the decoder again, which is
  the limitation above.

`scripts/loopback_relay.py` now has `configure_loss` (drops in both directions,
counted per direction) and is tested for the addressing above; it becomes usable the
moment the media path can be forced through it - an OS packet filter, a second
machine, or a real remote SFU. That is the honest state of the FEC question: it is
unmeasured, and RED is already doing what it can.
## The latency the profiles actually cost (2026-11-02)

The delay a profile applies is now measured, not assumed. `RemoteVideo` samples how
long each distinct picture waited between arrival and paint, and the playback report
carries `paint_residence_p50_ms`, `p95_ms`, `max_ms` and the sample count.

720p, 30 fps, one run per profile:

| Profile | Buffer | residence p50 | p95 | max |
| --- | ---: | ---: | ---: | ---: |
| Low | 45 ms | 71.0 | 71.9 | 78.3 |
| Balanced | 150 ms | 150.1 | 161.2 | 170.5 |
| Smooth | 300 ms | 303.0 | 310.3 | 317.9 |

The measured residence tracks the configured buffer within ~20 ms (one encode/decode
hop plus scheduling). A check, `applied_presentation_delay`, now requires the p50 to
be within 40 ms of the profile's buffer, so a profile that silently stops applying its
delay fails the run instead of passing on a stale number.

This is the receiver-side half of the trade only. A glass-to-glass figure would need
the sender's capture time in the receiver's clock domain; the capture stamp travels in
the frame metadata but in the sender's own steady clock, which the playout cannot map
(the attempted `localFor(captureTimestampUs)` produced zero samples before it was
removed). Closing that needs the sender to publish a capture-to-NTP relation, which is
a real protocol change rather than a measurement.
## Seventeen-case sweep (2026-11-02)

Running every case in the harness at 720p/30fps gave 16 of 17 on the first attempt.
The miss was `low` on `continuous_tone_without_gap`. Retries showed it is the
profile most sensitive to machine load rather than deterministically broken: three
consecutive `low` runs failed on three *different* gates (tone gap, `stereo_output`,
`composited_marker`) and six consecutive runs in a second batch all passed, with or
without its buffer raised to 70 ms. Low is 45 ms and sits closest to this machine's
scheduling jitter, which matches the jitter-case result where it was also the first to
fail.

Four harness faults surfaced only because that sweep existed:

- `painted` required a fixed 200 timed pictures, which short runs (source-exit)
  cannot reach. It now requires 40 % of the pictures the receiver admitted, floor 60 -
  which still catches a stalled presentation (110 of 515 = 21 %).
- Residence sampled every paint, so an arrival-paced tail after the source stopped
  dragged the median to ~21 ms on source-exit cases. It now samples timed pictures
  only, and those runs read 158-167 ms as the profile intends.
- `stall_resumed_fresh` demanded 60 recovered pictures in 2 s, i.e. 30 fps of
  recovery, while the paint-stall case runs at 30 fps. The requirement now scales
  with the run's frame rate (56 of ~57 at 30 fps passes).
- `switch_probe_completed` required an adopted switch for every switch case,
  including `switch-timeout`, whose whole point is that no switch happens. For that
  case it now requires the timeout observation to exist.