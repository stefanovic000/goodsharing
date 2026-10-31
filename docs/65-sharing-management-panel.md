# Sharing management: people panel, per-tile stop, share pill

Date: 2026-10-29, Europe/Budapest

Status: **implemented and verified on the owned harness**

The UI now answers "who is sharing, which tile is whose, and how do I stop this
one" without guessing.

## What the user sees

1. **People panel.** A 20x64 handle on the right edge (a `<` arrow) slides in a
   250 px panel listing everyone in the room: `You` first, then the other
   participants, each with a green `sharing` marker when they publish a screen.
   Clicking a sharing row watches that person's screen (1080p when offered,
   otherwise 720p) through the normal bounded path.
2. **Per-tile name and stop.** Every big shared tile shows a small tag in its
   top-right corner while the pointer is over it: the sharer's name (with
   `(sound)` on the tile that carries audio) and an `x` that stops only that tile
   (`stopWatchSlot`). Clicking elsewhere on the tile moves audio to it.
3. **Always-visible sharing notice.** While sharing, a centred pill under the bar
   reads "You are sharing" with the captured source name and a red **Stop**
   action, independent of the auto-hiding bar. The window title becomes
   "GoodSharing - sharing" and the bar shows "Sharing - <source>".

## Backend support

- `RoomController` emits `participantJoined(identity, name)` from the SDK's
  participant-connected callback; `ownerRemoved` already covered departures.
- `PreviewBackend` keeps a name roster, exposes `participants` (identity, name,
  sharing, self) derived from the announced shares, and `watchOwner(owner)` to
  watch by person instead of by list index.
- `GOODSHARING_TEST_UI_PANEL=1` opens the panel at startup so the harness can
  capture it.

## Layout notes

- The bar is fully opaque and hides 2.5 s after a watch starts (previously it
  only hid after a mouse move, which also left a 28 px strip over the picture).
- The QML file was rewritten because an earlier editing round had left replacement
  characters in several labels and glyphs; all labels are now plain ASCII.

## Second pass: rows, close arrow, in-panel sound, share rate, retry

- **Rows read as rows.** `You` stays first but is grey with a `this PC` label on
  the right, so it no longer looks like a header above the others. Everyone else
  shows their name and a green `sharing` marker when they publish a screen.
- **Close arrow is visible.** The `>` is anchored at the panel's top-right, below
  the bar (it used to sit behind it).
- **One sound slider, in the panel.** The bar no longer carries a slider. The
  panel has a `Sound` row with a slim custom track (4 px), a small knob and a
  percentage/mute toggle; it is visible on every watched screen because the panel
  is where people management lives.
- **Share frame rate.** The Share popup has a `Rate` choice (30 / 60 fps),
  persisted and applied at the capture source, so a 30 fps share really sends half
  the frames instead of encoding them at 60. Measured on the owned fixture: 30 fps
  -> 576 captured / 352 admitted frames; 60 fps -> 1151 / 704.
- **A share that is not ready yet is retried.** When the first subscription
  attempt fails, the viewer now retries up to three times at 400 ms intervals
  ("Waiting for the share to become available...") before showing the old
  "select it again" message, which removes the most common false failure.
- **Test isolation.** Tests pin the volume (`GOODSHARING_TEST_VOLUME=100`) so a
  saved UI volume of 0 cannot silently mute a run - that is exactly what happened
  once during this pass and is recorded here.

## Third pass: tag placement and share-rate default

- **The tile tag no longer hides under the bar.** The name/stop tag now starts
  6 px below the 28 px bar (`topMargin: chrome + 6`), so the floating tile name is
  fully visible over the picture.
- **30 fps is the default share rate.** Whole-screen shares at 60 fps encoded two
  streams at 60 fps from one 1920x1080 capture (copy + box-downscale + two
  encoders), which is the heaviest configuration and the one that felt laggy.
  The rate now defaults to 30 fps and stays selectable (30/60) in the Share popup;
  60 fps remains available for motion-heavy games. At 30 fps the same bitrate
  gives roughly twice the bits per frame, which also helps text sharpness.

## Fourth pass: watch failures explain themselves, volume in the row

