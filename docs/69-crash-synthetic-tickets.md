# The crash: synthetic presentation tickets

Date: 2026-10-31, Europe/Budapest

Status: **root cause found, fixed, verified at 720p/1080p and 30/60 fps**

## Symptom

Every machine that watched a share eventually lost the whole window: the friend's
PC, the old laptop and the first PC. Intermittent, so early runs looked fine. The
friend's client log showed a stream of new share ids and repeated
`audio unavailable; watching video only` lines, which we first read as "the sender
restarts its share".

## How it was pinned

Windows recorded the abort signature consistently:

```text
Application Error   GoodSharing.exe   Qt6Core.dll   c0000409
```

That is a Qt fail-fast, and it also explained why the test harness kept reporting
`Owned fixture did not become ready` - the fixture was aborting the same way.

Two harness bugs hid the evidence:

1. The harness never set `QT_PLUGIN_PATH`, so the fixture died with
   `Could not find the Qt platform plugin "windows"` - the `c0000409` above.
   Fixed in `local_media_fixture.client_env`.
2. Client exit codes were not recorded. They now are (`viewer_exit`,
   `sender_exit`), which turned "it crashes" into `3221225477` = `0xC0000005`,
   an access violation, with the sender exiting cleanly.

The access violation pointed at the viewer-only path, and a kill switch
(`GOODSHARING_TEST_NO_UNTIMED`) bisected it in one run:

| Build | viewer exit | case |
| --- | --- | --- |
| arrival-paced fallback enabled | `0xC0000005` | crash |
| fallback disabled | `0` | pass, 351 frames |

## Root cause

The fallback that presents pictures without a scheduler ticket synthesises a
`PresentationTicket`. That ticket carries a default-constructed
`candidateClock`, and the playout dereferences it when the picture is reported as
presented - an access violation on a clock that never existed.

## Fix

Synthetic tickets no longer reach the playout at all:

- `finishPresented` returns early when the shown picture came from the fallback
  (`shownUntimed_`);
- `snapshot` never queues a fallback ticket for deferred delivery;
- the fallback still presents the picture locally, so frames are not lost.

## Verification (local harness, owned SFU)

| Case | viewer exit | frames drawn | longest audio quiet |
| --- | ---: | ---: | ---: |
| 720p @ 30 | 0 | 509 | 10.0 ms |
| 720p @ 60 | 0 | 1019 | 0.0 ms |
| 1080p @ 30 | 0 | 508 | 2.3 ms |
| 1080p @ 60 | 0 | 1018 | 0.0 ms |

All checks pass; before the fix the same cases aborted with `0xC0000005`.

## Still open

- Audio *start* delay on the receiver: the sender publishes its first audio block
  29 ms after the share starts (measured), yet the viewer stays silent for seconds.
  The playout waits for a presentation anchor that does not exist yet; the
  planned fix is a bounded wait (about 300 ms) before letting audio run on its own
  clock.
- Crash dumps are armed on the laptop; this PC needs the admin registry step.
