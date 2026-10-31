# Encrypted receiving, timed video and actual audio output

Date: 2026-10-22, Europe/Budapest. A controlled mode now connects actual window/application-audio capture through the local encrypted transport to Qt video and WASAPI output. **Normal launches still use video-only receiving.** Enable the development path only through the owned `--smoke-watch --test-av-playback` test; it is not a release setting or a completed synchronized-playback acceptance gate.

## One watch, one output owner

`RemoteAvSession` creates and destroys its endpoint and audio scheduler on a dedicated COM-initialized thread. SDK readers borrow the scheduler under a lifetime lock; there is no extra PCM queue between them and the existing 60 ms ring. Stop closes the watch first, wakes/joins that worker and flushes the endpoint before releasing its state. Invalid clock/device state and encryption failures terminate admission; there is no crypto bypass or automatic reopening of a revoked watch.

A valid device-clock reading that reveals a short write-position gap of at most 30 ms is handled separately in coordinated mode: clear the PCM ring, Stop/Reset the endpoint and require a fresh common timeline and queued video frame. No previously queued PCM is replayed. The underlying watch must still pass the final encryption/owner checks. Longer gaps and endpoint API failures remain terminal. Native tests distinguish this recovery from a revoked watch and verify that non-coordinated playback retains its earlier terminal policy. Real A/V tests also require at least 95% media coverage across the active software output window and at most three such recoveries in their short runs, so repeated stop/start gaps cannot pass on a brief tone alone.

The worker registers its own thread with Windows' [Audio task scheduling](https://learn.microsoft.com/en-us/windows/win32/api/avrt/nf-avrt-avsetmmthreadcharacteristicsw) and requests [high priority within that task](https://learn.microsoft.com/en-us/windows/win32/api/avrt/nf-avrt-avsetmmthreadpriority). Registration is reverted on that thread, including a failed priority request. This uses the standard OS service and changes no system audio settings. The previous WASAPI endpoint still enforces its 15 ms target write-ahead, 30 ms hard first-sample limit and exact bounded packet sizes.

`RemoteVideo` has an optional timed mode. Frames receive a deadline and serial from the same source-NTP-to-receiver-local mapping as audio. Future frames wait; each paint chooses the newest due frame, and invalidated epochs discard pending/held images. Final owner/SID/generation permission is still checked around the actual drawing operation. Audio waits for the first matching video frame to be queued for presentation, and is flushed if painting falls behind or the estimated last audible sample would differ from the last queued video by over 80 ms.

The renderer reports painting and then uses Qt's [`frameSwapped` signal](https://doc.qt.io/qt-6.8/qquickwindow.html#frameSwapped) to notify audio. Qt defines this as queued presentation, not monitor scanout. The callback runs directly on the render thread and does not create QObjects or send Qt UI changes. Actual speaker/display timing remains unmeasured.

## Bounded common-clock adjustment

The first real capture/receiving runs painted video but played silence: decoded audio arrived about 23 ms after the provisional deadline and every PCM block was trimmed as late. Independent decoded video/audio can have different processing delays even when their RTP clocks map correctly. Treating callback time as capture time would conceal this mismatch and is not used.

In coordinated mode only, a valid audio block with under 10 ms remaining before its deadline may move the common timeline forward until that block has 20 ms headroom. Both outputs move together, old queued PCM is cleared, the device is reset and a new matching video presentation is required. Original sender timestamps are preserved. The latest decoded video's scheduling lead is capped at **80 ms**; an adjustment exceeding that cap invalidates the timeline instead of creating a deeper buffer. The original 30 ms lead is the initial anchor, not a fixed final delay for this coordinated mode. Non-coordinated endpoint tests retain the preceding policy.

The timed video queue now has **six pending slots plus one displayed image**, sufficient for the bounded 80 ms lead at target 60 FPS. Only occupied slots allocate pixel storage. At 1080p RGBA, the worst-case seven-image storage is about **55.4 MiB**, excluding SDK, decoder, Qt texture and other app memory. Untimed video still retains one image. The PCM ring remains six 10 ms stereo blocks; the scheduling core is 11,904 bytes on this build. This extra video storage is an explicit synchronization cost, not completion of the low-memory/CPU requirement. Hardware frame sharing and resource acceptance remain open.

## Controlled evidence

The A/V runner starts only owned fixture/client/server processes. It uses a real WGC window, isolated application audio with left/right 440/880 Hz tones, mandatory E2EE, one selected quality, Qt painting and actual WASAPI output. The receiving app records only its own process through Windows process loopback. Its bounded last-three-second ring verifies stereo; whole-run peak and a guarded post-stop interval cover wrong-key and quiet-stop checks. A separate owned session supplies only silence so recording continues after output stops. No microphone or unrelated audio/window is captured, and no VPS is used.

