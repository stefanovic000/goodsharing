"""Focused original-SDK regressions after adding bounded first-fault traces."""
import json
import time
from local_media_fixture import ROOT
from test_remote_av import run_case


if __name__ == '__main__':
    folder = ROOT / 'artifacts' / (time.strftime('%Y%m%d-%H%M%S') + '-first-fault-regressions')
    folder.mkdir()
    rows = []
    for case in ('switch-cancel', 'switch-source-exit', 'wrong-key', 'paint-stall', 'static-resume'):
        row = run_case(folder / case, case, trace_timing=True, duration_ms=58000 if case == 'switch-cancel' else None)
        rows.append(row)
        print(json.dumps(dict(case=case, passed=row['passed'], error=row.get('error'),
            failed_checks=[k for k, v in row.get('checks', {}).items() if not v])), flush=True)
        summary = dict(passed=all(r['passed'] for r in rows), original_installed_sdk=True, runs=rows)
        (folder / 'summary.json').write_text(json.dumps(summary, indent=2)+'\n', encoding='utf-8')
    print('Reports: ' + str(folder), flush=True)
    raise SystemExit(0 if all(r['passed'] for r in rows) else 1)
