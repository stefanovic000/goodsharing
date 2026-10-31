# Release readiness plan

Date: 2026-10-28, Europe/Budapest

Status: **planning**

Prerequisite: complete
[capture recovery and media validation](50-capture-recovery-and-media-validation-plan.md)
before treating any release milestone as active.

## Release objective

Produce a Windows client that can safely and predictably:

- share one screen or window with application or desktop audio;
- encode 1080p60 and 720p60 video locally with hardware encoding;
- watch multiple authenticated shares concurrently;
- select quality and control per-share audio;
- keep audio/video synchronized across separate PCs;
- fail closed on encryption, permission, key and session errors;
- reconnect under defined conditions without stale media;
- coexist with anti-cheat-protected games using Windows-managed APIs only.

The service requires a separately validated Ubuntu deployment with TLS, TCP/UDP
media, congestion controls, rollback and clear operational ownership.

## Release principles

- Historical test pass is not current-build acceptance.
- Local-only, synthetic and physical measurements must be labelled separately.
- Encryption and final presentation/device permission checks remain mandatory.
- Queue, age and latency limits may only change with reproduced evidence and
  before/after controls.
- Unrelated applications and systems remain untouched by tests.
- Every release candidate has immutable executable, SDK, server and dependency
  provenance.

## Milestone 0: Trustworthy local baseline

Source plan: capture recovery and media validation.

Deliverables:

- WGC capture restored with bounded diagnostics.
- Full local A/V matrix trustworthy and repeatable.
- Multi-share implementation planning starts only after this gate.

Exit criteria:

- Screen, application audio and desktop audio pass independently.
- 720p and 1080p encrypted A/V pass.
- Wrong-key, source-exit, paint-stall, static and switch cases pass.
- No unexplained audio gaps above the accepted continuity target.
- Owned process/listener cleanup is verified.

## Milestone 1: Multiple-share product path

Required product behavior:

- [ ] Display a gallery of up to three selected remote shares.
- [ ] Watch independent shares concurrently, not one active watch at a time.
- [ ] Keep each share's quality selection independent.
- [ ] Provide per-share volume, mute and stop controls.
- [ ] Show explicit connecting, waiting, encrypted, stalled, revoked and failed
  states per share.
- [ ] Bound the number of simultaneous decoders, image queues, PCM queues and
  audio devices.
- [ ] Prevent one failed share from revoking unrelated healthy shares.
- [ ] Define behavior when the viewer selects a fourth share.

Tests:

- [ ] Three local publisher/viewer processes.
- [ ] Mixed 720p and 1080p selections.
- [ ] One publisher exits while two continue.
- [ ] One wrong-key share remains blocked while healthy shares continue.
- [ ] Repeated selection and stop cycles do not leak threads, handles or
  decoders.
- [ ] Sustained resource and presentation measurements for all active shares.

Exit criteria:

- [ ] Three shares are viewable concurrently on the target PC.
- [ ] Audio output policy is defined for three concurrent shares.
- [ ] The shared encryption and presentation gates remain provably enforced.

## Milestone 2: Normal audio and synchronization

Required work:

- [ ] Enable synchronized remote audio in the normal UI.
- [ ] Replace the test-only playback switch with explicit user-facing audio
  controls.
- [ ] Validate audio continuity across quality changes, static video, pauses and
  reconnects.
- [ ] Measure speaker/display skew with external capture or a documented
  physical measurement method.
- [ ] Define device-change, endpoint-loss and default-device-switch behavior.
- [ ] Define per-share mixing and clipping behavior for multiple audio streams.
- [ ] Confirm no application audio feedback loop on a single-PC demo.

Exit criteria:

- [ ] Normal UI starts audio without hidden test flags.
- [ ] A/V continuity meets the unchanged acceptance limits.
- [ ] Physical speaker/display synchronization is measured, not inferred from
  software deadlines.
- [ ] Endpoint failure recovers or fails the affected share without stale media.

## Milestone 3: Production security and invitations

Required work:

- [ ] Require `wss://` for every non-loopback room endpoint.
- [ ] Permit plain `ws://` only through an explicit local test mode restricted
  to loopback addresses.
- [ ] Define invitation creation, expiry, revocation and room membership.
- [ ] Define identity ownership and display-name trust boundaries.
- [ ] Implement key generation, distribution, rotation and rekey behavior.
- [ ] Define reconnect/key-loss semantics and sticky encryption revocation.
- [ ] Verify tokens, keys and decrypted media never enter logs or reports.
- [ ] Threat-model malicious participants, stale tracks, SID reuse, event floods
  and malformed metadata.
- [ ] Add dependency vulnerability monitoring and security update ownership.

Exit criteria:

- [ ] A user cannot accidentally send signaling/tokens over plaintext to a
  remote endpoint.
