# Windows capture, audio, and compatibility

Status: capture and local preview are implemented; publication integration, receive/playback, and further lifecycle tests remain planned. See [measured capture/audio results](08-local-implementation-results.md) and [the next phase](10-next-local-integration-plan.md).

## Windows baseline

The prior read-only inspection found Windows 10 Pro 22H2, build 19045, on the current PC. Recheck capabilities when execution starts rather than treating this as a requirement for every friend’s PC. Target Windows x64; build an explicit capability report for each supported OS/GPU combination.

The project now uses MSVC, Windows SDK headers, project-local MSVC Qt 6.8.3, and LiveKit C++ 1.12.3. An earlier inspected system Qt installation used MSYS2/UCRT; keep that incompatible C++ ABI out of this build. Continue installing project dependencies locally when practical and avoid changing system tools as a setup side effect. See [dependency pins](../dependencies.lock.json).

## Video capture

Use Windows Graphics Capture through documented WinRT/Win32 interop. Keep a Windows-managed source selection/consent flow. Support a window or monitor; identify a selected window's owning PID through normal window APIs when associating its audio.

Capture on a worker pipeline using D3D11 textures and a monotonic clock. Maintain aspect ratio when scaling to 1920x1080 and 1280x720; default to SDR in the first release and explicitly handle HDR sources without washed-out output. Prefer GPU texture processing. Measure any GPU-to-CPU copy required by the selected SDK rather than calling the path zero-copy without evidence.

Request 60 FPS and measure capture, encode, receive, decode, and render independently. A static desktop can legitimately produce fewer changed frames; test frame-rate capacity with a deliberately moving source. Bound queues and discard stale video frames under overload rather than accumulating seconds of delay.

Handle source closure, resizing, display disconnection, minimized windows, display sleep, lock/unlock, and device removal. A lost or inaccessible source should stop its outgoing content and show a clear status, rather than silently switching to another window or the whole desktop.

## Audio modes chosen at share start

| Mode | Mechanism | Intended behavior |
| --- | --- | --- |
| Off | No outgoing shared-audio track | Video only. |
| Selected application | WASAPI process-loopback include-process-tree mode | Capture that app/game and its children; other applications, including independently running Mumble, remain outside the captured mix. |
| Desktop | WASAPI loopback / supported process-exclusion variant | Capture desktop sound while avoiding recapturing our own received-share playback. Mumble and notifications may be included. |

Both loopback approaches operate through Windows audio APIs without code injection into another process or a custom capture driver.

Microsoft's process-loopback sample lists build 20348 as its minimum, while OBS documents application-audio capture on Windows 10 version 2004 and later. Because the current PC reports build 19045, **probe actual activation at runtime** and test it; do not reject it solely from the sample's build number or promise support solely from the OBS version statement. Compile-time header availability does not prove runtime support.

For window sharing, start with its owning process tree. For a browser or app that renders audio in a separate process, validate its process relationship or allow an explicit audio-source choice. For monitor sharing, present a separate application selector if application audio is chosen. Process restart and exit must not cause capture of an unrelated reused PID.

Desktop capture should exclude this app's process tree where the supported API allows it, so received screen-share audio is not sent back into the room. Conventional endpoint loopback includes all sound on that output endpoint; if robust self-exclusion is unavailable, clearly expose the limitation and require explicit routing or disable the unsafe combination. Do not silently fall back from application audio to all desktop sound.

## Audio pipeline

- Normalize capture to a stable format, initially 48 kHz stereo, and encode as a screen-share audio track using a suitable Opus setting. Tune bitrate from measured quality rather than fixing a claim of losslessness.
- Preserve stereo and program audio; microphone-oriented noise suppression, echo cancellation, and automatic gain control should not alter the shared game/music track by default.
- Timestamp audio/video from a common monotonic timing model. Resample carefully for device clock drift and variable WASAPI packet sizes.
- Handle float/integer formats, silent buffers, discontinuities, channel layouts, device changes, and capture cancellation.
- Use bounded playback buffers; collect underrun, overrun, dropped-sample, and clock-drift counters.
- Support independent receive volume and mute per share. The sender hears its original app normally; its share preview must not add duplicate local audio playback.
- Stop, pause, reconnect, and source changes must coordinate both video and its associated audio.

Protected content or unavailable audio routes can yield silence or a capture failure; report that state and respect the platform restriction.

## Anti-cheat compatibility design

Compatibility with normal anti-cheat-protected play is a core product requirement throughout implementation. Initial named targets are Counter-Strike 2 and Rocket League, followed by the group's other Steam games individually. This is an implementation constraint now, not an optional feature to decide at release.

Keep capture and playback in the GoodSharing process, use documented Windows APIs and normal GPU drivers, run as a standard user, and show controls in the app's own window. Exclude game DLL injection, game-memory inspection, rendering hooks, game overlays, custom kernel drivers, and anti-cheat bypasses from this design.

Use normal Windows hotkey facilities where available and report conflicts; avoid introducing a keyboard hook just to get a global shortcut. There is no remote-control feature.

Review the capture boundary whenever changing capture, audio, shortcuts or dependencies. The current application code uses Windows Graphics Capture's `CreateForWindow`/`CreateForMonitor` interop and WASAPI process-loopback activation; it obtains a window's owning PID through `GetWindowThreadProcessId`, without opening the game for memory access or injecting code. Hardware encoding runs through Media Foundation in our own process.

Run actual game capture/coexistence checks as soon as the relevant local path is usable, alongside development and before release. Here, compatibility verification means checking capture, audio, normal gameplay and interference on the installed game/build; it does not mean seeking an anti-cheat whitelist or changing the capture architecture to interact with the game. Record the app/game/OS versions and observed results. Do not launch games, join matches, change launch/trust settings or disable protections automatically. If window capture is unavailable, offer an explicitly selected supported display capture or report the limitation. Observed results for one game/build do not certify every future update.

## Sources

- [Windows screen capture](https://learn.microsoft.com/en-us/windows/apps/develop/media-authoring-processing/screen-capture)
- [Microsoft application-loopback sample](https://learn.microsoft.com/en-us/samples/microsoft/windows-classic-samples/applicationloopbackaudio-sample/)
- [WASAPI loopback recording](https://learn.microsoft.com/en-us/windows/win32/coreaudio/loopback-recording)
- [OBS application-audio capture support](https://obsproject.com/kb/application-audio-capture-guide)
- [OBS game-capture compatibility guidance](https://obsproject.com/kb/game-capture-troubleshooting)
