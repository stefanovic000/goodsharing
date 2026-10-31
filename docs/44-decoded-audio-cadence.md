# Preserve decoded audio cadence within bounded source timing

Date: 2026-10-23, Europe/Budapest. Local owned fixtures only. Normal UI audio remains disabled pending complete continuity acceptance. The installed SDK, source capture timestamps, encoder/forwarding path, encryption gates and WASAPI device policy are unchanged.

## A failing control before the repair

The preceding checkpoint's diagnostic becomes a strict native control with `goodsharing_audio_stride_probe --require-continuous`. On the old scheduler it fails with `Variable source advancement inserted silence or trimmed decoded PCM`. The nominal 480-sample source advance passes, while the next 698-sample case fails. `artifacts/decoded-cadence-before.log` preserves that expected failure.

Both 698- and 262-sample source advances had been observed in actual decoded audio, along with even smaller advances. Decoded stereo blocks contain 480 samples per channel and occupy 10 ms on the 48 kHz output device. Applying a second source-timestamp spacing adjustment to this already-adapted PCM creates artificial gaps or trimming.

## Separate output duration from source advancement

Each queued PCM block now retains its original source timestamp and a separate receiver output deadline. In coordinated A/V mode, coherent consecutive decoded blocks occupy consecutive 10 ms device intervals. This uses the preceding scheduled block's deadline, never callback arrival as a replacement capture timestamp. Video's source/local mapping, pending deadlines and presentation-ticket serial remain unchanged.

Continuity requires the same trained audio epoch, positive RTP advancement below 960 samples, and a preceding accepted block received within the existing 60 ms residence bound. Within the same sender-report reference, the source-clock advance must agree with RTP to within 1 ms. A later valid report may refine that reference; the mapper already validates its slope and quantization, and the resulting audio correction remains capped at 20 ms. Regressing report counts cannot claim refinement. An advance of 960 samples or more may represent missing whole decoded blocks, so it is not stitched. Incoherent clocks also retain explicit discontinuity handling. A nominal fallback that would overlap the previous output interval is fenced.

The absolute correction from the original source-derived deadline is capped at 20 ms. Exceeding that cap clears PCM, invalidates the shared presentation timeline and requires normal fresh-clock/presentation recovery. It cannot accumulate an unbounded offset. Startup alignment rejection, clock/epoch loss, Stop and revocation clear the decoded-continuity state. The six-block/60 ms PCM queue and residence limit, 150 ms video lifetime, 80 ms video-lead/A/V bounds and 15/30 ms device fill/lead bounds are unchanged.

## Retain source-based output admission

Contiguous output does not make source timing or encryption optional. The renderer reports the source positions represented by emitted PCM. For an unstarted queued block with a known coherent successor, it estimates positions between their original source timestamps; otherwise it retains nominal intra-block sample spacing. A partially rendered block's estimate is not revised. These are software estimates, not physical synchronization measurements.

The adapter preserves its existing nominal A/V guards and additionally limits actual staged PCM source positions to the last presented video's plus/minus 80 ms window. A permitted source prefix can be committed while its future suffix remains queued. A block behind the source horizon invalidates the timeline and flushes. The final paired owner/SID/encryption admission remains at device commit. A native prepare-time MissingKey control verifies that corrected PCM cannot reach the device after revocation.

Splitting an output packet exposed a separate representation edge: integer microsecond timestamps for 48 kHz samples can disagree by one microsecond because truncation is not additive. The renderer now treats this one-microsecond difference as timestamp rounding rather than inserting a whole silent sample. It adds no buffering or network-jitter allowance. The intermediate failed native control and successful repair remain recorded separately.

## Native verification

All 22 native tests pass, including expanded controls within the existing scheduler and final-admission cases. The strict stride probe now preserves all 480 samples in the second block for source advances of 480, 698 and 262, with no synthetic silence or late trimming. Exact stereo sample controls additionally cover 36, 44 and 350-sample advances, RTP wrap, 997 consecutive output blocks over ten virtual seconds with alternating advancement, unchanged video deadlines, a source-limited prefix and exact suffix, stale-source fencing, excessive cumulative correction, missing whole blocks, inconsistent clocks, fresh recovery and terminal revocation. This is generated virtual-time verification, not a hardware soak or acoustic-sync measurement.

