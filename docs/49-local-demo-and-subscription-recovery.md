# Local interactive demo and subscription recovery

This increment adds a local two-client launcher and repairs SDK subscription
negotiation. It remains a development demo on this PC. Separate Windows PCs,
Internet congestion, sustained multiple-share load, physical speaker/display
sync, game coexistence and release security acceptance still need validation.
Mumble remains the voice client. No VPS service was deployed.

## Try the demo

Double-click `Start GoodSharing.cmd` in the project folder, or run:

```powershell
.\scripts\start-local-demo.ps1
```

The launcher starts its own loopback server on port 8765 and two normal app
windows, Client-1 and Client-2. It generates temporary room credentials and one
client-held encryption key. Both clients can publish and view. Neither captures
anything automatically. Pick a source and audio mode in one client, start the
share, then select it in the other client and choose 720p or 1080p.

For a one-PC test, use **application audio** from the selected source. Desktop
audio can capture sound played by the other client and feed it back into the
share. For an uncomplicated first check, share an owned media/fixture window.
The sender defaults to a 60 FPS target; receiving accepts variable delivery.

Close both demo windows to stop this launcher's server. Closing one window keeps
the other running. The launcher refuses a busy port; it does not close existing
listeners or unrelated apps. Its reports are under `artifacts/*-local-demo/`.
The exact local demonstration reporting mode is described below.

Ordinary `run-preview.ps1` launch retains the existing audio gate. The explicit
`--local-demo` mode enables experimental synchronized receiving, with credentials
read from environment variables and then cleared in each app. The launcher
passes no keys on command lines or to the SFU and writes no keys to reports.
Production invitations, identity verification and rekeying remain unfinished.

## What changed

The retained checkpoint-48 long-switch failure stops receiving RTP packets even
though the publisher continues sending. Clock reports then stop advancing and
expire. Treating the later unclocked audio as the original fault would miss the
transport failure.

Room connections now use separate publisher and subscriber peer connections.
The first actual trial exposed a missing stereo capability in the native
subscriber answer: the SFU offered stereo, but the answer did not request it.
The engine now applies the stereo SDP repair to subscriber answers as well as
publisher offers. Parameter matching distinguishes `stereo` from `sprop-stereo`,
preserves explicit `stereo=0`, and stays within active audio sections.

A subsequent trial received stereo and completed the first quality switch, but
returning to 720p timed out. Native WebRTC does not deliver another OnTrack event
when the SFU reuses a receiver's stream association. The pinned engine already
diffed receiver streams after single-peer answers; it now also does so after
subscriber offers. Existing dispatch deduplication prevents a native callback
and the diff from attaching the same stream twice.

The C++ wrapper also sent a fresh subscribe/unsubscribe command from its incoming
acknowledgement handlers. Those handlers now update local state without echoing
another FFI request. Both fixes live in checked, reproducible preparation recipes,
with generated patches and installed-artifact hashes.

Subscription timeout is now confined to its watch. A timed-out candidate is
revoked, its readers are stopped and its subscription is cancelled; healthy
audio/video watches stay active. A missing local unsubscribe callback has a
bounded retirement grace. Pending subscriptions are cancelled promptly when
their watch closes. Room-wide encryption and connection failures still revoke
room admission.

No audio queue, image queue, timestamp, permission or presentation-age budget
was widened. Mandatory per-track encryption readiness and final output
admission remain in place. A separate subscriber transport adds a peer
connection per room; resource measurements must account for that tradeoff.

The repeated pre-loopback group still passed only 14/17 media cases. Switching
and long static-picture sessions lost packets; a timeout case preserved media
but failed stereo separation. In a further sampled static failure, the server
resumed its audio forwarding counters after a short ingress interruption while
the receiving client's packet counter stayed frozen; native ICE later began
reconnecting. These are retained failures, not clock/PCM recovery successes.

Inspection of selected ICE pairs found that the purported loopback fixture
advertised and selected IPv6 VPN/virtual-adapter addresses. `bind_addresses`
restricts HTTP listeners and does not by itself restrict RTC interface gathering.
The fixture now filters SFU ICE candidates to IPv4 loopback. Native WebRTC also
ignores loopback adapters by default, so the first server-only restriction
correctly failed to connect. A new explicit `GOODSHARING_LOCAL_LOOPBACK=1` native
factory option enables loopback adapters for the fixture and local launcher.
Ordinary client defaults remain unchanged, and no PC/VPN configuration changes.
Long diagnostic checks audit both selected media addresses as loopback.

