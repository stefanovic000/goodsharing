# Decisions, evidence, and unresolved items

Date: 2026-10-20, Europe/Budapest. This record distinguishes discussion decisions and historical measurements from work still to be implemented.

## Recorded decisions

### Capture indicator (yellow border) - accepted, 2026-10-28

Windows Graphics Capture draws a one-pixel yellow indicator on the captured
window's own edge, and along the screen edge for display capture, on Windows 10
22H2. The user accepted it ("I am good with yellow border"), so the capture stack
stays WGC-only for windows. Rejected alternatives and why:

- **DXGI desktop duplication** (no indicator) - whole-monitor only, cannot target a
  window; the user declined adding it for now.
- **GDI `BitBlt`/`PrintWindow` window mode** (no indicator, no injection) - loses
  occlusion independence and returns black/static frames for some GPU-rendered
  apps; not needed once the indicator was accepted.
- **In-process graphics hook injection** (what Discord does: it injects
  `DiscordHook64.dll` and sets `use-graphics-capture: false`) - forbidden here by
  the documented no-injection/no-hooks anti-cheat boundary.
- **`IsBorderRequired(false)`** - unavailable on this OS: the interface is absent
  (`E_NOINTERFACE`), and it exists only on Windows 11 22H2+ for a packaged app with
  the restricted `graphicsCaptureWithoutBorder` capability.

The indicator is local only: captured frames contain no indicator pixels
(measured), so viewers are unaffected. Evidence and the measurements are in
[the conversion checkpoint](59-capture-conversion-libyuv-critical-path.md).

| ID | Decision |
| --- | --- |
| D01 | Build a separate Windows desktop app with a minimal UI. C++20 and Qt Quick/QML are the proposed client stack. |
| D02 | The first release covers screen sharing, viewing, and shared application/desktop audio. Mumble remains the voice client. |
| D03 | Microphone voice chat is a far-future option; preserve module boundaries and implement no voice system now. |
| D04 | Offer audio choices when starting a share: selected application, desktop, or off. |
| D05 | Target three simultaneous screen sharers, each offering 1080p60 and 720p60; encode on sending PCs and forward on the server. |
| D06 | Prefer LiveKit SDK/server reuse. Prove exact layer configuration and hardware support before committing to a media path. |
| D07 | Use Windows capture/audio APIs and keep capture outside the game process. Exclude injection, rendering hooks, game-memory access, kernel capture drivers, and in-game overlays. |
| D08 | Keep existing applications/services running. Cleanup only this project's own processes. |
| D09 | The Windows PC is the primary development/test environment. Final hosting may use port 8765 on the saved Ubuntu SSH destination after local release readiness. |
| D10 | Automate as much meaningful testing as possible, clearly distinguishing local/remote, simulated/real, and automated/manual results. |
| D11 | The original planning-only boundary was respected. The user's subsequent “continue” authorized local implementation and testing. |
| D12 | Final VPS deployment comes at the end. Earlier Ubuntu use is allowed only for a necessary, temporary, bounded test of a specific issue that local testing cannot reasonably cover; return to PC development afterward. |
| D13 | E2EE is feasible and recommended; keep room media keys on clients. Validate the pinned SDK before claiming privacy or performance. See [encryption design](07-encryption.md). |

## Historical evidence already in the workspace

These files predate this planning task and must be preserved:

- [Encoding and connection assessment](../oracle-vm-encoding-assessment.json)
- [SFU forwarding benchmark](../oracle-sfu-forwarding-benchmark.json)
- [Historical temporary benchmark runner](../temporary_sfu_benchmark.py)

The historical runner references a temporary VM directory that was removed. It is an audit artifact, not a currently runnable project setup or an instruction to execute it.

### VM and encoding findings

The earlier authenticated SSH inspection found two Ampere ARM cores, 12 GB configured RAM, and no hardware video encoder. The 2 Gbps figure came from instance configuration, not an Internet throughput test. Minecraft was left running.

