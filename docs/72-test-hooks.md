# Test hooks

Date: 2026-11-02, Europe/Budapest

These environment variables exist so the harness can drive and disturb the app
without a second build. They are all read at start-up or first use; none of them is
set in normal operation, and the app behaves identically when they are absent.

## Driving a run

| Variable | Effect |
| --- | --- |
| `GOODSHARING_TEST_URL`, `GOODSHARING_TEST_SUBSCRIBER_TOKEN`, `GOODSHARING_TEST_PUBLISHER_TOKEN` | Join a room without the UI. Tokens are minted by the harness. |
| `GOODSHARING_TEST_VOLUME` | Force the listening volume (0-100) so a saved UI volume cannot mute a test. |
| `GOODSHARING_TEST_SHARE_FPS`, `GOODSHARING_TEST_SHARE_BITRATE` | Pin the share frame rate / bitrate budget. |
| `GOODSHARING_TEST_UI_PANEL` | Keep the settings surface reachable for UI checks. |
| `GOODSHARING_TEST_GALLERY` | Auto-add every announced share as a gallery tile. |
| `GOODSHARING_TEST_SKIP_1080_SUBSCRIPTION` | Do not subscribe the 1080p track (cheaper control runs). |
| `GOODSHARING_TEST_DISABLE_AV_PLAYBACK` | Do not start the A/V playout at all. |
| `GOODSHARING_TEST_SKIP_1080_SUBSCRIPTION` | Skip the 1080p subscription in lifecycle tests. |

## Observing

| Variable | Effect |
| --- | --- |
| `GOODSHARING_TEST_AV_TRACE` | Record the bounded event trace, reset history and first fault in the report. Also enables the `reset ...` lines on the playout logger. |
| `GOODSHARING_TEST_AUDIO_TRACE=<dir>` | Write the source-clock trace used by the timing analysers. |

## Disturbing (impairment)

| Variable | Effect | What it can and cannot show |
| --- | --- | --- |
| `GOODSHARING_TEST_DROP=<percent>` | Drops that share of arriving audio blocks and video frames before the scheduler. | Loss **after decode**, so RED/NACK/FEC cannot recover it. It shows what missing decoded media does to the timeline, not what packet loss does. Read with `getenv`, not `qEnvironmentVariableIntValue` - the Qt accessor returns 0 for this variable in this process. |
| `GOODSHARING_TEST_DELAY_MS=<ms>` | Releases each arriving audio block that many ms late from a worker thread. | Lateness, which a buffer can absorb; this is the one that separates the profiles. Sleeping in the ingress callback instead kills the viewer, which is why it is queued. |
| `GOODSHARING_TEST_DELAY_WARMUP_MS=<ms>` | How long after the first arrival before the delay engages (default 3000). | Without a warm-up the impairment perturbs start-up and the results are non-monotonic. |
| `GOODSHARING_TEST_BUFFER_US=<us>` | Overrides the profile's presentation buffer. | Lets a buffer be measured without changing shipped defaults. |
| `GOODSHARING_TEST_OLD_TIMELINE=1` | Forces the delayed timeline off, restoring the pre-docs/70 behaviour. | The A/B arm for the model change. |
| `GOODSHARING_TEST_DISABLE_PRESENTATION_FLOOR` | Removes the 4 ms minimum spacing between presented pictures. | Isolates cadence from scheduling. |
| `GOODSHARING_TEST_DISABLE_AUDIO_RETRY` | Disables bounded stereo audio resubscription. | Isolates retry from the media path. |
| `GOODSHARING_TEST_NO_UNTIMED` | Refuses pictures that arrive without a presentation ticket. | Restores the pre-docs/68 "drop untimed" behaviour for comparison. |
| `GOODSHARING_TEST_FAST_PAINT` | Skips the artificial paint cost used by the paint-stall case. | Keeps control runs fast. |

## Rules that came out of using them

1. **An impairment must not block an SDK callback.** A sleep in the ingress path
   stalls the SDK thread and the viewer dies; use a queue with a worker.
2. **Engage after warm-up.** Disturbing the first seconds measures start-up, not
   steady state, and produces non-monotonic results that look like noise.
3. **Know which layer you are disturbing.** Above the decoder, no recovery applies;
   below the socket, RED/FEC/NACK do. The same 10% loss means different things in
   the two places.
4. **Report the disturbance in the summary** (`impairment`, `net_impairment`) so a
   run can never be mistaken for a clean one.

## Runner flags worth knowing

--repeat N runs each selected case N times and prints an aggregate line per case
(runs, passes, worst audio quiet, minimum coverage, total holds, worst hold). It is
the cheap way to separate a real regression from a flake: a single green run says
little on a PC that also runs the capture path.

--case all runs every case in one artifact, which is how the 17/17 result in
docs/73 was produced; --case static-all and --case switch-all narrow that.
