# Minimal full-bleed interface

Date: 2026-10-28, Europe/Budapest

Status: **rewritten; verified with a live share and a packaged build**

The old interface was a settings page with a video box. This rewrite makes the
shared picture the application: the window opens maximized and the picture fills
it, with one 28 px bar as the only permanent chrome.

## What the window contains

1. **The picture.** A full-window `remoteVideoSurface`. While watching, it is the
   only thing on screen and the bar fades out after 2.5 s without pointer motion;
   moving the pointer brings it back. Nothing is padded or framed.
2. **One bar.** A 6 px state dot, a one-line state text, and three text actions -
   `Share`, `Watch`, `Join`/`Leave`. A 1 px audio meter runs along the bar's
   bottom edge. No panels, no cards, no rounded boxes.
3. **Two popups.** `Share` holds source, audio mode, application selection and
   start/stop. `Watch` holds the share list, 720p/1080p, the latency profile and
   watch/stop. Each popup is 330 px wide and closes on outside click or Escape.
4. **The join card.** The only screen without a picture: room address, token and
   key, shown until a room is joined.
5. **Quick switcher.** When the room offers more than one share, a single floating
   chip row (26 px, bottom centre) lists them; clicking a chip switches the
   picture through the same bounded handover path used by the Watch popup. It is
   hidden while the bar is hidden, so it never competes with the picture.
6. **Overlays.** The minimized-source notice still covers the picture with
   "User minimized app", and the empty state is two short lines of text.

## Behaviour the rewrite enables

- **Watching while sharing.** The publisher and the viewer are independent in the
  backend (`startSharing` only ends a local preview, `watchShare` has no share
  guard), so the bar can start a share and then watch someone else. The picture
  always shows what you watch; your own capture is never displayed. The bar reads
  "Sharing · watching" with a green dot, and either side can be stopped alone
  without affecting the other.
- **Switching.** Choosing another share in `Watch` replaces the picture through
  the existing bounded handover path; quality and profile stay selectable while
  watching.
- **Escape** closes popups and the join card; the window starts maximized.

## Verification

- The app loads the rewritten QML and runs maximized
  (`artifacts/ui-shot-2.png`).
- A live 720p A/V case with visible windows shows the shared fixture filling the
  window with the bar auto-hidden (`artifacts/ui-shot-watching.png`); the case
  passes every check (`artifacts/ui-live-case.log`).
- The packaged build was regenerated and re-verified with the same A/V case
  (`artifacts/package-av-case.log`).

## Files touched

```text
ui/Main.qml            full rewrite
scripts/package.ps1    portable package builder (Qt deployment, notices, manifest)
packaging/README.txt   run/usage notes for the package
packaging/THIRD-PARTY-NOTICES.txt
CMakeLists.txt         install(TARGETS GoodSharing RUNTIME DESTINATION .)
```
