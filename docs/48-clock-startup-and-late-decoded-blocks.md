# Faster clock startup and bounded decoded audio phase recovery

Date: 2026-10-23, Europe/Budapest. Tests use owned local windows, generated stereo tones and encrypted loopback transport. Normal UI receiving sound remains disabled. No VPS deployment or unrelated process termination.

## Separate client and server clock-report timing

The new `server-only-500ms` option in `scripts/test_remote_av.py` changes only the isolated SFU report interval. It enables the same bounded diagnostics as the other comparison profiles and requires the server's 500 ms acknowledgment while requiring the client experiment acknowledgment to remain absent. All non-upstream profiles use the same verified SFU binary. The existing `500ms` profile changes both client and server reports.

Six fresh observations use the unchanged checkpoint 47 executable and SDK. Each passes its original media, timing, continuity, encrypted admission and cleanup checks:

| Selected video | Default reports | Server reports at 500 ms | Client and server at 500 ms |
| --- | ---: | ---: | ---: |
| 720p | 6.307 s | 3.843 s | 2.285 s |
| 1080p | 6.298 s | 3.803 s | 2.336 s |

These are first-tone startup times from the local observer, including connection and clock training. They do not measure steady-state end-to-end latency or physical speaker/display synchronization. There is one observation per table cell, so these are a controlled comparison, not a startup reliability distribution.

The SFU uses 0.53–0.90% of this 12-logical-processor machine and 74.1–75.7 MiB peak private memory in the measured middle windows. Receiver and sender measurements, including bounded tracing, are retained in the comparison record. This small set does not establish production overhead, RTCP wire bandwidth or multi-share capacity.

The comparison orchestrator initially fails its final collection step because it reads `sha256` instead of `binary_sha256`. The six individual raw media reports remain green. A corrected summarize-only pass collects those same reports without recapturing. The orchestration note and original media outcomes remain separate.

## Preserve valid output at a small late-source cadence overrun

The subsequent complete fast-profile group passes 20/21 actual cases overall. Its 1080p matrix case fails the unchanged continuous-tone check with a 140.021 ms quiet interval. The full matrix passes 12/13; the six comparison cases and both 58-second cases pass.

The first reset's retained trace reconstructs the next continuous PCM deadline as `3552997143406`, while the same common clock maps that incoming block to `3552997119551`. The 23.855 ms correction exceeds the existing 20 ms admission limit. The preceding queued tail remains 9.342 ms in the future, video arrived 4.999 ms earlier, and the event is an audio-ingress reset. This proves the first cadence-fence reset in this failed observation; it does not attribute every quiet sample or previous failure to that cause.

A deterministic reproduction stretches each decoded block's source advancement to 425 RTP ticks while preserving its 480 output samples, then resumes ordinary advancement. Previously crossing the correction fence flushes valid queued PCM and resets the paired video clock. Its before-control fails with `Late decoded block flushed valid PCM and the paired video clock`.

The new recovery discards only the incoming 10 ms decoded block when all of these hold: playback has committed media and locked alignment; no reset is pending; valid PCM remains queued; the last admitted tail deadline is more than the existing 4 ms write guard into the future; the input is contiguous with fresh, valid source timing; and its positive phase correction exceeds 20 ms but is at most 30 ms. Startup, expired tails, source discontinuities and the opposite correction direction retain the reset behavior.

The discarded block is never queued, retried or assigned a replacement timestamp. Existing queued PCM samples, their deadlines, the common clock and video tickets remain in place. The previous unconsumed block's source duration is resolved from the incoming valid source position, as with ordinary admission, and is not stretched across the discarded content. Tracking advances past the dropped input, while the last admitted deadline and arrival remain unchanged; discards cannot extend residence or postpone device output.

Admitted timing correction remains at most 20 ms. The separate 30 ms diagnostic bounds the phase offset of rejected input, not added buffering or an admitted correction. One block's content is lost and may produce a brief audio discontinuity; the recovery avoids flushing otherwise valid output.

## Keep the original deadline when audio source time advances ahead

The positive-only candidate passes 25/29 subsequent actual cases, including its complete 13-case fast matrix. Default static playback still hits the opposite cadence fence: the incoming source clock is 22.709 ms ahead of the continuous output phase, causing a 120.021 ms quiet interval. Default switching and cancellation and the longer fast cancellation also fail. Those three observations retain endpoint interruptions, with presentation expiry or sustained low headroom in the default cases; their quiet intervals reach 508.583 ms, 159.333 ms and 43.271 ms respectively. These endpoint/presentation failures are not established as fixed by either cadence recovery.

