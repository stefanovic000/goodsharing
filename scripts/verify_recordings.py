"""Independently decode bounded synthetic recordings, without rerunning capture."""
import argparse
import json
import os
from pathlib import Path
import subprocess
import time
from verify_livekit_sdk import ROOT, runtime_info


def verify_recordings(diagnostics, env, timeout, output=None):
    output = output or diagnostics
    output.mkdir(parents=True, exist_ok=True)
    checks = []
    for recording in sorted(diagnostics.glob('*.gsv')):
        report = output / (recording.stem + '.json')
        with (output / (recording.stem + '.log')).open('w') as log:
            child = subprocess.run([str(ROOT / 'build/bin/goodsharing_probe.exe'), '--decode-recording', str(recording), '--report', str(report)], env=env, stdout=log, stderr=subprocess.STDOUT, timeout=timeout, creationflags=subprocess.CREATE_NO_WINDOW)
        result = json.loads(report.read_text()).get('encoded_recording', {}) if report.exists() else {}
        checks.append({'recording': recording.name, 'passed': child.returncode == 0 and result.get('frames', 0) > 0 and result.get('pixel_markers') == result.get('frames') and result.get('pending_frames') == 0, 'result': result})
    return checks


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('recordings', type=Path)
    parser.add_argument('--timeout', type=int, default=300)
    args = parser.parse_args()
    if not 60 <= args.timeout <= 300:
        parser.error('Use a bounded offline deadline of 60 to 300 seconds per quality')
    output = ROOT / 'artifacts' / (time.strftime('%Y%m%d-%H%M%S') + '-recording-verification')
    output.mkdir()
    info = runtime_info()
    (output / 'runtime-info.json').write_text(json.dumps(info, indent=2))
    env = os.environ.copy()
    env['PATH'] = str(ROOT / '.deps/qt/6.8.3/msvc2022_64/bin') + os.pathsep + env['PATH']
    report = {'source': str(args.recordings.resolve()), 'timeout_per_quality_seconds': args.timeout, 'passed': False}
    try:
        report['checks'] = verify_recordings(args.recordings.resolve(), env, args.timeout, output)
        report['passed'] = len(report['checks']) == 2 and all(c['passed'] for c in report['checks'])
    finally:
        (output / 'summary.json').write_text(json.dumps(report, indent=2))
        print(json.dumps(report), flush=True)
        print('Reports: ' + str(output), flush=True)
    return 0 if report['passed'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
