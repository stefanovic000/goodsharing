# Static-picture audio validation

Date: 2026-10-22, Europe/Budapest. This increment checks an unchanged shared picture with ongoing stereo application audio. It changes the controlled test harness and adds a native stale-presentation control; the playback scheduler, encryption gates, video queue and SDK remain unchanged. Normal UI receiving still has no audio enabled.

The initial assumption that a static picture necessarily stops Windows capture callbacks did not match this PC. The owned fixture stops repainting, but WGC continues supplying newly timestamped frames containing the same pixels. Those real frames pass through the existing encoders, E2EE transport, decoder and queued-presentation checks. Audio can therefore continue without fabricating video timestamps, repeatedly acknowledging a held image or weakening the freshness limits.

This is scoped evidence for this Windows/GPU/window combination. If a source actually stops producing fresh video, coordinated audio still pauses. A future low-update protocol needs explicit source confirmation of unchanged content; audio packets or repainting an old image must not masquerade as that confirmation. It is not implemented here.

## Controlled observations

The owned window freezes its pixels after nine seconds while continuing its left/right 440/880 Hz tones. Metadata-only observations at receiver test times 12, 14 and 17 seconds record current presentation/audio counters. The source records its freeze time in the same PC's MSVC steady-clock domain; both static-period observations must be at least two seconds after the freeze. This same-PC phase check is test instrumentation, not a replacement for the production RTP/RTCP clock or a cross-machine latency metric.

Between the first two observations, the test requires at least 95% of elapsed 48 kHz PCM frames to be committed and at least 50 fresh queued video presentations. The final composited screenshot must show the frozen pixel marker. A separate resume case signals the owned fixture only after both observations have completed, then requires a different valid marker at the end. Source closure happens after the same static interval and must revoke the watch, clear the picture and leave quiet output. A wrong-key static case must never present video or commit media. Existing moving-video, wrong-key, source-close and intentional GUI-stall cases remain in the matrix.

The final own-process loopback observation checks stereo output and guarded quiet post-stop samples. The previous bounded PCM/video queues, 80 ms estimated A/V admission limit, endpoint reset checks and final owner/SID/generation permission leases remain in force. A new native control verifies that ongoing audio arrivals cannot keep an old video presentation alive and that presenting its expired ticket cannot restore audio.

## Instrumentation failures retained

The first stronger test used three extra synchronous window screenshot grabs during playback. Static 720p/1080p and resume runs failed the active-window audio coverage gate. Diagnostics then observed approximately one-second GUI-thread pauses while fresh decoded video continued to arrive. A test-only always-visible window control also failed, so occlusion was not established as the cause. Removing intermediate screenshot grabs produced a passing metadata-only control. The final screenshot check is retained. These observations do not prove the cause of every GUI pause or resolve earlier transport/bandwidth collapses.

The first resume test also scheduled source resumption by the source's startup timer. Source and viewer startup differ, so motion could resume before the viewer's second observation. File-triggered resume now follows that observation explicitly. The original failing reports remain untouched.

A later metadata-only 1080p run passed its static interval at 99.97% PCM delivery but failed whole-window coverage at 94.77%, below the unchanged 95% requirement. It had six common-clock alignments before the first checkpoint, with no presentation-age reset; this is not the same evidence as the approximately one-second GUI pauses. Its precise cause is unresolved. A passing repeat must not be used to erase this continuity failure.

That matrix also had one server-startup failure before either client launched: the owned SFU attempted to bind a transient host IPv6 tunnel address that was no longer valid. An attempted loopback-only candidate filter prevented direct SDK ICE connections. That failed experiment was aborted by verifying the exact runner identity and stopping only its recorded descendants; its partial reports, server configs and cleanup audit are retained. An IPv4-only control passed, but its larger matrix still had an ICE timeout before media started. Neither filter is retained: `local_media_fixture.py` is restored byte-for-byte to increment 33. No host adapters, firewall rules or unrelated listeners were changed. Infrastructure startup/ICE reliability remains open.

The failed connection exposed an analysis error: a checkpoint with no active playback had no `audio` object. The runner now preserves raw viewer/sender evidence before interpreting it and treats missing checkpoint media counters as zero, causing explicit failed checks rather than a key lookup exception. This does not turn a failed connection into a pass.

The first new native control supplied late audio, which triggered the intended common-clock alignment reset before the test reached its presentation-age check. Its source timestamps were corrected so audio arrives with headroom; the corrected native test checks stale presentation directly. No production admission rule changed to make the test pass.

Final results and provenance are recorded in the [validation record](../artifacts/20261022-static-av-validation.json), with [source](../artifacts/20261022-static-av-source.zip) and [binary](../artifacts/20261022-static-av-binaries.zip) snapshots. Original failed attempts are included separately from final-build results. Frozen increment 33 evidence remains unchanged.

The corrected native suite passes all **21 tests**. The final static-only suite passes all **five cases** on the original fixture configuration and the current app binary. The five moving/security A/V cases from the earlier metadata-only matrix also pass on that identical binary and original configuration; that matrix's failed static/infra cases remain separately recorded. The six normal video-only GUI cases also pass, including switching quality and holding a static picture. These are **16 selected final-build integration passes**, not a claim that every attempted matrix passed. A later passing repeat does not establish that the intermittent failures are fixed.

| Final static case | Observed interval | Committed PCM frames in interval | Fresh queued pictures in interval | Whole active-window PCM coverage |
| --- | ---: | ---: | ---: | ---: |
| 720p | 1.999 s | 96,000 | 111 | 100.00% |
| 1080p | 1.999 s | 95,998 | 114 | 98.97% |
| Resume motion | 2.000 s | 96,000 | 114 | 100.00% |
| Close source | 2.000 s | 95,965 | 114 | 99.82% before closure |
| Wrong key | No admitted media | 0 | 0 | No media |

The interval figures compare finite committed-frame counters with millisecond-resolution observation times, so their ratio can slightly exceed 100%; they are not physical playback measurements. Recent p95 estimated A/V lag is approximately 54–56 ms in these correct-key cases. Guarded post-stop loopback samples peak at magnitude one int16 count; wrong-key output never commits content. The final 1080p screenshot was inspected and displays the frozen fixture and matching quality picker.

## Remaining work

Resolve the intermittent whole-window continuity gaps before enabling normal audio controls, and then maintain audio across 720p/1080p selection changes. Background/minimized behavior, sources that truly stop video callbacks, long-load stability, device loss, physical speaker/display sync and drift remain open. Static pixels on this PC are not proof of sustained 60 FPS, low CPU/memory acceptance, anti-cheat game acceptance or Internet/VPS performance. Audio remains confined to the controlled test path; Mumble still supplies voice.
