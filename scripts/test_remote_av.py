"""Actual owned capture -> E2EE loopback SFU -> Qt deadlines + WASAPI.

Own-process loopback checks stereo output and quiet post-stop samples. Software
clock/paint lag is measured; physical speaker/display synchronization is not.
"""
import argparse
import base64
import ctypes
import hashlib
import hmac
import os
import secrets
import threading
import json
from pathlib import Path
import subprocess
import time
from local_media_fixture import LocalMediaFixture, ROOT
from process_footprint import ProcessFootprint
from test_capture_sharing import stop


def mint_token(identity, room, api_key, api_secret, can_publish):
    def enc(value):
        return base64.urlsafe_b64encode(json.dumps(value, separators=(',', ':')).encode()).rstrip(b'=')
    claims = {'iss': api_key, 'sub': identity, 'nbf': int(time.time()) - 5,
              'exp': int(time.time()) + 600,
              'video': {'roomJoin': True, 'room': room, 'canPublish': can_publish,
                        'canPublishData': True, 'canSubscribe': True}}
    body = b'.'.join([enc({'alg': 'HS256', 'typ': 'JWT'}), enc(claims)])
    signature = base64.urlsafe_b64encode(hmac.new(api_secret.encode(), body, hashlib.sha256).digest()).rstrip(b'=')
    return (body + b'.' + signature).decode()


def remote_client_env(server, room):
    env = os.environ.copy()
    env['PATH'] = str(ROOT / '.deps/qt/6.8.3/msvc2022_64/bin') + os.pathsep + env['PATH']
    plugins = ROOT / '.deps/qt/6.8.3/msvc2022_64/plugins'
    env['QT_PLUGIN_PATH'] = str(plugins)
    env['QT_QPA_PLATFORM_PLUGIN_PATH'] = str(plugins / 'platforms')
    env['GOODSHARING_TEST_URL'] = server['url']
    env.pop('GOODSHARING_LOCAL_LOOPBACK', None)
    env['GOODSHARING_TEST_PUBLISHER_TOKEN'] = mint_token('publisher', room, server['key'], server['secret'], True)
    env['GOODSHARING_TEST_SUBSCRIBER_TOKEN'] = mint_token('subscriber', room, server['key'], server['secret'], False)
    env['GOODSHARING_TEST_E2EE_KEY'] = secrets.token_hex(32)
    return env


class _FileTime(ctypes.Structure):
    _fields_ = [('low', ctypes.c_uint32), ('high', ctypes.c_uint32)]


def _system_times():
    idle, kernel, user = _FileTime(), _FileTime(), _FileTime()
    ctypes.windll.kernel32.GetSystemTimes(ctypes.byref(idle), ctypes.byref(kernel), ctypes.byref(user))
    def value(ft):
        return (ft.high << 32) | ft.low
    return value(idle), value(kernel), value(user)


class CpuSampler(threading.Thread):
    """Average and peak system CPU during a run, without third-party packages."""

    def __init__(self):
        super().__init__(daemon=True)
        self.samples = []
        self._stop = threading.Event()

    def run(self):
        previous = _system_times()
        while not self._stop.wait(0.5):
            current = _system_times()
            idle = current[0] - previous[0]
            busy = (current[1] - previous[1]) + (current[2] - previous[2])
            previous = current
            total = idle + busy
            if total:
                self.samples.append(100.0 * busy / total)

    def stop(self):
        self._stop.set()
        self.join(timeout=2)

    def report(self):
        if not self.samples:
            return {'average_percent': None, 'peak_percent': None, 'samples': 0}
        return {'average_percent': round(sum(self.samples) / len(self.samples), 1),
                'peak_percent': round(max(self.samples), 1), 'samples': len(self.samples)}



def checkpoint_count(path):
    # QSaveFile replaces this owned snapshot atomically. On Windows a brief
    # sharing conflict while it is replaced is not a completed observation.
    # The caller's original deadline still bounds all retries.
    try:
        return len(json.loads(path.read_text(encoding='utf-8')))
    except (PermissionError, FileNotFoundError):
        return 0


def check_continuous_tone(observed):
    tone = observed.get('continuous_tone_observation', {})
    return (tone.get('started') is True and tone.get('duration_ms', 0) >= 1000 and
            tone.get('observed_frame_coverage', 0) >= .95 and tone.get('quiet_frame_fraction', 1) <= .05 and
            tone.get('longest_quiet_ms', 9999) <= 40)


