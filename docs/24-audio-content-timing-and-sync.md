# Audio content timing and A/V synchronization evidence

Date: 2026-10-21, Europe/Budapest. Local synthetic diagnostics only. This follows [closed receiver startup](23-closed-receiver-startup.md). The UI remains a local capture preview; actual audio playback, rendered video, Internet behavior and release readiness are not demonstrated here.

## Outcome

The harness can now identify decoded audio content in time, rather than treating a constant stereo tone or regular callbacks as proof of freshness. It also estimates the content skew between audio and the video image that would be held at the same callback time. Neither is a physical speaker/display measurement.

All eleven publisher cases, twelve native checks and nine offline timing controls pass on the final build. The added publisher case deliberately revokes permission after encoding and before SDK handoff. The final clean audio/A-V diagnostic still fails matching coverage, and both fixed upload restrictions remain failed. These are retained failures, not release acceptance. See [the frozen evidence](../artifacts/20261021-audio-sync-validation.json) for final report paths, runtime/source hashes and preserved intermediate attempts.

No congestion-performance improvement is claimed. Production audio settings, crypto and SDK binaries are unchanged. The only production source change counts encoded frames that are discarded when permission is revoked, without changing the existing rejection/termination behavior.

## Test-only audio signal

With `--audio-trace`, the synthetic publisher modulates its existing 440 Hz left / 880 Hz right tones with a deterministic 20 ms amplitude code. Only generated test audio is used. The probe records accepted source block positions/creation times, decoded PCM and callback times. It retains at most 24 seconds of stereo PCM payload, or 4,608,000 bytes, plus bounded timing maps; overflow and invalid formats invalidate analysis. This buffer is confined to the probe and is not part of the application or a playout queue.

The offline NumPy analyser fits each channel's tone amplitude, then independently matches 480 ms code windows every 100 ms. It requires correlation at least 0.80 and separation of at least 0.12 from a distinct competing position. Fractional alignment accounts for an offset within the 10 ms block grid. A candidate must cover source blocks that were actually admitted. Future/noncausal matches are rejected within the documented 10 ms block-resolution tolerance.

Matched windows report synthetic block-creation-to-decoded-callback age, using this PC's steady clock. These are window averages, not per-packet age, real capture-to-speaker latency or clocks that can be compared across different PCs. Unmatched or ambiguous windows are retained. They can result from loss, concealment, time stretching or insufficient code confidence; their fraction is **not** an audio loss or unintelligibility percentage.

The diagnostic requires at least 90% matching coverage, audio window-age p95 at most 150 ms and callback gaps at most 100 ms in each applicable phase. Counter intervals or code windows crossing restriction boundaries are excluded explicitly. These checks supplement the existing stereo, media correctness, encryption, video latency/continuity and independent-decoding gates. They do not establish perceptual audio quality or guarantee every audio block is fresh.

The optional analyser requires NumPy; the tested environment supplies version 2.3.5. For example:

```powershell
python scripts/test_network_media.py --mode baseline --record-encoded --audio-trace --audio-analysis-python <python-with-numpy.exe>
<python-with-numpy.exe> scripts/test_audio_timing.py
```

## A/V sync is a requirement

The user explicitly confirmed that share audio must match the video. This includes the existing variable-FPS requirement: 60 is the default, and changes to 55, 45 or 30 FPS must not change audio speed or shift the media timeline.

The new estimator selects the **latest already-delivered video image** at a matched audio window's callback midpoint. It never selects a future frame to conceal a freeze. Skew is audio content time minus held-video content time: positive means audio leads the picture. It requires at least 90% matching coverage and p95 absolute estimated skew at most 80 ms, consistent with the integration plan's sync target. Unknown audio windows remain in the coverage denominator.

This models immediate presentation of callbacks. It does not implement or validate actual synchronized playback. Actual integration must map both media streams onto a shared per-share presentation timeline, use audio playout position to select/drop video, preserve source timing through quality/FPS changes, and handle clock drift. Any alignment buffer must fit the low-latency budget. Recovery must return to live content without accumulating seconds of queued media; a held image with advancing audio must be reported as a degraded/frozen state, not described as synchronized playback.

The actual flash/click capture-to-display/speaker test, p95 absolute sync error at most 80 ms, and drift at most 20 ms over ten minutes remain required before claiming usable A/V synchronization. Generation changes and final rendering/audio enqueue also still require the exact E2EE permission checks described in the encryption plan.

## Findings and retained failures

The [initial calibration](../artifacts/20261021-173240-network-media/summary.json) passes the existing clean network gates and audio timing: 173/179 windows match (96.65%), p95 window age is 46.73 ms and the largest callback gap is 11.39 ms. This predates the A/V estimator and is not reassigned to the final executable.

The [initial shallow restriction](../artifacts/20261021-173359-network-media/summary.json) matches only 9/36 pressure audio windows (25%), although callbacks keep arriving with a maximum 11.13 ms gap. Video has multi-second freezes. The [initial deep restriction](../artifacts/20261021-173454-network-media/summary.json) matches 15/36 pressure windows (41.67%); the matched audio's p95 window age is 1,150.27 ms, while the maximum callback gap is 11.10 ms. Constant callback cadence would miss both problems. These initial runs have their own executable/runtime hashes and are not claimed as final-build acceptance.

