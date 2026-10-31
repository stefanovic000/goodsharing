# Automated test plan

Status: local execution is authorized and initial native feasibility tests are in progress. The Windows PC is the primary test environment: automate local capture, audio, transport integration, UI, concurrent use, soak, and packaged-build checks there before final VPS deployment. Ubuntu is for final remote validation, with earlier temporary use only for a specific test that local tooling cannot reasonably cover.

## Test principles

1. Test observable behavior and media correctness, not code that mirrors the implementation.
2. Use controlled synthetic windows/audio and test-owned processes. Never capture an arbitrary existing window or microphone to produce fixtures.
3. Keep timestamps, frame IDs, quality, and real encoder identity in structured reports. Preserve the difference between requested, encoded, received, decoded, and presented frame rates.
4. Measure local capture, loopback transport, public Internet transport, and actual multi-PC behavior separately.
5. Tag unavailable tests as skipped or manual with a reason; never turn them into successful results.
6. Pin dependency versions and record OS/GPU/driver and build information. Redact participant tokens and authentication secrets.
7. Give each fixture owned PIDs, a bounded duration, and cleanup. Existing applications and services remain running.

## Controlled media fixture

Create a test-owned window rendering a moving pattern at 60 FPS, a visible monotonic frame ID/timestamp, fine text, and a periodic flash. Generate deterministic stereo audio with distinct frequencies on left/right channels and a click synchronized with the flash.

Run a second test-owned audio app emitting a different tone. That makes application-audio isolation measurable without using real conversations. A test-owned receiver plays a third identifiable tone to detect feedback into desktop capture. A short supported capture capability probe precedes these tests.

Use frame callbacks or decoded-frame buffers for automated measurements. Supplement with actual presentation timestamps when available; measuring a decoder callback alone is not proof that a display presented every frame. Save short controlled diagnostic media only when needed and keep it separate from user content.

## Test matrix

| Area | Automated checks | Evidence |
| --- | --- | --- |
| Share lifecycle | Start/stop/restart, canceled picker, source exit, rapid repeated actions | State transitions, bounded completion, no outgoing media after Stop. |
| Video capture | Window/monitor, moving frame IDs, resize, aspect ratio, cursor behavior | Captured size, frame age, missing/duplicate IDs, pixel checks. |
| Hardware media | Actual encoder/decoder identity, supported formats, concurrent sessions, unavailable hardware | Structured capabilities and measured encode/decode rates; no hidden software fallback. |
| Exact qualities | 1920x1080 and 1280x720, both 60 FPS; layer/track subscription selection | Decode each output and inspect resolution, timestamps, keyframes, bitrate, and frame IDs. |
| App audio | Include selected process tree; second app and receiver tones excluded; process exit/restart | Spectral analysis and sample counters. |
| Desktop audio | Test app tones included; received-playback tone excluded where supported; explicit unsupported state otherwise | Spectral analysis, route metadata, feedback detection. |
| Audio handling | Stereo, sample formats/rates, silence, irregular packet sizes, discontinuities | Correct channels, amplitude, resampling, underrun/overrun counters. |
| Audio/video sync | Synchronized flash/click, long-run drift, quality switch, reconnect | Decoded audio/video timing correlation. |
| Concurrent use | Three publishers; sharing while receiving two other shares | Per-stream rates, client/server CPU/RAM/GPU, audio stability. |
| Network impairment | Controlled delay, jitter, bandwidth limits, loss, blocked UDP | Recovery, bitrate/quality adaptation, bounded latency, explicit connectivity failure. |
| Access control | Expired/invalid tokens, room mismatch, join/rejoin, duplicate identity handling | Expected rejection and bounded retries; no signing secret in client or logs. |
| End-to-end encryption | Matching/missing/wrong keys, both video qualities and shared audio, reconnect, late joins, rotation | Actual encrypted publication types, correct decoded pixels/tones with matching keys, unusable media otherwise; measure overhead separately. |
| UI | Start dialog choices, stop control, tile controls, quality/volume, DPI/keyboard focus | Qt tests and screenshots of controlled states. |
| Resource cleanup | Repeated shares/connections, app exit, failure during setup | Owned processes/sockets gone; no growing handle/thread/queue count. |
| Client footprint | Idle joined room, sender only, viewer with one quality/share, combined three-share workload | CPU time per wall second and machine percentage with logical CPU count; private committed and resident working-set peaks; hardware/media paths and queue bounds. Compare equal workloads without diagnostic recording retention. |
| Packaging | Launch from packaged folder with clean runtime resolution | Startup/capture/transport smoke report without build-tree paths. |

Network impairment must apply only to the test's own traffic, using a test proxy or isolated fixture. Do not alter the PC's general network configuration or the Ubuntu host's global traffic control to affect unrelated sessions. If the necessary isolation/tooling is absent, record the gap rather than installing privileged interception software automatically.

## Initial quantitative targets

These are proposed acceptance targets, not measurements already achieved by the client.

