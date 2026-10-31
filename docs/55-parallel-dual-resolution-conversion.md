# Parallel dual-resolution conversion

Date: 2026-10-28, Europe/Budapest

Status: **720p sender cadence improved; 1080p conversion remains over budget**

This checkpoint follows the moving-cube investigation. It targets the measured
capture conversion cost before encoding, which capped the sender below 60 FPS.

## Measured baseline

The original single conversion worker performed these stages serially:

| Stage | Average |
| --- | ---: |
| BGRA source to I420 | 5.3 ms |
| 1080p letterboxed NV12 canvas | 10.0 ms |
| 720p letterboxed NV12 canvas | 4.2 ms |
| Total | approximately 19.5 ms |

A 60 FPS frame period is 16.67 ms. The original 720p video-only run therefore
admitted 505 of 598 captured frames and replaced 91 raw frames before
conversion. The moving source could never maintain a complete 60 FPS history.

## Change

The BGRA-to-I420 conversion remains shared and serial. After it completes, the
independent 1080p and 720p canvas constructions run concurrently. The result is
still a pair of exact-resolution NV12 frames passed to the existing encoders.

New bounded reports record:

- `source_conversion_average_ms`;
- `canvas_1080_average_ms`;
- `canvas_720_average_ms`;
- `converted_frames`.

No queue, age, encryption or presentation bound was changed.

## 720p result

Video-only 720p:

| Metric | Before | After |
| --- | ---: | ---: |
| Captured frames | 598 | 618 |
| Admitted frame pairs | 505 | 614 |
| Raw replacements | 91 | 3 |
| Average conversion | 19.5 ms | 14.5 ms |
| Maximum conversion | 47.1 ms | 31.4 ms |

Evidence:

`artifacts/20261028-050406-remote-viewing/720/sender.json`

Synchronized 720p A/V:

- 1,149 of 1,150 captured frame pairs admitted;
- zero raw replacements;
- average conversion 13.9 ms;
- maximum conversion 23.7 ms;
- effective visible source progression 59.99 FPS;
- two catch-up intervals after the presentation floor;
- p95 visible spacing 29.1 ms;
- stereo output passed;
- one 85.4 ms audio quiet interval remained.

Evidence:

`artifacts/20261028-050436-remote-av/720/viewer.json`

The sender is no longer the limiting frame source in the 720p run.

## 1080p result

The next 1080p A/V attempt still exceeded the frame budget:

- 1,137 captured frames;
- 1,009 admitted frame pairs;
- 127 raw replacements;
- average conversion 18.2 ms;
- average source conversion 6.3 ms;
- average 1080p canvas conversion 11.9 ms;
- maximum conversion 33.4 ms.

The 1080p canvas must finish before both outputs can be submitted, so converting
the 720p canvas concurrently cannot lower the current critical path.

Evidence:

`artifacts/20261028-050548-remote-av/1080/sender.json`

That run also retained severe audio/device failures:

- 560 ms longest quiet interval;
- 82.8% estimated media-window coverage;
- nine endpoint-gap resets;
- stereo downmix.

These audio failures are not attributed to parallel conversion.

## Current executable

```text
build/bin/GoodSharing.exe
SHA-256 7EC89F0B8129E54CD88226FA509075A724D6049F5DE5DB94577BDAC8D9F6A1C4
```

All 22 native tests pass.

Final exact-hash 720p A/V sample:

- 1,114 of 1,140 captured frame pairs admitted;
- 25 raw replacements;
- average conversion 15.7 ms;
- source conversion 5.3 ms;
- concurrent 1080p canvas 10.7 ms;
- concurrent 720p wall interval 10.7 ms;
- maximum conversion 32.5 ms;
- visible source progression 59.91 FPS;
- 18 duplicate and 196 skipped source positions;
- five catch-up intervals and 101 presentation-floor drops;
- p95 visible spacing 34.5 ms;
- stereo passed;
- longest audio quiet interval 5.25 ms;
- the complete 720p A/V check passed.

Evidence:

`artifacts/20261028-050730-remote-av/720/summary.json`

The two canvas measurements overlap and must not be added to the source
conversion time.

## Remaining work

1. Reduce the 1080p critical path below the 16.67 ms frame period.
2. Replace per-frame async thread creation with a persistent parallel worker if
   it materially reduces tail latency.
3. Avoid redundant I420 plane scaling or move the transform to the GPU.
4. Rerun three 720p and three 1080p same-hash motion tests.
5. Keep the split-watch and audio failures as separate investigations.