- **"Remote share is unavailable" now names the reason.** Every refusal point in
  `RoomController::watchImpl` records a short cause (`room not ready`,
  `already watching this share`, `share is gone or changed quality`,
  `track not announced yet`, `three share limit`, `watch backlog`), and the
  viewer's status appends it, including during the retry window. A repeated
  failure can now be diagnosed from the bar instead of guessed at.
- **Volume lives in the person row.** The separate `Sound` block is gone; the row
  of the person whose audio is playing (slot 0's owner) carries a 58 px slider
  with a 3 px track and a 9 px knob, right before the `sharing` label. Other rows
  show only their state, and `this PC` keeps its label.

## Fifth pass: restarted shares reconnect instead of failing

The truncated status "share ..." identified the failure as share changed: the
share list still held the previous generation of a sender that had stopped and
restarted sharing. The viewer now re-resolves the selection by owner and share id
against the live list before spending a retry:

- newer generation -> "Share was restarted; reconnecting..." and an immediate
  re-watch with the fresh generation;
- share no longer listed -> "That share has ended." instead of asking the viewer
  to select it again;
- otherwise the bounded 3x400 ms retry runs, with the reason shown in brackets.

## Sixth pass: the catalog is authoritative for the generation

Reported after a **fresh** start (no restart): the watch still answered
`share changed`. The watch used the generation stored in the share-model
snapshot; the catalog (which validates a subscription) keeps its own current
generation. A model snapshot taken a few milliseconds earlier, or one that never
saw the newest announcement, therefore asked the catalog for an older generation
and the catalog refused.

- `RemoteShareCatalog` now exposes `generation(owner, id)` and
  `RoomController::currentShareGeneration()` returns it.
- `PreviewBackend::beginWatch` takes the **catalog's** generation for that share
  before subscribing, so the model snapshot can no longer cause a refusal.
- The mismatch text now carries both numbers:
  `share changed (want gN, catalog gM)`. A `catalog g0` means the catalog never
  accepted the announcement at all, which is a different failure and is now
  distinguishable from a snapshot lag.

## Seventh pass: 720p fallback, readable failure text

- When a watch at 1080p cannot be created (the share offers only 720p, or the
  1080 track is not in the catalog), the viewer now falls back to 720p once and
  reports "1080p is unavailable; trying 720p..." instead of leaving the viewer at
  "select it again".
- The watch popup status wraps onto two lines, so the bracketed reason
  (want gN, catalog gM) is readable instead of truncated.

## Eighth pass: the catalog was poisoned by its own history limit

The wrapped message finally showed `share changed (want g1, catalog g1)`: the
generation matched, so the refusal had to come from elsewhere. `manifest()`
returns nothing whenever the catalog holds a sticky `error_`, and `generation()`
does not check it - exactly the observed pair.

That sticky error comes from the bounded history budgets. A long session that
starts and stops shares repeatedly tombstones three SIDs per start; at 512
tombstones (or 64 share entries) the catalog used to fail shut for the rest of
the room, so every later watch answered "share changed" while the share list
still looked healthy.

- Tombstone overflow now drops the SID history and continues instead of poisoning
  the catalog; the entry limit first prunes retired entries.
- The refusal text appends the catalog's own error when there is one, so a
  poisoned catalog is visible rather than inferred.

## Ninth pass: watch the layer the catalog actually holds

Even with matching generations the catalog can hold only one video layer for a
share (for example when the 1080p publication was replaced). The model can still
advertise both, so a 1080p watch failed with `share changed` while a 720p watch
worked.

- `RoomController::hasShareQuality(owner, id, quality)` reports whether the
  catalog holds that layer.
- `beginWatch` now takes the other layer when the requested one is missing, and
  **never upgrades** a 720p request to 1080p (an upgrade silently changed the
  quality the UI had asked for and broke the quality-control check).
- The refusal text lists both layers: `share changed (g1; catalog 1080:no
  720:yes)`, which makes a genuinely empty catalog distinguishable from a
  layer mismatch.

## Tenth pass: the share was already watched

`share changed (g1; catalog 1080:yes 720:yes)` ruled out the manifest entirely:
the catalog had both layers and the generation matched. The remaining refusal in
`watchImpl` is the SID-conflict loop - a watch that already holds the same
publication cannot be created again - and that path returned without recording a
reason, so the status kept showing the previous attempt's text.

How a duplicate arises: while a previous main watch is still retiring,
`addWatch` (panel click) creates an **extra tile** for the same share; the next
Watch then asks for the main picture on a share an extra still holds, and the
conflict check refuses it.

- The main picture now **reclaims** the share from any extra before subscribing.
- `beginExtra` refuses a share that is already the main picture or already a
  tile, with "Already watching this share." / "That share is already on screen."
- The conflict path records "track already watched", so the status can no longer
  show a stale reason from an earlier attempt.

## Eleventh pass: a share without audio failed the whole watch

The instrumented lookup answered `no audio sid`, which finally identified the
real failure: the viewer was created with audio enabled (the local-demo entry
point calls `enableTestAvPlayback()`), so it subscribed the share's audio track
*before* the video, and the share had been published **without audio**. The
audio manifest was empty, the subscription returned null, and the code treated
that as the failure of the whole watch.

- The video subscription no longer depends on the audio one. A share published
  with "No audio" (or whose audio capture failed) now plays its picture; the
  watch log records `audio unavailable; watching video only`.
- Verified with the audio-enabled path unchanged: 720p passes with audio, and the
  new branch only triggers when the audio sid is missing.

Likely cause on the sender side: the Share popup's Audio selector was left at
"No audio", or application/desktop audio capture failed, so no audio track was
published at all.

## Twelfth pass: the no-audio fallback crashed the app

Making the video subscription independent of audio exposed a real crash: the
playout session (`RemoteAvSession`) was still created and started with a **null
audio track**, and its worker dereferences the audio watch
(`audio->closed()`, `bindMedia(audio, video)`), so clicking Watch terminated the
process.

- `RemoteAvSession::run` returns immediately when either track is null.
- `beginWatch` resets the playback session when the share has no audio, so a
  video-only watch never constructs or starts a playout worker.
- The watch log records `audio unavailable; watching video only` for that case.

Verified: 720p and minimized still pass with audio, and the audio-enabled viewer
globs no longer hand a null track to the worker.

## Thirteenth pass: the one-second hitch was the keyframe interval

A one-minute whole-desktop share logged captured=+300 per 5 s (60 fps),

eplaced 27 over the minute, 
ateLimited=0, stale=0 - the sender loses no
frames, so the rhythm was not frame loss. The encoder was emitting an IDR every
ps frames, i.e. exactly **every second**; each IDR is a burst that reads as a
hitch.

- The encoder GOP is now ps * 10 (ten seconds) with explicit refresh requests
  still covering periodic refresh and loss recovery (PeriodicKeyframe, SDK
  keyframe requests), which removes the once-per-second burst.
- The same log also proved the self-preview keeps working after a share
  restart (164 preview frames in the three-second second share), so that report
  points at the tile rather than the capture.

## Fourteenth pass: the slider could not be dragged

The participant row drew a full-size click area after the row content, so it sat
on top of the volume slider and swallowed the press: the drag never reached the
slider's own mouse handler.

- The row's click area is now declared first; the name, slider and status are
  painted after it and therefore receive the events in the slider's area.
- The slider's grab target grew from 14 px to 20 px tall.

## Verification

| Run | Result |
| --- | --- |
| `--case 720` | pass |
| `--case 1080` | pass |
| `--case smooth` | pass |
| `--case minimized` | pass |
| `test_gallery.py --publishers 2` | pass |

People panel evidence: `artifacts/ui-panel-viewer2.png` (window grab with the
panel open: `You`, `publisher  sharing`). Sharing pill and preview tile:
`artifacts/ui-shot-sharing4.png`.

## Files touched

```text
ui/Main.qml                                        full rewrite: rail, panel, tile tags, pill
src/room_controller.h, src/room_controller.cpp     participantJoined signal
src/preview_backend.h, src/preview_backend.cpp     roster, participants, watchOwner
src/main.cpp                                       panel test hook
```

## Next

1. Three-publisher run with the panel (verifies three sharing rows and stop per
   row).
2. Per-tile quality choice from the tile tag (currently the quality is chosen
   when the tile is added).
3. Separate-PC validation, signing, clean-machine install.
