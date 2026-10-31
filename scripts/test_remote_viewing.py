"""Two actual GUI apps: owned WGC window -> encrypted publication -> Qt paint.

Local only. Tests video presentation, not synchronized audio or sustained FPS.
Only processes started by this runner are stopped.
"""
import argparse
import hashlib
import json
import os
import subprocess
import time
from local_media_fixture import LocalMediaFixture, ROOT
from process_footprint import ProcessFootprint
from test_capture_sharing import stop


def run_case(folder, case, trace_motion=False):
    folder.mkdir()
    fixture = LocalMediaFixture(folder / 'server')
    children, logs, profiles = [], [], []
    result = {'case': case, 'passed': False, 'local_only': True,
              'audio_playback_tested': False, 'unrelated_processes_stopped': False}
    try:
        fixture.start()
        room = 'remote-view-' + case
        env = fixture.client_env(room)
        env['QML_IMPORT_PATH'] = str(ROOT / '.deps/qt/6.8.3/msvc2022_64/qml')
        app_exe = ROOT / 'build/bin/GoodSharing.exe'
        result['app_sha256'] = hashlib.sha256(app_exe.read_bytes()).hexdigest()
        def launch(name, command, environment=env):
            log = (folder / (name + '.log')).open('w'); logs.append(log)
            child = subprocess.Popen(command, env=environment, stdout=log, stderr=subprocess.STDOUT)
            children.append(child)
            return child
        source_exit = case == 'source-exit'
        ready = folder / 'source.json'
        source_command = [str(ROOT / 'build/bin/goodsharing_fixture.exe'), '--hide-cursor', '--seconds',
                          '25', '--ready-file', str(ready)]
        if source_exit: source_command += ['--quit-file', str(folder / 'close-owned-source')]
        if case == 'static': source_command += ['--static-after-ms', '3000', '--frozen-file', str(folder / 'frozen-source.json')]
        source = launch('source', source_command)
        deadline = time.monotonic() + 8
        while not ready.exists():
            if source.poll() is not None or time.monotonic() > deadline:
                raise RuntimeError('Owned source readiness failed')
            time.sleep(.1)
        source_info = json.loads(ready.read_text())
        viewer_env = env.copy()
        viewer_env['GOODSHARING_TEST_DISABLE_AV_PLAYBACK'] = '1'
        if trace_motion:viewer_env['GOODSHARING_TEST_AV_TRACE']='1'
        wrong = case == 'wrong-key'
        if wrong:
            key = bytearray.fromhex(viewer_env['GOODSHARING_TEST_E2EE_KEY'])
            key[0] ^= 0xff
            viewer_env['GOODSHARING_TEST_E2EE_KEY'] = key.hex()
        command = [str(app_exe), '--smoke-watch', '--quality', '1080' if case == '1080' else '720',
                   '--report', str(folder / 'viewer.json'), '--screenshot', str(folder / 'viewer.png')]
        if case == 'switch': command += ['--smoke-switch']
        if source_exit: command += ['--view-ready-file', str(folder / 'view-ready.json')]
        viewer = launch('viewer', command, viewer_env)
        sender = launch('sender', [str(app_exe), '--smoke-share-hwnd', source_info['hwnd'], '--smoke-pid',
                                   str(source.pid), '--audio-off', '--report', str(folder / 'sender.json'),
                                   '--screenshot', str(folder / 'sender.png')])
        profiles = [ProcessFootprint(viewer, 'GUI selected-quality receiver and Qt presentation').start(),
                    ProcessFootprint(sender, 'GUI WGC sender, dual encoders and preview').start()]
        if source_exit:
            # Teardown is the subject of this case. Confirm content was painted
            # before asking only our own fixture to close, with a bounded wait.
            deadline = time.monotonic() + 8
            observed = folder / 'view-ready.json'
            while not observed.exists():
                if viewer.poll() is not None or time.monotonic() > deadline:
                    raise RuntimeError('Viewer did not paint 120 frames before the source-close deadline')
                time.sleep(.05)
            result['source_close_readiness'] = json.loads(observed.read_text())
            (folder / 'close-owned-source').write_text('close this owned fixture', encoding='utf-8')
        viewer.wait(timeout=19); sender.wait(timeout=20)
        view = json.loads((folder / 'viewer.json').read_text())
        send = json.loads((folder / 'sender.json').read_text())
        stats = view.get('video') or view.get('last_selection', {}).get('video', {})
        watch = view.get('watch') or view.get('last_selection', {}).get('watch', {})
        drawn = stats.get('drawn_distinct_frames', 0)
        checks = {
            'owned_apps_exit': viewer.returncode == 0 and sender.returncode == (3 if source_exit else 0),
            'requested_and_saved': view.get('requested') and view.get('screenshot_saved'),
            'room_idle': view.get('room_idle'),
            'view_stopped': not view['after_stop']['watching'] and not view['after_stop']['busy'],
            'bounded_and_valid': stats.get('pending_images', 0) <= 1 and stats.get('invalid_frames', 0) == 0,
            'audio_disabled': not stats.get('audio_playback_enabled', False) and watch.get('audio_sid', '') == '',
            'actual_source_published': send.get('capture', {}).get('video_pairs_admitted', 0) > 100,
            'display_policy': (drawn == 0 and not view['composited_marker_valid']) if wrong else
                              (drawn > 100 and (not view['composited_marker_valid'] if source_exit else view['composited_marker_valid'])),
            'closed_when_expected': watch.get('closed') if wrong or source_exit else view['watching'],
            'executable_unchanged': hashlib.sha256(app_exe.read_bytes()).hexdigest() == result['app_sha256'],
            'quality_control_matches': view.get('quality_control') == ('1080p' if case in ('1080', 'switch') else '720p'),
        }
        if case == 'switch':
            first = view['before_switch']
            checks['quality_replaced'] = (first.get('watch', {}).get('quality') == 720 and
                                          first.get('video', {}).get('drawn_distinct_frames', 0) > 100 and
                                          watch.get('quality') == 1080)
        if case == 'static':
            frozen = json.loads((folder / 'frozen-source.json').read_text())
            result['frozen_source'] = frozen
            checks['static_picture_visible'] = view['watching'] and view['composited_pixel_id'] == frozen['last_pixel_id']
        if source_exit:
            checks['painted_before_close'] = result['source_close_readiness']['drawn_frames'] >= 120
        participants = fixture.participants(room)
        checks['no_remaining_participants'] = not participants
        result.update(viewer=view, sender=send, checks=checks, passed=all(checks.values()))
    except Exception as error:
        result['error'] = str(error)
    finally:
        if profiles:
            result['footprint'] = [profile.stop() for profile in profiles]
            result['passed'] &= all(not p['errors'] and p['sampled_lifetime'] for p in result['footprint'])
        for child in reversed(children): stop(child)
        for log in logs: log.close()
        result['owned_clients_stopped'] = all(child.poll() is not None for child in children)
        result['owned_server_stopped'] = fixture.stop()
        result['passed'] &= result['owned_clients_stopped'] and result['owned_server_stopped']
        (folder / 'summary.json').write_text(json.dumps(result, indent=2), encoding='utf-8')
    return result


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--case', choices=['all', '720', '1080', 'switch', 'wrong-key', 'source-exit', 'static'], default='all')
    parser.add_argument('--trace-motion',action='store_true',help='Retain bounded frame-swapped source-position records')
    args = parser.parse_args()
    folder = ROOT / 'artifacts' / (time.strftime('%Y%m%d-%H%M%S') + '-remote-viewing')
    folder.mkdir()
    cases = ['720', '1080', 'switch', 'wrong-key', 'source-exit', 'static'] if args.case == 'all' else [args.case]
    results = []
    for case in cases:
        result = run_case(folder / case, case, args.trace_motion); results.append(result)
        print(json.dumps({'case': case, 'passed': result['passed'], 'error': result.get('error'),
                          'failed_checks': [k for k, v in result.get('checks', {}).items() if not v]}), flush=True)
    summary = {'passed': all(r['passed'] for r in results), 'runs': results}
    (folder / 'summary.json').write_text(json.dumps(summary, indent=2), encoding='utf-8')
    print('Reports: ' + str(folder), flush=True)
    raise SystemExit(0 if summary['passed'] else 1)
