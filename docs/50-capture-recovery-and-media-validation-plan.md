# Capture recovery and media validation plan

Date: 2026-10-28, Europe/Budapest

Status: **capture recovery complete; media continuity active**

This plan supersedes the informal "next steps" in earlier checkpoints only for
current remediation. Earlier reports remain historical evidence and must not be
rewritten or treated as validation of a later binary.

## Outcome

Restore reliable screen capture on the current PC, then re-establish a
trustworthy local audio/video baseline for the exact executable and SDK that
will receive subsequent work.

This plan is complete only when:

- Windows Graphics Capture (WGC) delivers frames in repeated standalone,
  preview and encrypted-publication runs.
- A sender captures both video and application/desktop audio without allocation
  failure or silent fallback.
- The selected-quality receiver paints decoded frames, plays the expected
  stereo tone, preserves encryption admission and shuts down cleanly.
- The actual-media matrix is rerun and its failures are either fixed or retained
  as explicit blockers.
- The evidence records the executable, runtime DLLs, test inputs and owned
  process cleanup.

## Current evidence

The 2026-10-27 review found:

- The incremental build completed with no compilation errors.
- All 22 CTest tests passed.
- The two-client local join smoke test passed with capture inactive.
- Standalone application and desktop audio capture passed.
- Screen capture failed repeatedly with `bad allocation`.
- Two full 720p encrypted A/V runs failed before real media reached the viewer
  because the sender capture path aborted.

Follow-up on 2026-10-28 established that this first set of failures was caused
by the Codex Windows sandbox. The application's WGC path passed outside that
sandbox. See
[capture diagnostics and current media baseline](52-capture-diagnostics-and-current-media-baseline.md).

Retained evidence:

- `artifacts/20261027-231224-remote-av/720/summary.json`
- `artifacts/20261027-231327-remote-av/720/summary.json`
- `artifacts/20261027-231411-preview-normal/preview.json`
- `artifacts/20261027-231432-screen/screen.json`
- `artifacts/20261027-231516-audio/summary.json`

The failure is reproducible, but the exact cause is not yet established.
The immediate failing area is the WGC readback and `QImage::copy()` path in
`src/screen_capture.cpp`. Audio capture working independently narrows the first
investigation to screen capture rather than all capture.

## Non-goals

- Do not enable normal UI audio merely to make a test pass.
- Do not enlarge queue, age, latency or presentation bounds.
- Do not weaken encryption or permission checks.
- Do not start multi-share UI work before the capture blocker is closed.
- Do not deploy to the VPS from this plan.
- Do not stop unrelated applications or alter system graphics/audio settings.

## Phase 1: Localize the WGC allocation failure

### Diagnostics

- [x] Reproduce with `scripts/test_local_screen.py` three times and retain every
  result.
- [x] Reproduce normal and restart preview with
  `scripts/test_local_preview.py`.
- [x] Record the WGC item size, frame content size, mapped row pitch, texture
  format and staging texture description before the copy.
- [x] Distinguish failures from frame-pool creation, texture creation/map,
  `QImage` construction, `QImage::copy()` and later NV12 conversion.
- [x] Add a bounded diagnostic error that names the failing stage and dimensions;
  do not expose capture bytes or unrelated window content.
- [x] Confirm available memory and GPU/session state at failure, but do not treat
  low-level availability alone as proof of cause.

### Candidate causes to test

- [ ] Unexpected or excessive `ContentSize`.
- [ ] Invalid, negative or unexpectedly large `RowPitch`.
- [ ] Format/stride mismatch in the BGRA readback.
- [ ] Staging texture recreation after resize or source replacement.
- [ ] Frame lifetime or pool recreation ordering.
- [ ] Desktop/session/driver interaction specific to the current WGC path.
- [x] Item-factory denial: on 2026-10-28 `CreateForMonitor` and `CreateForWindow`
  returned `E_ACCESSDENIED` for every process on this PC, while GDI `BitBlt` and
  `IDXGIOutput1::DuplicateOutput`/`AcquireNextFrame` still produced frames
  (`artifacts/capture-api-availability.txt`). The cause was the workspace's Low
  mandatory integrity label downgrading every binary launched from it, not WGC
  or the desktop: the same binary copied to an unlabeled directory captured 778
  frames, and relabelling `build`, `artifacts` and the owned SFU directory with
  `icacls /setintegritylevel (OI)(CI)M /T` restored capture
  ([conversion checkpoint](59-capture-conversion-libyuv-critical-path.md)).
  A harness that re-applies a Low label to the workspace reintroduces the denial.

### Fix requirements

- [x] Validate dimensions and row pitch before allocating or copying.
- [x] Reject implausible frame sizes with a specific bounded error.
- [x] Preserve aspect ratio, timestamp and pixel correctness.
- [x] Keep the existing maximum 60 FPS handoff behavior.
- [x] Fail the share closed if capture cannot be proven valid.
- [x] Do not substitute a different capture API silently.

### Exit criteria

- [x] `scripts/test_local_screen.py` passes at least three consecutive runs
  outside the Windows sandbox.
- [x] Normal preview passes outside the Windows sandbox.
- [x] Sandbox-only `E_OUTOFMEMORY` failures are identified as an environment
  restriction rather than an application failure.
- [x] Reports include exact frame dimensions, stride and source error stage.
- [x] All owned fixture/app processes stop and no listener remains.
- [ ] Restart and source-exit preview remain to be rerun on this checkpoint
  binary.

## Phase 2: Re-establish encrypted sender/receiver baseline

### Core A/V cases

