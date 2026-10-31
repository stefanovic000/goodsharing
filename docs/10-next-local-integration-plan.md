# Next phase: usable local screen sharing

Date: 2026-10-20, Europe/Budapest. This is the retained integration plan. **Step 1 has been implemented and verified in bounded local checks; step 2 is in progress following the user's next “continue”; steps 3 onward remain future work.** Preserve existing applications/services and all historical reports.

Subsequent execution: step 1 was authorized, paused, then resumed by the user. [Current results](12-step1-results.md) record passing stereo/lifecycle and sustained-video checks, preserved failures, and remaining contention/load limits. [The handoff](11-step1-handoff.md) preserves the earlier pause. These bounded checks do not complete the later integrated-app or release gates.

## Deliverable and starting point

Deliver a Windows app that joins a local private room, shares a selected window/display with application audio, desktop audio, or no audio, and displays another client's share. Support manual 1080p/720p selection, per-share volume/mute, coordinated Stop, and clear connection/encryption errors. Mumble continues to handle voice.

The app now previews and publishes actual selected capture/audio through the [capture adapter and room controls](25-capture-publication-integration.md), and [renders one selected remote video](26-remote-video-integration.md) with manual quality choice and final paint permission checks. Remote sound remains disabled pending a verified common media clock and synchronized playback. Earlier separate synthetic tests verified hardware H.264 encoding, paired exact-resolution tracks, encrypted forwarding and decoded pixels. The historical five-minute baseline achieved 59.81/59.96 decoded FPS and 47.79/51.65 ms submission-to-decoded p95, with zero inspected marker mismatches. Those historical measurements exclude actual capture, presentation and Internet transport and do not apply to the new integrated app. See [the measured baseline](09-frame-metadata-fix.md).

Keep C++20/Qt Quick, Windows Graphics Capture, WASAPI loopback, Media Foundation hardware H.264, and the pinned LiveKit client/server. Keep the codec-safe metadata envelope and publication pacing. Use two paired video tracks and one optional audio track per logical share. A normal viewer receives one video quality per share. This phase includes no transport rewrite, codec migration, voice client, recording, game overlay, or VPS deployment.

User preference clarified on 2026-10-20: **60 FPS remains the default target**, with continuous variable delivery such as 55, 45 or 30 FPS accepted by the viewer. The sustained benchmark minimum is not a runtime disconnect threshold. **Low latency is a primary requirement:** prefer reduced detail or a brief stutter/freeze while recovering to live content over accumulating a long playback backlog.

The user explicitly reconfirmed on 2026-10-21 that **share audio must match video**. Use a common per-share presentation timeline; variable FPS and quality changes must not change audio speed or shift synchronization. Bound alignment within the latency budget and recover to live media together. A held image with advancing audio is a degraded/frozen state, not successful synchronization. [The callback-level diagnostic](24-audio-content-timing-and-sync.md) now estimates audio content age and held-image skew, but actual flash/click speaker/display validation and the ten-minute drift gate remain required.

[Native audio-clock investigation](27-audio-media-clock.md) now exposes optional timing and verifies candidate media clocks against content. The live gate remains failed because timing is absent/unusable; the normal viewer keeps sound disabled. Do not complete the synchronization checkbox from regular callbacks or content-age measurements alone.

[Original sender audio timing](28-source-audio-capture-time.md) now carries the first-sample capture timestamp through native source submission and rejects expired or ambiguous timed input. This closes the source submission gap only. Expose and validate actual receiver media-clock mapping before implementing synchronized playback; the SDK's remote audio-track callback intentionally omits local capture time.

The user also confirmed **low CPU and memory use as primary requirements** on 2026-10-20. Track idle, sender, viewer and combined footprints as integration proceeds. Keep media queues bounded, avoid repeated allocations/copies where measured, let idle workers sleep on notifications, and use hardware encoding/decoding and GPU rendering where supported and verified. Do not trade latency or encryption readiness for lower reported resource use. Synthetic sender-plus-both-quality-receiver measurements include test work and software decoding; they are not a finished-client budget. The first bounded measurements and event-driven-worker changes are recorded in [resource results](15-resource-footprint.md).