def check_stall_observations(view):
    """Require a real GUI block with audio continuity and fresh video recovery.

    Audio priority intentionally keeps an established timeline alive while video
    presentation is blocked. The final stop must still revoke and fence playback.
    """
    names = ['before', 'after_block', 'recovery_100_ms', 'recovery_500_ms', 'recovery_2000_ms']
    rows = view.get('paint_stall_observations', [])
    checks = dict(paint_stall_exercised=False, stall_started_during_media=False,
                  stall_audio_timeline_preserved=False, stall_audio_continued=False,
                  stall_picture_frozen=False, stall_resumed_fresh=False,
                  stall_permissions_enforced=False)
    if [row.get('phase') for row in rows] != names:
        return checks
    audio = [row.get('playback', {}).get('audio', {}) for row in rows]
    fields = ['sampled_receiver_us', 'last_presented_receiver_us', 'last_media_audible_end_us',
              'reset_serial', 'device_resets', 'device_commits', 'committed_media_frames',
              'presented_video_frames']
    if not all(all(isinstance(row.get(key), (int, float)) for key in fields) for row in audio):
        return checks
    before, after, resumed = audio[0], audio[1], audio[-1]
    identity = ['owner', 'share_id', 'generation', 'video_sid', 'audio_sid']
    same_watch = all(all(row.get('watch', {}).get(key) == rows[0].get('watch', {}).get(key)
                         for key in identity) and row.get('watch', {}).get('ready') is True for row in rows)
    block_us = after['sampled_receiver_us'] - before['sampled_receiver_us']
    checks['paint_stall_exercised'] = (view.get('paint_stall_performed') is True and same_watch and
                                      view.get('maximum_test_gui_tick_gap_ms', 0) >= 200 and block_us >= 200000)
    checks['stall_started_during_media'] = (before.get('clock_anchored') is True and before.get('alignment_locked_after_media_commit') is True and
        0 <= before['sampled_receiver_us'] - before['last_presented_receiver_us'] <= 80000 and
        abs(before['last_media_audible_end_us'] - before['sampled_receiver_us']) <= 30000)
    timeline = audio[0:2]
    checks['stall_audio_timeline_preserved'] = (
        all(a.get('clock_anchored') is True and
            a.get('alignment_locked_after_media_commit') is True and
            a.get('failed') is False and a.get('wasapi_connected') is True
            for a in timeline) and
        after['reset_serial'] == before['reset_serial'] and
        after['device_resets'] == before['device_resets'])
    minimum_audio_frames = block_us * 48000 / 1000000 * .75
    checks['stall_audio_continued'] = (
        after.get('audio_continuity_through_video_stall') is True and
        after['committed_media_frames'] - before['committed_media_frames'] >= minimum_audio_frames and
        after['device_commits'] - before['device_commits'] >= minimum_audio_frames / 480)
    checks['stall_picture_frozen'] = (
        0 <= after['presented_video_frames'] - before['presented_video_frames'] <= 1)
    elapsed = resumed['sampled_receiver_us'] - after['sampled_receiver_us']
    media = resumed['committed_media_frames'] - after['committed_media_frames']
    checks['stall_resumed_fresh'] = (elapsed >= 1500000 and resumed.get('clock_anchored') is True and
        resumed.get('alignment_locked_after_media_commit') is True and media >= 48000 and
        resumed['presented_video_frames'] - after['presented_video_frames'] >= 60 and
        media * 1000000 / 48000 / elapsed >= .90)
    stopped = view.get('after_stop', {}).get('last_selection', {})
    stopped_playback = stopped.get('playback', {})
    stopped_audio = stopped_playback.get('audio', {})
    stopped_endpoint = stopped_playback.get('endpoint', {})
    checks['stall_permissions_enforced'] = (
        stopped_playback.get('closed') is True and stopped_audio.get('closed') is True and
        stopped.get('watch', {}).get('closed') is True and
        stopped.get('audio_watch', {}).get('closed') is True and
        stopped_audio.get('failed') is False and
        stopped_endpoint.get('running') is False and
        stopped_endpoint.get('reserved_frames') == stopped_endpoint.get('padding_frames') == 0)
    return checks


