# Readability, sharpness and join diagnostics

Date: 2026-10-28, Europe/Budapest

Status: **four reported problems addressed; re-verified on the owned matrix**

Reported from real use: unreadable text in parts of the UI, the settings popup
touching the top bar, blurry text in the shared picture even with little motion, a
status line that kept flashing "No new video frames", and joins failing after the
app was restarted a few times.

## What changed

1. **Text contrast.** The `ComboBox` dropdown lists inherited the default
   `palette.window` (white) while the text colour was light, so open lists were
   effectively white on white. The window palette now defines `window`,
   `windowText`, `base`, `alternateBase`, `highlight`, `highlightedText`, `mid`,
   `dark` and `shadow`, so open lists, sliders and tooltips stay dark with light
   text.
2. **Popup separation.** Both popups now open 6 px below the 28 px bar with an
   opaque background and a 1 px border, so the settings no longer merge with the
   share/watch bar.
3. **Sharpness at low motion.** The encoder was CBR at 6 Mbps (1080p) and 3 Mbps
   (720p) with `MaintainFramerateAndResolution`, which spends bits on motion that
   is not there and blurs text. Now 1080p runs at 10 Mbps, 720p at 5 Mbps, and the
   degradation preference is `MaintainResolution`: under pressure the encoder
   drops frames instead of resolution, which keeps text legible. The periodic
   refresh interval grew from roughly 3 s to roughly 8 s, removing the regular
   quality dip a refresh used to cause.
4. **Stall status.** The status line used the instantaneous
   `held_image_without_update` flag, which is true whenever the 16 ms UI tick runs
   between two frames - so it flashed every few seconds. It now requires **1.5 s
   without an admitted frame**, and only the transition is announced.
5. **Join after restart.** When the server rejects a join because the identity is
   still connected (the usual case right after restarting the app), the bar now
   reads "This identity is still connected to the room; wait about 15 seconds and
   join again." instead of a generic rejection.

## Root cause of the blur: the transport estimate capped the encoder

The configured bitrate was not what the encoder used. The published track reports
`configured_encoder_bitrate_bps = 3,636,667` against a configured 10 Mbps: the
SDK's rate-control feedback (the transport estimate) pinned the encoder at
3.6 Mbps, so 1080p60 text was compressed at roughly a third of the intended rate.
That explains "blurry even when nothing moves".

The publisher now keeps a floor at 60% of the configured rate and adapts above it
(`encoded_video_output.cpp`), with a counter for how often the floor engaged. The
same 1080p case now reports `configured_encoder_bitrate_bps = 6,000,000` on the
1080p encoder while 720p stays at 3.6 Mbps (its floor is 3 Mbps).

Additional cadence work: the UI presentation tick went from 16 ms to 8 ms, the
Smooth profile lead from 50 ms to 80 ms, and the periodic refresh interval from
~3 s to ~8 s. The report now also records `maximum_paint_ms` and
`average_paint_ms` so the presentation cost is measurable (1080p measured 2.8 ms
average, 8.9 ms worst on this PC).

## Remaining stutter

Frame admission still trails publication: the viewer admits roughly 700 frames in
18 s (~39 fps) while the sender hands off ~60 fps, with presentation catch-up
drops around 40-50 per run. The queue-size experiment (6 to 12 slots) did not
change that, so it is not queue overflow; the harness runs with a minimized viewer
window, which throttles paints, so part of the drop count is an artefact of the
test setup. This needs a visible-window measurement before changing the cadence
again - the new paint counters and queue high-water figures make that diagnosable.

The composited-marker check now retries the grab up to three times (60 ms apart)
when a grab catches a blank frame, which removed the intermittent false failure
(2/2 clean 1080p runs afterwards).

## Second pass: bar, sharing awareness, preview

- **Bar.** The bar is now fully opaque (`#0b0d11`), so the state text and actions
  stay readable over any shared picture. Every remaining palette role was filled
  in (`light`, `midlight`, `brightText`, `placeholderText`, `toolTipBase`,
  `toolTipText`, `accent`), which removes the white hover background that made
  control labels unreadable.
- **Sharing awareness.** A bordered pill sits under the bar while sharing:
  green dot, "You are sharing", and the captured source name. The window title
  changes to "GoodSharing · sharing", and the bar reads
  "Sharing · <source>" with `Stop share` always reachable.
- **Optional self-preview.** The Share popup has a "Show my preview while
  sharing" checkbox (persisted). When enabled, the sharer's own capture appears
  as a 320x180 tile in the bottom-right corner, labelled "Your preview"; with it
  off, nothing of their own picture is shown, only the pill and bar state.

Evidence: `artifacts/ui-shot-sharing4.png` (bar, pill and preview tile while
sharing).

## Verification

| Run | Result |
| --- | --- |
| `--case 720` | pass |
| `--case static` | pass (stall observations unchanged) |
| `--case minimized` | pass |
| `test_gallery.py --publishers 2` | pass |
| packaged build, 720p | pass |

App hash for these runs:
`AB62D2BFB4C4FABFCE906E899B3FFA8E5CA5736714BAE2BDA7DD1EB1A2CB9AB8`.

## Still open

- Whether 10/5 Mbps is enough for the sharpest text at 1080p60; a real-desktop
  comparison is needed (the owned fixture is synthetic). If text still softens, the
  next step is a content-aware rate control or a screen-content tuning hint.
- The exact cause of the earlier join failures beyond duplicate identity is not
  proven; the bar now reports the server's reason, which makes the next occurrence
  diagnosable.
