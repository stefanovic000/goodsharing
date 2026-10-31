# Endpoint exhaustion recovery and minimized A/V tests

Date: 2026-10-28, Europe/Budapest

Status: **terminal endpoint deadlock fixed; 1080p conversion contention remains**

This checkpoint closes the long audio dropouts that appeared after the
audio-priority changes. It also updates the paint-stall test policy and makes
the owned GoodSharing test windows start minimized.

## Root cause

When WASAPI ran out of submitted audio, `IAudioClock` could remain on a frozen
position while still reporting a positive lead below one millisecond.
`WasapiPlayoutSink` classified that state as `prepare_too_soon` and returned
without writing or recovering. The condition could repeat indefinitely:

- the bounded PCM queue filled and overflowed;
- the device repeated silence;
- reset, underrun and endpoint-gap counters remained zero;
- decoded channel difference stayed approximately `1.25`, proving the stereo
  transport content was intact.

This was the common cause of the earlier multi-second drops across 1080p,
static, static-resume and switch cases.

## Fix

An exhausted write position now raises the existing bounded
`EndpointDiscontinuity` recovery path. The first observation records a
`deadline_too_soon` diagnostic with position, lead, submitted frames, QPC age
and padding. The endpoint is flushed and refilled instead of waiting forever.

The real device probe reproduced the exact signature:

```text
reason=deadline_too_soon
lead_us=0
padding_frames=0
short_discontinuities=1
```

The after-revoke silence and stereo checks passed.

## Paint-stall policy

`check_stall_observations()` in `scripts/test_remote_av.py` now matches the
implemented audio-priority policy. It verifies:

- the audio timeline and device identity remain valid;
- decoded audio continues to commit during the GUI block;
- the picture remains frozen while painting is blocked;
- fresh video resumes afterward;
- the final stop still revokes and fences playback.

The fresh paint-stall case passed with all checks enabled.

## Minimized test windows

Owned GoodSharing viewer and sender windows now start minimized unless
`--visible-view` is requested. The small generated fixture window remains
visible because Windows Graphics Capture requires a capturable source window.

Minimized mode still exercised real painting and captured screenshots. The 720p
control passed `test_windows_minimized=true`.

## Verification

All 22 native tests pass.

Individual A/V runs after the fix:

| Case | Result | Longest audio quiet |
| --- | --- | ---: |
| 720p traced | Pass | not retained in trace-only summary |
| 720p minimized | Pass | 4.29 ms |
| paint-stall | Pass | 3.81 ms |
| static 720p minimized | Pass | 3.29 ms |
| static 1080p minimized | Pass | 6.71 ms |
| static 1080p traced | Pass | 0.94 ms |

The full minimized matrix improved from 9/14 to 12/14. Multi-second audio
silence disappeared. The two remaining failures were:

1. `static-1080`: longest quiet **49.06 ms** after two bounded endpoint-gap
   recoveries; a second isolated run passed audio but measured 46.54 ms p95
   draw lateness.
2. `switch`: longest quiet **50.02 ms** after one endpoint-gap recovery and one
   bounded headroom rebase; audio remained stereo and continuous otherwise.

Those 49-50 ms gaps are the remaining endpoint recovery cost, dominated by the
22 ms silent device bootstrap. They are no longer the unbounded deadlock.

## Remaining 1080p limit

The latest failed 1080p sender still exceeded one frame period:

- captured frames: 1,145;
- admitted frame pairs: 1,021;
- raw replacements: 124;
- average source conversion: 5.95 ms;
- average 1080p canvas: 12.06 ms;
- average 720p canvas: 12.08 ms;
- average total conversion: 18.03 ms;
- maximum conversion: 51.12 ms.

The two canvases overlap, so their wall interval is about 12 ms. The limiting
critical path is still source conversion plus the 1080p canvas. This also
raised GUI draw p95 to 46.54 ms in the traced failure.

## Current artifacts

```text
GoodSharing.exe
A9AABD3CADF8E03242058D10EE20DF720CE3E177EB9B5476636F5C968B3B9B7C

livekit.dll
FBEDCAFD56C5D4F007B1EEE660AFA2C09B4B5CD247922C7B7FD23AF3A8D036DF

livekit_ffi.dll
8BBF8C07FD4D99614E2FCFB51459F8ACAA2C3EB0A12B7CCFCC34B00667B1017C
```

Primary reports:

- `artifacts/20261028-162032-remote-av/summary.json`
- `artifacts/20261028-162706-remote-av/summary.json`
- `artifacts/20261028-162743-remote-av/summary.json`
- `artifacts/20261028-162100-wasapi-stall.json`

## Next

1. Avoid redundant plane copies when source and fitted dimensions match.
2. Move 1080p plane scaling to libyuv or a persistent parallel pipeline.
3. Investigate the intermittent weak-left-channel three-second tail while
   preserving the already-correct submitted stereo ratio.
4. Repeat 720p and 1080p three times after the conversion critical path drops
   below 16.67 ms.

