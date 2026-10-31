# Moving-cube cadence and presentation floor

Date: 2026-10-28, Europe/Budapest

Status: **speed-up bursts suppressed; residual presentation choppiness remains**

This checkpoint investigates the reported moving-cube symptom: the cube
sometimes appears to skip ahead or move faster during an A/V run.

## Findings

Two separate causes contributed:

1. The controlled fixture used a 16 ms timer. Its frame counter therefore
   advanced at 62.5 positions per second while the publication target is
   60 frames per second.
2. Delayed delivery caused visual catch-up. A frame could be presented only
   3–7 ms after the previous frame even though its source deadline was
   12–18 ms later.

The transport was not losing the moving source positions. The non-Qt actual
capture receiver received 612 source positions over 10.18 seconds:

- effective source rate: 60.09 positions/second;
- duplicate source positions: 0;
- skipped source positions: 1;
- median receive interval: 16.637 ms;
- p95 receive interval: 18.994 ms.

Evidence:

`artifacts/20261028-044649-capture-sharing/application-720/viewer.json`

## Fixture cadence fix

`tools/media_fixture.cpp` originally used:

```text
QTimer::setInterval(16)
```

That requests 62.5 Hz. The fixture now uses a single-shot absolute 16,667 µs
schedule, producing an observed 59.85–60.09 source positions per second without
catch-up rendering.

This keeps the test input aligned with the application's 60 FPS publication
target.

## Present-point instrumentation

Test-only motion tracing now records the displayed 16-bit source frame number
when:

- the non-Qt capture receiver decodes a frame; or
- the Qt viewer's frame has passed `frameSwapped`.

Records contain only:

- source frame number;
- transport ordinal;
- source capture timestamp;
- receive/presentation timestamp.

No captured pixels or audio content are retained.

`scripts/analyze_motion_trace.py` reports source-frame progress, duplicate
positions, skipped positions, catch-up intervals and draw interval
distribution. Its offline controls pass in
`scripts/test_motion_trace_analysis.py`.

## Presentation changes

Incoming frames now request a Qt update immediately instead of waiting only for
the 16 ms polling timer. Existing deadline, permission, queue and age checks
remain in place.

The viewer also enforces a 10 ms minimum between actual presentation updates.
A delayed frame that would otherwise appear too soon is dropped rather than
accelerating the visible motion. This is test-overridable as
`GOODSHARING_TEST_DISABLE_PRESENTATION_FLOOR=1` for A/B comparison only.

## A/B results

All samples below use the corrected 60 FPS fixture and frame-swapped tracing.

| Profile | Drawn frames | Duration | Duplicate positions | Catch-up intervals | Minimum interval | p95 interval |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| No floor, initial | 892 | 16.19 s | 0 | 71 | 3.7 ms | 31.2 ms |
| No floor, explicit control | 889 | 16.23 s | 88 | 58 | 3.4 ms | 32.2 ms |
| 10 ms floor | 805 | 16.20 s | 0 | 1 | 10.9 ms | 33.4 ms |
| Final hash | 762 | 16.23 s | 33 | 6 | 9.3 ms | 37.4 ms |

The 10 ms floor removes nearly all extreme catch-up presentations. It does not
make delivery perfectly smooth:

- delayed frames are dropped;
- the final sample still had 33 duplicate source positions;
- 75 frames were withheld by the floor;
- p95 visible spacing exceeded two display periods;
- a 126 ms maximum spacing remained.

The final sample also retained the separate intermittent audio failures:

- stereo output failed;
- longest audio quiet interval: 131.2 ms.

Evidence:

`artifacts/20261028-044929-remote-av/720/viewer.json`

## Same-hash repetition

Two additional final-hash runs were analyzed:

| Evidence | Drawn | Duplicate positions | Catch-up intervals | Minimum interval | p95 interval | Audio result |
| --- | ---: | ---: | ---: | ---: | ---: | --- |
| `044929` | 762 | 33 | 6 | 9.3 ms | 37.4 ms | Stereo and continuity failed |
| `045108` | 797 | 49 | 2 | 10.8 ms | 32.8 ms | Stop-quiet and continuity failed |
| `045133` | 782 | 0 | 5 | 9.4 ms | 34.3 ms | Full pass |

Across these runs:

- effective source progression stayed at 59.85–59.97 positions/second;
- extreme catch-up intervals stayed at 2–6 instead of 58–71 without the floor;
- 33–49 duplicate source positions remained in two runs;
- p95 visible spacing remained 32.8–37.4 ms;
- occasional 126–128 ms stalls remained;
- one of three complete A/V checks passed.

The acceptance gate is therefore not met.

The remaining visible position skips are primarily missing intermediate frames,
not overall temporal acceleration. The position advances at the correct average
source rate, but when several intermediate frames are late, the next available
position is farther away.

## Current executable

```text
build/bin/GoodSharing.exe
SHA-256 4E849AF7317897F0D9C271C167CE2E222AED86D40414EE2C5D3B75F0EA3AD3E8
```

```text
build/bin/goodsharing_fixture.exe
SHA-256 AEA9C7533250FA46EB931A0469FF4CD6E801730A751B48D3DF574D32AA75D437
```

All 22 native tests pass, including the updated handover burst controls.

## Interpretation

- The original "moves faster" bursts were a real presentation defect.
- A 16 ms synthetic source timer also gave the fixture an unintended 62.5 Hz
  cadence, making the issue easier to see.
- The transport can carry every source position in the non-Qt receiver.
- The Qt viewer still loses or delays updates during host/network stalls.
- Dropping late catch-up frames prevents acceleration but cannot reconstruct a
  smooth 60 FPS display when the transport or renderer misses deadlines.

## Next acceptance gate

- [ ] Three consecutive same-hash GUI runs with no presentation interval below
      10 ms.
- [ ] At least 58 effective source positions per second over 10 seconds.
- [ ] No duplicate source position unless the source itself was static.
- [ ] p95 presentation spacing at or below 20 ms.
- [ ] Maximum uninterrupted delivery stall below 50 ms in the steady-state
      local run.
- [ ] Audio stereo and continuity checks pass in the same runs.

Do not increase capture, queue or presentation age limits to clear this gate.
Compare the combined-watch receiver, which received 612/612 source positions,
against the split-watch GUI path, which received fewer than the source provided.
Reduce conversion/render cost and locate the transport/read stalls; do not hide
them by loosening the age bounds.

