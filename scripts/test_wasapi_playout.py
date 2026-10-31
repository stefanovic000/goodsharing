"""Owned low-volume stereo -> actual WASAPI -> own-process loopback checks.

No screen capture, network, microphone, unrelated process capture or OS settings.
Permission notifications are synthetic test events, not an E2EE transport test.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import time
from process_footprint import ProcessFootprint

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--case', choices=['all', 'normal', 'mute', 'unverified', 'revoke-in-prepare', 'stall'], default='all')
    args = parser.parse_args()
    folder = ROOT / 'artifacts' / (time.strftime('%Y%m%d-%H%M%S') + '-wasapi-playout')
    folder.mkdir()
    executable = ROOT / 'build/bin/goodsharing_wasapi_playout_probe.exe'
    digest = hashlib.sha256(executable.read_bytes()).hexdigest()
    env = os.environ.copy()
    env['PATH'] = str(ROOT / '.deps/qt/6.8.3/msvc2022_64/bin') + os.pathsep + env.get('PATH', '')
    summary = dict(local_only=True, actual_wasapi=True, own_process_loopback=True,
                   real_screen_capture=False, synthetic_permission_events=True,
                   physical_speaker_display_sync_measured=False, passed=False, runs=[])
    cases = ['normal', 'mute', 'unverified', 'revoke-in-prepare', 'stall'] if args.case == 'all' else [args.case]
    for case in cases:
        result = dict(case=case, passed=False, executable_sha256=digest, unrelated_processes_stopped=False)
        child, profile = None, None
        report = folder / (case + '.json')
        try:
            with (folder / (case + '.log')).open('w') as output:
                child = subprocess.Popen([str(executable), '--case', case, '--report', str(report)],
                                         env=env, stdout=output, stderr=subprocess.STDOUT, creationflags=subprocess.CREATE_NO_WINDOW)
                profile = ProcessFootprint(child, 'WASAPI probe with own-process loopback and a silent observation session').start()
                child.wait(timeout=20)
            native = json.loads(report.read_text()) if report.exists() else {}
            result.update(exit_code=child.returncode, report=report.relative_to(ROOT).as_posix(),
                          executable_unchanged=hashlib.sha256(executable.read_bytes()).hexdigest() == digest)
            result['passed'] = bool(child.returncode == 0 and native.get('passed') and result['executable_unchanged'])
            if not result['passed']:
                result['error'] = native.get('error', 'Probe report missing or failed')
        except Exception as error:
            result['error'] = str(error)
        finally:
            if child is not None and child.poll() is None:
                child.terminate()
                try:
                    child.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    child.kill()
                    child.wait(timeout=5)
            if profile:
                result['footprint'] = profile.stop()
                result['passed'] &= not result['footprint']['errors'] and result['footprint']['sampled_lifetime'] is not None
            result['owned_client_stopped'] = child is not None and child.poll() is not None
            result['passed'] &= result['owned_client_stopped']
        summary['runs'].append(result)
        print(json.dumps({k: result.get(k) for k in ('case', 'passed', 'error')}), flush=True)
    summary['passed'] = bool(summary['runs']) and all(r['passed'] for r in summary['runs'])
    (folder / 'summary.json').write_text(json.dumps(summary, indent=2) + '\n')
    print('Reports: ' + str(folder), flush=True)
    return 0 if summary['passed'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