## Validation

The final loopback group passes **22/22 native tests**, **4/4 native stereo
regressions**, and the native shutdown regression. The normal two-client UI
smoke and room lifecycle integration pass, including eight join/leave cycles,
publication/subscription cleanup, pending-connect cancellation and an idle-room
check. Both UI clients join with capture, sharing and watching inactive.

The final actual-media group passes **14/17 cases**: 12/14 matrix cases, the
58-second server-only-reporting switch run, and no-trace 720p receiving. The
58-second static 1080p run fails continuity. Three original failures remain:

| Case | Retained failure |
| --- | --- |
| Matrix static-to-motion | 130 ms quiet interval; static interval PCM coverage 93.38%, below 95%. |
| Matrix withheld subscription | 102 ms quiet interval; candidate cancellation, current-watch preservation, stereo and all handover-specific checks pass. |
| Long static 1080p | 152 ms quiet interval; final stereo and static picture remain valid. |

Each failure's first recorded playback fault is the existing sustained-low-
headroom reset. Later endpoint/presentation recovery is also observed in these
sessions. This identifies the next reproduction target; it does not prove that
one reset accounts for every silent sample. The unchanged 40 ms continuity
limit remains failed. This checkpoint is runnable for local trials, with
intermittent audio gaps; it is not a reliable release.

The final 58-second switch and static sessions each select three media candidate
pairs whose server and client addresses are in `127/8`. The server masks host
octets in remote addresses; `127.0....` still establishes the loopback prefix.
This verifies the corrected fixture's path. It does not establish that every
earlier failure was caused by a VPN adapter. Final non-wrong-key first-tone
startup spans 1.834–2.006 seconds with test client reporting and 3.809–3.850 seconds
with unchanged client reporting. These are startup times, not physical
end-to-end latency measurements.

Footprint samples remain attached to every actual case. Low CPU/memory release
targets remain open: the current sender still uses CPU color conversion and
scaling before its two hardware encoders. The loopback no-trace 720p precheck
uses about 1.99 CPU cores and 287 MiB peak private memory for that sender, and
0.58 core/161 MiB for the receiver including its test observer. These are short
single-share measurements on this PC, not two-share or release budgets.

Validation results are recorded with current executable and SDK hashes in
`artifacts/20261023-transport-recovery-validation.json`. Failed intermediate
experiments and checkpoint-48 evidence remain preserved. Results from older
SDK builds do not validate this changed runtime.

The launcher smoke check joins both normal clients and verifies that audio
receiving is enabled while capture, publication and watching all remain off.
The live matrix uses owned fixture windows and distinct generated stereo tones,
real WGC/WASAPI capture, encrypted SFU forwarding, Qt presentation and own-process
playback observation. It does not measure physical speaker/display alignment.

The fault case deliberately withholds the 1080p subscription callback and
requires the existing 720p picture and persistent audio output to survive its
five-second timeout. That test injection is trace-gated and absent from the
interactive launcher's environment.

The launcher uses the existing explicit local SFU 500 ms report profile while
leaving client report defaults unchanged. This is not a production reporting
policy; the experimental Go server's dependency notices and distribution remain
open. Trace-only client 500 ms tests are labelled separately.

```powershell
.\scripts\start-local-demo.ps1 -Smoke
C:\Users\Hello\MiniConda3\python.exe scripts/test_remote_av.py --case all --clock-profile 500ms --trace-timing --visible-view
C:\Users\Hello\MiniConda3\python.exe scripts/test_remote_av.py --case switch --clock-profile server-only-500ms --trace-timing --visible-view
```

SDK rebuilds can use `build-patched-sdk.ps1 -TestTransport -TestShutdown`.
The combined build defers old-engine installation until the matching engine
has rebuilt; app builds continue to reject stale SDK hashes.

Next reproduce the three-consecutive-block `<10 ms` headroom fence against the
retained exact PCM/deadline traces before changing recovery. Keep source A/V,
endpoint residence, freshness and final encryption permission gates intact.
