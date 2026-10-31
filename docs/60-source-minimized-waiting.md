?# Minimized source: tell viewers instead of freezing

Date: 2026-10-28, Europe/Budapest

Status: **behaviour implemented and verified; audio and video both stay
continuous through a minimized source**

When the shared window is minimized, Windows Graphics Capture stops delivering
frames (measured: 0 frames while minimized, 786 frames while merely covered). The
share was never stopped — there was simply no video — but viewers saw an
unexplained frozen picture. Now the sender detects the transition, tells the room,
and viewers show **"User minimized app"** while the share, audio and encryption
stay alive; restoring the window resumes the same share.

## What changed

1. `src/screen_capture.{h,cpp}` polls `IsIconic` for window sources (monitor
   captures have no minimized state) every 50 ms and reports transitions through a
   new `SourceState` callback, without touching the capture session — so the same
   session resumes when the window is restored.
2. `src/capture_publisher_input.{h,cpp}` forwards that callback and records
   `source_minimized` in its report.
3. `src/preview_backend.{h,cpp}` marshals the transition to the Qt owner, calls
   `RoomController::publishShareSourceState`, updates the sender status to
   `Sharing · source window minimized; viewers are waiting`, and tracks
   `shareSourceMinimized_` for `sharingReport()`.
4. `src/room_controller.{h,cpp}` adds `publishShareSourceState(bool)` (bounded
   command on the transport thread) and a delegate `onUserPacketReceived` handler
   that accepts only topic `gs-source`, validates `{share_id, source_minimized}`
   and emits `sourceStateChanged(owner, shareId, minimized)`.
5. `src/share_publisher.{h,cpp}` publishes a reliable data packet on topic
   `gs-source`; a failed packet is ignored (advisory state never revokes a share).
6. `src/preview_backend.cpp` sets `remoteSourceMinimized_` for the watched share
   and exposes `remoteStatus_ = "User minimized app · waiting for it to be
   restored"`; the generic stall text no longer overwrites that notice. Both
   reports carry `source_minimized`.
7. `ui/Main.qml` overlays the remote surface with `User minimized app` and
   `Waiting for them to restore it…` while the flag is set.
8. `tools/media_fixture.cpp` gained `--minimize-file` / `--restore-file`, and the
   owned test tokens now grant `canPublishData` (`scripts/local_media_fixture.py`).
9. `scripts/test_remote_av.py` has a new `minimized` case: the fixture is
   minimized after the first checkpoint and restored after the second.

## Measured behaviour