def check_quality_switch(view, case):
    rows = view.get('switch_observations', [])
    stopped = view.get('after_stop', {}).get('last_selection', {})
    history = view.get('handover_history') or stopped.get('handover_history', [])
    playbacks = [row.get('playback', {}) for row in rows if row.get('playback')]
    if stopped.get('playback'):
        playbacks.append(stopped['playback'])
    identity = [(p.get('session_id'), p.get('audio_sid')) for p in playbacks]
    checks = dict(switch_probe_completed=view.get('switch_complete') is True,
        persistent_audio_identity=len(identity) >= 3 and bool(identity[0][0]) and bool(identity[0][1]) and len(set(identity)) == 1,
        one_output_worker=bool(playbacks) and all(p.get('worker_starts') == 1 for p in playbacks) and stopped.get('playback', {}).get('worker_starts') == 1)
    if case == 'switch-source-exit':
        cancelled = next((h for h in history if h.get('outcome') == 'cancelled'), {})
        candidate = stopped.get('candidate_watch') or cancelled.get('candidate_watch', {})
        candidate_video = stopped.get('candidate_video') or cancelled.get('new_video', {})
        warming = next((s for s in rows if s.get('phase') == 'candidate_warming'), {})
        checks.update(stopped_during_warmup=bool(warming.get('candidate_watch')) and bool(candidate) and candidate.get('closed') is True and not view.get('watching'),
                      no_candidate_presented=candidate_video.get('queued_presentations', -1) == 0 and stopped.get('committed_handovers') == 0,
                      audio_role_revoked=stopped.get('audio_watch', {}).get('closed') is True)
        return checks
    intervals = view.get('audio_observer', {}).get('intervals', [])
    expected = 1 if case in ('switch-cancel', 'switch-timeout') else 2
    checks['switch_audio_observed'] = len(intervals) == expected and all(
        i.get('closed') and i.get('duration_ms', 0) >= 1000 and i.get('observed_frame_coverage', 0) >= .95 and
        i.get('quiet_frame_fraction', 1) <= .05 and i.get('longest_quiet_ms', 9999) <= 40 for i in intervals)
    if case in ('switch-cancel', 'switch-timeout'):
        checks.update(candidate_cancelled=len(history) == 1 and history[0].get('outcome') == 'cancelled' and stopped.get('cancelled_handovers') == 1,
                      no_unqueued_quality_commit=stopped.get('committed_handovers') == 0 and stopped.get('playback', {}).get('audio', {}).get('video_handovers') == 0,
                      no_candidate_presented=bool(history) and history[0].get('new_video', {}).get('queued_presentations', -1) == 0)
        if case == 'switch-timeout':
            before=next((s for s in rows if s.get('phase')=='before_first'),{})
            after=next((s for s in rows if s.get('phase')=='after_timeout'),{})
            checks['timeout_keeps_current_media']=(after.get('watching') is True and bool(before.get('watch',{}).get('video_sid')) and after.get('watch',{}).get('video_sid')==before.get('watch',{}).get('video_sid') and
                after.get('receiver_local_us',0)-before.get('receiver_local_us',0)>=5000000 and
                after.get('playback',{}).get('audio',{}).get('committed_media_frames',0)-before.get('playback',{}).get('audio',{}).get('committed_media_frames',0)>=4*48000)
    else:
        checks.update(two_video_commits=len(history) == 2 and [(h.get('from'), h.get('to')) for h in history] == [(720, 1080), (1080, 720)] and
                      all(h.get('outcome') == 'committed' and h.get('new_video', {}).get('queued_presentations', 0) >= 1 for h in history),
                      native_and_gui_agree=stopped.get('committed_handovers') == stopped.get('playback', {}).get('audio', {}).get('video_handovers') == 2)
    live_audio = [row.get('playback', {}).get('audio', {}) for row in rows if row.get('playback')]
    checks['clock_preserved_through_observation'] = bool(live_audio) and all(
        all(isinstance(a.get(key), (int, float)) and a[key] == live_audio[0].get(key) for key in ('reset_serial', 'device_resets')) for a in live_audio)
    checks['one_warming_image'] = bool(history) and all(h.get('new_video', {}).get('warm_queue_high_water', 99) <= 1 for h in history)
    checks['clock_and_device_preserved'] = bool(history) and all(
        all(isinstance(h.get('before_playback', {}).get('audio', {}).get(k), (int, float)) and h['before_playback']['audio'][k] == h.get('after_playback', {}).get('audio', {}).get(k)
            for k in ('reset_serial', 'device_resets')) for h in history)
    return checks