- [ ] Invitation and rekey lifecycle has automated positive and negative tests.
- [ ] Whole-app security review is completed against the documented threat model.

## Milestone 4: Performance and compatibility

Current known constraint:

- The sender performs CPU color conversion and scaling before two hardware
  encoders. Captured content now uses one shared pinned-libyuv I420 pass with
  same-size canvases copied rather than scaled
  ([conversion checkpoint](59-capture-conversion-libyuv-critical-path.md)), which
  measures 4.10–6.35 ms per frame in eight passing owned A/V runs; the MSVC x64
  libyuv build resolves its portable row kernels, so further headroom needs
  clang-cl or a GPU path.
- One prior 1080p share used about 1.99 CPU cores and 287 MiB peak private
  memory.

Required work:

- [ ] Establish numerical CPU, memory, GPU, frame-rate and latency budgets.
- [ ] Move capture conversion/scaling to an appropriate GPU/native path where
  feasible.
- [ ] Measure idle, 1-share, 3-share and viewing-only scenarios separately.
- [ ] Measure sender and viewer load during simultaneous three-share operation.
- [ ] Test Counter-Strike 2 and Rocket League capture coexistence without
  injection, hooks or overlays.
- [ ] Test window resize, display change, HDR/SDR, mixed DPI and source closure.
- [ ] Test sustained 1080p60 at low and constrained upload rates.
- [ ] Verify congestion recovery preserves valid H.264 reference chains.

Exit criteria:

- [ ] Resource budgets are met on the target hardware.
- [ ] Three simultaneous shares are practical on the target PC.
- [ ] Game compatibility is validated on real games rather than inferred solely
  from API choice.

## Milestone 5: Separate-PC and Internet validation

Local work:

- [ ] Run sender and viewer on separate Windows PCs.
- [ ] Measure software and physical A/V synchronization.
- [ ] Run two and three publishers on separate machines.
- [ ] Test sleep/resume, network change, endpoint change and reconnect.

Remote service:

- [ ] Deploy only to the approved project service scope.
- [ ] Use TLS for signaling and APIs.
- [ ] Verify TCP and UDP media paths on port 8765.
- [ ] Verify advertised ICE addresses do not expose unintended interfaces.
- [ ] Test NAT traversal, packet loss, jitter, bandwidth drops and recovery.
- [ ] Add service health, bounded logging, restart and rollback procedures.
- [ ] Preserve existing services and avoid VM reboot/resize unless separately
  authorized.

Exit criteria:

- [ ] Separate-PC local-network matrix passes.
- [ ] Internet-path matrix passes with measured latency, loss and resource data.
- [ ] Reconnect and rollback are validated.

## Milestone 6: Packaging and distribution

Required work:

- [ ] Add a CMake install/package target and deploy required Qt runtime/QML
  plugins.
- [ ] Decide and add the application's own license.
- [ ] Enumerate every shipped DLL, plugin and transitive native component.
- [ ] Complete third-party notices, source offers and replacement/relinking
  requirements for Qt, LiveKit, WebRTC, FFmpeg, OpenH264/patent terms, libyuv,
  soxr and Go dependencies.
- [ ] Pin and verify downloaded build tools before execution, including
  `rustup-init.exe`.
- [ ] Add a reproducible source/binary archive and build manifest.
- [ ] Code-sign release executables and document key custody.
- [ ] Include accessible third-party notices in the installed application.
- [ ] Test install, upgrade, uninstall and missing-runtime behavior.

Exit criteria:

- [ ] A clean machine can install and run the packaged build.
- [ ] Distribution/legal review is complete.
- [ ] Release artifacts have verified provenance and signatures.

## Engineering controls

- [ ] Initialize a reviewed source-control baseline or provide an equivalent
  immutable change history.
- [ ] Add a repeatable short gate for build, component tests, capture, smoke and
  one A/V path.
- [ ] Add a separate long-run matrix and soak gate.
- [ ] Add static analysis and dependency/CVE checks.
- [ ] Track every known failure with owner, severity, evidence and target
  milestone.
- [ ] Retain failed evidence; never overwrite a checkpoint.
- [ ] Require same-hash validation for any result promoted to release evidence.

## Release blockers

A candidate must not be distributed while any of these is true:

- Screen capture fails or is unreliable.
- Wrong-key input produces pixels or accepted audible media.
- Multi-share capability required by the release definition is missing.
- Normal audio remains test-only.
- Remote sessions can silently use plaintext transport.
- Invitation/rekey lifecycle is undefined.
- Physical synchronization is inferred rather than measured.
- Required game coexistence or performance budgets are unverified.
- Packaging notices or codec obligations are incomplete.
- Any required local, separate-PC or Internet gate lacks current-build evidence.