These tests use actual encrypted transport notifications, rather than the synthetic permission events in increment 32. They require stereo isolation, at least 95% media-filled device frames and active-window media coverage, bounded queues, actual queued video presentations, estimated audio/video lag no greater than 80 ms, quiet post-stop observations, zero endpoint padding/reservations and clean owned-process teardown. Wrong keys must produce zero content commits and no presented pixels. A 220 ms interruption of the owned Qt GUI thread, triggered only after two seconds of actual media and at least 120 queued video frames, exercises audio hold/reset and fresh-timeline recovery. Source closure exercises coordinated teardown. Per-event diagnostics are opt-in; normal acceptance runs do not incur that trace serialization overhead.

All earlier attempts remain available. They include the silent-output runs, a manifest-order race that tried selecting audio before its announcement, and short 1080p output discontinuities. The manifest must now contain both selected video and audio before the controlled viewer starts. Other retained runs have a sender bandwidth target collapsing to 30 kbps, very low video delivery and expired/missing timing; those transport failures are not declared resolved by the A/V changes. No pass is inferred from these originals; their reports and probe hashes are retained separately from final evidence.

Visual review also found a stale 720p picker during a 1080p controlled watch. The picker now follows an actual selection change without overwriting an unapplied user choice on unrelated state updates. Tests check its displayed value, and the experimental mode's footer correctly describes enabled experimental A/V. Normal launches retain their video-only wording.

The final build passes **21 native tests and 20 local integration cases**: five encrypted A/V scenarios, six normal video-only GUI cases, four capture/audio-isolation cases, and five standalone endpoint cases on an identical endpoint-probe binary. Two additional resolution repeats passed before the UI wording/picker repair; they are retained separately from the final GUI-binary results. The video-only static-image and quality-switch cases remain covered, while those behaviors with continuous audio are not claimed complete.

| Final A/V case | Queued video frames | Device frames filled with media | Active software-window media coverage | Recent p95 estimated A/V lag |
| --- | ---: | ---: | ---: | ---: |
| 720p | 632 | 99.998% | 99.998% | 57.07 ms |
| 1080p | 653 | 99.987% | 99.987% | 56.70 ms |
| Source closure | 127 before closure | 100% | 100% before closure | 55.12 ms |
| Controlled painting stall | 469 | 98.288% | 96.030% | 53.97 ms |
| Wrong key | 0 | 0 media commits | No media | Not applicable |

All actual final A/V lag admissions stay within 80 ms. Normal resolution cases have about 16 ms p95 software paint lateness; stop/join takes 2–5 ms in these short checks. Post-stop process-loopback samples peak at magnitude one 16-bit sample count. No short endpoint-gap recovery occurred in the final hardware cases; that branch is covered by native fault-injection tests. The controlled GUI painting stall does exercise actual output flush and renewed presentation. These software measurements do not establish speaker/display sync or a hard maximum end-to-end latency.

Short resource samples include the test-only own-process observer. The viewer's middle window averages **0.60 CPU core equivalents at 720p** and **0.96 at 1080p**, with whole-process private-memory peaks of about **162/208 MiB**. The dual-quality sender with preview averages about **2.21 cores**, peaking around **290–292 MiB**. These are observations on this PC, not accepted footprint budgets, hardware-decoder proof, a sustained-FPS result or a memory-leak soak. They leave substantial room for renderer/capture optimization.

The [validation record](../artifacts/20261022-av-integration-validation.json), [source snapshot](../artifacts/20261022-av-integration-source.zip) and [binary snapshot](../artifacts/20261022-av-integration-binaries.zip) retain exact results, software timing/resource samples, original failures and provenance. These remain local development snapshots requiring the pinned Qt runtime.

## Remaining work

The 80 ms comparison is an admission bound in the estimated common clock; its percentile measurements do not independently prove physical sync. RTCP timing is infrastructure supplied and remains unauthenticated by E2EE. Measure speaker/display synchronization and ten-minute drift, support static-image audio, maintain audio continuity across quality changes, handle endpoint/device loss and long load, and verify Internet/congestion behavior before exposing audio in normal receiving controls.

Production invitations/rekey/reconnect, historical raw-SDK crypto anomalies, clock bootstrap defaults, sustained captured/presented 60 FPS, three-share workload budgets, game coexistence, packaging and release acceptance remain open. Default launch still captures nothing automatically. Mumble continues to provide voice independently.
