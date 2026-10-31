# Closed receiver startup

Date: 2026-10-21, Europe/Budapest. This follows [the captured startup/congestion failures](22-budget-aware-keyframe-refresh.md). Tests use synthetic media on this PC. The UI remains a local capture preview; no SSH/VPS deployment, game interaction or unrelated application shutdown is involved.

## Identified setup windows

The pinned SDK installs its receiver metadata transformer before constructing the decryptor, then installs the decryptor alone, then replaces it with a metadata/decryption chain. The first intermediate path can send encrypted H.264 into a decoder. The readiness observer is also connected after constructor attachment, so the first crypto event can precede the per-track listener.

The [diagnostic control](../artifacts/20261021-163024-network-media/summary.json) widens metadata setup by 200 ms for each video track. [Its setup trace](../artifacts/20261021-163024-network-media/startup-analysis.json) verifies 201 ms intervals and seven H.264 decoder diagnostic lines before completed decryption setup. Its final media checks still pass after recovery. This establishes an invalid decoding path without pretending that every control must show visible corruption.

An intermediate repair deferred metadata attachment and closed each receiver when its cryptor was constructed. [Seven of eight delayed startups](../artifacts/20261021-163942-receiver-startup/summary.json) still produced decoder diagnostics, despite zero inspected pixel errors. While the room worker sets up one track, another native receiver can already receive media before reaching its own cryptor constructor. Closing only inside asynchronous room setup is too late. This failed attempt and its exact DLL/source snapshot are preserved.

The original 43-frame visible corruption and earlier clean-network pixel failure remain historical failures. The evidence above identifies unsafe paths that can corrupt startup decoder state; it is not a byte-for-byte replay proving the unique cause of every historical image or raw-audio anomaly.

## Repair

The native `PeerConnection::OnTrack` callback now installs a closed transformer before forwarding a new receiver to asynchronous Rust room setup. It has no output callback, worker or media queue and drops setup input before decoding. The same closed class protects cryptor construction.

Encrypted video metadata handlers are built detached. The manager configures metadata plus crypto, registers the exact owner/SID listener and stores the cryptor before explicitly activating the complete graph. There is no intermediate metadata-only or crypto-only receiver installation. Sender setup also activates only after its full graph and listener exist; the app's captured-content readiness gate remains in place.

An encrypted publication with no local E2EE configuration leaves the early guard closed. It cannot enter the SDK's explicitly unencrypted compatibility path. That compatibility path is retained for genuinely unencrypted SDK diagnostic fixtures; it is not an encryption-failure fallback and adds no user setting. GoodSharing's normal verified receiver still requires per-track crypto readiness.

The strict cryptor stays enabled and retains sticky revocation. Re-enabling a revoked cryptor cannot resume it. Metadata graph replacement after activation revokes that cryptor rather than rewiring it. AES-GCM, the parser repair, nonce generation and key derivation are unchanged. The shared placeholder is recorded in SDK build provenance; app builds reject a missing/mismatched input.

## Diagnostic interpretation

`startup_diagnostics.py` checks each claimed room receiver has ordered early guard, cryptor guard and activation events. Both video metadata steps must be detached, with the requested setup delay actually measured. A normal stereo fixture claims three room receivers. SDP also announces dormant receivers: these have an early guard and never become an application track, so they correctly remain closed without activation.

The first final-run checker incorrectly counted those dormant receivers as missing activations. [The original eight-run report](../artifacts/20261021-165221-receiver-startup/summary.json) remains failed and unchanged. [A separate assessment](../artifacts/20261021-startup-order-reassessment.json), recording the corrected parser hash, revalidates the same child reports, setup logs, runtime hashes, independent recordings and cleanup. All eight native child fixtures pass with zero pixel errors and zero H.264 decoder diagnostics. No test was rerun to erase a failed observation.

The [fresh corrected-checker run](../artifacts/20261021-165851-receiver-startup/summary.json) also passes normally, without reassessment. It inspects 144 frames per quality, verifies 200/201 ms setup intervals, and records zero damaged frames or decoder diagnostics. The final delayed-start evidence is eight assessed runs plus this fresh run, not nine original successful harness exits.

## Final validation

The final engine is `7de2c8fa388825b73cf1e6999b8c32f2cab56c64cc4075261e65be180a1ebc43`, cumulative Rust patch `a90b4f9369221de81dced353d85e5c1fa1df41893c5c9a118a43792fbd2f473a`, and publisher executable `58afa278f5a7ab5b56e44e2da660ff2a2c4f2a93f95c10b5db55a41da8a34cea`. The C++ wrapper and repaired cryptor source are unchanged. No earlier test/soak result is reassigned to this DLL.

