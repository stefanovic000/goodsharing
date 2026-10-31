# Preserve the next due picture during decoded bursts

Date: 2026-10-23, Europe/Budapest. Local owned fixtures and generated tones. Normal UI receiving sound remains disabled, and no VPS service is deployed.

## Reproduce a hole created by queue replacement

[Checkpoint 46](46-bounded-earlier-video-presentation.md) passes 24/25 actual integration cases. Its static 1080p failure has a presentation-age interruption while valid pictures continue arriving; its queue reports nine discarded pictures. That observation alone cannot identify which discard caused the interruption, and its untimed maximum GUI-gap scalar cannot establish the cause either.

A separate native Qt reproduction fills the six-image queue with eight generated, clocked RGBA pictures. Previously overflow always removed the oldest queued picture. When every picture is still future-dated, that removes the very picture due next. On its original display deadline, the queue contains only later pictures and cannot present anything. The before-control fails with `Full future queue discarded the next-due picture and invented a presentation hole`.

## Retain the next opportunity and the newest successors

When the queue is full, it now removes the oldest picture only if the successor is already due. Drawing would select the newer due picture anyway. Otherwise it preserves the head, removes the second slot, and appends the new input. The other five positions therefore track the newest successors while one picture remains available for the next deadline.

Removal uses at most four moves of image handles. It adds no pixel copy, image allocation, queue slot, presentation delay, worker or dependency. Source timestamps and every retained ticket's deadline remain unchanged. The queue stays at six pending images plus the current displayed image. Existing expiry, timing validation, selected-role permission, drawing and frame-queue admission still apply. Candidate warmup still retains its single first image. The video-only path keeps its previous behavior.

An initial native trial retained the oldest five pictures and only replaced the future tail. The existing newest-due-picture regression rejected it: a newer due picture could have been discarded. The final head-plus-newest-successors policy passes that original assertion unchanged. Its diagnostic counter, `next_due_preserved_on_overflow`, counts the preservation branch; it is separate from total scheduled drops. The counter adds eight bytes to each video receiver.

## Verification

All 22 native tests pass. The new Qt controls cover an all-future queue, a queue with exactly one due picture, and a queue with multiple due pictures. They require the correct exact first pixel, the newest due pixel later, six-image storage, two recorded replacements, unchanged clock serial, genuine frame-queue acknowledgment and black pixels/zero fake-device PCM after paired audio revocation. Existing newest-due, stale-frame, clock-reset, warmup, handover and encrypted-admission controls remain in the same suite.

The current executable passes **26/26 actual local integration cases**: all 13 A/V matrix cases; a separate cancellation case; 58-second cancellation, static-to-motion and static 1080p cases; all five endpoint cases; ordinary 720p/1080p video; and both controlled native-expiry cases. Every app-based run checks the executable hash and its owned-process cleanup.

The longest measured quiet interval across the steady-tone cases is 8 ms, below the unchanged 40 ms limit. The targeted longer static 1080p case has a 0.021 ms longest quiet interval. First-tone startup in these steady observations is still 6.295–6.405 seconds with default reporting. That startup duration is separate from running media latency and remains work for the demo.

No queue-preservation or earlier-presentation counter fires in the final actual group. The deterministic Qt/native controls execute these branches, and checkpoint 46 separately retains the real earlier-presentation observation. The current green group therefore does not establish that queue replacement caused every improvement over previous failed sessions. Those failures remain recorded.

The final trace audit preserves 34 reader and 34 encoder traces without truncation and checks the unchanged capacity/A/V/cadence bounds. All seventeen stopped A/V reports include full history; twelve still-running snapshots are compact. Maximum observed live snapshot cost is 994 microseconds. The group records 151 source-horizon silence frames and five device-only recoveries. Each native-expiry case observes one rejected old block and continued fresh publication; wrong-key decoding remains zero. The post-test process audit finds zero project executables and records no process termination.

Results, current source/binary archives and the verification index use the `20261023-next-due-video-queue` prefix in `artifacts/`. The preceding checkpoint's failure, evidence and archives are checked unchanged before saving. Normal UI sound remains disabled. Next address default-reporting startup and validate physical A/V synchronization and a separate-PC demo before enabling it.

The PCM ring/residence remains 60 ms, actual-source A/V admission stays within plus/minus 80 ms, scheduled picture wait stays at most 80 ms and bounded earlier presentation stays at most 20 ms. Physical speaker/display synchronization, repeated startup reliability, separate Windows PCs, sustained multiple shares, Internet congestion, game coexistence and whole-app security acceptance remain open. Mumble remains the voice client.
