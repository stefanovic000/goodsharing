# Remote SFU deployment and first internet A/V case

Date: 2026-10-30, Europe/Budapest

Status: **SFU deployed on the Oracle host; 720p A/V case passes through it**

This is the separate-machine step of the roadmap: media no longer goes through a
local SFU on the same PC.

## Host and layout

Oracle Cloud instance `132.226.207.140` (Ubuntu 24.04 LTS, aarch64, 2 cores,
12 GB RAM), reached with the existing key through `~/.ssh/config` alias `sfu`.

```text
/home/ubuntu/goodShare/
  livekit-server     1.13.10 (arm64)
  livekit.yaml       signalling 7880/tcp, media 7881/udp, use_external_ip
  livekit.key        API key + secret (root:livekit 0640)
  logs/livekit.log
```

`livekit.service` (systemd, `User=livekit`, `WorkingDirectory=/home/ubuntu/goodShare`)
runs as `livekit` with ACL traverse/read on that folder only; enabled and
restart-on-failure. The host firewall (netfilter, not ufw) rejects everything
except SSH and Minecraft by default, so two rules were added and persisted with
`netfilter-persistent`: `7880/tcp` and `7881/udp`.

## No peer-to-peer, by design

LiveKit is an SFU: every participant has one WebRTC connection, to the server.
There is no client-to-client media path. The TCP/TURN media fallback was
**disabled on request** (`rtc.tcp_port: 0`), so media is UDP only to the server;
clients that cannot reach UDP 7881 cannot connect (previously they would fall
back to TCP through the same server).

## Harness support

`scripts/test_remote_av.py` gained a remote mode:

```powershell
python scripts/test_remote_av.py --case 720 --server-url ws://132.226.207.140:7880
```

It skips the owned local SFU, mints publisher/subscriber JWTs from the API key in
`artifacts/sfu-credentials.ini` (or `--server-key/--server-secret`), generates a
fresh 64-hex E2EE key per run and leaves the loopback ICE override unset.

## First remote result

| Metric | Value |
| --- | --- |
| case | 720p, 18 s watch |
| server | `ws://132.226.207.140:7880` (room `remote-av-720`) |
| frames drawn / admitted | 330 / 335 |
| longest audio quiet | 4.21 ms |
| audio coverage | 1.000 (peak 1672) |
| checks | all passed |

The SFU log shows the room and both participants. Frames per second over the
internet (~19 fps of a 30 fps share) are lower than the same-PC loopback runs, so
the next tuning step is congestion behaviour on a real path rather than another
local measurement.

## Remote measurements and tuning

Repeated 720p cases through the Oracle server, all with two clients on this PC
over the public internet:

| Run | Frames drawn | Longest audio quiet | Notes |
| --- | ---: | ---: | --- |
| first | 330 | 4.21 ms | passed, ~18 fps of a 30 fps share |
| smooth profile | 332 | 5.69 ms | same rate; the 80 ms lead tripped the lead gates |
| 1080p | 316 | 0.02 ms | same ~18 fps at triple the bitrate |
| 60 fps share | 594 | passed | delivery scales with the offer (~55 %) |
| 1.5 Mbit/s share | 563 | 0.0 ms | full rate at a bitrate the path carries |
| after the floor relaxation | 594, 589 | 0.0 ms | full rate in two runs |
| same build, next run | 390 | **682 ms** | failed: real-path jitter/loss returned |

Conclusion: the path is variable. At ~10 Mbps the uplink carries the stream most
of the time (full 30 fps), but a slower passage loses about half the frames or
delivers a long audio gap. Lowering the publisher to 1.5 Mbps restored the full
rate, which points at the uplink rather than at the SFU (the server stayed idle,
76 % CPU free, only benign data-channel warnings).

Changes made for this:

- **Transport-estimate follow.** The encoder still keeps the screen-share floor
  (`60 %` of the configured rate) on healthy links, but if the estimate stays
  below it for 2.5 s the publisher accepts the estimate instead of pinning the
  bitrate; the counters `bitrate_floor_hits` / `bitrate_floor_relaxed` show which
  side won.
- **Test override.** `GOODSHARING_TEST_SHARE_BITRATE` sets the 1080p budget (720p
  is half) so a constrained path can be probed without a rebuild.

## Remote rooms pick the smooth buffer by default

The local profiles were tuned on loopback. A room whose host is not this machine
now defaults to **Smooth** (80 ms video lead) unless the user picked a profile in
the UI: streamProfileExplicit records that choice, and the test override
GOODSHARING_STREAM_PROFILE still wins. The harness gained --profile auto to
exercise the default (it skips the pin and allows the smooth lead gate).

The path was degraded during these checks, so the numbers below show range rather
than a regression: auto-profile runs measured 167 and 135 frames with the 80 ms
lead applied, while runs an hour earlier on the same code measured 594 and 589
frames. A path that delivers 135 frames cannot pass the audio coverage gates, so
those failures describe the network at that moment.

## Attempted: shedding frames under a collapsed estimate (reverted)

A frame-grid shed was added to EncodedVideoOutput: when the transport estimate
sits below half the configured bitrate, skip input frames on a fixed grid instead
of letting the path drop arbitrary ones. Measured, it misfired: on a loaded PC the
estimate drops for **CPU** reasons, so the output started shedding 126 frames in a
run where the encoder was merely starved, and the viewer's delivery fell with it.
The change was reverted; the bitrate-floor follow stays because it only relaxes the
bitrate and does not remove frames the viewer could have presented.

The same session also re-hit the known 41-50 ms audio flake
(continuous_tone_without_gap, gate 40 ms) while unrelated applications held the
CPU around 75 %; earlier runs of the same build passed on an idle machine.

## Open items

1. A rate limiter that reacts to the transport estimate by lowering the frame
   rate as well as the bitrate, and FEC on the audio path: the 682 ms gap and the
   135-frame run are both real-path events, not local behaviour.
2. A second physical PC (current run is two processes on one PC over the internet).
3. Signing and the clean-machine install test.
4. Micro-cut investigation (on hold per the user).

## First two-machine result (2026-10-31)

Second PC: Windows 11 Pro, Ryzen 7 5700U, app copied to Desktop\GoodSharing, room
\home-test\ through the Oracle SFU. Viewer started from the desktop, publisher on
the first PC.

| Metric | Value |
| --- | ---: |
| watch state | watching encrypted audio/video |
| frames submitted / admitted | 3038 / 3038 |
| frames drawn | 2329 (~77 % of offered) |
| longest audio quiet | 0 ms |
| paint average / maximum | 3.80 / 13.1 ms |

The viewer exited when the share ended, which is the smoke-mode behaviour. Two
obstacles had to be fixed first: a stale access token (401 invalid signature) and a
room key that did not match the publisher's (the client joined, could not decrypt
and reported the share as ended). With matching key and token the client received
and admitted every delivered picture; the remaining shortfall is paint cost on the
laptop, the same limit measured on the first PC.