For two concurrent shares with 1080p passthrough plus 720p and 360p encoding, a short synthetic H.264 test reached about 49-51 FPS per share with ultrafast encoding. Removing 360p raised the result to about 61-63 FPS per share in a later short test, which provides little real-time headroom. These tests support sender-side encoding rather than relying on VM transcoding.

### SFU forwarding findings

An earlier temporary test ran official LiveKit server 1.13.10 and CLI 2.19.0 with prerecorded H.264 1080p60/720p60 input and encrypted media over local loopback UDP.

| Logical viewers receiving all three shares | Actual delivered quality | Approx. frame-marker rate | SFU CPU as fraction of total VM capacity | Outgoing video/transport payload |
| --- | --- | --- | --- | --- |
| 3 | 720p | 59.98-60.02 FPS | 6.71% | 29.16 Mbps |
| 10 | Adaptive mix of 1080p and 720p | 60.04-60.12 FPS | 24.22% | 150.76 Mbps |

The larger test reported per-receiver packet-loss figures from 0% to approximately 0.054%. It had 30 subscriber connections representing ten logical viewers of three screens, in three rooms with one publisher each. The measurement windows were about 25 seconds, within 42-second subscriber runs.

The load generator, receivers, SFU, and Minecraft shared one VM. Total VM busy time during the larger measurement was about 75.68%, while the SFU alone accounted for 24.22% of total capacity. Keep those figures separate.

Original scenario filenames said all-1080p, but subscribers selected qualities adaptively. The JSON report explicitly corrects that label. This was **not** an all-1080p worst-case test and did not decode video, validate rendered image quality, capture real Windows screens/audio, or traverse the public Internet.

The temporary VM tools/processes were removed, and the historical cleanup confirmed Minecraft and Codex were still running. These results justify a native client prototype, not a guarantee of client hardware encoding, anti-cheat approval, Internet latency, audio quality, or a maximum viewer count.

## Resolve during execution, not by asking a long questionnaire now

| Item | Planned resolution |
| --- | --- |
| Windows SDK can publish exact hardware-encoded 1080p60/720p60 | Capability spike, actual encoder telemetry, decoded-media tests. Use the documented fallback if needed. |
| Available friends' GPUs/Windows versions | Start with the current PC; record each other machine when it becomes available. Do not assume NVIDIA-only support. |
| Process-loopback support on Windows 10 build 19045 | Runtime probe and controlled app-audio isolation test, accounting for Microsoft/OBS documentation differences. |
| Desktop sound feedback | Prove self-playback exclusion or expose a supported routing limitation before enabling that combination. |
| Hostname/TLS/private-group credentials | Resolve when a public Ubuntu endpoint is needed; do not deploy an insecure placeholder. |
| TCP/UDP 8765 reachability | Inspect listeners and network rules after execution starts; validate actual media connectivity. |
| Restrictive-network fallback | Add ICE/TCP or TURN only if needed and after resolving any additional public listener requirement. |
| Actual game capture/coexistence | Initial targets: Counter-Strike 2 and Rocket League, then other Steam games individually. Preserve the no-injection/no-hooks boundary throughout development and run environment-specific capture/coexistence checks when available. |
| Fine visual styling and app name | Use GoodSharing as a working name, restrained dark styling, and plain controls; refine after media feasibility passes. |
| Voice feature | Explicitly outside this release; reconsider only on a future user request. |

## Planning handoff

Initial native SDK, hardware encoding, capture, and encrypted synthetic-media evidence is recorded in [the implementation report](08-local-implementation-results.md) and [the mismatch fix report](09-frame-metadata-fix.md). The user subsequently authorized step 1 of [local integration](10-next-local-integration-plan.md); [its results](12-step1-results.md) record the patched stereo/lifecycle APIs and local verification. Full app integration and deployment remain later work.
