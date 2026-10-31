"""Local managed receiving, including independent audio and staged video subscriptions.

Split cases inspect decoded transport; they do not render or prove audible handover.
"""
import argparse
import hashlib
import json
import subprocess
import time
from local_media_fixture import LocalMediaFixture, ROOT
from process_footprint import ProcessFootprint


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--case', choices=['all', 'normal', 'wrong-key', 'split', 'split-wrong-key'], default='all')
    parser.add_argument('--profile-resources', action='store_true')
    args = parser.parse_args()
    folder = ROOT / 'artifacts' / (time.strftime('%Y%m%d-%H%M%S') + '-remote-share')
    fixture = LocalMediaFixture(folder)
    summary = {'local_only': True, 'actual_capture': False, 'passed': False, 'runs': []}
    try:
        fixture.start()
        for case in ['normal', 'wrong-key', 'split', 'split-wrong-key'] if args.case == 'all' else [args.case]:
            room = 'remote-share-' + case
            env = fixture.client_env(room)
            env['GOODSHARING_REMOTE_CASE'] = case
            executable = ROOT / 'build/bin/goodsharing_remote_share_probe.exe'
            digest = hashlib.sha256(executable.read_bytes()).hexdigest()
            report = folder / (case + '.json')
            profile = None
            with (folder / (case + '.log')).open('w') as output:
                with subprocess.Popen([str(executable), '--report', str(report)], env=env,
                                      stdout=output, stderr=subprocess.STDOUT, creationflags=subprocess.CREATE_NO_WINDOW) as child:
                    try:
                        if args.profile_resources:
                            profile = ProcessFootprint(child, 'managed synthetic sender/viewer; split cases briefly decode two video qualities; no rendering').start()
                        child.wait(timeout=90)
                    finally:
                        if child.poll() is None:
                            child.kill()
                            child.wait(timeout=5)
                        if profile:
                            (folder / (case + '-footprint.json')).write_text(json.dumps(profile.stop(), indent=2), encoding='utf-8')
            native = json.loads(report.read_text(encoding='utf-8')) if report.exists() else {}
            selections = native.get('selections', [])
            success = child.returncode == 0 and native.get('passed') is True and not native.get('error')
            success &= digest == hashlib.sha256(executable.read_bytes()).hexdigest()
            for selection in selections:
                watch, inspected = selection.get('watch', {}), selection.get('inspection', {})
                verification = watch.get('receiver', {})
                success &= watch.get('closed') is True and watch.get('stopped') is True and watch.get('reader_error') == ''
                success &= verification.get('verification_required') is True
                success &= inspected.get('pixel_errors', 1) == 0 and inspected.get('metadata_errors', 1) == 0 and inspected.get('dimension_errors', 1) == 0
                if case == 'wrong-key':
                    success &= verification.get('verified_tracks') == 0 and inspected.get('frames') == 0 and inspected.get('audio', {}).get('frames') == 0
                else:
                    expected = 2 if watch.get('audio_sid') else 1
                    success &= verification.get('known_tracks') == expected and verification.get('verified_tracks') == expected
                    success &= inspected.get('frames', 0) >= 60 and 0 < inspected.get('p95_ms', 9999) <= 150
                    readers = verification.get('completed_readers', [])
                    success &= len(readers) == 1 and readers[0].get('sid') == watch.get('video_sid') and readers[0].get('queue_high_water', 9999) <= 4
                    if watch.get('audio_sid'):
                        success &= inspected.get('audio', {}).get('stereo_passed') is True
            if case == 'normal':
                success &= len(selections) == 4 and native.get('rapid_cancels') == 8 and native.get('generation_restart_passed') is True
                rates = native.get('variable_rates', [])
                success &= [r.get('offered_fps') for r in rates] == [55, 45, 30, 60] and all(r.get('inspected', 0) > r['offered_fps'] / 2 for r in rates)
                success &= [s['inspection']['height'] for s in selections] == [720, 1080, 720, 720]
                owner_exit = native.get('owner_disconnect', {})
                last_watch, last_frames = owner_exit.get('watch', {}), owner_exit.get('inspection', {})
                success &= last_watch.get('closed') is True and last_watch.get('stopped') is True and last_watch.get('reader_error') == ''
                success &= last_frames.get('frames', 0) >= 30 and all(last_frames.get(k, 1) == 0 for k in ['pixel_errors', 'metadata_errors', 'dimension_errors'])
            elif case == 'wrong-key':
                success &= len(selections) == 1
            else:
                split = native.get('split', {})
                success &= split.get('passed') is True and split.get('actual_playback') is False and split.get('shared_audio_subscription') is True
                split_selections = split.get('selections', [])
                success &= len(split_selections) == (2 if case == 'split-wrong-key' else 4)
                for selection in split_selections:
                    watch, inspected = selection.get('watch', {}), selection.get('inspection', {})
                    verification = watch.get('receiver', {})
                    success &= watch.get('closed') is True and watch.get('stopped') is True and watch.get('reader_error') == ''
                    success &= verification.get('verification_required') is True
                    success &= all(inspected.get(field, 1) == 0 for field in ('pixel_errors', 'metadata_errors', 'dimension_errors'))
                    if case == 'split-wrong-key':
                        success &= verification.get('verified_tracks') == 0 and inspected.get('frames') == 0 and inspected.get('audio', {}).get('frames') == 0
                    else:
                        success &= verification.get('known_tracks') == 1 and verification.get('verified_tracks') == 1
                        if watch.get('audio_sid'):
                            success &= watch.get('video_sid') == '' and watch.get('quality') == 0 and inspected.get('frames') == 0
                            success &= inspected.get('audio', {}).get('stereo_passed') is True
                        else:
                            success &= inspected.get('frames', 0) >= 60 and inspected.get('audio_callbacks') == 0
                if case == 'split':
                    success &= all(split.get(field) is True for field in ('audio_sid_unchanged', 'same_audio_watch_for_both_quality_changes', 'generation_retired_all_roles'))
                    success &= split.get('audio_callbacks_after_return', 0) > split.get('audio_callbacks_after_high', 0) > split.get('audio_callbacks_before_candidate', 0) > 0
            if profile:
                success &= not profile.report()['errors'] and profile.report()['sampled_lifetime'] is not None
            text = (folder / (case + '.log')).read_text(errors='replace')
            log_error = any(message in text for message in ('failed to negotiate', 'wrong state: closed', 'panicked at', 'peer connection is closed, cannot create offer', 'Attempted to drop unknown FFI handle'))
            success &= not log_error
            participants = fixture.participants(room)
            deadline = time.monotonic() + 3
            while participants and time.monotonic() < deadline:
                time.sleep(.1)
                participants = fixture.participants(room)
            success &= not participants
            check = {'case': case, 'passed': bool(success), 'report': str(report.relative_to(ROOT)),
                     'executable_sha256': digest, 'remaining_participants': len(participants), 'lifecycle_log_error': log_error}
            summary['runs'].append(check)
            print(json.dumps(check), flush=True)
        summary['passed'] = bool(summary['runs']) and all(c['passed'] for c in summary['runs'])
    except Exception as error:
        summary['error'] = str(error)
    finally:
        summary['owned_server_stopped'] = fixture.stop()
        folder.mkdir(parents=True, exist_ok=True)
        (folder / 'summary.json').write_text(json.dumps(summary, indent=2), encoding='utf-8')
        print('Reports: ' + str(folder), flush=True)
    return 0 if summary['passed'] and summary['owned_server_stopped'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
