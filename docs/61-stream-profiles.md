# Stream profiles: latency versus smoothness

Date: 2026-10-28, Europe/Budapest

Status: **three profiles implemented, selectable in the UI and verified**

Watching used to expose one hardcoded timing policy. It now exposes a local
preference with three profiles, applied when a watch or share starts, and persisted
between sessions.

## Profiles

| Profile | Common lead | Video wait bound | Presentation floor | Sender capture age |
| --- | ---: | ---: | ---: | ---: |
| Low latency | 25 ms | 80 ms | 10 ms | 120 ms |
| Balanced (default, previous behaviour) | 30 ms | 80 ms | 10 ms | 150 ms |
| Smooth | 50 ms | 80 ms | 10 ms | 250 ms |

The lead is the common audio/video target: how far ahead of the mapped sender
timeline playout sits. Larger values absorb jitter at the cost of delay. The
sender capture age is how old a captured frame may be before the publisher drops
it as stale; a larger budget trades freshness for fewer drops under load.

## What changed

1. `src/stream_profile.h` - `StreamProfile` enum, the timing table above and a
   parser for `low`/`balanced`/`smooth`.
2. `AudioPlayout` - the lead and video wait bound became runtime members with a
   validating `configureTiming()`; the previously hardcoded 80-100 ms windows and
   30 ms anchor lead now read them. Defaults keep the old behaviour.
3. `RemoteVideo` - the 10 ms presentation floor is settable before binding.
4. `RemoteAudioPlayout` / `RemoteAvSession` - forward the timing before binding.
5. `SharePublisher::Config` - `maximumCaptureAgeUs` replaces the hardcoded 150 ms
   capture-age checks and the encoded handoff budget.
6. `PreviewBackend` - `streamProfile` property plus `setStreamProfile()`, persisted
   with `QSettings`, and a `GOODSHARING_STREAM_PROFILE` override that wins for
   tests. The profile is applied to each new watch (lead, wait bound, floor) and
   to each new share (capture age).
7. `ui/Main.qml` - a `Low latency / Balanced / Smooth` selector next to the
   quality choice.
8. `scripts/test_remote_av.py` - `--profile` flag plus dedicated `low` and
   `smooth` cases; the smooth case uses profile-aware bounds (video lead up to
   130 ms, media fill at least 0.90) because its larger lead deliberately buffers
   more.
9. The playback report now exposes `target_video_lead_ms` and
   `video_presentation_wait_limit_ms` from the configured values instead of
   constants, so a run proves which profile it used.

## Tuning evidence

- **20 ms lead rejected.** The first low-latency value left too little runway:
  668 frames were admitted but only 61 presented, with 31 early drops - frames
  arrived with under 10 ms of runway and were refused. 25 ms presents normally.
- **60 ms smooth rejected.** With a 60 ms lead the first media commit was
  intermittently delayed: 2 of 4 runs wrote 2.2-2.6 s of silence at startup
  (106k-124k silent frames, media fill 0.78-0.81, `presentation_waits` 214).
  50 ms is stable: 3 of 3 runs, silent frames 1, media fill 1.000, waits 13.
- **Video wait bound kept at 80 ms** in all profiles. Lowering it starved
  presentation (it bounds how early a scheduled picture may be held), so the
  profile varies the lead and the sender budget instead.

## Verification

- Native tests: **22/22 pass** (`artifacts/native-tests-profiles-final.log`).
- `--case low`: pass, configured lead 25 ms, 596-603 frames presented, longest
  audio quiet 2.5 ms (`artifacts/av-final-low.log`).
- `--case smooth`: 3 of 3 pass, configured lead 50 ms, 607-613 frames presented,
  longest quiet 0.02-2.94 ms (`artifacts/av-smooth50-*.log`).
- `--case 720` (balanced): pass, configured lead 30 ms (`artifacts/av-final-720.log`).
- `--case minimized`: still passes after the timing refactor
  (`artifacts/av-final-minimized.log`).
- App hash for these runs: `9C81B1EFEF0DB223A6609571498C19F2B444F37438C92B6D54FE7DC0A4712D1D`.

One intermittent observation, unchanged by this work: a balanced 720p run once
measured a 60 ms quiet interval with one endpoint gap reset while every other run
stayed at 0.02-3 ms. That is the pre-existing endpoint-recovery cost described in
docs/58, not a profile effect; the run is preserved.

## Next

1. Three-share UI phase: gallery of three remote shares, per-share quality and
   volume, bounded decoders and mixing.
2. Separate-PC validation, then the Ubuntu SFU deployment.
3. Packaging: install target, notices, signing, clean-machine install test.
