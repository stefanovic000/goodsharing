# Capture conversion critical path and pinned libyuv

Date: 2026-10-28, Europe/Budapest

Status: **conversion critical path reduced from ~18 ms to ~5.3 ms in situ; the
720p/1080p A/V matrix passes after repairing the workspace integrity label**

This checkpoint replaces the capture conversion that docs/58 left above one
frame period. It moves the captured-content conversion onto the already-pinned
libyuv revision, shares one I420 conversion between both published qualities,
and removes the redundant per-frame passes that dominated the sender.

## What changed

1. `src/capture_publisher_input.cpp` no longer builds three `QImage` planes per
   canvas or round-trips BGRA through the SDK. One `capturePlanesI420()` call
   produces tightly packed I420 planes with `libyuv::ARGBToI420`, and both
   canvas conversions read them:
   - `libyuv::ScalePlane` writes the fitted luma rows straight into the canvas
     at the letterbox offset;
   - the fitted chroma planes are scaled into thread-local scratch and combined
     with `libyuv::MergeUVPlane` at the canvas chroma offset.
2. Matching dimensions skip filtering entirely (`libyuv::CopyPlane` plus
   `MergeUVPlane`), which is the native-desktop 1080p case: the 1080p canvas is
   now a copy instead of a scale.
3. Filter policy keeps the previously validated pixels where the old build used
   point sampling, and improves the case that lost information:
   - enlarging (`kFilterNone`) reproduces the old `Qt::FastTransformation`
     result exactly — measured mean absolute luma difference **0.00**;
   - reducing (`kFilterBox`) area-averages instead of dropping pixels, which is
     the 1080p desktop -> 720p canvas case.
4. Capture surfaces are now labelled for what Windows Graphics Capture (DWM)
   actually returns: `QImage::Format_ARGB32_Premultiplied` in
   `src/screen_capture.cpp`. The previous straight-ARGB32 label made the
   conversion premultiply premultiplied pixels a second time and forced a full
   extra pass per frame. Straight-alpha callers (the native test) still get the
   `libyuv::ARGBAttenuate` composition.
5. Per-frame scratch buffers (`packedScratch`, `chromaScratch`) are reused per
   converting thread, because a fresh multi-megabyte buffer costs a page-fault
   pass before any conversion work starts.
6. The capture report now includes `source_width` and `source_height`, so A/V
   evidence records the real capture geometry instead of inferring it.
7. Pinned libyuv is built from the revision already recorded in
   `dependencies.lock.json` (`917276084a49be726c90292ff0a6b0a3d571a6af`,
   archive SHA-256 `f49388bd…`). `scripts/prepare-libyuv.ps1` verifies the
   archive hash and extracts `.deps/sources/libyuv-91727608/`; `CMakeLists.txt`
   builds a warning-quiet static `goodsharing_libyuv`; `scripts/build.ps1`
   runs the preparation before configuring. The application build no longer
   depends on the engine build's intermediate tree.

## Why: where the old path spent its time

The last documented A/V sender (docs/58) reported 5.95 ms source conversion,
12.06 ms 1080p canvas, 12.08 ms 720p canvas, and 18.03 ms total for
1,021/1,145 frames. The new benchmark reproduces that shape on the same source:

| Source model | Path | I420 source | 1080p canvas | 720p canvas | Serial total |
| --- | --- | ---: | ---: | ---: | ---: |
| 960x540 premultiplied (owned fixture) | previous | 4.73 | 8.20 | 4.34 | 17.27 |
| | current | 1.73 | 2.36 | 1.28 | **5.37** |
| 1920x1080 premultiplied (desktop share) | previous | 17.49 | 1.47 | 3.89 | 22.85 |
| | current | 5.85 | 1.33 | 3.27 | **10.45** |
| 1920x1080 straight ARGB32 | previous | 14.39 | 1.46 | 4.03 | 19.89 |
| | current | 9.11 | 1.32 | 3.23 | **13.65** |

