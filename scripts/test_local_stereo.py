"""Distinct left/right tones through SFU, optional actual Windows playback."""
import argparse
import json
import subprocess
import time
from local_media_fixture import LocalMediaFixture, ROOT


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--case', choices=['all', 'off', 'stereo', 'timed-stereo', 'participant', 'playback', 'mono-control', 'wrong-key'], default='all')
    args = parser.parse_args()
    folder = ROOT / 'artifacts' / (time.strftime('%Y%m%d-%H%M%S') + '-stereo')
    fixture = LocalMediaFixture(folder)
    results = []
    summary = {'local_only': True, 'saved_audio_content': False, 'runs': results}
    try:
        fixture.start()
        cases = ['mono-control', 'off', 'stereo', 'timed-stereo', 'participant', 'playback', 'wrong-key'] if args.case == 'all' else [args.case]
        for case in cases:
            mode = 'off' if case == 'off' else 'wrong-key' if case == 'wrong-key' else 'shared-key'
            env = fixture.client_env(case, mode)
            env['GOODSHARING_PLAYBACK'] = '1' if case == 'playback' else '0'
            env['GOODSHARING_MONO_CONTROL'] = '1' if case == 'mono-control' else '0'
            env['GOODSHARING_PARTICIPANT_STREAM'] = '1' if case == 'participant' else '0'
            env['GOODSHARING_TEST_TIMED_CAPTURE'] = '1' if case == 'timed-stereo' else '0'
            report = folder / (case + '.json')
            log = folder / (case + '.log')
            with log.open('w') as output:
                child = subprocess.run([str(ROOT / 'build/bin/goodsharing_stereo_probe.exe'), '--report', str(report)], env=env, stdout=output, stderr=subprocess.STDOUT, timeout=30, creationflags=subprocess.CREATE_NO_WINDOW)
            data = json.loads(report.read_text()) if report.exists() else {}
            received = data.get('receive', {})
            success = child.returncode == 0 and bool(data) and not data.get('error') and data.get('receive_error') == '' and data.get('close_error') == '' and data.get('close_ms', 99999) < 2000
            if case == 'mono-control':
                tones = received.get('tones', [{}])[0]
                success &= received.get('channels') == 1 and received.get('stereo_passed') is False and tones.get('440', 0) > 100 and tones.get('880', 0) > 100
            elif case == 'wrong-key':
                success &= data.get('encrypted_publication') is True and data.get('decryption_failures', 0) > 0
                success &= all(max(ch.get('440', 9999), ch.get('880', 9999)) < 40 for ch in received.get('tones', []))
                success &= received.get('max_100ms_tone_amplitude', 9999) < 200
            else:
                success &= received.get('stereo_passed') is True and data.get('sample_rate') == 48000
                if case == 'playback':
                    success &= data.get('process_loopback', {}).get('stereo_passed') is True and data.get('loopback_error') == '' and data.get('playback', {}).get('error') == '' and data.get('playback', {}).get('rejected_frames', 1) == 0
            if mode != 'off':
                success &= data.get('encrypted_publication') is True and data.get('publisher_encryption_ok_events', 0) >= 1
            # Preserve the race as a failure, rather than suppressing SDK logs.
            log_text = log.read_text(errors='replace')
            lifecycle_error = any(message in log_text for message in ('failed to negotiate', 'wrong state: closed', 'panicked at', 'Attempted to drop unknown FFI handle'))
            success &= not lifecycle_error
            deadline = time.monotonic() + 3
            participants = fixture.participants(case)
            while participants and time.monotonic() < deadline:
                time.sleep(.1)
                participants = fixture.participants(case)
            success &= not participants
            result = {'case': case, 'passed': bool(success), 'lifecycle_log_error': lifecycle_error, 'remaining_participants': len(participants), 'report': str(report.relative_to(ROOT)), 'receive': received, 'close_ms': data.get('close_ms')}
            results.append(result)
            print(json.dumps(result), flush=True)
    finally:
        summary['owned_server_stopped'] = fixture.stop()
        folder.mkdir(parents=True, exist_ok=True)
        (folder / 'summary.json').write_text(json.dumps(summary, indent=2))
        print('Reports: ' + str(folder), flush=True)
    return 0 if results and all(r['passed'] for r in results) and summary['owned_server_stopped'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
