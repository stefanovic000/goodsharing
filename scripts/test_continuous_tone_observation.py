"""Offline negative controls for complete generated-tone observation."""
import argparse
import copy
import hashlib
import json
from pathlib import Path
from test_remote_av import check_continuous_tone


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--summary', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    assert not args.output.exists(), 'Refusing to overwrite observation controls'
    suite = json.loads(args.summary.read_text(encoding='utf-8'))
    observed = next(r['viewer']['audio_observer'] for r in suite['runs'] if r['passed'] and
                    r.get('viewer', {}).get('audio_observer', {}).get('continuous_tone_observation'))
    checks = {'actual_positive_observation_accepted': check_continuous_tone(observed),
              'missing_observation_rejected': not check_continuous_tone({})}
    for name, replacement in {
        'not_started': {'started': False},
        'too_short': {'duration_ms': 999},
        'missing_samples': {'observed_frame_coverage': .94},
        'long_mid_session_gap': {'longest_quiet_ms': 40.01},
        'excess_quiet_samples': {'quiet_frame_fraction': .051},
    }.items():
        changed = copy.deepcopy(observed)
        changed['continuous_tone_observation'].update(replacement)
        checks[name + '_rejected'] = not check_continuous_tone(changed)
    assert all(checks.values()), checks
    result = dict(passed=True, source_summary=str(args.summary), source_sha256=hashlib.sha256(args.summary.read_bytes()).hexdigest(), checks=checks)
    args.output.write_text(json.dumps(result, indent=2) + '\n', encoding='utf-8')
    print(json.dumps(result))
