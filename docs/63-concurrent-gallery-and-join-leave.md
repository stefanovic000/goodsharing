# Concurrent gallery and clean join/leave

Date: 2026-10-28, Europe/Budapest

Status: **two concurrent tiles verified with two publishers; join/leave hardened**

This is the first slice of the multiple-share milestone: the viewer can hold the
main picture plus two additional video tiles, the tiles follow the announced share
list, and leaving or losing a publisher releases only what it owned.

## Gallery

- **Slots.** Slot 0 is the original watch path and keeps audio, the playback
  engine, quality handover and the minimized notice. Slots 1 and 2 are
  video-only: each has its own `RemoteVideo` and `VideoSurface`, bound before the
  track so the ready hook cannot be missed.
- **Audio policy.** Only the main picture plays audio. Extra tiles are muted by
  construction (no audio track is subscribed), so three concurrent shares never
  compete for the single WASAPI device.
- **Layout.** One tile fills the window; two split it 50/50 with a one-pixel
  gutter; three split it in three. Tile tags (share owner plus a short id, and a
  stop cross) appear only when there is more than one tile, so the single-share
  screen stays untouched.
- **Add and remove.** `addWatch` fills the next free slot (main first, then the
  extras); `stopWatchSlot(0..2)` stops one tile without touching the others.
  A tile ends when its track closes **or** its share is no longer announced, which
  is what happens when the publisher stops, leaves or loses encryption readiness.
- **Test hook.** `GOODSHARING_TEST_GALLERY=1` fills the gallery from every
  announced share, used by the new owned gallery test.

## Volume and focus

- **Volume and mute.** The playout core applies a gain to every committed sample
  (setGain, unity by default, clamped 0-1), so the bar's Sound toggle and
  slider change loudness without touching queueing, deadlines or the A/V
  mapping. The level is persisted with QSettings; unity remains the default so
  the measured path is unchanged unless the listener moves the slider.
- **Focus.** Clicking a tile calls ocusSlot(n), which stops that tile, promotes
  its share to the main picture (audio, playout, handover) and demotes the former
  main picture into the freed video-only slot. Audio follows the click; the two
  video streams keep running through the ordinary start/stop path.
- **Stop.** Each tile tag carries a × that calls stopWatchSlot(n) and leaves
  the other tiles untouched.

## Join and leave

- `leaveRoom` stops every tile (extras first), clears the share list, resets the
  status to "Not connected." and emits one state change, so the join card comes
  back cleanly and a second join starts from a neutral state.
- A controlled share start refreshes the owned-window list before lookup and
  retries for up to six seconds once the room is ready, instead of exiting the
  moment a window appears slightly later than the first enumeration.
- The reconnect path keeps its fail-closed behaviour: an interrupted room shows
  the reconnect message, drops watches and shares, and requires a fresh share,
  because encryption readiness must be re-established.

## Verification

| Run | Result |
| --- | --- |
| `scripts/test_gallery.py` (2 publishers, 1 viewer) | pass: 2 slots at 12 s, extra tile 612 admitted / 525 drawn, primary 942 drawn, back to 1 slot after the second publisher left |
| `--case 720` | pass, unchanged single-watch path |
| `--case minimized` | pass, minimized notice still correct |
| packaged build (`packaging/dist`) | pass, packaged executable runs the 720p case |

Evidence: `artifacts/final-gallery.log`, `artifacts/20261028-230151-gallery`,
`artifacts/final-720.log`, `artifacts/final-minimized.log`,
`artifacts/package-av-case-4.log`. App hash for these runs:
`1CD2748E54D686C9D5A9FCAB2CF3A992C8079E20A772B518F08B9F4EA3BFD4CE`.

## Three tiles and a mismatched key

scripts/test_gallery.py --publishers 3 runs three owned publishers and one
viewer holding all three tiles:

| Run | Result |
| --- | --- |
| 3 healthy publishers | pass: 3 slots at 12 s, both extra tiles admitted 360/332 and drew 290/276 frames, primary drew 125; after the third publisher stopped on its own the gallery fell back to 2 slots with 1 extra |
| 3 publishers, the third on a mismatched room key | pass: 3 slots, healthy tiles drew 409 and 339 frames, the mismatched tile admitted 0 and drew 0, and the viewer stayed watching - a share that cannot be decrypted does not revoke the healthy tiles |

The mismatched-key tile produces no frames because the SDK drops frames it cannot
authenticate; the viewer's own watch state is unaffected, which is the isolation
the plan asked for. The tile disappears when that publisher leaves, like any other.

## Observed flakiness

Two intermittent checks appeared while re-running the standard 720p case on this
busy machine, both preserved rather than hidden:

- composited_marker failed once in six runs (the window grab caught a black frame
  while 556 frames had already been drawn); the other five decoded the marker.
- One packaged run reported one endpoint gap recovery (gap_resets=4, coverage
  0.977); the same packaged case then passed. This is the endpoint-recovery cost
  documented in docs/58, not a gain or gallery effect - the gain path is skipped
  entirely at unity.

## Files touched

```text
src/preview_backend.h, src/preview_backend.cpp   slots, extras, reconciliation, leave
src/main.cpp                                     extra video items, share-start retry
ui/Main.qml                                      three-tile gallery, tile tags
scripts/test_gallery.py                          two-publisher owned gallery case
```

## Next

1. Per-tile quality and volume controls, and a focus action that moves audio to
   another tile.
2. Gallery case with three publishers and a wrong-key tile beside healthy tiles.
3. Then separate-PC validation with the Ubuntu SFU, signing and the
   clean-machine install test.