## 1. Close the immediate API gaps

- [x] Resolve stereo receive before claiming usable stereo program audio. The stock SDK omitted channel-count selection; the pinned wrapper/engine patches and source/build steps are recorded in [step-1 results](12-step1-results.md).
- [x] Verify distinct left/right tones through publication, track/participant reception, and actual Windows playback. Mono remains an expected failing stereo negative control; the 3% crosstalk limit is unchanged.
- [x] Investigate the SDK close/renegotiation warning with bounded connect/disconnect cycles, define ownership/destruction order, and verify stalled setup cleanup. Final normal and controlled decoder-load suites pass. Earlier intermittent contention failures remain recorded, with better diagnostics and later load-hardening gates; they are not reclassified as passes merely because cleanup exited.
- [x] Record dependency redistribution/license requirements and keep required source changes reproducible. A native build is not yet a redistributable package; see [the dependency record](../third_party/README.md).

The bounded stereo investigation is complete; broader startup/shutdown behavior under contention remains a later integration gate. Current software video decode is an explicit baseline: investigate hardware decode if resource measurements prevent the later concurrent workload from meeting its target; do not describe the present path as hardware decoding.

## 2. Extract reusable room and share components

Current increments: `EncodedVideoOutput`, `TrackReader`/`ShareReceiver`, a Qt `ShareModel`, and an asynchronous `RoomController` have been extracted/added; see [the extraction report](13-transport-extraction.md). The coordinated `SharePublisher`, per-track encryption readiness, verified receive admission and bounded raw-input mailboxes are recorded in [the publisher report](14-share-publisher.md). [Resource work](15-resource-footprint.md) removes unnecessary idle polling/allocations. `RemoteShareCatalog`/`RemoteShare` now bind managed receiving to authenticated owner, current generation and one selected quality, with optional stereo, cancellation and teardown; see [managed receiving results](16-managed-receiving.md). Actual capture, presentation, congestion recovery and publication recovery remain open.

Proposed module boundaries; exact filenames may follow implementation needs:

| Component | Responsibility |
| --- | --- |
| `RoomController` | Join/leave, connection state, encryption readiness, participant/track events, subscription commands. |
| `SharePublisher` | Own a logical share, capture/scaling, two encoders, network feedback, one optional audio source, coordinated stop. |
| `ShareReceiver` | Group tracks by owner/share ID, select one quality, decode, render, and play associated audio. |
| `ShareModel` | Qt-facing share tiles, owner/status, selected quality, mute/volume. |
| `MediaDiagnostics` | Per-stage rates, queue age/drops, latency boundaries, encryption errors, resources; redact secrets. |

- [x] Move working harness transport behavior into reusable components while keeping the synthetic harness as a regression client. Avoid copying the entire probe into the UI backend.
- [x] Define share identity, video-quality mapping, optional audio association, and a generation identifier so late callbacks cannot revive a stopped/restarted share. Validate remote track announcements and handle partial publication/cleanup. The managed path binds one watch to an exact manifest, requires per-SID receive verification and completes unsubscribe before reuse; bounded local checks cover cancellation, variable input FPS, restart, owner departure and viewer leave/rejoin. Rendering-time stale-frame rejection and continuous-audio video-only quality switching remain later integration work.
- [x] Implement explicit joining, ready, starting-share, sharing, stopping, reconnecting, failed, and leaving transitions. SDK callbacks must not block the Qt GUI thread or hold capture locks during network calls.
- [ ] Use bounded queues: the newest raw frame may replace stale raw input; preserve accepted encoded access units in order. On sustained transport backlog, perform explicit recovery through a fresh keyframe or stop with an error instead of building unbounded delay or silently dropping dependent encoded frames.
- [ ] Enforce and measure time/age limits alongside frame-count bounds. Four queued frames represent about 67 ms at 60 FPS and 133 ms at 30 FPS, per queue stage when full; they are neither an intentional fixed delay nor a total end-to-end bound. Inspect sender SDK/pacing, receiver jitter/decoded queues and presentation age during test-owned upload restrictions and recovery. Keep audio protected, reduce bitrate first, discard obsolete raw/decoded images where safe, and use explicit fresh-keyframe recovery for obsolete encoded dependencies. The SDK/network backlog-age policy and congestion-recovery verification are still incomplete; do not claim a hard latency ceiling from the existing queue capacities or loopback results.
- [x] Preserve bitrate/keyframe feedback and independent pacing for each video output. The extracted output passed short bitrate-transition/key checks and a five-minute encrypted run; see [the extraction results](13-transport-extraction.md). This does not implement automatic quality fallback.
  [The later refresh increment](22-budget-aware-keyframe-refresh.md) budgets optional time-based keyframes by their measured size and adds fixed upload caps for repeatable diagnostics. Pressure latency, audio protection and intermittent startup decoding remain open; the earlier five-minute result does not apply to this build.
  [Closed receiver startup](23-closed-receiver-startup.md) subsequently blocks the native setup windows and passes bounded delayed-start regressions. Broader historical failure coverage and the pressure gates remain open.