- [All twelve native checks](../artifacts/startup-order-final-native-run.log) pass. The cryptor fixture additionally verifies that registering audio/video callbacks cannot make the closed placeholder forward any of its 16 inputs. [The native policy diagnostic](../artifacts/20261021-165218-cryptor-policy/summary.json) passes its four ordinary and six edge cases, retaining hostile-input rejection and valid encrypted round trips. This static-library test is separate from actual SDK forwarding.
- [All ten publisher cases](../artifacts/20261021-165452-share-publisher/summary.json) pass through the final SDK. The new `receiver-unconfigured` case and the wrong-key case each admit zero video content/preroll frames, zero audio sample frames and zero verified tracks. Disabling encryption during active sharing closes the failed gate, rejects old input, records zero subsequent video handoffs and cannot resume by re-enabling the manager. This does not claim recall of already authorized content or complete in-flight audio/presentation revocation coverage.
- The [explicitly unencrypted audio control](../artifacts/20261021-165535-stereo/summary.json) and [encrypted raw stereo control](../artifacts/20261021-165542-stereo/summary.json) pass. Three raw wrong-key audio attempts ([one](../artifacts/20261021-165548-stereo/summary.json), [two](../artifacts/20261021-165555-stereo/summary.json), [three](../artifacts/20261021-165602-stereo/summary.json)) have zero measured known-tone amplitude, including short windows. Their raw audio readers still receive silence/PLC sample frames; they are not zero-PCM-callback tests. Earlier raw-audio failures remain unexplained rather than being overwritten by these passes.
- [Managed receiving](../artifacts/20261021-165608-remote-share/summary.json) passes normal and wrong-key cases, variable 55/45/30/60 FPS, quality/generation changes and cancellation. [Room control](../artifacts/20261021-165625-room-controller/summary.json) passes eight cycles and owned publisher cleanup. These are bounded functional checks, not long-term resource stability or three-share capacity proof.
- [The 18-second relay with 200 ms receiver setup delays](../artifacts/20261021-165643-network-media/summary.json) passes startup order, measured delays, pixels/metadata/dimensions, stereo, independent source decoding and route/cleanup checks. It inspects 1,044 live frames each and has source-to-decoded-callback p95 of 29.99/25.80 ms at 1080p/720p. This is synthetic one-PC timing, not display or Internet latency or sustained-60-FPS acceptance.
- [The 200 ms SDK-input stall](../artifacts/20261021-165709-network-media/summary.json) passes with one stale SDK input rejected per quality and fresh reference recovery, zero decoder diagnostics, 1,067 inspected frames each and p95 of 28.74/24.50 ms. The injected pause is not a 60 FPS benchmark.

Both upload restrictions still fail the unchanged latency gate, even though inspected media, source recordings, startup order and decoder diagnostics are correct:

| Four seconds at 650,000 bps | 1080p | 720p | Result |
| --- | --- | --- | --- |
| [50 ms FIFO](../artifacts/20261021-165735-network-media/summary.json), pressure p95 | 211.09 ms, 9 frames | 1,968.14 ms, 5 frames | **Fail**; maximum video gaps 5,139 / 1,191 ms. |
| Same shallow run, first fresh frame after restoration | 1,527 ms | 186 ms | Recovery latency passes; pressure age and 1080p continuity fail. |
| [1,000 ms FIFO](../artifacts/20261021-165758-network-media/summary.json), pressure p95 | 856.90 ms, 38 frames | 868.89 ms, 50 frames | **Fail**; maximum gaps 1,689 / 757 ms. |
| Same deep run, first fresh frame after restoration | 961 ms | 207 ms | Recovery/continuity pass; pressure latency fails. |

[Shallow phase analysis](../artifacts/20261021-165735-network-media/phase-analysis.json) reports a 0.233 pressure audio concealment-counter fraction; [deep analysis](../artifacts/20261021-165758-network-media/phase-analysis.json) reports 0.179. These counter ratios and queue weighted means do not prove audible continuity or source-to-playback age. Audio protection under pressure remains open. There is no controlled network-performance improvement claim.

[Provenance verification](../artifacts/startup-order-provenance.json) confirms idempotent preparation, all locked patch hashes, identical native/SDK closed-header source, the Rust shutdown regression and retained control/intermediate DLLs matching their own reports. [Ninja reports no pending build work](../artifacts/startup-order-final-build-check.log). [The final read-only audit](../artifacts/startup-order-final-process-check.json) finds zero owned probes/SFUs and stops nothing. [Frozen source/runtime/results](../artifacts/20261021-startup-order-validation.json) retain the control, failed intermediate, counting correction and final evidence separately.

## Remaining work

The identified initialization paths are closed and pass the bounded tests above. Broader startup/load coverage and every historical failure are not thereby resolved. Whole-app E2EE release validation remains open: final rendering/playback admission, in-flight revocation, production invitations/rekey/reconnect and historical raw-audio anomalies still require work.

Next address congestion recovery and audio continuity with the fixed cap, including coordinated video bitrate allocation and last-resort 1080p pause/restoration only after sustained measured insufficiency. Preserve the user's bitrate-first preference, audio/720p continuity where bandwidth permits, exact crypto permissions, hysteresis and a fresh restoration keyframe. Actual capture publication/UI, hardware decode, three-share capacity, resource soak and actual game coexistence remain separate open gates. No new worker or media buffer was added by the startup guard; this is not a measured finished-app CPU/RAM improvement or a mathematical zero-leak guarantee.
