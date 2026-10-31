# Step 1 handoff — paused at the user's request

**Historical pause snapshot. The user resumed this work on 2026-10-20; current implementation and verification are in [step-1 results](12-step1-results.md).** The status and failures below describe the earlier pause, not the resumed build.

Date: 2026-10-20, Europe/Budapest. **Paused, incomplete. Do not mark step 1 or its exit gates complete.** No implementation should continue until the user resumes it.

No VPS/SSH changes were made. No unrelated application or service was stopped. At pause, the owned build had already exited; no Cargo/compiler, media-probe, or local SFU process remained in the process check.

## Implemented and built

- Added a reproducible C++ SDK wrapper patch exposing `AudioStream::Options.sample_rate` and `num_channels`, forwarding them through both track and participant receive paths. Existing defaults remain 48 kHz mono. Original release files remain in `.deps/livekit-1.12.3/`; the rebuilt wrapper is installed separately in `.deps/livekit-patched/`.
- Added project-local wrapper/source preparation scripts and a reviewable patch in `patches/livekit-cpp-1.12.3-stereo-build.patch`. The wrapper build reuses the release FFI DLL and omits only upstream's internal UniFFI build-version test adapter, avoiding a Rust build for the wrapper itself. Public room/media/token APIs remain present.
- Wrapper dependencies built successfully after setting `protobuf_MSVC_STATIC_RUNTIME=OFF` to match the other C++ objects' `/MD` runtime. The full app/probe build then passed. Logs: `artifacts/wrapper-build.log`, `artifacts/app-build.log`.
- Added `src/audio_playback.*`: bounded 48 kHz stereo WASAPI output with volume, queue/rejection/underrun metrics, and an owned worker. Tested with quiet synthetic audio only; it is not wired into the app UI.
- Added `src/room_connection.*`: an owned asynchronous join wrapper whose Stop joins the connector before disconnecting. It avoids concurrent SDK connect/disconnect state publication. Cancellation is not yet fast enough in the stalled-handshake test.
- Added `tools/stereo_probe.cpp`, `tools/lifecycle_probe.cpp`, `scripts/local_media_fixture.py`, `scripts/test_local_stereo.py`, and `scripts/test_local_lifecycle.py`.
- Hardened the existing native media receiver's stop admission: stop rejects late subscribed-track callbacks and moves stream/reader ownership out under a lock before closing/joining.

`scripts/build.ps1` now explicitly selects `.deps/livekit-patched`, requires its headers, and copies runtime DLLs after building. A fresh checkout must build the wrapper before the app. Setup/readme integration and dependency-manifest updates are still pending.

## Measurements and failures retained

| Test | Result | Evidence |
| --- | --- | --- |
| Encrypted stereo PCM, C++ wrapper + original FFI | Passed once: two distinct channels, left/right wanted amplitudes 3931/3920, cross-channel ratios approximately 0.0116%/0.0161%; no remaining server participants. This does not establish reliable stereo yet. | [Stereo run](../artifacts/20261020-025455-stereo/summary.json) |
| Stereo through actual WASAPI playback/process loopback | Failed: receiver reported two channels but both carried the same downmix. The process-loopback measurement correctly detected the same failure. Playback itself accepted 241,440 frames with no rejection, a 10 ms queue high-water mark, and a 22 ms device buffer. Do not weaken the channel-separation check. | [Playback run](../artifacts/20261020-025513-stereo/summary.json) |
| Twenty join/publish/disconnect cycles + three RAII abandonments + invalid token + cancelled stalled join | Failed overall: twenty normal cycles disconnected, maximum explicit Stop was 11.71 ms, room membership was empty, but old-engine negotiation-after-close errors reproduced and setup cancellation took 4009.69 ms against the 2000 ms test limit. | [Lifecycle run](../artifacts/20261020-025821-lifecycle/summary.json) |

Both stereo logs also contain one `Attempted to drop unknown FFI handle` warning. Determine its ownership/lifetime cause; the current stereo runner only gates the negotiation errors, so its single passing stereo result is not proof of fully clean lifecycle behavior.

