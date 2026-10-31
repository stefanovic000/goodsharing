# Bounded earlier video presentation

Date: 2026-10-23, Europe/Budapest. Local project-owned fixtures and generated stereo tones. Normal UI sound remains disabled; no VPS deployment. The installed LiveKit SDK, capture timestamps, cryptors and endpoint seed/wait policy are unchanged.

## The reproduced presentation hole

The resumed compact-reporting build passes 23/24 actual integration cases, indexed in [checkpoint 45](45-device-and-source-horizon-recovery.md). Its failed short cancellation observation retains an 80 ms presentation-age expiration as the first fault. Four valid, modestly early pictures are discarded, leaving approximately 92 ms between admitted source positions. Audio then reaches the existing source horizon and the stale-presentation fence. Later device resets compound the interruption. The publisher remains active without a recorded publication failure. This explains a specific scheduler failure in that observation; other historical gaps and startup failures remain distinct.

The native before-control fails with `Modestly early pictures were discarded instead of scheduled within the existing wait bound`. It represents consecutive early pictures, rather than assuming that the next picture immediately fits the old admission limit.

## Separate the picture's display target from the PCM timeline

After a real media commit locks alignment, a valid selected video frame whose common-clock target is 80–100 ms beyond receipt can receive an earlier display target: receipt plus 80 ms. The advance is strictly positive and at most 20 ms. The original source clock and mapped target remain in its presentation ticket. Every existing queued ticket retains its original target. Audio capture timestamps, queued PCM deadlines, clock epoch and reset serial remain unchanged.

Ticket validation requires the unchanged mapping, the validated source clock/epoch, the original bounded receipt time, the exact display target and the maximum 20 ms advance. Startup cannot use this path. Clock discontinuities beyond its bound, changed epochs, expired video and lost presentation still face the existing timing fences. Candidate quality warmup retains its original projection rules; an earlier active picture cannot adopt the candidate.

The common-clock target can therefore be up to 100 ms beyond receipt for this running path, while the scheduled picture wait stays at most 80 ms. Diagnostics report those two quantities separately. The integration checks explicitly require mapped lead no greater than 80 ms plus the observed bounded advance, scheduled wait no greater than 80 ms, and advance no greater than 20 ms. This changes how a slightly early picture is scheduled; it does not expand a media queue or the actual-source A/V admission horizon.

Every PCM commit still checks actual source positions against the last queued picture within plus/minus 80 ms and holds final paired permission. Drawing and frame-queue acknowledgment still check the selected video and audio roles. A native Qt control draws a real generated RGBA image on the earlier target, requires no presentation acknowledgment before the frame queue, and verifies that revoking audio between drawing and queuing prevents acknowledgment and subsequent pixels/output.

## Keep long-running clock arithmetic bounded

The first candidate passes the initial 25-case integration group, but none of those runs observes the new earlier-presentation counter. Those passes are retained as intermediate observations. Review identifies a missing origin-rebasing operation in the new running branch. A ten-second virtual-time control fails with `Sustained earlier pictures failed to rebase the unchanged common timeline`.

The corrected branch moves the clock origin using the existing algebraic translation, preserving every mapped deadline and source position. The sustained control then requires unchanged targets and reset serial across more than 600 consecutive earlier pictures. The standalone core additionally checks the exact 20 ms boundary, unchanged queued PCM and exact stereo output, startup denial and the larger-jump fence. Ticket controls reject altered source, epoch, receipt, display target, mapped target and extreme integer values. The adapter/Qt controls retain paired final-permission checks during a staged quality change and revocation.

The scheduler remains allocation-free and is 12,144 bytes, an increase of 24 bytes from checkpoint 45. Each presentation ticket adds one 64-bit mapped-target field. No additional image buffer, worker, polling handle or runtime dependency is introduced. The six-block/60 ms PCM bound, six pending image slots, four-millisecond audio write guard and device lead limits remain unchanged.

## Verification status

The corrected executable builds successfully and passes all 22 native tests, including the new sustained and Qt controls. It passes **24/25 actual local integration cases**: the A/V matrix passes 12/13, the separate short cancellation and both 58-second observations pass, endpoint checks pass 5/5, ordinary video passes 2/2 and controlled native-expiry checks pass 2/2. The initial 25/25 candidate group remains intermediate evidence and is not counted for this corrected executable.

The final static 1080p observation fails the unchanged 40 ms tone-gap limit, with a 113.229 ms longest quiet interval and 99.04% media-window coverage. It records 132 earlier tickets and 119 actual earlier presentations, up to 19.942 ms, with scheduled wait at most 80 ms. Thus the path executes on real encrypted video, but does not fix every presentation-age interruption. Its first fault is an 80 ms presentation-age expiration without an endpoint discontinuity. The video queue reports nine discarded pictures. The GUI reports a maximum 215 ms heartbeat gap, whose time is not retained, so that scalar cannot be assigned as this fault's cause.

Across the final observations, all sixteen stopped reports include history; eleven running snapshots are compact. The independent trace audit retains 32 reader and 32 encoder traces, verifies their bounds and original outcomes, and records 21 source-horizon silence frames and three device-only recoveries. Maximum observed live snapshot cost is 1.152 ms. Both native-expiry cases observe one rejected old block and continued fresh publication; the wrong-key case decodes no video. The final process audit finds zero project executables. No unrelated application is stopped.

The next investigation is a bounded-queue behavior: dropping the oldest future picture when full may discard the very picture due next. A separate deterministic reproduction is required before changing that policy. This remaining failure is retained; receiving sound is not enabled in the normal UI. Results, source/binary archives and the verification index use the `20261023-video-presentation-advance` prefix in `artifacts/`.

Normal UI receiving sound remains disabled. Physical speaker/display synchronization, long drift, two separate Windows PCs, sustained multiple shares, Internet congestion, game coexistence and whole-app security acceptance remain separate work. Mumble remains the voice client.
