# Audio priority and bounded jitter recovery

Date: 2026-10-28, Europe/Budapest

Status: **audio continuity substantially improved; device starvation remains**

This checkpoint implements a low-latency audio-priority policy and records the
remaining failures without relaxing encryption, video freshness or terminal
device-error handling.

## Changes

### Endpoint rounding tolerance

Device-clock deficits up to 3 ms are treated as clock-sampling noise instead of
underruns. The real captured case reported a **1 µs** deficit, which previously
flushed the device. Larger negative leads still trigger endpoint recovery.

### Twenty-millisecond audio target

The WASAPI scheduler now refills until approximately 20 ms of first-sample lead
instead of 15 ms. The existing hard first-sample bound remains 30 ms.

This adds at most 5 ms of nominal buffering and reduces avoidable backpressure
underruns.

### Adaptive audio-only offset

After three consecutive low-headroom decoded blocks, queued and future audio
deadlines are shifted later by a bounded amount:

- target headroom: 15 ms;
- maximum accumulated audio-only offset: 40 ms;
- video deadlines and the common clock are not moved;
- queued PCM is retained and shifted with its original order.

The existing 80 ms A/V source window continues to limit permitted separation.

### Audio priority through video stalls

Once audio has been played, missing or stale video presentation no longer forces
an immediate audio/video flush. Audio continues on the already-established
source clock while the picture remains frozen. The endpoint and final encryption
permissions are still checked for every commit.

When video returns:

- an early video packet cannot steal the established audio anchor;
- out-of-window video is dropped;
- only a source-clock epoch change or a jump beyond the existing 2-second
  boundary can force a common reset.

### Bounded alignment fallback

If 64 consecutive audio blocks cannot fit behind the video anchor, audio
switches once to its own incoming sender clock rather than resetting forever or
dropping indefinitely. Video can later rejoin that mapped timeline.

### Bounded stereo restart

If decoded channels remain identical for the first second after committed audio,
the test/demo viewer performs at most two controlled resubscription attempts. A
genuinely mono source therefore cannot cause a retry loop.

This path has not yet recovered every observed decoded-ratio-zero failure.

## Real 720p results

Recent untraced groups:

| Evidence | Longest quiet | Stereo | Result |
| --- | ---: | --- | --- |
| `055543` | 0.02 ms | Pass | Pass |
| `055609` | 6.85 ms | Pass | Pass |
| `055634` | 7,312.83 ms | Fail | Failed |

Two of three runs pass. The remaining failure is not decoded-audio starvation:

- decoded channel-difference ratio remained 1.25;
- audio callbacks remained continuous;
- six PCM blocks remained queued;
- front deadline lead was -37.7 ms;
- only 682 device commits completed instead of the approximately 1,400
  expected;
- no low-headroom or presentation reset was recorded.

The later endpoint instrumentation changed that conclusion. In a subsequent
mono failure:

- decoded channel-difference ratio was 0;
- decoded audio callbacks remained continuous;
- the endpoint committed with a maximum gap of only 12.9 ms;
- there were no short discontinuities;
- recent output was two identical channels.

The remaining fault is therefore in the split audio subscription/decode path,
not source capture, local device refill or final mixing.

The 20 ms target improved continuity, but an untraced group still passed only
two of four runs. With server debug enabled, three consecutive runs passed. This
is retained as nondeterministic evidence, not a stable acceptance result.

## 1080p result

One current 1080p run retained stereo but failed continuity:

- longest quiet interval: 838.5 ms;
- estimated media-window coverage: 81.7%;
- 14 device resets;
- one low-headroom reset;
- one adaptive audio offset;
- three bounded alignment reanchors.

This remains open and overlaps the separate 1080p conversion overrun.

## Current executable

```text
build/bin/GoodSharing.exe
SHA-256 ECBE8EF740E50DF9D51656FC89550A6C1463A185AE5319E42C183B4939E99A6C
```

All 22 native tests pass.

## Interpretation

- A small jitter buffer is standard and appropriate for low-latency media.
- Twenty milliseconds is a reasonable initial target on this endpoint.
- Audio continuity is materially better at 720p.
- Resetting video on every audio timing disagreement was the wrong default.
- The remaining intermittent mono/silence failure is in split audio
  subscription/decode delivery.
- The test-only video presentation-floor override remains the experimental
  control for comparing drop-late versus accelerate-on-lateness.

## Next

- Compare combined and split audio subscription delivery on the current build.
- Capture a decoded-ratio-zero failure with server-debug SDP metadata.
- Validate the two-attempt stereo resubscription on a decoded-ratio-zero failure.
- Rerun 720p and 1080p three times after the endpoint fix.

