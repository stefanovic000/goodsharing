# Recovery of externally encoded references

Date: 2026-10-21, Europe/Budapest. This follows the [sender-age increment](17-congestion-and-frame-age.md). Step 2 and the upload-pressure gate remain open until the final measurements below satisfy their targets. All tests use owned synthetic media on this PC; the UI remains a local capture preview.

## Identified failure path

The hardware encoder's recorded H.264 frames independently decoded correctly, while live frames were damaged under upload restrictions. Disabling WebRTC's media-optimization frame dropper had not removed every place where frames could be skipped. The [release-branch encoder](https://raw.githubusercontent.com/webrtc-sdk/webrtc/m150_release/video/video_stream_encoder.cc) also drops inputs under congestion-window pressure and when newer input replaces pending work. Such drops are safe for raw encoder inputs but can break references when the input is already encoded.

The new source-side ordinal counts **encoded access units accepted into the SDK**, independent of capture FPS or raw-frame admission. A gate in the pass-through encoder detects missing, repeated or reordered ordinals and waits for a fresh keyframe. It forwards keyframe requests upstream and reports its withheld frames through the SDK encoder callback. A replacement source or encoder starts with fresh readiness. The gate stores only scalar state and one shared source-state reference; it adds no video queue or worker.

The gate's shared header is used by native unit tests and copied into the reproducible engine patch. SDK build manifests record its input SHA-256, and app builds reject stale input/header provenance. Existing patched shutdown, E2EE readiness and stereo changes remain in the engine patch.

## Intermediate evidence and limitations

- [The first guarded deep-buffer test](../artifacts/20261021-034207-network-media/summary.json) passed inspected media correctness but failed latency. The relay also received a Windows UDP connection-reset error during teardown; this run is not an overall pass. Backend relay sockets now disable that Windows ICMP-to-reset behavior, matching the frontend. The later [relay self-check](../artifacts/20261021-035104-relay-self-check.json) passes rate, flow and storage bounds after that fix.
- [The first shallow-buffer test](../artifacts/20261021-034300-network-media/summary.json) passed correctness on delivered frames but froze through the recovery observation window. That is a failed recovery, not successful playback. Later diagnostics show why a gate alone is insufficient: repeated skipped inputs can starve externally generated recovery keyframes.
- [The diagnostic shallow-buffer test](../artifacts/20261021-034814-network-media/summary.json) directly logs missing input sequences and dependent-frame rejection, including sequences 348/350/352/354/356. It delivered no inspected pixel/metadata/dimension errors and independently decoded the sender recordings. After restoration, the first fresh 720p/1080p frames arrived in 163/264 ms, and post-restoration p95 was 71/113 ms. **The run still fails**: pressure-time 1080p p95 was 349 ms and its maximum exceeded two seconds. A separate earlier shallow attempt froze completely; a later recovery does not explain away that variability.

Bounded native logs run only when the network-telemetry test flag is set. They report ordinals and flags, never media bytes or keys. Ordinary runs omit the diagnostic output.

## Congestion policy

The dedicated SDK now requests congestion-window **bitrate pushback** instead of its encoded-input frame-dropping mode. The window, its 350 ms additional-time setting and 30,000 bps minimum pushback target remain enabled. RTP pacing, feedback and congestion control remain active. Upstream byte-based raw admission then reacts before encoding creates references. The sequence gate remains necessary for other skipped/replaced inputs.

The test probe asks the pinned binary to parse this policy and records whether its congestion window and pushback are enabled and frame-only dropping is disabled. The network runner requires that runtime evidence in addition to route, measured-rate, correctness and latency checks. This verifies actual parser behavior rather than assuming a field-trial string took effect. This policy applies to this app's dedicated SDK environment; it changes no Windows network setting or Mumble behavior.

The final variant also enables application-limited bandwidth recovery probing and the pacer's existing fresh-keyframe flushing policy. That policy skips older packets for the same video stream when a new keyframe starts, except while a keyframe is already being paced. It is not arbitrary dependent-frame eviction. These controls are present in the [matching release-branch pacer source](https://raw.githubusercontent.com/webrtc-sdk/webrtc/m150_release/modules/pacing/pacing_controller.cc). The video environment requests 0–60 ms playout delay, while preserving the SDK's explicit zero-playout option when requested. This is a playout configuration, not a total frame-age bound: waiting for missing dependencies, transport and decoding can still take longer.

Audio uses a ten-packet jitter-buffer storage limit and fast acceleration instead of the SDK's default 200-packet buffer. Packet duration varies, so the count limit is not a hard millisecond limit. Receiver statistics and concealed samples remain necessary. No global audio device or Mumble setting is changed.

## Queue diagnostics

The network probe now samples both sender and receiver stats asynchronously. The receiver reports video jitter residence and shared-audio jitter/concealed samples as well as decoded-frame timing. `artifacts/analyze_network_stages.py` computes weighted counter-delta means for intervals ending in each phase; these are **not p95 values**, an exact per-frame age decomposition or an audio gap detector.

In the first guarded deep-buffer attempt, recovery-interval video jitter means were about 197/433 ms at 720p/1080p and audio jitter averaged about 817 ms. Sender packet-queue means were about 12/17 ms in those intervals. This locates substantial residual delay in receiving after restoration; it does not make the one-second simulated router harmless or prove a complete cause. The run's audio also concealed samples. Full-run stereo tones alone never establish low-latency, gap-free audio.

`test_network_media.py --queue-age-ms 50` adds a shallow FIFO scenario while retaining the original 1,000 ms default. Both use four seconds at 60% of measured warmup traffic, with unchanged acceptance targets. The relay can still drop packets during either cap; encoded-input sequence protection does not guarantee recovery from arbitrary network loss or server-side omissions.

## Final measurements

The final engine SHA-256 is `60aca98bf8a6715a49a3b5e3f4c1c151dc5df573c0b66e497dc200cacb98c0f5`; its Rust patch is `94776bc6b194342179c6de69ccf5c849e8bd85ecfc0ea67bb93d9e842211492d`. The publisher executable is `b27b5b1a69affdb99c9dec98963c1c5dd20a1ef34f2106c25faa7eaff12a38aa`. Several attempts share that executable but use different engine DLLs: each run's `runtime-info.json` supplies the actual lineage. [Frozen validation and sources](../artifacts/20261021-reference-recovery-validation.json) keep these distinct. The C++ wrapper is unchanged.

| Final runtime scenario | 1080p | 720p | Outcome |
| --- | --- | --- | --- |
| [Clean relay](../artifacts/20261021-040000-network-media/summary.json), decoded p95 | 31.48 ms | 27.13 ms | Pass; 1,078 inspected live content frames per quality, no pixel/metadata/dimension errors, correct independent recordings. |
| [Deep FIFO pressure](../artifacts/20261021-035936-network-media/summary.json), pressure p95 | 951.40 ms | 931.37 ms | **Fail** during the four-second cap; the relay itself reaches 940.67 ms queue age. |
| Same deep attempt, first fresh frame after restoration | 1,211 ms | 463 ms | Within the two-second fresh-recovery condition in this run. |
| Same deep attempt, p95 for frames captured at least two seconds after restoration | 33.13 ms | 45.15 ms | Recovery-period latency passes; longest whole-run inter-frame gaps are 1,946/809 ms. |
| [Shallow FIFO pressure](../artifacts/20261021-035754-network-media/summary.json), pressure p95 | 776.32 ms | 671.88 ms | **Fail**, despite a 50 ms relay queue bound; maxima are 2,456/1,142 ms. |
| Same shallow attempt, first fresh frame after restoration | 317 ms | 177 ms | First-fresh condition passes. |
| Same shallow attempt, recovery-period p95 | 234.36 ms | 103.16 ms | 1080p remains over the latency target; **overall pressure gate remains failed**. |

Both final pressure runs pass inspected live media correctness, stereo tone analysis and independent source decoding. Both verify the actual UDP restriction and 36 sender plus 36 receiver route samples, the configured recovery policies, zero remaining participants and complete owned-server cleanup. These bounded passing pixel checks do not prove the root cause of every historical startup corruption or future loss condition. A quick first fresh frame does not imply every subsequent frame remains fresh.

The deep attempt's recovery-interval counter means are 4.61/2.27 ms for the sender packet queue and 2.37/1.52 ms for receiver video jitter at 1080p/720p. Audio jitter averages about 59.57 ms during those recovery intervals, but the full audio report contains **50,791 concealed samples**. This is not gap-free audio. Different attempts use different instantaneous estimates and feedback histories; the table is not a controlled before/after improvement estimate.

[Six native tests](../artifacts/reference-recovery-ctest.log) pass, including cold-start/keyframe readiness, missing/repeated/reordered encoded ordinals, fresh-keyframe recovery and source replacement. [All eight publisher cases](../artifacts/20261021-040033-share-publisher/summary.json) pass with independent decoding, preserving stereo, audio-off, wrong-key rejection, cancellation, rollback and revocation.

[The final 200 ms encoded-output stall](../artifacts/20261021-040136-share-publisher/summary.json) passes: each quality discards one stale encoded frame and completes one fresh-keyframe recovery, with no pending frames/open recovery gate. Audio submits 501 blocks with 10 ms mailbox high-water; independent recordings are correct. [The ten-second 250 ms owner-stall test](../artifacts/20261021-040146-share-publisher/summary.json) also passes with independent recordings and bounded owned resource sampling. Neither intentional-stall case is a sustained-60-FPS gate.

[Managed receiving](../artifacts/20261021-040213-remote-share/summary.json) passes normal and wrong-key scenarios, including variable cadence, selected qualities, generation replacement and teardown. [Controller checks](../artifacts/20261021-040230-room-controller/summary.json) pass eight cycles, cancellation and owned publisher restart/cleanup. Joined-idle CPU is 0.039% of 12 logical processors over about ten seconds; the next command completes in 1.38 ms. Owned publisher Stop peaks at 224.19 ms in that suite. These short checks do not classify historical resource-growth failures.

The owner-stall client includes synthetic capture generation, hardware encoding, software decoding of **both** qualities, pixel/stereo inspection and in-memory recordings. Its sampled lifetime averages **9.05% CPU on 12 logical processors / 1.086 core equivalents**, with **147.32 MiB peak working set / 233.08 MiB peak private committed memory**. This short workload includes startup and a fault. It establishes no controlled CPU/RAM improvement, three-share budget, hardware-decode claim, steady-state stability or leak-free result. The smaller configured audio packet buffer is not a measured whole-app memory saving.

The source-level Rust shutdown regression passed on the first sequence-guard variant before the diagnostic/environment-policy additions; its [build log](../artifacts/chain-engine-build.log) is preserved. The final engine manifest correctly records that this Rust test was not rerun for that final DLL. Final media/controller disconnect checks cover the runtime lifecycle separately. No earlier five-minute result is assigned to the new engine.

## Remaining work

The 150 ms sender handoff-entry budget remains local to the sender. The network gate is still failed: pressure-time latency and dependable continuous recovery need further work. Add age-aware SDK queue/presentation recovery without breaking encoded references; do not use another PC's raw monotonic timestamp as if the clocks were synchronized. Audio concealment/gaps and A/V alignment need phase-specific measurement. Sustained insufficiency still needs the planned last-resort 1080p pause/restoration policy with hysteresis.

No total capture-to-display age ceiling, synchronized remote clocks, pressure-time audio continuity, production reconnection/rekey, three-share capacity or hardware decoding is established here. WGC/WASAPI publication and receiving/playback UI remain pending. Existing startup-corruption and resource-growth failures stay open. Games, unrelated applications, SSH and the VPS remain untouched.