The mono negative control, participant receive path, wrong-key stereo case, and complete stereo suite have not run yet. The existing video regression suite has not been rerun after this turn's wrapper/receiver changes.

## Engine patch prepared, not built or verified

Pinned engine source: Rust commit `a6f93a16`, FFI version `0.12.82`, matching the original SDK build metadata. The source ZIP omits Git submodules; the required libyuv source has now been fetched at exact commit `917276084a49be726c90292ff0a6b0a3d571a6af` and verified by SHA-256.

`scripts/prepare_livekit_engine.py` and `patches/livekit-rust-a6f93a16-shutdown.patch` contain proposed changes:

1. Cancel debounced negotiation during close, reject new offers after closure, cancel in-flight offer results on closure, clear the offer callback, and wake fast-negotiation tasks waiting for an answer. Keep synchronous socket closure. A native Rust regression test was added but has not compiled/run.
2. Advertise the native audio source's actual stereo layout in `AddTrackRequest.stereo`. The existing SDK leaves this unset. This is a concrete candidate fix for the observed intermittent downmix; it remains unverified.

The first engine build failed because the libyuv submodule was missing. That dependency is now present and the build script handles it, but **the build has not been resumed after that correction**. The old failed build finished all its remaining jobs and exited before pause. Its diagnostic log is `artifacts/engine-build.log`.

The prepared source files were restored from the verified ZIP and rewritten as UTF-8 after a Python default-encoding error during patch preparation. The preparation script now uses explicit UTF-8 and temporary-file replacement. Its generated patch must still receive code review and runtime verification.

Project-local build tools/dependencies now include Rust 1.97.1 under `.tools/rustup`/`.tools/cargo`, protoc 25.3 under `.tools/protoc`, and official WebRTC `webrtc-89d790b` under `.deps/webrtc-89d790b`. No system PATH or default system toolchain was changed. Build scripts contain the recorded source/archive hashes and use two Cargo jobs/four C++ jobs.

## Resume order

1. Review the proposed shutdown/stereo patch and source preparation scripts. Resume the engine build using `scripts/build-livekit-engine.ps1` with the appropriate Python path. Its Cargo artifacts are cached in `.deps/build-livekit-engine`; it downloads/uses only project-local dependencies. Preserve the failed log before redirecting a new build to the same filename if retaining that diagnostic detail is useful.
2. Once successful, verify the installed `.deps/livekit-patched/bin/livekit_ffi.dll` and import library are the patched engine, record hashes/build provenance, and rebuild the app. **Running the wrapper install again can restore the original FFI DLL, so install/build the engine after the wrapper.** Harden this build ordering before calling the setup reproducible.
3. Run the native Rust shutdown regression and the local lifecycle suite. Fix stalled-join cancellation (suspected extra signaling validation/fallback time) while keeping the timeout gate. Do not claim cancellation is immediate just because the caller requests stop.
4. Run all stereo cases, repeat encrypted receive/playback across fresh sessions, and investigate the unknown-handle warning. Preserve the 3% cross-channel limit, known-tone thresholds, and encrypted-publication/key checks. Add short-window wrong-key tone checks equivalent to the existing media harness before accepting the new audio suite as complete.
5. Add per-cycle resource measurements and explicit failure-path/cleanup checks where needed; verify no room participants or owned processes remain. Recheck original metadata/pacing/video/encryption regressions after the new runtime is installed.
6. Finish the dependency/license requirement record: preserve LiveKit/third-party notices and exact modified sources/build steps; inspect Qt module SBOMs and the pinned WebRTC aggregate license, FFmpeg/OpenH264 and Rust dependencies; record packaging obligations without marking redistribution cleared. Official Qt/Apache/FFmpeg license documentation was read, but no complete notice bundle or license register was written yet.
7. Update `dependencies.lock.json`, setup/build instructions, README and roadmap with actual passing results and remaining limits. Keep every step-1 checkbox open until its evidence is complete. Do not begin full room/share UI integration or deployment as part of finishing step 1.

The next-phase plan remains [document 10](10-next-local-integration-plan.md). Earlier passing video evidence remains [document 09](09-frame-metadata-fix.md); it is historical baseline evidence, not validation of the unbuilt engine patch.
