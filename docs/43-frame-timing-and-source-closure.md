# Establish A/V before closure and trace decoded delivery

Work started 2026-10-22 and finished 2026-10-23, Europe/Budapest. Snapshot filenames retain the start date. Local owned fixtures only. Normal UI audio remains disabled. This checkpoint changes test readiness and optional diagnostics; the installed SDK, cryptors, audio scheduler and endpoint policy remain unchanged.

## Source-close readiness

The A/V source-close marker now waits for the existing A/V readiness predicate as well as 120 distinct drawn video frames: at least 96,000 committed audio frames and 120 presented video frames with a live playback worker. It writes these counters and an explicit `av_required` flag. The Python fixture verifies this evidence before asking its owned source to close. Video-only tests retain their video-only readiness. The timer captures its A/V mode by value.

An early diagnostic build passes its source-close case with 102,261 committed audio frames and 267 presented/drawn video frames. That result belongs to that build. Later failures, including a source-close device-frame coverage failure after actual playback, remain failures. The corrected fixture removes premature closure as an explanation; it does not approve receiving continuity.

A separate intermediate static-resume attempt encounters Windows error 13 while opening an atomically replaced checkpoint snapshot. The fixture now treats a missing file or transient sharing conflict as a pending observation and retries within its original deadline. Malformed JSON is still an error. Offline controls cover missing, temporarily inaccessible, valid and malformed snapshots. No media threshold or observation deadline is loosened.

## Bounded diagnostic records

`GOODSHARING_TEST_AV_TRACE` enables at most 8,192 timing records per encoder or reader, with explicit omitted-record counters. The normal path allocates none of these vectors. Records contain clock/identity metadata, never pixels, PCM, invitations or keys. Encoder records identify successful SDK handoffs, their original capture identity and start/end times. Reader records measure blocking read start, decoded read completion and callback return, along with RTP/RTCP timing and video identity or audio sample count. Traces are serialized after shutdown rather than in the one-second sender-rate journal.

The controlled backend retains at most sixteen completed-watch reports while tracing, so asynchronous subscription cleanup cannot discard a stopped video reader's evidence. Analysis deduplicates repeated snapshots by SID, maps a video SID to the stopped watch's quality, matches capture timestamp plus frame ID, and retains events around the first playback fault. Offline controls distinguish a 160 ms decoded-read gap from a 10 ms successful handoff gap, handle RTP wrap and retain missing evidence and original failed outcomes.

SDK read completion is after transport, decoding and SDK queues; it is not packet arrival. A successful `captureFrame` handoff is not proof of RTP transmission. Read-completion-to-callback-return duration includes the small diagnostic metadata step. Correlation uses the same PC's steady clock; it cannot be used across separate PCs without another clock relationship. Timing metadata never grants media permission.

In the retained intermediate 720p attempt, the video reader receives and reads 536 frames, then receives no further decoded frame while the sender continues successful SDK handoffs. Its last matched identity is frame 543; handoffs continue with later identities around the first playback fault. The first retained reset is an endpoint discontinuity about 125 ms after the final decoded video read. The last completed reader callbacks take only a few milliseconds. This narrows the missing-delivery boundary beyond the application callback; it does not identify whether transmission, forwarding, decoding or another SDK stage stopped progressing. Later rate feedback also falls to 30 kbit/s, but feedback and subsequent raw-frame throttling do not establish the original cause.

## Reproduce variable audio source advancement

The intermediate 1080p trace includes 480-sample decoded audio blocks whose trained, same-epoch RTP timestamps advance by 698 samples at approximately 10 ms read cadence. A preceding checkpoint also observed a smaller-than-nominal source advance. The pinned receiver calls NetEq `GetAudio`, then forwards the resulting frame timestamp to the audio sink. Its bundled API documents 10 ms decoded output and separate acceleration/deceleration sample statistics. This supports investigating adaptive output timing; these observations alone do not attribute every non-nominal stride to a specific NetEq operation.

The offline `goodsharing_audio_stride_probe` feeds the unchanged scheduler two 480-sample constant-PCM blocks at 10 ms arrival spacing, varying their source timestamp stride. The first block renders all 480 samples in each case:

| Source RTP advance | Second media frames | Second output frames | Trimmed samples | Synthetic silence |
| --- | ---: | ---: | ---: | ---: |
| 480 | 480 | 480 | 0 | 0 |
| 698 | 262 | 480 | 0 | 218 |
| 262 | 260 | 261 | 219 | 1 |

The current scheduler projects each decoded block onto a nominal source-sample timeline. These controls reproduce how that model inserts about 4.54 ms of silence or trims about 4.56 ms of decoded content when source advancement differs from decoded duration. They are diagnostic reproductions, not acceptance tests approving those outcomes, and do not prove the cause of all audible gaps. No sample-clock fallback, extra buffering or receiver policy change is adopted here.

## Final validation

All **22 native tests pass**. The offline analysis and checkpoint-sharing controls pass; the separate scheduler audit retains the undesirable behavior above rather than treating it as an acceptance pass. Actual-data controls check 53 retained reader traces across intermediate and final attempts: each stays within its 8,192-record capacity and none is truncated.

The final build passes **8/17 integration cases**:

| Final-build suite | Passed |
| --- | ---: |
| Complete A/V matrix | 5/13 |
| 58-second quality-cancellation observation | 0/1 |
| 58-second static-to-motion observation | 1/1 |
| Normal video-only 720p and 1080p | 2/2 |

The final ordinary source-close case has 99,400 committed audio frames and 271 drawn/presented frames before closure. The static-source-close case has 109,971 audio frames and 127 drawn/presented frames. Both pass. Both wrong-key cases also pass. The switch-source-exit case never reaches A/V readiness before its unchanged deadline and remains a failure. Other failed cases include normal 720p/1080p, paint-stall readiness, static 1080p, static-to-motion and quality switching. The checkpoint-file sharing error does not recur in the final matrix, but its static-to-motion case still fails real media checks.

The longer cancellation attempt has a 2,491.979 ms longest quiet interval and an endpoint discontinuity as its first recorded fault. The longer static-to-motion attempt passes with an 8 ms longest quiet interval and no recorded post-commit fault. These different outcomes establish remaining intermittency. Final trained audio traces include both 262- and 698-sample strides, and some even smaller strides; the fixed-duration/source-advancement mismatch is present in this build's actual evidence.

Intermediate builds and their failed or successful attempts are kept separately, including the initial source-close pass and the first full 5/13 matrix. They are not counted toward the final build's acceptance. The [validation manifest](../artifacts/20261022-frame-timing-validation.json), [source archive](../artifacts/20261022-frame-timing-source.zip) and [binary archive](../artifacts/20261022-frame-timing-binaries.zip) freeze all outcomes and verify unchanged preceding evidence and SDK DLLs. The final read-only process audit finds no workspace executable still running.

Next implement and test an audio-output timing model that accounts for decoded cadence versus source advancement, while preserving the 60 ms PCM bound, original source timing, bounded video horizon and final encryption admission. Separately trace RTP transmission, reception and decoder progress around the observed successful-handoff/decoded-delivery boundary. Normal UI audio remains disabled until continuity is reliable. Physical speaker/display synchronization, Internet congestion, multiple sharers and game coexistence remain open. No VPS service was deployed and no unrelated application was closed.
