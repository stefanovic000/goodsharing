# Device recovery and source-horizon waiting

Date: 2026-10-23, Europe/Budapest. Local owned fixtures and generated audio only. This checkpoint does not enable normal UI audio or deploy the VPS. The installed LiveKit SDK and its capture/receiver clock patches remain unchanged.

**Resumed local verification:** the compact-live-reporting build passes 22/22 native checks and 23/24 actual integration cases. A shorter cancellation run still fails continuity; normal UI sound remains disabled. The earlier pause snapshot is preserved separately.

## Preserve the first publication failure

The publisher retains a bounded, immutable first failure with an application-owned operation/code, height, local time, original capture time/age, operation duration and submitted-audio count. It never copies arbitrary SDK exception text into a report. Known encoder expiry/backlog/recovery, video rejection/response failures, permission loss and ambiguous audio callback timeout have distinct codes. Normal stop/revocation still closes the gate. This improves diagnosis; an unobserved earlier generic failure is not retrospectively assigned a cause.

Test-only, nonblocking, one-second RTP statistics now record sender packet/byte/frame progress and receiver packet/byte/assembled/decoded progress. The existing bounded handoff/read traces remain separate. A successful SDK handoff is not equated with RTP transmission, and one-second counter deltas are not packet-arrival timestamps. `scripts/analyze_transport_progress.py` retains original outcomes and separates SSRCs and counter resets; its offline controls pass.

An isolated generated-input encoder-idle probe submits one frame and polls for 350 ms at each quality. Both hardware encoders complete their frame without pending expiry. Thus an encoder waiting for another raw frame is not established as the cause of the historical publication failures, and the existing encoder age/backlog fences are not weakened.

## Confirmed expired audio rejection

The pinned unbuffered native source validates matching 10 ms PCM and the original capture age before calling its sinks. Its exact `InvalidState` rejection can therefore mean that a frame aged past the same 100 ms limit while entering the SDK, after the application's age check.

The publisher now drops that block only when both facts hold: this exact known rejection completed, and the original capture timestamp is now more than 100 ms old. It continues with a different fresh block, with normal permission checks. It does not retry the rejected block. A callback timeout, unknown error, future/invalid timestamp or unexpired rejection still stops publication. The counter `native_expired_audio_rejections` distinguishes this case from application-side expiry. Native controls retain the exact 100,000/100,001 microsecond boundary and reject timeout/unknown/future attempts to use this policy. The real native source test verifies the known completion and that rejected input never reaches its sink.

A publisher-only controlled 140 ms delay after the application age check exercises the actual native rejection and later fresh publication, with a separate wrong-key control. Final results and counters are recorded in the validation manifest.

## What the device failure actually measured

The diagnostic long cancellation run fails with a 140.083 ms longest quiet interval. Its first endpoint snapshot records zero padding, 236,218 submitted frames, device position 944,872 and frequency 192,000. Both counts represent the same stream position. The reported lead of minus one microsecond comes from extrapolation just beyond that position; it is not a measurement of a one-microsecond audible outage. The device buffer really is empty.