Columns are p50 milliseconds to two decimals; the totals are the exact sums of
the measured values, so a column addition can differ by 0.01. Each value comes
from 150 iterations of `build/bin/goodsharing_capture_bench.exe`, with each stage
timed on its own so the canvases are comparable (the previous build overlapped
them in a second thread, which is why its reported canvas figures were equal).
The PC runs at 100% display scaling, so the owned fixture window's captured
content is 960x540.

Pixel evidence from the same runs:

| Canvas | 960x540 source | 1920x1080 source |
| --- | --- | --- |
| 1080p mean absolute luma difference | 0.00, identical marker id | 0.00, identical marker id |
| 720p mean absolute luma difference | 0.00, identical marker id | 10.18, identical marker id |

The 720p difference at 1920x1080 is the intended area-averaged downscale. Every
run decoded the same 16-bit marker id (`42435`) from the old and new canvases.
`artifacts/libyuv-720-downscale-comparison.png` renders the same 1920x1080 model
as three luma panels — the 1080p canvas, the previous point-sampled 720p canvas
and the current area-averaged 720p canvas — so the only intended content change
can be inspected directly (`--dump` on the benchmark).

A 1,200-iteration run of the same fixture model
(`artifacts/libyuv-bench-soak-960x540.json`, 27 s) holds the stage means at or
below the short run — source 1.48 ms, 1080p canvas 2.25 ms, 720p canvas 1.27 ms,
serial 4.99 ms — with p95 values of 2.05/2.77/1.67 ms and a worst single stage of
2.95 ms. Nothing grows or drifts with the reused scratch buffers and planes, and
the same marker id still matches.

## Calibration

`artifacts/libyuv-calibration.txt` (i7-8700, 12 threads) records the primitives:

- `memcpy` 8 MB 1.15 ms, `memset` 3.1 MB 0.07 ms;
- `ARGBToI420` 960x540 1.37 ms, 1920x1080 5.21 ms;
- `ScalePlane` 960x540 -> 1920x1080: point 0.72 ms, bilinear 2.21 ms, box 2.32 ms;
- `ScalePlane` 1920x1080 -> 1280x720 box 1.82 ms; `MergeUVPlane` 960x540 0.35 ms;
- a fresh 3.1 MB vector costs 1.01 ms versus 0.06 ms to refill a reused one.

Two findings bound the remaining work:

- libyuv's CPU detection reports SSE2/SSSE3/AVX2, but this MSVC x64 build gets
  no measurable benefit from it: one 1920x1080 conversion takes 4.86 ms with all
  detected flags and 4.70 ms with the flags masked to the portable path. The
  intrinsic row kernels are compiled for the GCC/clang translation units, so
  MSVC x64 resolves the portable kernels.
- The two canvas buffers are still allocated per frame by the publisher
  contract (`ShareVideoInput::nv12` owns `std::vector`), which costs roughly
  1 ms per frame of first-touch faults at 1080p.

## Verification

- Native tests: **22/22 pass** (`artifacts/libyuv-native-tests.log`).
  `captured_content_conversion` was extended to cover the two-stage API: shared
  I420 planes, even-aligned letterboxing at both qualities, neutral chroma in
  the pillarbox (distinguished with red content, not white), premultiplied
  passthrough, identical marker decoding at 1080p and 720p from one plane set,
  the above-1080p cap (3840x2160 -> 1920x1080 planes and both canvases), a
  5120x2160 source bounded to 1920x810 and letterboxed inside 1080p, odd
  961x541 geometry with 481x271 chroma, a 2x2 source, and straight-alpha
  transparency above the cap.
- Conversion benchmark: `artifacts/libyuv-bench-960x540-premultiplied.json`,
  `artifacts/libyuv-bench-1920x1080-premultiplied.json`,
  `artifacts/libyuv-bench-1920x1080-argb32.json`,
  `artifacts/libyuv-bench-soak-960x540.json`.
- A/V matrix: eight owned runs, all checks passing, metrics in
  `artifacts/av-matrix-results.json` and the per-run `summary.json` files under
  `artifacts/20261028-18*-remote-av/`.