The core remains allocation-free and fixed-capacity. Its native `sizeof` is 12,112 bytes, an increase of 168 bytes from the preceding 11,944-byte core. New counters expose adjusted blocks, discontinuities, correction-limit resets and maximum applied correction.

The cadence reset counter covers both the 20 ms correction cap and a discontinuity whose nominal fallback would overlap the preceding block. An aggregate reset count cannot identify which guard caused a particular first playback fault; a maximum applied correction below 20 ms does not prove the cap was reached. Queued-video ticket validation remains based on its unchanged mapping, independently of these audio-source estimates.

## Actual integration results

The first candidate passes 10/17 actual cases, including ordinary 720p and 1080p A/V, but its longer static-to-motion run has a 93.271 ms quiet interval. Its first fault occurs when the RTP advance remains 480 samples while the mapped NTP advance changes to 8,501 microseconds at sender report 15. The initial 1 ms consistency check mistakes this 1.499 ms report-reference refinement for incoherent PCM timing and fences the stream. A new exact reproduction fails that candidate before the repair. The corrected rule distinguishes a new valid report from disagreement within one unchanged report reference, without changing the correction cap, buffer bounds, source horizon or crypto admission. The initial candidate's binary, source, tests and analyses remain separately retained and are not counted as final passes.

The final sender-report-aware candidate passes **11/17 integration cases**, separately from the first candidate's 10/17:

| Final suite | Passed |
| --- | ---: |
| Complete A/V matrix | 8/13 |
| 58-second cancellation | 0/1 |
| 58-second static-to-motion | 1/1 |
| Normal video-only 720p and 1080p | 2/2 |

Ordinary 720p and 1080p A/V pass all checks, with longest quiet intervals of 6.479 ms and 1.896 ms. Both wrong-key cases, ordinary source closure, paint-stall recovery, short cancellation and closure during a warming switch pass. Remaining short-suite failures are static 720p, static 1080p, static-to-motion, static-source-close readiness and the round-trip quality switch. The static-to-motion sender reports `Media publication failed; share stopped`; the static-source-close case never reaches established A/V before the unchanged deadline. These remain failures.

The final longer static-to-motion run passes with an 8 ms longest quiet interval. The longer cancellation run fails only whole-session tone continuity, with a 189 ms longest quiet interval and an endpoint discontinuity as its first retained playback fault. A passing run can still contain bounded recovery: the successful long static-to-motion observation also records an endpoint discontinuity. It does not prove interruption-free output or physical synchronization.

Actual controls check 27 reader traces and 28 encoder traces without truncation. They observe 18,789 blocks using bounded cadence adjustment. Maximum applied correction is at most 18.418 ms in these observations, and no final run records a cadence-fence reset. Thus the retained failures are not counted as successful fixes, and these runs do not assign every remaining gap to cadence correction. There is no claim of a statistically established reliability improvement from comparing two small, intermittent matrices.

The [validation manifest](../artifacts/20261023-decoded-cadence-validation.json), [source archive](../artifacts/20261023-decoded-cadence-source.zip) and [binary archive](../artifacts/20261023-decoded-cadence-binaries.zip) preserve both candidates, both failing-before controls, native verification, all real outcomes and unchanged preceding evidence/SDK DLLs. The final process audit is read-only and finds no workspace executable still running.

Next distinguish SDK publication expiry/rejection from ambiguous asynchronous callback timeouts, retain the first structured source failure, and trace RTP transmission/reception/decoding around the successful-handoff/decoded-delivery interruption. Do not retry an ambiguous completed operation or weaken encryption, freshness or latency gates. Endpoint underrun recovery and repeated sustained A/V/quality-switch acceptance remain open before normal UI audio is enabled. No VPS service was deployed and no unrelated application was closed.