Microsoft documents the device position together with its frequency and correlated QPC value, while padding counts queued frames. The new first-discontinuity snapshot preserves these measurements independently. See [IAudioClock::GetPosition](https://learn.microsoft.com/en-us/windows/win32/api/audioclient/nf-audioclient-iaudioclock-getposition) and [IAudioClient::GetCurrentPadding](https://learn.microsoft.com/en-us/windows/win32/api/audioclient/nf-audioclient-iaudioclient-getcurrentpadding).

That diagnostic's source keeps publishing, and sampled RTP send/receive/decoded counters progress near the failure. This narrows the observation but cannot rule out shorter gaps between one-second samples. A same-build longer run with only the owned viewer kept visible passes; one comparison does not establish occlusion as the universal cause.

## Reset the short device failure without resetting valid video

Previously every recoverable device underrun also cleared PCM, invalidated the common timeline and discarded queued video tickets. The native before-control fails with `Short device gap discarded a valid common timeline`.

For an already running, aligned stream, a short device failure now flushes the physical endpoint while preserving a valid common source mapping and its output high-water mark. Preservation additionally requires the previous output end within plus/minus 30 ms of now, video ingress within the existing 150 ms lifetime and a presentation within 80 ms. The endpoint's clipped negative lead alone cannot establish freshness. Longer output absence still invalidates the full timeline. Startup, missing/invalid clocks, stale presentation, source/A/V fences, failed physical reset and crypto revocation keep their existing handling.

No pending PCM is assigned a new timestamp. Rendering trims late samples against their original output deadlines; it cannot replay them after device restart. Native controls check an actual physical reset, unchanged ticket/deadline/serial, exact fresh PCM after discarding an old block, and zero commits after prepare-time MissingKey. Another control presents a one-microsecond reported gap after more than 30 ms actual output absence and requires the full timeline fence. The first-fault trace now also retains a device-only recovery when its serial remains unchanged.

The first real longer run with this repair still fails: its 42.021 ms longest quiet interval exceeds the unchanged 40 ms requirement, and device resets during cancellation violate the existing preservation check. Both observed underruns preserve the timeline. The longer static-to-motion case passes. These initial outcomes are retained separately from the final candidate.

## Do not starve a due device while waiting for a picture

The preceding trace shows ready PCM waiting at the actual-source A/V horizon after a permitted prefix was committed. Repeated zero-output renders leave the endpoint empty. A generated virtual-time reproduction fails with `Source horizon wait starved a due endpoint instead of bounded silence`.

The running renderer now keeps the future output window open until the existing four-millisecond write guard. If the actual PCM source is still ahead of the last presented picture, that due window contains silence only. The PCM stays queued; a later render trims anything whose original deadline has become late and emits only fresh samples inside the source horizon. A valid prefix remains a prefix, rather than filling its future suffix prematurely. Startup cannot use this behavior before a successful media commit.

The adapter still bounds the window's nominal A/V position before rendering and checks actual staged source positions afterward. Every silence or media device commit still holds the final paired permission lease. This does not authorize out-of-horizon PCM, stale video, untrained timing or output after revocation. It adds neither a queue nor delay to the source timeline. The 60 ms PCM capacity/residence, 80 ms A/V horizon, 20 ms cadence correction cap, 15 ms normal device fill target, 30 ms hard device lead and existing physical startup seed remain unchanged.

Native controls require waiting before the guard, exact zero output at the guard, unchanged video timing/serial, exact fresh stereo after late PCM is discarded, and no startup bypass. `source_horizon_silent_frames` distinguishes this deliberate running silence from admitted media. The allocation-free scheduler grows by eight bytes to 12,120 bytes. The source-position arithmetic now uses an explicit signed divisor, removing its prior signed/unsigned comparison warnings.

## Verification and remaining acceptance

The next candidate passes 23/24 integration cases, including both longer sessions, all five standalone endpoint cases, normal 720p/1080p video and both controlled native-expiry cases. Its short static-to-motion case still has a 40.021 ms quiet interval, exceeding the same 40 ms limit. It records 29 real source-horizon silence frames and three device-only recoveries across those observations. These runs establish that both paths execute, but do not attribute every pass or gap to either repair.

A shortened, 15 ms silent seed after device restart is rejected. The real mute/unmute control drains all 720 seeded frames before any media commit on that restart; non-coordinated playback correctly fails closed. Its longer cancellation case also fails. Only this owned experiment's command sequence is canceled; the incomplete static-to-motion case and its raw files remain preserved without being counted as a completed result. The original full driver seed is restored for every start/reset.

A further experiment restores the original seed and waits on the actual device event plus a per-worker high-resolution timer. Microsoft describes the timer flag for short expiration delays in [CreateWaitableTimerExW](https://learn.microsoft.com/en-us/windows/win32/api/synchapi/nf-synchapi-createwaitabletimerexw). Its five standalone endpoint cases pass, but its longer cancellation run still fails, with an approximately 1.607-second quiet interval and eleven device discontinuities. Maximum event wait is about 3.963 ms while the first fault's pump gap is 10.468 ms: work or scheduling outside the wait remains relevant. This single run does not establish that the timer caused those interruptions. It also does not justify adopting another timer handle as a continuity fix, so the final candidate restores the original event waiting and full driver seed.

## Keep timing-history serialization outside active playback

The existing session rebuilt the current 128-event trace, the reset ring and the immutable first fault's preceding events every 100 ms on the audio worker. GUI reports could also rebuild that history while holding the session/scheduler locks. After a first fault, repeatedly formatting the retained history adds work to the same worker trying to recover.

Live snapshots now retain scalar diagnostics and bounds without constructing those test-history arrays. Stop/error paths flush output first and then serialize the complete retained history once. Compact reporting never clears or overwrites the underlying rings/first fault. Normal idle waiting, the media scheduler, final admission and sink policy are unchanged by this reporting change. Reports measure maximum live snapshot duration; it is an observed worker-cost diagnostic, not a real-time scheduling guarantee.

A native control checks that compact reporting preserves counters, omits the bulky export, and leaves the complete first-fault history available afterward. Offline timing/recovery analysis now prefers the stopped snapshot, so a compact live snapshot cannot mask the retained failure. Its regression control passes. The final candidate adds no polling handle or expanded media queue.

Final native verification passes all 22 tests, with both failing-before reproductions retained. An intermediate native suite fails the existing capture-age assertion; the final suite and 20 repeated native source checks pass with its original assertion bounds and unchanged SDK clock bridge. The intermediate failure remains unexplained and is not described as repaired.

The preceding source-horizon candidate passes 23/24 actual integration cases. The following precise-timer candidate passes 22/24, with failures in longer cancellation and the short round-trip switch; the timer is not adopted. All preceding raw outcomes remain preserved. The current executable includes compact live reporting with the original endpoint seed/wait policy and has native verification only. Normal UI receiving sound remains disabled until continuity/startup acceptance is resolved. Physical speaker/display synchronization, two separate Windows PCs, sustained multi-share footprint, game coexistence and Internet testing remain separate acceptance work. Mumble remains the voice client. No unrelated application is closed and no VPS service is deployed.

The pause manifest and source/binary snapshots use the `20261023-device-recovery-paused` prefix in `artifacts/`. The completed recovery verification is indexed by `20261023-device-recovery-validation.json`; it retains the failed cancellation case and does not grant release acceptance. The preceding decoded-cadence snapshot remains unchanged.

## Resume handoff

- Current build/native evidence: `artifacts/device-recovery-live-build.log`, `device-recovery-live-native.log`, `device-recovery-live-native-detail.log` (22/22).
- Current implementation: confirmed-expired native audio rejection drops, sanitized first publication failure, sender/receiver RTP progress, bounded device-only recovery, source-horizon silence at the existing write guard, and compact live reports with full history after Stop.
- Native before-controls: `device-recovery-red.log` and `device-source-wait-red.log`. The intermediate source-capture-age failure remains unexplained; original bounds/SDK are unchanged, and the later native suite plus 20 repeats pass.
- Real matrices: `publication-diagnostic-*`, `publication-progress-*`, `device-recovery-*`, `device-recovery-guard-*`, `device-recovery-restart-*` (rejected shorter seed, canceled sequence), and `device-recovery-precise-*` (timer experiment, not adopted). Preserve every outcome.
- First next action: run owned local A/V, longer cancellation/static-to-motion and endpoint checks on the **current** executable. Verify final reports retain `test_history_included=true` while live reports are compact, and inspect `maximum_live_snapshot_us`. Use `artifacts/device-recovery-analysis-batch.py` with the new run prefix; prefer stopped history in the analysis scripts.
- Still open: intermittent startup with only one sender report, remaining actual continuity failures, and unexpected historical publication failures without a reproduced first structured cause. No normal UI audio enablement or VPS deployment yet.

## Resumed compact-reporting verification

The current executable matches the pause snapshot. The completed local set passes **23/24** cases: the full A/V matrix passes 12/13, both 58-second observations pass, endpoint checks pass 5/5, ordinary 720p/1080p video passes 2/2 and controlled native-expiry checks pass 2/2. Actual trace bounds, encryption denial and post-stop silence remain checked. All fifteen final A/V reports include complete history; ten still-running snapshots demonstrate compact export. Maximum observed worker snapshot cost is 824 microseconds. These observations do not establish the reporting change as the cause of any individual continuity improvement.

The failed short cancellation case has 48.37% media-window coverage and a 390.917 ms longest quiet interval. Its first retained failure is an 80 ms presentation-age expiration, after 6,080 media frames. A run of four modestly early video frames is discarded; admitted video deadlines straddle approximately 92 ms of source time. The sender remains active without a recorded publication failure. Later repeated endpoint and presentation resets compound the interruption. The first two sender reports were also late in this run (first tone at 9.440 seconds); prior startup failures remain open.

The final local set retains 29 reader traces, 30 encoder traces, 30 source-horizon silence frames and 23 device-only recoveries. Both forced native-expiry checks observe exactly one rejected old block and subsequent fresh publication, including wrong-key video denial. No shortened seed or high-resolution wait timer is installed. A read-only post-test process audit finds zero project executables; no unrelated process is stopped.

The next investigation is bounded earlier presentation of modestly early video, preserving the common audio timeline and final A/V/permission gates. This report describes the completed compact-reporting candidate; subsequent changes require their own native and actual integration evidence.