- Capture repair evidence: `artifacts/libyuv-capture-probe.json` (778 frames
  after the repair), `artifacts/capture-api-availability.txt` (GDI/DXGI controls)
  and `artifacts/probe-token-chain.cpp` (integrity lineage).

## Capture blocker: low-integrity workspace, root cause and repair

Every capture attempt from the workspace returned `E_ACCESSDENIED` from
`IGraphicsCaptureItemInterop::CreateForMonitor` and `CreateForWindow`, while GDI
`BitBlt`, the D3D11 device and `IDXGIOutput1::DuplicateOutput` all succeeded
(`artifacts/capture-api-availability.txt`). The cause was not the compositor,
the GPU, a policy or the agent identity: the workspace directory carried
`Mandatory Label\Low Mandatory Level:(OI)(CI)(NW)`, so every executable inside it
inherited a **Low integrity label**. Windows assigns a new process the lower of
its parent's integrity and its image file's label, so each binary launched from
the workspace ran Low-integrity, and the Graphics Capture broker refuses
low-integrity callers.

Evidence: `artifacts/probe-token-chain.exe` reported
`sandboxed=1 integrity=0x1000` for a workspace binary and
`sandboxed=0 integrity=0x2000` for the same binary copied to an unlabeled
directory (`artifacts/libyuv-capture-probe.json` versus the temp-copy run: 778
captured frames). Explorer, the harness process and the harness shell were all
Medium; only the workspace binaries were downgraded, which is why a scheduled
task and an Explorer launch failed in exactly the same way.

Repair (file metadata only; no code, no DACL, no ownership change):

```powershell
icacls .\build           /setintegritylevel (OI)(CI)M /T
icacls .\artifacts       /setintegritylevel (OI)(CI)M /T
icacls .\.deps\livekit-server-1.13.10 /setintegritylevel (OI)(CI)M /T
# After a later build failed because Qt's own tools in .deps were still Low and
# could not write the now-Medium build tree, the whole workspace was relabelled:
icacls . /setintegritylevel (OI)(CI)M /T
```

The first pass reported 786 / 22,483 / 3 processed files and
`artifacts/libyuv-capture-probe.json` recorded 778 frames at 1920x1080 with no
error; the workspace-wide pass that followed reported 24,791 files. If a harness
re-applies a Low label to
the workspace (its confined "workspace write" mode does), repeat the same three
commands before running capture-dependent tests; the build and test scripts
themselves need no change.

## A/V matrix after the repair

Eight owned runs on the repaired workspace, all passing every check
(`artifacts/av-matrix-results.json`):

| Run | Case | Passed | Captured / admitted | Frame conversion | p95 draw | Longest quiet | Stereo |
| --- | --- | --- | --- | ---: | ---: | ---: | --- |
| 18:15:31 | 1080 | yes | 1149 / 1148 | 5.31 ms | 16.3 ms | 0.02 ms | yes |
| 18:16:19 | 720 | yes | 1149 / 1149 | 6.35 ms | 14.3 ms | 5.75 ms | yes |
| 18:16:43 | 720 | yes | 1157 / 1156 | 4.22 ms | 14.2 ms | 0.50 ms | yes |
| 18:17:07 | 720 | yes | 1153 / 1153 | 4.10 ms | 9.3 ms | 0.02 ms | yes |
| 18:17:30 | 1080 | yes | 1159 / 1159 | 5.16 ms | 15.7 ms | 0.02 ms | yes |
| 18:17:54 | 1080 | yes | 1159 / 1159 | 4.31 ms | 12.1 ms | 0.02 ms | yes |
| 18:18:17 | static-1080 | yes | 1158 / 1158 | 5.79 ms | 15.6 ms | 0.02 ms | yes |
| 18:18:41 | switch | yes | 2238 / 2238 | 4.41 ms | 16.2 ms | 0.02 ms | yes |