| Metric | Initial target and measurement |
| --- | --- |
| Video rate | Moving 60 FPS fixture: each requested quality averages at least 59 decoded FPS over a 5-minute clean local run after warmup. Report presentation rate separately. |
| Frame stalls | No unexplained decoded-frame gap above 250 ms on a clean local path; report percentiles and intentional source inactivity separately. |
| Local video latency | Capture-to-decoded-frame p95 at most 150 ms under the target workload. Report presentation and Internet latency separately. |
| Audio/video offset | Absolute flash/click alignment error p95 at most 80 ms; additional drift at most 20 ms over 10 minutes. |
| Quality changes | Reach requested quality within 2 seconds on a clean path, recover through a valid keyframe, and keep audio continuous. |
| Stopping capture | End publication within 1 second, release capture resources promptly, and show capture stopped. |
| Reconnect | On restoration after a controlled 10-second outage, reconnect within 10 seconds or display a clear terminal/retry state; no duplicate share/audio tracks. |
| Resource stability | After warmup, a 60-minute local soak shows no continuing memory/handle/thread growth trend. Compare repeated equal-workload windows and investigate growth, rather than judging one allocation spike. |
| Server workload | For three publishers and the initial small-group viewer scenarios, SFU sustained CPU below 70% of the two-core VM capacity and no new resource exhaustion; report total VM load and existing-workload variation separately. |

Tune targets only from recorded evidence, and label any changed requirement or degraded mode. The final Internet latency requirement depends on actual geography and networks; do not invent a guarantee from the local targets.

Low CPU and memory use is a primary user requirement. Establish numerical client budgets from integrated measurements on target hardware, keeping the existing latency/correctness gates. The owned-process sampler in `scripts/process_footprint.py` uses only handles from test-created subprocesses and samples every 250 ms with bounded storage. `test_share_publisher.py --profile-resources` records the synthetic sender plus receiver of both qualities separately from the loopback SFU; it excludes the Python driver and its optional load workers. Its middle window excludes the first five and last two sampled seconds but is not an exact media-phase marker. Private bytes are committed memory, not resident RAM; sampled peaks can miss shorter spikes. Software decode, pixel inspection and retained recordings must be identified before comparing to an actual app. Short runs do not establish leak-free operation or the 60-minute soak gate.

## Execution tiers after the user asks to start

### Tier 1: every meaningful change

Build with warnings enabled; run focused lifecycle/format/routing tests and a short controlled capture/audio smoke test. Use MSVC diagnostics and suitable sanitizers where compatible with the actual dependencies. Avoid adding tests for cosmetic reversible edits or rerunning the full soak without a relevant reason.

### Tier 2: media integration

Run a local SFU using a supported prebuilt runtime. Start independent controlled publishers/receivers, decode the actual streams, and verify the two video qualities and audio isolation. Report true simulcast versus paired tracks accurately. Test both qualities explicitly; an adaptive subscriber may choose 720p even when a test is named 1080p.

### Tier 3: target workload and soak

Run three publisher equivalents and 3 then 10 logical viewers. Record the actual number of client connections, subscriptions, rooms, and delivered qualities. A server-side packet-forwarding load generator is not a replacement for Windows GPU decoding/rendering. Share-and-view tests on one PC are useful but do not prove performance on three independent PCs.

Run short exploratory load tests first. Run 5-minute correctness, 10-minute sync/drift, and 60-minute soak tests only after shorter tests pass. Long tests run as background owned processes so progress and cancellation remain responsive.

### Tier 4: Ubuntu and the public path

Enter this tier for final deployment after the PC readiness gate in roadmap milestone 5 passes. Earlier temporary Ubuntu diagnostics must state the specific blocker, why local testing is insufficient, and their cleanup; they do not replace local tests or constitute release deployment.

Use the [port-8765 deployment plan](05-ubuntu-deployment.md). Check listeners, authentication, actual ICE candidate selection, TLS, packet loss, bandwidth, and decoded/rendered output through the Internet. Record the SFU process CPU separately from load generators and existing services. Bound generated traffic and duration; stop if test-owned load threatens existing workload stability.

### Tier 5: human-dependent acceptance

These checks accompany the relevant earlier tier; their numbering does not postpone PC checks until after deployment. Complete local headset/speaker listening, available game capture/coexistence checks, HDR appearance, and UI usability before the final VPS stage where possible. The no-injection/no-hooks/no-game-memory constraint applies from the start. Initial game targets are Counter-Strike 2 and Rocket League; add other Steam games individually. Check window/display capture, selected-app audio, normal gameplay, focus/alt-tab/fullscreen behavior and frame-time impact as soon as a relevant local path is usable, then repeat relevant checks on the integrated/packaged client. Record actual versions and observations; synthetic fixtures or source inspection alone do not mark a game as tested. Game launches, matches or launch/trust-setting changes are not automatic fixture operations. Each friend's GPU/network and public-path behavior require those environments later. Automate preparation and telemetry; do not mark checks complete without performing them.

## Reports and cleanup

Each run should produce a small Markdown summary and machine-readable JSON with environment, versions, scenario, sample duration, actual qualities, frame-rate/latency/audio statistics, CPU/RAM/GPU, test results, skipped tests, and limitations. Keep logs local or access-controlled. The existing historical JSON files remain unchanged as evidence of earlier experiments.

Screenshots and diagnostic media must contain controlled fixtures only unless the user explicitly selects real content. Cleanup verifies that only test-owned services/processes were stopped and pre-existing applications remain running. Failed runs preserve enough logs for diagnosis and still attempt bounded cleanup.
