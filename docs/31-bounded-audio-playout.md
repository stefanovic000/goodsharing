# Bounded audio scheduling and final commit admission

Date: 2026-10-22, Europe/Budapest. This increment supplies a tested scheduling core and endpoint contract for future remote sound. **The normal sharing UI still does not play remote audio.** WASAPI endpoint wiring, connection to the actual Qt presentation deadline and physical speaker/display synchronization are pending.

## Scheduling behavior

`AudioPlayout` stores at most six 10 ms blocks of stereo 48 kHz signed 16-bit PCM. Its complete fixed storage is **11,872 bytes** on the tested Windows x64 build. Ingress and rendering do not allocate PCM buffers. The adapter has one additional 1,920-byte staging block; SDK, decoder, renderer and device allocations are outside this figure.

The model maps valid decoded video sender-report time onto a receiver-local deadline 30 ms after admission. Audio uses the same sender clock and exact sample positions. Sender NTP and receiver steady-clock epochs are never directly compared. This is a scheduling reference, not a measurement of screen latency or a commitment by the current Qt renderer to paint at that deadline. The eventual presentation adapter must coordinate both outputs with a real device clock.

The decoded PCM ring is limited to 60 ms capacity and 60 ms residence. Overflow evicts the oldest block. Samples more than 2 ms late are trimmed to the current scheduled sample position; early or absent samples produce silence. Endpoint reservations are at most 480 stereo frames and cannot place their first audible sample more than 30 ms into the future. These are local scheduler limits; they do not bound upstream WebRTC/decoder queues or total network latency.

Missing timing, invalid domains, incompatible epochs or a video clock older than 150 ms clear the timeline and request a device reset. Reset acknowledgments carry a serial so an old reset cannot acknowledge a newer timeline. A stopped or revoked instance cannot reopen. A new watch requires a new instance. Valid zero RTP timestamps and timestamp wrap are supported. The conservative 150 ms video rule currently also mutes a legitimate static-image share with continuing audio; static-picture continuity needs explicit treatment before enabling playback.

Volume accepts finite values from zero to one. Mute/unmute requests a timeline reset; muted audio advances with silence after fresh timing instead of retaining an audible backlog. Quality switching still replaces the watch, so uninterrupted shared audio through a video-quality switch is unfinished.

## Permission and endpoint boundary

`RemoteShare::presentAudio` checks the selected owner, video/audio SIDs, verified encryption state, watch lifetime and room admission. It serializes the actual endpoint commit with watch revocation. Timing metadata alone grants no permission: RTCP sender-report timestamps are still infrastructure supplied and are not E2EE-authenticated content metadata.

`RemoteAudioPlayout` checks permission at ingress and again around the actual sink commit. Buffer preparation happens outside the watch's revocation lock, allowing a revoke during preparation to win the final check. A commit already admitted under that lock finishes before watch revocation returns. Subsequent pumping cancels and flushes the endpoint. This cannot recall already played sound, and asynchronous hardware flushing is not implemented or proven by this model.

The sink contract separates prepare, commit, cancel and reset. A real reset must stop/reset the device before reporting success. Commit errors, failed resets and oversized reservations fail closed without automatic restart. Endpoint methods must be bounded and must not re-enter the watch or adapter. Device-thread pumping, including after revocation, is the future adapter's responsibility.

The SDK bridge rejects missing video identity, wrong selected-quality dimensions, rotation or non-RGBA data, unclocked audio and non-10 ms stereo PCM. Synthetic priming cannot establish the content clock. Immutable owner/SID strings are cached in the watch to avoid repeated conversion during final presentation checks. No SDK or cryptor patch changes in this increment.

## Validation

All **19 native tests** pass, including the two new scheduler/commit tests. The pure model checks exact stereo deadlines, unknown clocks, RTP wrap, duplicate rejection, bounded overflow/residence, late trimming, epoch/reset fencing, terminal revocation and variable 60/55/45/30 FPS. A **600-second virtual-time** sequence checks the identity of **59,987 output blocks**, including recovery from a 100 ms simulated render clog. This is not a real-time soak, device drift measurement or physical A/V test.

The fake endpoint covers final permission checks, foreign owners, incomplete verification, selected-audio requirements, revoke during preparation, revoke waiting for an already admitted commit, room invalidation, failed commits/resets, malformed SDK events and mute/unmute. Staged media and successfully committed media are counted separately. It performs no actual audio playback.

The user explicitly authorized PC screen/audio tests on this date. Current-build integration results and exact source/binary hashes are recorded in the validation snapshot linked below; frozen historical reports retain their original capture-consent status. Fixture tests use project-owned windows and generated tones and stop only their own processes. They do not exercise the new scheduler against a physical audio endpoint.

All **12 local integration cases** pass on the final build:

- Two generated-media managed-receiving cases: normal selection, 55/45/30/60 FPS changes, generation restart, cancellation and owner disconnect; and wrong-key rejection.
- Four actual WGC/application-audio capture cases: application audio at 720p, audio off at 1080p, wrong key and source closure. The application-audio case preserves the 440/880 Hz left/right tones and excludes the unrelated 660 Hz tone.
- Six actual two-app GUI viewing cases: 720p, 1080p, quality switch, wrong key, source closure and a static picture. Remote sound remains disabled in these GUI cases.

Short process-footprint samples accompany all capture/viewing cases. In the application-audio 720p capture case, the sender's roughly five-second middle window averages **2.04 CPU core equivalents** and peaks at about **290 MiB private memory**. This includes capture, two encoders and preview, and is not the scheduler's footprint. These short checks do not establish low-resource acceptance or sustained 60 FPS. The final SDK provenance/build check passes with no rebuild needed; no project executables remain, and no unrelated processes were stopped.

The [validation record](../artifacts/20261022-audio-playout-validation.json), [source snapshot](../artifacts/20261022-audio-playout-source.zip) and [binary snapshot](../artifacts/20261022-audio-playout-binaries.zip) preserve the final evidence. These remain development snapshots requiring the pinned Qt runtime.

The first snapshot incorrectly hashed its own still-active output log. Its record and archives are retained under `audio-playout-first-*` names; a separate check records the single log mismatch. The corrected snapshot excludes the active writer log. Test binaries and results are unchanged.

## Next work

Implement the WASAPI endpoint using its clock/padding to report actual first-audible time and enforce stop/reset on timeline or permission changes. Coordinate that endpoint with the selected video's presentation deadline, support static-image audio and preserve the common audio track through quality switches. Then test actual synchronized playback, device loss, mute/stop/revoke and ten-minute drift before enabling remote sound in the UI.

Production clock bootstrap selection, historical raw-SDK crypto anomalies, production invitation/rekey/reconnect, congestion latency, last-resort 1080p pause, sustained captured 60 FPS, three-share CPU/memory budgets, game coexistence, distribution licensing and VPS/Internet validation remain open. This increment does not close those gates.
