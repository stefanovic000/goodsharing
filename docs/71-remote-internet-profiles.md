# Profiles on the real internet path

Date: 2026-11-02, Europe/Budapest

Status: **measured on the Oracle VM; Low fails there, Balanced and Smooth pass**

## Why this run exists

docs/67 left the remote path degraded: 135-167 frames per run against 594-589 an
hour earlier on loopback, and it named frame-rate-aware rate limiting and audio FEC
as the next steps. The presentation-buffer work in docs/70 then changed what the
receiver can absorb, but it had only ever been measured on loopback. This run asks
the obvious question against the same VM: does the buffer help over the internet?

## Setup

Same PC for both clients (sender and viewer), the owned harness, and the remote SFU:

```
python scripts/test_remote_av.py --case <profile> --share-fps 30 \
    --server-url ws://132.226.207.140:7880
```

Credentials came from `artifacts/sfu-credentials.ini`. 720p, 30 fps, 18 s watch.
The SFU speaks plain `ws://`; a `wss://` attempt fails inside the SDK with
"received corrupt message", which is worth knowing before the next attempt.

## Results

| Profile | Buffer | Result | drawn | audio blocks | resets | residence p50 |
| --- | ---: | --- | --- | ---: | ---: | ---: |
| Low | 45 ms | **fail** | 434/438 | 755 | 5 | 16.8 ms |
| Balanced | 150 ms | pass | 452/459 | 820 | 2 | 156.8 ms |
| Smooth | 300 ms | pass | 441/451 | 814 | 279.0 ms | |
| Smooth | 300 ms | pass | 433/444 | 813 | 302.6 ms | |
| Smooth | 300 ms | pass | 455/465 | 820 | 304.4 ms | |

Low's failure is the one already understood from the jitter case: the picture
timeline never settles, so presentation falls back to arrival pacing (residence
16.8 ms is the tell - pictures are painted immediately instead of waiting for their
slot) and the `painted`/`frame_queue_observed` gates fail. Balanced and Smooth hold
the timeline, and their residence matches the configured buffer, so the delay really
is being applied across the internet hop.

## What it means

The internet path's jitter exceeds the 45 ms budget, which is exactly the condition
the larger profiles exist for. Three consecutive Smooth runs all pass at 279-304 ms
residence with two resets, against docs/67's 135-167 frames: the degradation it
recorded is absorbed by time, not by rate limiting or FEC. FEC may still be needed
for loss (the docs/70 loss case shows 10% erasure defeats every profile), but jitter
alone no longer does.

This is not yet a two-machine test: both clients share one PC's clock, uplink and
capture path. It also does not measure glass-to-glass latency, only the receiver-side
residence. Both remain open.