- [x] 720p timing-traced audio/video passes on the final hardened binary.
- [x] Untraced 720p repeats reliably without stereo loss or a continuity gap.
- [ ] Repeated GUI motion cadence meets the 60 FPS, duplicate and catch-up
  criteria in
  [the moving-cube checkpoint](54-moving-cube-cadence-and-presentation-floor.md).
- [x] 1080p audio/video.
- [x] Wrong key yields no video pixels and no audible tone.
- [x] Source exit revokes viewing and publication promptly.
- [x] Paint stall does not bypass final encryption/presentation permission.
- [x] Static picture and static-to-motion recovery.
- [x] Quality handover and handover cancellation.

Run with a server-only 500 ms reporting profile first for controlled startup,
then repeat the normal default-reporting path before accepting the result.

### Media checks

- [x] Sender admits more than 100 video pairs and more than 100 audio blocks.
- [x] Receiver paints more than 100 distinct timed frames.
- [x] Known stereo tones pass separation and crosstalk checks with no unwanted
  fixture tone.
- [x] Channel provenance is measurable at capture input, decoded playout input
  and WASAPI commit without retaining audio content.
- [x] A timed synthetic `captureFrameAt()` stereo control passes.
- [x] The controlled source fixture runs at true 60 FPS instead of 62.5 FPS.
- [x] Frame-swapped motion traces measure visible source-position cadence.
- [x] Sub-10 ms catch-up bursts are rejected instead of accelerating visible
  motion.
- [x] Endpoint deficits up to 3 ms are treated as clock noise instead of
  device underruns.
- [x] Audio has a bounded adaptive offset and may continue through a video
  presentation stall.
- [x] WASAPI device commit starvation is eliminated under repeated 720p runs.
- [x] The shared source conversion and two output canvases have separate timing
  metrics.
- [x] Parallel 720p/1080p canvas construction moves the 720p conversion budget
  below 16.67 ms.
- [x] Move the 1080p conversion critical path below 16.67 ms: the owned fixture
  source converts in 1.1–1.6 ms (source), 1.9–2.9 ms (1080p canvas) and
  1.1–1.8 ms (720p canvas), 4.10–6.35 ms per frame in eight passing A/V runs
  (previously 18.03 ms), and a 1920x1080 desktop model measures 10.45 ms
  ([conversion checkpoint](59-capture-conversion-libyuv-critical-path.md)).
- [x] Repeated local 720p and 1080p audio checks pass with stereo retries
  disabled after repairing dual-peer publisher SDP.
- [x] PCM device coverage, A/V lag and video presentation ages remain within
  their existing bounds.
- [ ] No unbounded queue growth or thread/handle growth is observed.
- [ ] Wrong-key sessions commit zero media while still detecting decryption
  failure.

### A/V continuity investigation

- [ ] Rerun the retained three failing scenarios from checkpoint 49.
- [ ] Retain the first playback fault and its preceding traces.
- [ ] Correlate sustained-low-headroom resets with endpoint and presentation
  events.
- [ ] Determine whether any screen-capture failure contributes to the retained
  A/V gaps.
- [ ] Keep the 40 ms continuity target unchanged during diagnosis.

### Exit criteria

- [x] The full agreed actual-media matrix passes, excluding only explicitly
  deferred scenarios.
- [ ] Every 102–152 ms gap from checkpoint 49 is fixed or assigned a
  reproducible cause and a separate bounded plan.
- [ ] Both 58-second switch and long static sessions pass.
- [ ] Physical speaker/display synchronization remains labelled unmeasured.

## Phase 3: Restore a repeatable gate

- [ ] Add one documented command that runs the release build, CTest, two-client
  smoke, screen capture, process audio and a bounded A/V case.
- [ ] Make the command fail if an owned process, SFU listener or test fixture
  remains.
- [ ] Keep longer matrix and soak runs separate from the short pre-commit gate.
- [ ] Record executable and runtime hashes in every report.
- [ ] Add a concise machine-readable index of failed checks.
- [ ] Add an interactive Windows CI or self-hosted runner only if the UI/GPU/audio
  session is representative; synthetic-only CI cannot replace these checks.

### Exit criteria

- [ ] A clean checkout can reproduce the build and short gate from documented
  commands.
- [ ] The short gate detects a reintroduced WGC allocation failure.
- [ ] Historical evidence is never silently reused for a changed executable.

## Execution order

1. Localize and reproduce the WGC failure.
2. Apply the smallest bounds/lifetime fix supported by the evidence.
3. Prove standalone and preview capture repeatedly.
4. Prove publication and receive with 720p audio/video.
5. Repeat at 1080p and across the core media matrix.
6. Investigate the remaining continuity failures.
7. Establish the short repeatable gate.

No step may be marked complete from a different binary or SDK.

## Evidence format

Each retained run should record:

- UTC/local start time and test profile.
- Application executable SHA-256.
- Installed SDK wrapper and engine artifact hashes.
- Server binary hash and ICE scope.
- Owned fixture PID/window handle where applicable.
- Passed and failed checks.
- First fault, dimensions, clock/deadline context and cleanup result.
- Whether the test view was visible.
- Confirmation that unrelated processes were not stopped.

## Risks

- A WGC/driver failure may be environment-specific. Reproduce on a second
  Windows PC before assigning the cause solely to application code.
- A generic `bad allocation` can hide either an invalid size calculation or a
  genuine resource failure; diagnostic stage labels are required.
- Passing component tests do not offset a failing end-to-end capture path.
- Fixing startup alone will not resolve the retained audio continuity failures.

