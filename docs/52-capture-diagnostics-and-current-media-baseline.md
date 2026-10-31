# Capture diagnostics and current media baseline

Date: 2026-10-28, Europe/Budapest

Status: **capture recovery complete; intermittent media continuity remains**

This checkpoint resolves the current screen-capture investigation and records a
fresh encrypted A/V baseline. The capture blocker was caused by the Codex
Windows sandbox, not by the application's Windows Graphics Capture implementation.

## Outcome

- WGC capture passes on the current PC outside the Codex Windows sandbox.
- Normal preview passes on the rebuilt application.
- Application-audio capture, stereo isolation and encrypted transport pass in
  their focused tests.
- The final timing-traced 720p encrypted A/V case passes completely.
- Untraced 720p repetitions still expose intermittent stereo loss or audio
  continuity failures.

The screen-capture phase is closed. Audio continuity and intermittent split
subscription/stereo behavior are now the active blockers.

## Sandbox finding

Inside the Codex Windows sandbox, both window and monitor capture failed before
frame delivery:

```text
query graphics capture support: bad allocation
```

`bad allocation` is C++/WinRT's representation of `E_OUTOFMEMORY`. The same
probe passed immediately when run outside the Windows sandbox. Audio capture
was not affected.

Therefore:

- WGC, native preview and actual A/V integration tests that need screen capture
  must run in the signed-in interactive desktop session outside the Codex
  Windows sandbox.
- Synthetic transport tests that do not need WGC may remain sandboxed.
- A sandbox failure must not be recorded as an application regression.
- The authorization for PC capture tests remains scoped to project-owned
  fixtures and generated tones.

## Code changes

`src/screen_capture.cpp` now:

- Checks whether Windows Graphics Capture is supported before item creation.
- Names the failing stage for every capture error.
- Includes frame/item dimensions and mapped row pitch when frame readback fails.
- Rejects non-positive or excessive item/frame dimensions before allocation.
- Rejects impossible row pitches before constructing the `QImage`.
- Preserves the existing fail-closed and 60 FPS behavior.

`tools/screen_probe.cpp` now supports monitor capture for environment diagnosis
and treats monitor runs as valid without requiring a marker window.

No queue, age, timing, encryption or presentation bound was widened.

## Validation

Final executable:

```text
build/bin/GoodSharing.exe
SHA-256 5EEA56065352F258E6A94E7FF2B9AE3E15F8ABBACC946ECCECC006299191172C
```

Final screen probe:

```text
build/bin/goodsharing_screen_probe.exe
SHA-256 4E18FA59D305CB949CE2AE25606C358E645CA983B7A678F7947FCA261D79CA64
```

Passed:

- Release build of the complete application and affected probes.
- CTest: 22/22.
- Three consecutive WGC probes outside the sandbox.
- Normal preview outside the sandbox.
- Actual application capture with stereo/isolation:
  `artifacts/20261028-041025-capture-sharing/application-720/summary.json`.
- Synthetic encrypted stereo:
  `artifacts/20261028-040917-stereo/summary.json`.
- Synthetic encrypted stereo with Windows playback:
  `artifacts/20261028-041059-stereo/playback.json`.
- Final timing-traced encrypted 720p audio/video:
  `artifacts/20261028-041257-remote-av/720/summary.json`.

The final timing-traced 720p run recorded:

- First tone: 4.414 seconds.
- Longest quiet interval: 0.021 ms.
- Stereo separation: passed.
- Estimated media window coverage: 99.9996%.
- Device resets: 5.
- Endpoint-gap resets: 1.
- Presentation resets: 0.
- Owned processes and server stopped.

## Retained intermittency

Four final-hardened untraced/traced 720p samples produced:

| Evidence | Result |
| --- | --- |
| `20261028-040831-remote-av` | Stereo failed; 1,434.75 ms quiet interval. |
| `20261028-040938-remote-av` | Stereo failed; longest quiet interval 8 ms. |
| `20261028-041217-remote-av` | Stereo passed; 132 ms quiet interval. |
| `20261028-041257-remote-av` | Complete pass. |

This confirms intermittency rather than a stable pass distribution. The initial
split-subscription probe also failed once with no decoded selections and passed
on immediate retry:

- Failed: `artifacts/20261028-041129-remote-share/split.json`.
- Passed: `artifacts/20261028-041204-remote-share/split.json`.

The focused actual-capture path passed stereo while the A/V playback path
intermittently downmixed both channels to equal amplitudes. The synthetic
transport path remained stereo-correct. The next investigation should therefore
compare:

1. combined watch versus split audio/video watches;
2. raw decoded `AudioFrameEvent` channel energy;
3. `AudioPlayout` admission;
4. `WasapiPlayoutSink` output;
5. own-process observation after device rendering.

These measurements now exist. A failing follow-up run retained a source ratio of
1.25 but recorded decoded and WASAPI ratios of 0, localizing downmix after
capture and before decoded playout. See
[channel provenance and media repeatability](53-channel-provenance-and-media-repeatability.md).

## Correctness interpretation

- The earlier sandbox `bad allocation` is not current evidence of a broken app.
- Historical checkpoint 49 results remain valid only for their recorded binary.
- The final 720p pass validates its exact hash, but does not establish a
  repeatable stereo or continuity rate.
- A single passing run must not close the intermittent media issue.
- The 40 ms continuity target remains unchanged.

## Next step

Continue
[the capture recovery and media validation plan](50-capture-recovery-and-media-validation-plan.md)
at Phase 2. Channel provenance is established; the next reproduction must pair
a stereo failure with server-debug SDP evidence and a first-fault trace.