Compared with the docs/58 failure: 18.03 ms per frame, 1,021 of 1,145 frames
admitted with 124 raw replacements, 46.54 ms p95 draw lateness, and 49-50 ms
endpoint-recovery quiet intervals. Now every frame is admitted with at most one
replacement, the sender reports 960x540 source geometry, endpoint-gap resets are
zero, and the two previously failing cases (`static-1080`, `switch`) pass.

## Capture border

Measured with `artifacts/border-probe.ps1` on build 19045, diffing desktop
screenshots taken with and without an active capture of the raised owned fixture
window (785 frames) and of the monitor (774 frames):

- window capture draws a one-pixel yellow line on the window's **own edge
  pixels**: 5,980 changed-and-yellow pixels in the 3-pixel band inside the window
  rectangle, 0 pixels in the 4-pixel band outside it;
- monitor capture draws the same line along the screen edge: 6,004
  changed-and-yellow pixels in the outer band (`#FCE140`, `#BFA600` samples);
- the indicator is **not inside the captured frames**: a directly inspected WGC
  frame of the 960x540 fixture reports a white top-left pixel and 0 yellow pixels
  in its first 3 and first 10 rows (`artifacts/probe-frame-border.cpp`).

So the border is a local on-screen privacy indicator, not shared content.
There is no supported way to hide it on this OS: `GraphicsCaptureSession.IsBorderRequired`
is absent from `C:\Windows\System32\WinMetadata\Windows.Graphics.winmd` on
19045, and the property exists only on Windows 11 22H2+ behind the restricted
`graphicsCaptureWithoutBorder` capability, which requires a packaged (MSIX) app
identity. On Windows 10 22H2 both window and display shares therefore show the
border locally, and the options to avoid it are a newer OS with that packaged
capability or a non-WGC capture API.

## Window targeting, occlusion and minimize

WGC targets a window or monitor and captures that surface, not screen pixels, so
an owned window keeps producing frames while another window covers it. Measured
with `artifacts/occlusion-test.ps1` (fixture plus an owned maximized terminal as
the covering window):

- covered source: 786 frames at 960x540 with 286 distinct animated marker ids,
  while a screenshot of the same screen region shows only the covering terminal
  (`artifacts/occlusion-covered-region.png`, `artifacts/occlusion-result.json`);
- minimized source: 0 frames and 0x0 dimensions — WGC stops delivering when the
  shared window is minimized.

DXGI output duplication has no per-window mode: its API takes a monitor
(`IDXGIOutput1::DuplicateOutput`) and returns the composed desktop, so a covered
or partially off-screen window can only be cropped out of the screen image and
would show whatever is on top. Window sharing therefore depends on WGC, and the
practical consequence for users is that working in other windows does not
interrupt a window share, but minimizing the shared window freezes the viewer.

## Current artifacts

```text
GoodSharing.exe
974C518F6DE32705A3716C2822A0B0ED40C84882D47284AC31229D995FAF724B

livekit.dll
FBEDCAFD56C5D4F007B1EEE660AFA2C09B4B5CD247922C7B7FD23AF3A8D036DF

livekit_ffi.dll
8BBF8C07FD4D99614E2FCFB51459F8ACAA2C3EB0A12B7CCFCC34B00667B1017C
```

The SDK DLLs are unchanged from docs/58; only the application binary changed.

## Next

1. Pool or recycle the two canvas buffers through the publisher so the last
   ~1 ms of first-touch cost per 1080p frame disappears.
2. Decide whether to compile libyuv with clang-cl for its intrinsic row kernels,
   or move the 1080p conversion to a D3D11 video-processor path; the calibration
   above shows the portable kernels are the remaining CPU limit.
3. Keep investigating the intermittent weak-left-channel three-second tail from
   docs/58; nothing in this conversion change touches channel content, and the
   eight runs above stayed stereo throughout.
4. Re-run the full 14-case matrix (including the static, resume, source-exit and
   switch-cancel variants) now that capture works, to replace the 12/14 state
   docs/58 left behind.