`artifacts/20261028-185804-minimized-overlay/viewer.png` is the rendered result:
the remote area shows **`User minimized app`** with `Waiting for them to restore
it…`, the status line repeats it, `Stop viewing` and the room connection stay
active, and the share is still watched (`source_minimized=true`, `watching=true`
in that run's report). The capture was taken while the source stayed minimized
until the viewer's screenshot, using
`artifacts/verify-minimized-overlay.py`.

`artifacts/20261028-185521-remote-av/minimized/summary.json`, checkpoint interval
12 s / 14 s / 17 s:

| At checkpoint | source_minimized | status | presented video | committed PCM |
| --- | --- | --- | --- | --- |
| 12 s | false | Watching encrypted audio/video | 291 | 265,440 |
| 14 s | true | **User minimized app · waiting for it to be restored** | 300 | 336,959 |
| 17 s | false | Watching encrypted audio/video | 456 | 474,250 |

- the picture stops advancing to within the minimize latency: 9 frames (< 0.35 s)
  between the first two observations, 156 frames after the restore;
- the share is never stopped: the watch stays open, `viewing` remains true and
  playback is still anchored at the last observation;
- the sender keeps capturing audio while minimized
  (`audio_blocks_admitted` 1,935 = 19.35 s of audio), so nothing is paused
  sender-side;
- every feature check passes: `minimized_state_observed`,
  `viewer_says_user_minimized_app`, `picture_frozen_while_minimized`,
  `picture_resumed_after_restore`, `view_alive_after_restore`,
  `sender_capture_healthy`, `sender_audio_continued`, `audio_timeline_recovered`.

## Audio continuity while minimized (fixed)

The first version failed the project-wide continuity gates: longest audio quiet
was 652 ms in one run and 840 ms in another. The traced first fault (kind 1, the
audio admit path) showed `serial_before=3 serial_after=4`, `was_anchored=true`,
`was_alignment_locked=true`, `clock_unavailable=false`, and an audio sender time
1.33 s ahead of the last video sender time.

Cause: only `AudioPlayout::video()` advanced the common origin
(`anchorSender_`/`anchorLocal_`). With video intentionally absent, the audio clock
kept advancing while the origin stayed put, so the bounded delta in the audio path
eventually exceeded its 2 s limit and invalidated a timeline that was still
perfectly contiguous - one device rebuild, one audible gap.

Fix in `src/audio_playout.cpp`: with `audioContinuity_` enabled, contiguous audio
now advances the origin exactly as video does (same 1 s threshold, same
local<->sender mapping, deadlines untouched), and the transition is counted as
`audio_anchor_advances` in the playback report. No bound was widened and no
queue/latency limit changed.

Result on the same case (`artifacts/20261028-191843-remote-av/minimized`): every
check passes, longest quiet **0.02 ms**, frame coverage 1.0, quiet fraction 0.79%,
committed PCM 262,512 -> 358,512 -> 502,512 over the same checkpoint interval
(exactly 2.0 s of audio per 2 s interval), `audio_anchor_advances=2`,
`device_resets=4` instead of 7.

## Full matrix on the resulting binary

`--case all` on app hash `37F4B668FD5FA3E38EAAB39A22465CBBC2AE847551C4EF5736F72B41DC09EC20`
(`artifacts/20261028-191925-remote-av`): **14 of 15 cases pass** - 720, 1080,
source-exit, paint-stall, static, static-1080, static-resume, static-source-exit,
static-wrong-key, minimized, switch, switch-cancel, switch-source-exit and
switch-timeout.

| Case | Frame conversion | Admitted | Replaced | p95 draw | Longest quiet | Coverage | Gap resets |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 720 | 5.87 ms | 1139 | 0 | 14.5 ms | 0.29 ms | 1.000 | 0 |
| 1080 | 6.50 ms | 1156 | 0 | 14.7 ms | 0.02 ms | 1.000 | 0 |
| minimized | 5.06 ms | 1026 | 0 | 13.4 ms | 7.06 ms | 1.000 | 0 |
| switch | 5.36 ms | 2233 | 0 | 16.4 ms | 3.79 ms | 1.000 | 0 |
| static-1080 | 5.05 ms | 1151 | 0 | 15.3 ms | 0.02 ms | 1.000 | 0 |
| paint-stall | 5.08 ms | 1150 | 0 | 13.9 ms | 0.02 ms | 1.000 | 0 |

Every passing case reports zero endpoint-gap resets and full live-window coverage;
the source-exit cases stop the tone by design, so their quiet interval covers the
post-exit window instead.

The first full run failed `wrong-key` on `owned_apps_exit` only: the sender's
`capture_was_active=false` and `room_ready=false` at the end, with the badge still
`PREPARING SHARE`, i.e. it exited before its own publication became active while
the wrong-key viewer was already revoked. The same case then passed twice in
isolation (`artifacts/20261028-192604-remote-av`,
`artifacts/20261028-192628-remote-av`), so it is recorded as an intermittent
sender-readiness race rather than a deterministic failure. The failed run is
preserved.

## Files touched

```text
src/screen_capture.h, src/screen_capture.cpp
src/capture_publisher_input.h, src/capture_publisher_input.cpp
src/share_publisher.h, src/share_publisher.cpp
src/room_controller.h, src/room_controller.cpp
src/preview_backend.h, src/preview_backend.cpp
src/audio_playout.h, src/audio_playout.cpp
src/remote_audio_playout.cpp
ui/Main.qml
tools/media_fixture.cpp
scripts/local_media_fixture.py, scripts/test_remote_av.py
```

## Verification

- Native tests: **22/22 pass** (`artifacts/native-tests-after-audio-hold.log`,
  `artifacts/libyuv-native-tests.log`).
- `--case minimized`: every check passes, including the two project-wide audio
  continuity gates (`artifacts/20261028-191843-remote-av`).
- Full matrix: 14 of 15 pass, the remaining `wrong-key` exit is a preserved
  flake that passed twice in isolation.
- Environment note: the workspace was relabelled to Medium integrity
  (`icacls <workspace> /setintegritylevel (OI)(CI)M /T`) because a Low label on
  `build` alone left Qt's own tools (still Low in `.deps`) unable to write build
  output; see [docs/59](59-capture-conversion-libyuv-critical-path.md).

