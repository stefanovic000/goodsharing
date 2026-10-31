"""Bounded 2x2 client/SFU timing experiment with generated media only."""
import argparse
import json
from pathlib import Path
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--seconds', type=int, default=20)
    args = parser.parse_args()
    if not 18 <= args.seconds <= 20: parser.error('Use 18 to 20 seconds to retain eight seconds after baseline training')
    folder = ROOT / 'artifacts' / (time.strftime('%Y%m%d-%H%M%S') + '-clock-bootstrap')
    folder.mkdir(exist_ok=False)
    cases = [('baseline', 0, 0, 0), ('client-only', 500, 0, 0),
             ('server-only', 0, 500, 0), ('both', 500, 500, 0), ('both-delayed', 500, 500, 50)]
    result = dict(synthetic_only=True, real_screen_capture_performed=False, same_server_build_for_all_cases=True,
        synchronized_playback_verified=False, normal_app_defaults_changed=False, passed=False, runs=[])
    try:
        for name, client, server, delay in cases:
            log = folder / (name + '.log')
            with log.open('w') as stream:
                child = subprocess.run([sys.executable, str(ROOT / 'scripts/test_media_clock_live.py'),
                    '--seconds', str(args.seconds), '--clock', 'rtcp', '--rtcp-report-ms', str(client),
                    '--sfu-rtcp-report-ms', str(server), '--audio-delay-ms', str(delay), '--clock-server'],
                    stdout=stream, stderr=subprocess.STDOUT, timeout=args.seconds + 150)
            paths = [Path(line.removeprefix('Reports: ')) for line in log.read_text().splitlines() if line.startswith('Reports: ')]
            if len(paths) != 1: raise RuntimeError('Missing owned media-clock report: ' + name)
            path = paths[0] / 'summary.json'
            report = json.loads(path.read_text())
            row = dict(case=name, exit=child.returncode, report=path.relative_to(ROOT).as_posix(),
                media_passed=report.get('media_passed', False), clock_gate_passed=report['passed'],
                source_capture_passed=report.get('sender_capture_clock', {}).get('passed', False),
                startup_seconds=[q['warmup_seconds'] for q in report.get('rtcp_clock', {}).get('qualities', [])],
                all_owned_processes_stopped=report.get('owned_client_stopped', False) and report.get('owned_server_stopped', False),
                remaining_participants=report.get('remaining_participants'), error=report.get('error'))
            result['runs'].append(row)
            print(json.dumps(row), flush=True)
            if (row['error'] or not row['media_passed'] or not row['source_capture_passed'] or
                    not row['all_owned_processes_stopped'] or row['remaining_participants'] != 0):
                raise RuntimeError('Media/cleanup failure in ' + name)
        baseline = json.loads((ROOT / result['runs'][0]['report']).read_text())
        fast = json.loads((ROOT / result['runs'][3]['report']).read_text())
        delayed = json.loads((ROOT / result['runs'][4]['report']).read_text())
        comparisons = []
        assert len({json.loads((ROOT / row['report']).read_text())['server_runtime']['binary_sha256'] for row in result['runs']}) == 1, 'Server builds differ across timing cases'
        assert len({json.dumps(json.loads((ROOT / row['report']).read_text())['runtime'], sort_keys=True) for row in result['runs']}) == 1, 'Client runtimes differ across timing cases'
        for quality in fast['rtcp_clock']['qualities']:
            match = next(q for q in delayed['rtcp_clock']['qualities'] if q['height'] == quality['height'])
            change = match['median_signed_clock_offset_ms'] - quality['median_signed_clock_offset_ms']
            comparisons.append(dict(height=quality['height'], signed_offset_change_ms=change,
                maximum_absolute_change_ms=10, delay_invariance_passed=abs(change) <= 10))
        result['delay_comparison'] = comparisons
        result['startup_improvement_verified'] = bool(fast['passed'] and delayed['passed'] and
            max(q['warmup_seconds'] for q in fast['rtcp_clock']['qualities']) <
            min(q['warmup_seconds'] for q in baseline['rtcp_clock']['qualities']))
        result['passed'] = bool(result['startup_improvement_verified'] and all(q['delay_invariance_passed'] for q in comparisons))
    except Exception as error:
        result['error'] = str(error)
    finally:
        (folder / 'summary.json').write_text(json.dumps(result, indent=2) + '\n')
    print('Reports: ' + str(folder))
    return 0 if result['passed'] else 1


if __name__ == '__main__': raise SystemExit(main())