The opposite-direction native before-control fails with `Ahead decoded block flushed valid PCM instead of keeping its original deadline`. When the same running/queued/future-tail conditions hold and the correction is at most 30 ms, the new input can instead retain its nominal common-clock deadline. This is allowed only when that deadline is at most 50 ms after arrival, leaving the existing 60 ms residence budget for its complete 10 ms block. The already queued audio and every video ticket keep their deadlines. Ordinary rendering emits silence in the bounded interval before the new audio's original deadline.

The native reproduction verifies an exact 22.708 ms silent interval, every retained and new PCM sample across a split output packet, uninterrupted clock serial, later ordinary audio and the unchanged 20 ms admitted correction limit. Another running control requires reset when the new deadline would exceed the residence budget. Startup, expired-tail and earlier missing-block/invalid-clock controls remain enforced. Both corrections add four diagnostic scalars, 32 bytes in total; the fixed scheduling object reaches 12,176 bytes. There are no new allocations, buffers, workers or dependencies. SDK and reporting defaults remain unchanged.

## Current build validation

All 22 native tests pass on the final build. The final actual local group passes **14/18 cases**: the full fast matrix passes 10/13; targeted default static and default cancellation pass; targeted default switching fails; both 58-second fast static 1080p and cancellation runs pass. All these observations explicitly keep the owned viewer visible above other windows. This is a more explicit visible-load condition than checkpoint 47's ordinary matrix, and its historical green group is not validation of this changed executable.

The four current failures remain open:

| Profile and case | Observed failure |
| --- | --- |
| Default switching | First tone at 27.274 s; the quality handover probe does not complete. After startup its longest quiet interval is 4.396 ms. |
| Fast 1080p | 900.708 ms quiet interval and missing final composited marker; endpoint interruption and presentation expiry are recorded. |
| Fast static | 47.438 ms quiet interval, above the unchanged 40 ms limit; one bounded 20.062 ms phase gap and one endpoint interruption are recorded. |
| Fast switching | 22.900 s quiet interval, missing stereo/marker checks; endpoint interruption, presentation expiry and later unclocked input are recorded. |

Every non-wrong-key final fast observation starts its tone within 1.806–2.368 s. This startup improvement does not establish continuous playback. The two current wrong-key cases retain zero committed media and zero drawn video. Actual source-A/V, picture wait/advance, PCM/image capacity and cadence correction bounds pass the audit even in the failed sessions: stale or uncertain media is withheld rather than used to cover a gap.

The real nominal-deadline gap branch is observed once at 20.062 ms, with no cadence-fence reset in that case; the complete session still fails continuity. No late-block discard is observed in the final actual group. The deterministic controls execute both branches and their ineligible cases. Current results therefore do not establish that every earlier live gap is fixed or that the new recovery caused improvements in other sessions.

The final audit retains 37 reader and 36 encoder traces without truncation, all 18 complete stopped histories, and 13 compact live snapshots. Maximum measured live snapshot cost is 976 microseconds. The final process audit finds zero project media apps/servers and stops nothing. The complete increment retains 68 actual observations across three builds/groups; only the final 18 validate the final executable. Earlier failed outcomes, the positive-only executable/source archive and checkpoint 47 evidence remain unchanged.

The final manifest and source/binary archives use `20261023-clock-phase` in `artifacts/`; raw outcomes and the open-failure index use `clock-phase`. These are development evidence, not a releasable demo. Normal UI audio remains disabled.

## Reproduction and remaining work

```powershell
C:\Users\Hello\MiniConda3\python.exe scripts/test_remote_av.py --case 1080 --clock-profile server-only-500ms --trace-timing --visible-view
C:\Users\Hello\MiniConda3\python.exe scripts/test_remote_av.py --case all --clock-profile 500ms --trace-timing --visible-view
```

The faster reporting mode remains an explicit local test profile. It currently requires the SDK's test trace flag, and its isolated SFU is not yet a redistributable server artifact. A production reporting policy must be independent of diagnostic tracing and include the pending Go dependency notices.

Next reproduce the drained-endpoint/refill boundary and its two time estimates before changing recovery. For example, a final 1080p failure reports device position `2952396` at 192000 ticks/s and `738099` submitted 48 kHz frames: those are exactly equal sample positions. Its first reported deficit is one microsecond, while the software's committed-end estimate remains 4.651 ms in the future. This is an observed measurement disagreement, not proof that the physical dropout lasts one microsecond or that the underrun is false. A recovery must retain the existing stale-media, source-A/V and final permission gates. Separately trace the delayed first usable clocks and the decoded/presented video freshness gaps under a visible viewer.

Normal UI audio, physical synchronization, separate Windows PCs, Internet congestion, sustained multiple shares, game coexistence and whole-app security acceptance remain open. Mumble remains the voice client.