def run_case(folder, case, trace_timing=False, visible_view=False, app_path=None, duration_ms=None, clock_profile='upstream', server_debug=False, disable_presentation_floor=False, disable_audio_retry=False, server=None):
    folder.mkdir(parents=True)
    fixture = None if server else LocalMediaFixture(folder / 'server')
    children, logs, profiles = [], [], []
    result = dict(case=case, passed=False, local_only=True, actual_capture=True,
                  real_encrypted_receiving=True, physical_speaker_display_sync_measured=False,
                  unrelated_processes_stopped=False)
    result['remote_server'] = server['url'] if server else None
    cpu = CpuSampler()
    cpu.start()
    result['trace_timing_enabled'] = trace_timing
    result['test_view_kept_visible'] = visible_view
    result['test_windows_minimized'] = not visible_view
    result['clock_profile'] = clock_profile
    result['server_debug'] = server_debug
    result['presentation_floor_disabled'] = disable_presentation_floor
    result['audio_retry_disabled'] = disable_audio_retry
    try:
        if clock_profile not in ('upstream', 'same-server-default', 'server-only-500ms', '500ms'):
            raise ValueError('Unknown clock profile')
        room = 'remote-av-' + case
        if server:
            if clock_profile != 'upstream':
                raise ValueError('Clock profiles need the owned local SFU')
            env = remote_client_env(server, room)
        else:
            fixture.start(sfu_rtcp_report_ms=500 if clock_profile in ('server-only-500ms', '500ms') else 0,
                          clock_server=clock_profile != 'upstream', server_debug=server_debug)
            result['server_runtime'] = fixture.server_info
            env = fixture.client_env(room)
        env['QML_IMPORT_PATH'] = str(ROOT / '.deps/qt/6.8.3/msvc2022_64/qml')
        env.pop('GOODSHARING_TEST_AV_TRACE', None)
        env.pop('GOODSHARING_TEST_RTCP_REPORT_MS', None)
        env.pop('GOODSHARING_TEST_AUDIO_TRACE', None)
        if clock_profile != 'upstream':
            env['GOODSHARING_TEST_AUDIO_TRACE'] = str(folder / 'source-clock-trace')
        if clock_profile == '500ms':
            env['GOODSHARING_TEST_RTCP_REPORT_MS'] = '500'
        if trace_timing:
            env['GOODSHARING_TEST_AV_TRACE'] = '1'
        app = app_path.resolve() if app_path is not None else ROOT / 'build/bin/GoodSharing.exe'
        result['client_loopback_enabled'] = env.get('GOODSHARING_LOCAL_LOOPBACK') == '1'
        result['app_path'] = str(app)
        result['app_sha256'] = hashlib.sha256(app.read_bytes()).hexdigest()

        def launch(name, command, environment=env):
            log = (folder / (name + '.log')).open('w'); logs.append(log)
            child = subprocess.Popen(command, env=environment, stdout=log, stderr=subprocess.STDOUT)
            children.append(child)
            return child

        ready = folder / 'source.json'
        switching = case.startswith('switch')
        source_exit = case in ('source-exit', 'static-source-exit', 'switch-source-exit')
        watch_ms = duration_ms or (36000 if case == 'switch' else 18000)
        result['watch_duration_ms'] = watch_ms
        static = case.startswith('static')
        fixture_seconds = max(50 if switching else 35, (watch_ms + 6999) // 1000)
        command = [str(ROOT / 'build/bin/goodsharing_fixture.exe'), '--hide-cursor', '--seconds', str(fixture_seconds),
                   '--right-tone', '880', '--ready-file', str(ready)]
        if source_exit:
            command += ['--quit-file', str(folder / 'close-owned-source')]
        if static:
            command += ['--static-after-ms', '9000', '--frozen-file', str(folder / 'frozen-source.json')]
        if case == 'static-resume':
            command += ['--resume-file', str(folder / 'resume-owned-source')]
        if case == 'minimized':
            command += ['--minimize-file', str(folder / 'minimize-owned-source'),
                        '--restore-file', str(folder / 'restore-owned-source')]
        source = launch('source', command)
        deadline = time.monotonic() + 8
        while not ready.exists():
            if source.poll() is not None or time.monotonic() > deadline:
                raise RuntimeError('Owned fixture did not become ready')
            time.sleep(.1)
        source_info = json.loads(ready.read_text())
        profile = {"low": "low", "smooth": "smooth"}.get(case, args.profile)
        # Pin the profile unless the run asks for the app's own default.
        if profile == "auto":
            env.pop("GOODSHARING_STREAM_PROFILE", None)
        else:
            env["GOODSHARING_STREAM_PROFILE"] = profile or "balanced"
        # The frame rate is a saved UI choice too; pin it for every run.
        env["GOODSHARING_TEST_SHARE_FPS"] = str(args.share_fps)
        viewer_env = env.copy()
        # Keep the audio path deterministic: a saved UI volume must not mute a test.
        viewer_env.setdefault("GOODSHARING_TEST_VOLUME", "100")
        if disable_presentation_floor:
            viewer_env['GOODSHARING_TEST_DISABLE_PRESENTATION_FLOOR'] = '1'
        if disable_audio_retry:
            viewer_env['GOODSHARING_TEST_DISABLE_AUDIO_RETRY'] = '1'
        if case == 'switch-timeout':
            viewer_env.update(GOODSHARING_TEST_AV_TRACE='1',GOODSHARING_TEST_SKIP_1080_SUBSCRIPTION='1')
        wrong = case in ('wrong-key', 'static-wrong-key')
        if wrong:
            key = bytearray.fromhex(viewer_env['GOODSHARING_TEST_E2EE_KEY']); key[0] ^= 0xff
            viewer_env['GOODSHARING_TEST_E2EE_KEY'] = key.hex()
        command = [str(app), '--smoke-watch', '--test-av-playback', '--quality', '1080' if case in ('1080', 'static-1080') else '720',
                   '--report', str(folder / 'viewer.json'), '--screenshot', str(folder / 'viewer.png'), '--watch-duration-ms', str(watch_ms)]
        if switching:
            command += [{'switch': '--test-av-switch', 'switch-timeout': '--test-av-switch', 'switch-cancel': '--test-av-switch-cancel', 'switch-source-exit': '--test-av-switch-source-exit'}[case]]
        if source_exit:
            command += ['--switch-stage-ready-file' if switching else '--view-ready-file', str(folder / 'view-ready.json')]
        if case == 'paint-stall':
            command += ['--test-paint-stall']
        if static or case == 'minimized':
            command += ['--test-av-checkpoints', '--test-av-checkpoint-file', str(folder / 'checkpoints.json')]
        if visible_view:
            command += ['--test-view-visible']
        else:
            command += ['--test-window-minimized']
        viewer = launch('viewer', command, viewer_env)
        sender_command = [str(app), '--smoke-share-hwnd', source_info['hwnd'], '--smoke-pid', str(source.pid),
                          '--smoke-duration-ms', str(watch_ms + 2000), '--report', str(folder / 'sender.json'),
                          '--screenshot', str(folder / 'sender.png')]
        if not visible_view:
            sender_command += ['--test-window-minimized']
        sender = launch('sender', sender_command)
        profiles = [ProcessFootprint(viewer, 'Timed GUI receiver, WASAPI worker and test-only own-process observer').start(),
                    ProcessFootprint(sender, 'Actual WGC/application-audio sender with two encoders').start()]
        if clock_profile != 'upstream':
            if fixture:
                profiles.append(ProcessFootprint(fixture.server, 'Owned SFU for the same-build report-cadence comparison').start())
        if case == 'static-resume':
            checkpoints = folder / 'checkpoints.json'; deadline = time.monotonic() + 17
            while checkpoint_count(checkpoints) < 2:
                if viewer.poll() is not None or time.monotonic() > deadline:
                    raise RuntimeError('Static observations did not arrive before motion-resume deadline')
                time.sleep(.05)
            (folder / 'resume-owned-source').write_text('resume this owned fixture')
        if case == 'minimized':
            # Minimize the owned source after the first observation and restore it
            # after the second, so the middle checkpoint sees a minimized source.
            checkpoints = folder / 'checkpoints.json'; deadline = time.monotonic() + 17
            while checkpoint_count(checkpoints) < 1:
                if viewer.poll() is not None or time.monotonic() > deadline:
                    raise RuntimeError('First observation did not arrive before the minimize deadline')
                time.sleep(.05)
            (folder / 'minimize-owned-source').write_text('minimize this owned fixture')
            while checkpoint_count(checkpoints) < 2:
                if viewer.poll() is not None or time.monotonic() > deadline:
                    raise RuntimeError('Second observation did not arrive after minimizing the source')
                time.sleep(.05)
            (folder / 'restore-owned-source').write_text('restore this owned fixture')
        if source_exit:
            observed = folder / 'view-ready.json'; deadline = time.monotonic() + 16
            while not observed.exists():
                if viewer.poll() is not None or time.monotonic() > deadline:
                    raise RuntimeError('Timed viewer did not establish A/V playback before close deadline')
                time.sleep(.05)
            result['source_close_readiness'] = json.loads(observed.read_text())
            if not switching:
                readiness = result['source_close_readiness']
                if not (readiness.get('av_required') is True and readiness.get('drawn_frames', 0) >= 120 and
                        readiness.get('committed_media_frames', 0) >= 96000 and readiness.get('presented_video_frames', 0) >= 120):
                    raise RuntimeError('Source-close marker did not prove established A/V playback')
            if static:
                checkpoints = folder / 'checkpoints.json'
                while checkpoint_count(checkpoints) < 2:
                    if viewer.poll() is not None or time.monotonic() > deadline:
                        raise RuntimeError('Static checkpoints did not arrive before source-close deadline')
                    time.sleep(.05)
            (folder / 'close-owned-source').write_text('close this owned fixture')
        try:
            viewer.wait(timeout=watch_ms/1000+8)
        except subprocess.TimeoutExpired:
            viewer.kill()
        result['viewer_exit'] = viewer.returncode
        try:
            sender.wait(timeout=watch_ms/1000+9)
        except subprocess.TimeoutExpired:
            sender.kill()
        result['sender_exit'] = sender.returncode
        view, send = (json.loads((folder / name).read_text()) for name in ('viewer.json', 'sender.json'))
        # Lead/wait come from the profile the app reports, so a larger presentation
        # buffer is judged against its own bound rather than the old fixed leads.
        profile_name = 'smooth' if (case == 'smooth' or args.profile == 'auto') else (args.profile or 'balanced')
        profile_fill = .90 if case == 'smooth' else .95
        video = view.get('video') or view.get('last_selection', {}).get('video', {})
        playback = view.get('playback') or view.get('last_selection', {}).get('playback', {})
        stopped = view['after_stop'].get('last_selection', {}).get('playback', {})
        profile_lead = (playback.get('audio') or {}).get('target_video_lead_ms', 55) + 25
        profile_wait = (playback.get('audio') or {}).get('video_presentation_wait_limit_ms', profile_lead + 30)
        frame_ms = 1000.0 / (args.share_fps or 30)
        audio, endpoint = stopped.get('audio', {}), stopped.get('endpoint', {})
        observed = view['audio_observer']
        checks = dict(
            owned_apps_exit=viewer.returncode == 0 and sender.returncode == (3 if source_exit else 0),
            requested=view.get('requested') and view.get('test_av_playback'),
            bounded_video=video.get('pending_images', 99) <= video.get('scheduled_queue_capacity', 7) and video.get('scheduled_queue_high_water', 99) * frame_ms <= profile_lead + profile_wait + 2 * frame_ms,
            bounded_pcm=audio.get('queue_high_water_blocks', 99) * 10 <= profile_lead + 60,
            endpoint_flushed=not endpoint.get('running', True) and endpoint.get('padding_frames', -1) == endpoint.get('reserved_frames', -1) == 0,
            quiet_after_stop=observed.get('post_stop_frames', 0) >= 9600 and observed.get('post_stop_peak', 9999) < 40,
            observer_ok=not observed.get('error') and observed.get('own_process_only'),
            actual_source=send.get('capture', {}).get('video_pairs_admitted', 0) > 100 and send.get('capture', {}).get('audio_blocks_admitted', 0) > 100,
            room_idle=view.get('room_idle') and not view['after_stop']['busy'],
            same_binary=hashlib.sha256(app.read_bytes()).hexdigest() == result['app_sha256'],
            quality_control=view.get('quality_control') == ('1080p' if case in ('1080', 'static-1080') else '720p'),
            no_worker_error=not stopped.get('error') and not audio.get('failed', True),
        )
        if clock_profile == '500ms':
            checks['client_fast_reports_acknowledged'] = all('GoodSharing RTCP report experiment: interval_ms=500' in
                (folder / name).read_text(encoding='utf-8-sig') for name in ('sender.log', 'viewer.log'))
        if clock_profile == 'server-only-500ms':
            checks['client_reports_unchanged'] = all('GoodSharing RTCP report experiment:' not in
                (folder / name).read_text(encoding='utf-8-sig') for name in ('sender.log', 'viewer.log'))
        if clock_profile in ('server-only-500ms', '500ms'):
            checks['server_fast_reports_acknowledged'] = 'GoodSharing SFU RTCP experiment: interval_ms=500' in (folder / 'server/server.log').read_text(encoding='utf-8-sig')
        # Keep raw evidence even if a later analysis step encounters missing
        # state after a failed connection or a revoked watch.
        result.update(viewer=view, sender=send, checks=checks)
        if wrong:
            checks.update(zero_media=audio.get('committed_media_frames', -1) == 0 and endpoint.get('commits', -1) == 0,
                          quiet_whole_run=observed.get('whole_run_peak', 9999) < 40,
                          no_pixels=video.get('drawn_distinct_frames', 0) == 0 and not view['composited_marker_valid'])
        else:
            checks.update(painted=video.get('timed_draws', 0) > 100,
                          frame_queue_observed=video.get('wait_for_frame_queue') and video.get('queued_presentations', 0) > 100,
                          heard_audio=observed.get('whole_run_peak', 0) > 100 and audio.get('committed_media_frames', 0) > 48000,
                          media_filled_device_frames=audio.get('committed_media_frames', 0) / max(1, endpoint.get('submitted_frames_including_silence', 0)) >= profile_fill,
                          live_window_coverage=audio.get('estimated_media_window_coverage', 0) >= .95,
                          bounded_output_gap_recovery=audio.get('endpoint_gap_resets', 9999) <= 3,
                          bounded_av_lag=audio.get('maximum_estimated_av_lag_ms', 9999) <= 80,
                          bounded_common_video_lead=audio.get('maximum_aligned_video_lead_ms', 9999) <= profile_wait + 20,
                          bounded_video_presentation_wait=audio.get('maximum_video_presentation_wait_ms', audio.get('maximum_aligned_video_lead_ms', 9999)) <= profile_wait + 20,
                          bounded_video_presentation_advance=audio.get('maximum_video_presentation_advance_ms', 0) <= 20,
                          bounded_draw_lag=video.get('recent_p95_draw_lateness_ms', 9999) <= 40)
            if clock_profile in ('server-only-500ms', '500ms'):
                tone = observed.get('continuous_tone_observation', {})
                checks['first_tone_within_five_seconds'] = tone.get('started') is True and 0 < tone.get('first_tone_startup_ms', 9999) <= 5000
            if not source_exit:
                checks.update(stereo_output=observed['recent_audio'].get('stereo_passed', False),
                              composited_marker=view['composited_marker_valid'],
                              still_watching=view['watching'])
                if case != 'paint-stall':
                    checks['continuous_tone_without_gap'] = check_continuous_tone(observed)
        if case == 'paint-stall':
            checks.update(check_stall_observations(view))
        if switching:
            checks.update(check_quality_switch(view, case))
        if static:
            frozen = json.loads((folder / 'frozen-source.json').read_text())
            result['frozen_source'] = frozen
            samples = view.get('av_checkpoints', [])
            checks['static_checkpoints_recorded'] = len(samples) == 3
            if len(samples) == 3:
                a, b, c = samples
                checks['source_frozen_before_observations'] = all(s['receiver_local_us'] > frozen['freeze_local_us'] + 2000000 for s in (a, b))
                if wrong:
                    checks['static_wrong_key_never_presents'] = all(
                        s.get('video', {}).get('queued_presentations', 0) == 0 and
                        s.get('playback', {}).get('audio', {}).get('committed_media_frames', 0) == 0 for s in samples)
                else:
                    elapsed = (b['test_elapsed_ms'] - a['test_elapsed_ms']) / 1000
                    delivered = b.get('playback', {}).get('audio', {}).get('committed_media_frames', 0) - a.get('playback', {}).get('audio', {}).get('committed_media_frames', 0)
                    queued = b.get('video', {}).get('queued_presentations', 0) - a.get('video', {}).get('queued_presentations', 0)
                    result['static_interval'] = dict(seconds=elapsed, committed_media_frames=delivered,
                        estimated_pcm_coverage=delivered / max(1, elapsed * 48000), fresh_queued_presentations=queued)
                    checks['view_active_at_both_static_checkpoints'] = all(
                        s['watching'] and s.get('video', {}).get('last_draw_visible', False) and s.get('playback', {}).get('audio', {}).get('clock_anchored', False) for s in (a, b))
                    checks['audio_continues_during_static_interval'] = elapsed >= 1.8 and delivered / max(1, elapsed * 48000) >= .95
                    checks['fresh_transport_during_static_interval'] = queued >= 50
                    if source_exit:
                        checks['static_source_close_revokes'] = not c['watching'] and not view['composited_marker_valid'] and not view['watching']
                    elif case == 'static-resume':
                        checks['motion_resumes'] = c['watching'] and view['composited_marker_valid'] and view['composited_pixel_id'] != frozen['last_pixel_id']
                    else:
                        checks['static_picture_visible_at_end'] = view['composited_pixel_id'] == frozen['last_pixel_id']
        if case == 'minimized':
            samples = view.get('av_checkpoints', [])
            checks['minimize_observations_recorded'] = len(samples) == 3
            if len(samples) == 3:
                a, b, c = samples
                presented = [s.get('playback', {}).get('audio', {}).get('presented_video_frames', 0) for s in (a, b, c)]
                delivered = [s.get('playback', {}).get('audio', {}).get('committed_media_frames', 0) for s in (a, b, c)]
                result['minimize_observations'] = dict(
                    source_minimized=[s.get('source_minimized') for s in (a, b, c)],
                    status=[s.get('status') for s in (a, b, c)],
                    presented_video_frames=presented, committed_media_frames=delivered)
                checks['minimized_state_observed'] = [s.get('source_minimized') for s in (a, b, c)] == [False, True, False]
                checks['viewer_says_user_minimized_app'] = str(b.get('status', '')).startswith('User minimized app')
                # The minimize is applied within ~50 ms of the trigger, so a few
                # frames queued before it still arrive; 20 frames is < 0.35 s.
                checks['picture_frozen_while_minimized'] = presented[1] - presented[0] <= 20
                checks['picture_resumed_after_restore'] = presented[2] - presented[1] >= 60
                checks['view_alive_after_restore'] = c['watching'] is True and view.get('watching') is True
                checks['sender_capture_healthy'] = not send.get('capture', {}).get('error') and send.get('capture', {}).get('video_pairs_admitted', 0) > 100
                # The sender keeps capturing audio while the window is minimized,
                # so a playback gap is a viewer-side re-anchor, not a capture stop.
                checks['sender_audio_continued'] = send.get('capture', {}).get('audio_blocks_admitted', 0) >= 1500
                checks['audio_timeline_recovered'] = c['playback']['audio']['clock_anchored'] is True and delivered[2] > delivered[1]
        checks['no_remaining_participants'] = (not fixture.participants(room)) if fixture else True
        result.update(viewer=view, sender=send, checks=checks, passed=all(checks.values()))
    except Exception as error:
        result['error'] = str(error)
    finally:
        cpu.stop()
        result['system_cpu'] = cpu.report()
        if profiles:
            result['footprint'] = [profile.stop() for profile in profiles]
            result['passed'] &= all(not p['errors'] and p['sampled_lifetime'] for p in result['footprint'])
        for child in reversed(children): stop(child)
        for log in logs: log.close()
        result['owned_clients_stopped'] = all(child.poll() is not None for child in children)
        result['owned_server_stopped'] = fixture.stop() if fixture else True
        result['passed'] &= result['owned_clients_stopped'] and result['owned_server_stopped']
        (folder / 'summary.json').write_text(json.dumps(result, indent=2))
    return result


def remote_server(args):
    if not args.server_url:
        return None
    key, secret = args.server_key, args.server_secret
    if not key or not secret:
        creds = ROOT / 'artifacts' / 'sfu-credentials.ini'
        values = {}
        for line in creds.read_text(encoding='utf-8').splitlines():
            if '=' in line:
                name, value = line.split('=', 1)
                values[name.strip()] = value.strip().strip('"')
        key = key or values.get('api_key', '')
        secret = secret or values.get('api_secret', '')
    if not key or not secret:
        raise SystemExit('Missing server credentials')
    return {'url': args.server_url, 'key': key, 'secret': secret}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    cases = ['720', '1080', 'low', 'smooth', 'wrong-key', 'source-exit', 'paint-stall', 'static', 'static-1080', 'static-resume', 'static-source-exit', 'static-wrong-key', 'minimized', 'switch', 'switch-cancel', 'switch-source-exit', 'switch-timeout']
    parser.add_argument('--case', choices=['all', 'static-all', 'switch-all'] + cases, default='all')
    parser.add_argument('--trace-timing', action='store_true', help='Enable bounded per-event timing diagnostics; normal tests keep this off')
    parser.add_argument('--visible-view', action='store_true', help='Keep owned test windows visible; otherwise start them minimized')
    parser.add_argument('--profile', choices=['low', 'balanced', 'smooth', 'auto'], help='Stream profile for both owned apps; default is the app setting')
    parser.add_argument('--share-fps', type=int, choices=[30, 60], default=30,
                        help='Share frame rate for the owned sender; pinned so a saved UI choice cannot leak into a run')
    parser.add_argument('--server-url', help='Use a remote LiveKit SFU instead of the owned local one, e.g. ws://host:7880')
    parser.add_argument('--server-key', help='API key for --server-url (default: artifacts/sfu-credentials.ini)')
    parser.add_argument('--server-secret', help='API secret for --server-url (default: artifacts/sfu-credentials.ini)')
    parser.add_argument('--server-debug', action='store_true', help='Record owned SFU transport diagnostics for connection failures')
    parser.add_argument('--disable-presentation-floor', action='store_true', help='Test-only A/B control; allow catch-up frames less than 10 ms apart')
    parser.add_argument('--disable-audio-retry', action='store_true', help='Test-only control; disable bounded stereo resubscription')
    parser.add_argument('--app-exe', type=Path, help='Explicit archived project app for a same-PC regression control; default is the current build')
    parser.add_argument('--watch-duration-ms', type=int, help='Longer owned local observation, 18000-58000 ms; default depends on case')
    parser.add_argument('--clock-profile', choices=['upstream', 'same-server-default', 'server-only-500ms', '500ms'], default='upstream',
                        help='Explicit local report-cadence comparison; all non-upstream profiles use the identical verified SFU build; server-only-500ms keeps client reporting unchanged')
    global args
    args = parser.parse_args()
    if args.watch_duration_ms is not None and not 18000 <= args.watch_duration_ms <= 58000:
        parser.error('--watch-duration-ms must be between 18000 and 58000')
    folder = ROOT / 'artifacts' / (time.strftime('%Y%m%d-%H%M%S') + '-remote-av'); folder.mkdir(parents=True)
    rows = []
    selected = cases if args.case == 'all' else [c for c in cases if c.startswith('static')] if args.case == 'static-all' else [c for c in cases if c.startswith('switch')] if args.case == 'switch-all' else [args.case]
    for case in selected:
        row = run_case(folder / case, case, args.trace_timing, args.visible_view, args.app_exe, args.watch_duration_ms, args.clock_profile, args.server_debug, args.disable_presentation_floor, args.disable_audio_retry, remote_server(args)); rows.append(row)
        print(json.dumps(dict(case=case, passed=row['passed'], error=row.get('error'),
                             failed_checks=[k for k, v in row.get('checks', {}).items() if not v])), flush=True)
    summary = dict(passed=all(row['passed'] for row in rows), runs=rows)
    (folder / 'summary.json').write_text(json.dumps(summary, indent=2))
    print('Reports: ' + str(folder), flush=True)
    raise SystemExit(0 if summary['passed'] else 1)


if __name__ == '__main__':
    main()