The [final clean run](../artifacts/20261021-202200-network-media/summary.json) passes inspected media, independent decoding and route/startup checks, but matches only 155/178 audio windows (87.08%). Matched audio p95 age is 113.65 ms. Estimated A/V absolute-skew p95 is approximately 75.97 ms, but sync coverage is only 154/178 at 1080p and 155/178 at 720p. Both diagnostics therefore remain **failed**. The unmatched portion is inconclusive; neither an earlier calibration pass nor the matched-window p95 is used to erase it.

Final shallow/deep restriction results are recorded separately in the frozen evidence. Their 650,000 bps caps apply to all owned uplink traffic for four seconds, with 50 ms / 1,000 ms relay FIFOs. No OS-wide traffic rules are changed. The existing 150 ms video-age and two-second continuity/recovery checks retain their thresholds. Audio/A-V checks add failure conditions; they cannot convert an existing failed video run into a pass.

| Final restriction | Matched pressure audio windows | Matched-audio age p95 | Estimated absolute held-image skew p95, 1080p / 720p |
| --- | --- | --- | --- |
| [50 ms FIFO](../artifacts/20261021-202345-network-media/summary.json) | 16/36, 44.44% | 82.33 ms | 3,009.50 / 1,796.56 ms |
| [1,000 ms FIFO](../artifacts/20261021-202410-network-media/summary.json) | 17/35, 48.57% | 972.72 ms | 1,607.56 / 331.54 ms |

Both remain **failed**. The matched subset does not stand for unmatched audio. Shallow video callback-age p95 is 1,017.70 / 379.28 ms with a 2,604.61 ms maximum 1080p gap. Deep video callback-age p95 is 775.78 / 824.55 ms; continuity/recovery pass there, but pressure freshness and audio/A-V gates do not. The hypothetical held-image skew can exceed delivered-frame latency because a frozen image keeps aging between video callbacks. Each final run retains correct inspected media, independent source decoding, verified relay routing/caps, closed startup order and zero H.264 decoder diagnostics.

The final publisher executable is `d5e1f3bebf7329289c22f0df7b38c8a83cdf9171da617705a6e65f32f2155f88`. The engine remains `7de2c8fa388825b73cf1e6999b8c32f2cab56c64cc4075261e65be180a1ebc43`. Earlier snapshots retain their own hashes. The final build has no pending Ninja work; cleanup is audited separately without stopping unrelated processes.

A preliminary experiment removed extra keyframe requests on large bitrate cuts. Both [shallow](../artifacts/20261021-172618-network-media/summary.json) and [deep](../artifacts/20261021-172654-network-media/summary.json) restrictions still failed latency. The change was reverted before the timing work. Its reports and a reconstructed functional diff are retained; its executable hash is recorded, but its binary was not archived before rebuilding, so there is no exact replay-binary claim.

## Revocation accounting repair

An [intermediate ten-case run](../artifacts/20261021-173533-share-publisher/summary.json) passed. The next [unchanged-behavior repeat](../artifacts/20261021-173819-share-publisher/summary.json) failed `disable-encryption`: one output had 97 completed encodes but 96 handoffs and zero stale/dependency drops. Permission was revoked inside the encoded-output callback before SDK handoff. That discarded output had no diagnostic counter, so the test's encoded-frame partition failed.

The failed run still records a failed/closed gate, zero video handoffs after revocation, rejected old input, and no resumption after re-enabling the manager. This observation is an accounting failure, not evidence of an admitted post-revocation video frame. It is also not proof that all in-flight audio or presentation revocation is solved.

`revoked_encoded_drops` now counts that existing rejection path before throwing. The publisher test retains the exact completed-encode partition, including this counter, and requires zero such drops in ordinary cases. The new `revoke-in-output` case deterministically revokes the exact track permission immediately before handoff, requires at least one counted rejection, and still requires zero later handoffs. [All eleven final cases](../artifacts/20261021-202109-share-publisher/summary.json) pass, including zero admitted video/audio with wrong keys or absent receiver crypto configuration. No encryption fallback or readiness exception was added.

[All twelve native checks](../artifacts/audio-sync-final-native-run.log) pass. [Nine offline timing controls](../artifacts/audio-sync-final-offline-tests.log) cover known delay/gain/fractional alignment, stale audio, silence/constant-tone rejection, a real coded-signal dropout, malformed/truncated maps, unadmitted source windows, noncausal matches, held-image/future-frame handling and insufficient audio coverage. These controls validate diagnostic behavior within their scope; they do not repair network congestion or implement playback scheduling.

## Next work

Coordinate video bandwidth reduction to protect audio, then measure the same audio-content, held-image sync, video continuity and freshness gates. Preserve bitrate reduction first, use 1080p pause only as a last resort after sustained insufficiency, and restore it with measured headroom, hysteresis and a fresh keyframe. Reliable clean-path matching and actual common-clock playback also remain open. Actual capture publication/UI, hardware decode, three-share capacity, resource soak, game coexistence and Internet deployment remain separate gates. No unrelated applications/services were closed and no VPS deployment was performed.
