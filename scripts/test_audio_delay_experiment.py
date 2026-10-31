"""Same-runtime default/40 ms NetEq comparison on owned local A/V fixtures."""
import hashlib
import argparse
import json
import os
import shutil
from pathlib import Path
import time
from analyze_av_recovery import analyze
from local_media_fixture import ROOT
from test_remote_av import run_case
from verify_livekit_sdk import runtime_info


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--resume', type=Path, help='Preserve and continue the first two completed, owned attempts from an interrupted comparison')
    args = parser.parse_args()
    variant = ROOT / '.deps/audio-delay-experiment'
    info = json.loads((variant / 'build-info.json').read_text(encoding='utf-8-sig'))
    assert info['experimental_only'] and not info['installed_into_sdk'] and info['source_restored']
    assert sha(ROOT / 'scripts/build-audio-delay-experiment.ps1') == info['script_sha256']
    assert sha(variant / 'experimental-channel-receive.cpp') == info['experimental_receiver_sha256']
    native = ROOT / '.deps/sources/rust-a6f93a16/rust-sdks-a6f93a16/webrtc-sys/src/goodsharing_channel_receive.cpp'
    assert sha(native) == info['original_receiver_sha256']
    marker = json.loads((native.parents[2] / '.goodsharing-close-patched').read_text(encoding='utf-8'))
    source_root = native.parents[2]
    assert all(sha(source_root / path) == expected for path, expected in marker['source_sha256'].items())
    assert sha(source_root / 'Cargo.lock') == info['baseline_engine_build']['cargo_lock_sha256']
    baseline = runtime_info()
    executable = variant / 'client/GoodSharing.exe'
    engine = variant / 'client/livekit_ffi.dll'
    assert sha(executable) in (info['app_sha256'], sha(ROOT / 'build/bin/GoodSharing.exe'))
    # The engine build copies an app for completeness; use the final native
    # diagnostic build for both comparison arms and identify it independently.
    shutil.copy2(ROOT / 'build/bin/GoodSharing.exe', executable)
    info['test_app_sha256'] = sha(executable)
    assert sha(engine) == info['experimental_engine_sha256'] != info['baseline_engine_sha256']
    # Every other DLL stays byte-identical to the tested installed runtime.
    for path in (ROOT / 'build/bin').glob('*.dll'):
        if path.name != 'livekit_ffi.dll':
            assert sha(variant / 'client' / path.name) == sha(path), path.name
    folder = ROOT / 'artifacts' / (time.strftime('%Y%m%d-%H%M%S') + '-audio-delay-experiment')
    folder.mkdir()
    rows = []
    previous = os.environ.get('GOODSHARING_TEST_NETEQ_MAX_DELAY_MS')
    try:
        cases = [('switch-cancel', limit, 58000) for limit in (0, 40, 40, 0)] + [('wrong-key', 40, 18000)]
        for trial, (case, limit, duration) in enumerate(cases, 1):
            os.environ['GOODSHARING_TEST_NETEQ_MAX_DELAY_MS'] = str(limit)
            name = f'{case}-{trial}-max-{limit}'
            if args.resume and trial <= 2:
                origin = args.resume.resolve()
                assert origin.is_relative_to(ROOT / 'artifacts')
                original = json.loads((origin / name / 'summary.json').read_text(encoding='utf-8'))
                assert original['app_sha256'] == info['test_app_sha256'] and original['owned_clients_stopped'] and original['owned_server_stopped']
                shutil.copytree(origin / name, folder / name)
                row = original
                row['resumed_from'] = str(origin / name)
            else:
                row = run_case(folder / name, case, trace_timing=True, app_path=executable,
                               duration_ms=duration, clock_profile='500ms')
            row['original_case'] = row.get('original_case', row['case']);row['case'] = name
            row['original_passed'] = row.get('original_passed', row['passed'])
            row['audio_delay_experiment'] = dict(limit_ms=limit, runtime=info)
            log_path = folder / name / 'viewer.log'
            viewer_log = log_path.read_text(encoding='utf-8-sig') if log_path.exists() else ''
            row.setdefault('checks', {})
            if 'viewer' not in row and (folder / name / 'viewer.json').exists():
                row['diagnostic_viewer'] = json.loads((folder / name / 'viewer.json').read_text(encoding='utf-8'))
            row['checks']['audio_delay_experiment_acknowledged'] = f'GoodSharing audio delay experiment: max_ms={limit}' in viewer_log
            row['checks']['experimental_runtime_unchanged'] = sha(engine) == info['experimental_engine_sha256'] and sha(executable) == info['test_app_sha256'] == sha(ROOT / 'build/bin/GoodSharing.exe')
            row['checks']['installed_runtime_unchanged'] = runtime_info() == baseline
            row['passed'] = row['original_passed'] and all(row['checks'].values())
            rows.append(row)
            (folder / name / 'summary.json').write_text(json.dumps(row, indent=2)+'\n', encoding='utf-8')
            summary = dict(passed=all(r['passed'] for r in rows), diagnostic_only=True,
                           same_experimental_binary=True, installed_sdk_changed=False, runs=rows)
            (folder / 'summary.json').write_text(json.dumps(summary, indent=2)+'\n', encoding='utf-8')
            print(json.dumps(dict(trial=trial, limit_ms=limit, passed=row['passed'], error=row.get('error'),
                failed_checks=[k for k, v in row.get('checks', {}).items() if not v])), flush=True)
    finally:
        if previous is None:
            os.environ.pop('GOODSHARING_TEST_NETEQ_MAX_DELAY_MS', None)
        else:
            os.environ['GOODSHARING_TEST_NETEQ_MAX_DELAY_MS'] = previous
    result = analyze(folder / 'summary.json')
    (folder / 'analysis.json').write_text(json.dumps(result, indent=2)+'\n', encoding='utf-8')
    print('Reports: ' + str(folder), flush=True)
    raise SystemExit(0 if all(row['passed'] for row in rows) else 1)