- [x] Establish bounded synthetic CPU/memory sampling and remove periodic polling from audio input and idle joined rooms; release unneeded startup canvases after readiness. See [resource results](15-resource-footprint.md). Integrated sender/viewer/combined budgets, hardware decode/rendering and long-term resource stability remain open.
- [ ] Treat publisher fallback to 720p-only as a last resort (user preference, 2026-10-20). Reduce video bitrate first and reserve bandwidth for audio. Pause the outgoing 1080p publication only when sustained measured uplink pressure still prevents usable delivery; viewer quality selection alone does not reduce upload for these separate tracks. Keep audio continuous where bandwidth permits, show the temporary limitation, and restore 1080p only after sustained recovery with headroom and a fresh keyframe. Define thresholds and hysteresis from controlled tests before enabling automatic fallback; this policy is planned, not implemented.
- [x] Require a client-held key and ready encryption handlers for every active outgoing track before captured content enters publication. Audio-off means two video handlers; audio-on means three. Prime only synthetic black/silence, time out closed, and handle encryption failure after startup. Never substitute real captured content for preroll.
- [ ] Reconnect without duplicate tracks, stale callbacks, or cleartext fallback. Recheck encryption readiness before resuming content. Keep production invitation/rekey design as a separate security gate.
- [ ] Mandatory E2EE release gate (user reaffirmed 2026-10-21): zero tolerated plaintext-content fallback or unauthenticated video/audio exposure on missing/failed encryption. Gate every outgoing track and final presentation/playback; invalidate queued media on failure and re-establish readiness after reconnect/quality switch/rekey. Resolve historical raw-SDK wrong-key failures and prove test-only bypasses are isolated from shipped application paths. See [the exact failure contract](07-encryption.md#mandatory-failure-behavior). Current passing bounded tests do not close this gate.

Use a per-run loopback-only fixture to issue short-lived participant tokens and supply the local test key to authorized test clients. Keep the server signing secret out of the desktop binary; keep media keys out of tokens, SFU configuration, logs, and share metadata. The fixture is not the production invitation service.

## 3. Connect actual video and audio capture

- [x] Feed one selected Windows capture source into both video outputs, retaining capture timestamps across conversion, scaling, encoding, and transport. Separate the fixture's visible source-frame ID from publication IDs so tests can detect repeated or stale images. The app publisher and independent actual-window viewer now exercise this path; sustained/per-source lifecycle coverage remains below.
- [ ] Convert captured frames to NV12 and scale with preserved aspect ratio into exact 1920x1080 and 1280x720 canvases. Use a defined SDR background/letterbox policy and test 16:9, ultrawide, portrait, and resized sources. Do not stretch content or silently change output resolution.
- [x] Start with a correct, bounded conversion path compatible with the existing capture/encoder interfaces. Measure readback/conversion cost; use GPU processing where evidence justifies it. The current shared CPU conversion and rate-limited readback are measured in the capture integration report; GPU conversion/zero-copy and final footprint budgets remain open.
- [ ] Feed selected-app or desktop WASAPI blocks into the associated audio track, preserving stereo and a common monotonic timing model. Audio-off publishes no audio track. Keep shared program audio free from unintended microphone DSP.
- [ ] Stop video and audio together on Stop, source exit, capture loss, room leave, or unrecoverable failure. Stop may leave room membership intact. Do not automatically switch to another source.
- [ ] Handle resize, minimized/static windows, source restart, device loss, and cancellation. A static source need not generate 60 distinct images; moving fixtures are used to measure frame-rate capacity.

For desktop audio, exclude this application's own playback process tree. Test this in a client that both shares and receives. Other independent clients on the same PC are external audio sources and can contaminate desktop-loopback tests; isolate controlled scenarios or use application capture for multi-client load runs. Do not mistake that test topology for a feedback loop inside one app or install virtual audio drivers as a shortcut.

## 4. Add receiving, playback, and the minimal room UI

- [x] Add join/leave and connection status to the existing restrained Qt Quick window. Keep local preview available and clearly distinguish previewing from transmitting. The current development address/token/key form is not the production invitation flow.
- [ ] Show one tile per logical remote share, grouping its qualities/audio. Do not present the two quality tracks as two separate shares. Avoid playing the sender's own share audio locally.
- [ ] Render remote video with bounded frame age and instrument decode-to-Qt-render timing. Keep uploads/copies and GUI work observable before choosing a more complex GPU renderer.
  [Remote video integration](26-remote-video-integration.md) adds one selected quality, a one-image handoff, 150 ms local first-paint admission, static-image handling and final drawing permission checks. Sustained rendering rates and complete timing instrumentation remain open. The grouped selector is not yet a multi-tile layout.
- [ ] Add stereo playback, per-share mute/volume, output-device handling, bounded buffers, and underrun/drift counters. Keep self-playback excluded from desktop capture.
- [ ] Implement manual 1080p/720p selection first. Request a valid keyframe for the selected track, make any overlap brief and bounded, retire the old subscription, and keep audio continuous. Add Auto only after both manual paths pass and a measured policy exists.
  Video-only manual selection now waits for old subscription/reader teardown before replacement; bounded GUI switching passes. Continuous associated audio and repeated-switch performance remain pending.
- [ ] Provide Share/Stop, source/audio selection, enlarge/fullscreen, keyboard focus, and useful source/network/key errors. Keep codec/SDK details in diagnostics rather than normal controls. Do not label a share encrypted before readiness is established.

## 5. Automate proof of the integrated behavior

Build a launcher that starts only its own local SFU, controlled fixture, and independent publisher/viewer app processes. It must refuse an occupied port, avoid arbitrary user content, bound every operation, preserve failure reports, and clean up only its own PIDs. Use loopback TCP/UDP 8765 where free.

Run short checks first. After they pass, run the applicable sustained gates:

| Scenario | Required evidence |
| --- | --- |
| Existing synthetic regressions | Safe framing passes, legacy bad framing and deliberately wrong IDs fail as expected, correct/wrong-key audio/video gates retain their thresholds. |
| Actual captured window to independent viewer | Moving visible source IDs survive capture/scaling/encoding/reception; correct size/aspect, no inspected marker mismatch. Source-ID analysis must account for scaling coordinates and codec tolerance. |
| Each selected quality, then both outputs in the diagnostic harness | Five minutes of controlled 60 FPS motion; each quality averages at least 59 decoded FPS. Report capture, offered, accepted, encoded, decoded, inspected, and Qt-rendered rates separately. |
| Capture latency | Actual capture timestamp to decoded buffer p95 at most 150 ms on the clean local path. Separately report fixture-render-to-capture and decode-to-Qt-render delay. Qt/compositor events are not proof of photons on a physical display. |
| Audio routes and stereo | Wanted tones present; unrelated app/own-playback tones excluded as appropriate; distinct left/right channels verified; audio-off has no outgoing audio track. |
| Encryption lifecycle | Correct key works; wrong/missing key yields no usable test media; startup contains no fixture content before readiness; late join/reconnect and encryption failures do not downgrade. Keep the extra short-window tone check. |
| Quality switch and controls | Repeat switches, mute/volume, source stop/restart; selected video recovers within 2 seconds, audio remains continuous, no long-lived duplicate subscription. |
| Insufficient sender upload | Isolate bandwidth limits to test-owned traffic. Verify bitrate reduction happens first; brief dips do not disable 1080p. Under sustained insufficient bandwidth, verify last-resort 1080p pause actually reduces sender upload, 720p and audio continue where capacity permits, and recovery avoids repeated quality oscillation. Report severe limits where audio continuity cannot be maintained. |
| Stop and failure cleanup | Stop publication within 1 second; source exit, cancel-during-start, missing encoder, disconnect-during-stop, and app exit leave no orphan tracks/workers. |
| Reconnect | Controlled disruption of test-owned connections only; bounded recovery within 10 seconds after restoration or an explicit terminal error; no duplicate share/audio or stale content. |
| Audio/video sync | Controlled flash/click: p95 absolute offset at most 80 ms; additional drift at most 20 ms over 10 minutes. |

If available tooling cannot safely isolate network disruption, record that test as blocked with its concrete reason; do not change the PC's global firewall/network settings. Do not weaken thresholds to pass an implementation. Repeat extended tests only after relevant changes or failures justify them.

Measure GPU/CPU, memory, encoder-session capacity, queues, frame age, and audio buffers during these runs. Produce JSON plus a short Markdown report with exact versions and scenario topology. Preserve the earlier baseline and failures. A one-PC result does not prove friends' hardware, Internet latency, or anti-cheat compatibility.

## Exit and following phases

This phase ends when independent local app clients can share actual capture, receive/render video, play verified stereo audio, switch qualities, stop/restart, and recover connections with the gates above passed. The existing capture/network harnesses must remain passing. No checkbox is completed by this plan alone.

Then proceed in this order:

1. Three simultaneous logical shares; a client sharing while watching two others. Start short, then measure five-minute correctness and ten-minute sync. Measure software-decode cost and hardware encoder session availability before promising 60 FPS under that workload.
2. Multi-viewer forwarding/load tests, a 60-minute resource soak, real text/high-motion quality checks, and repeatable game-performance comparisons. Keep one-PC simulated loads distinct from actual multi-PC tests.
3. Client-held invitations and authenticated rekey/rejoin behavior; Windows packaging, required notices, local headset/game checks, and an app smoke test outside the build tree. Missing access to another person's machine remains an explicit later test.
4. Final Ubuntu deployment and public-path tests on the reserved endpoint, after local readiness. Resolve TLS/hostname, credentials, ICE/media bindings, and any extra ports there; port 8765 alone is not a blanket promise of connectivity through every network. Preserve Mumble, Minecraft, and all other services.

The publisher/readiness increment is recorded in [its report](14-share-publisher.md); [managed receiving](16-managed-receiving.md) covers remote-generation ownership. [Sender age/recovery](17-congestion-and-frame-age.md) and [SDK reference/recovery policy](18-preencoded-reference-recovery.md) add a sender-local handoff-entry budget, fresh-keyframe protection, raw admission and controlled upload testing. Normal and local-stall regressions pass. Current pressure runs have correct inspected pixels, but pressure-time latency and consistent recovery remain outside target. Finish that gate before calling step 2 complete. Earlier startup-corruption and resource-growth failures remain open. Actual captured-media integration follows in step 3. No VPS work is needed to begin it.

[SDK queue-age and decoded-delivery checks](19-queue-age-and-decoded-delivery.md) add sender enqueue age and post-decode image filtering without discarding H.264 references. Clean and deterministic local-stall checks pass; both restricted-upload runs still fail latency/continuous delivery. The strict gate also rejects long freezes. The overall integration phase remains open despite the newer capture and remote-video UI.

[Cryptor parser repair results](21-cryptor-parser-repair.md) resolve the reproduced H.264 framing/IV edge while preserving zero-output wrong-key viewing and stop-on-encryption-disable behavior. Congestion recovery and full E2EE/capture-to-presentation integration remain open.
