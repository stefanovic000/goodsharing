"""Pinned native cryptor controls and strict-policy checks, no user media/network."""
import hashlib
import argparse
import json
from pathlib import Path
import subprocess
import time
from verify_livekit_sdk import runtime_info

ROOT = Path(__file__).resolve().parents[1]

def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--with-edge-diagnostics', action='store_true', help='Also reproduce the unresolved valid H.264 small-RTP-timestamp round trip; failures remain failures')
    parser.add_argument('--pinned-control', action='store_true', help='Use the original pinned archive implementation, preserving its known parser failures')
    args = parser.parse_args()
    folder = ROOT / 'artifacts' / (time.strftime('%Y%m%d-%H%M%S') + '-cryptor-policy')
    folder.mkdir(exist_ok=False)
    summary = {'passed': False, 'local_only': True, 'network_used': False,
               'captured_media': False, 'saved_keys_or_payloads': False, 'runs': [], 'edge_cases': []}
    try:
        runtime = runtime_info()
        (folder / 'runtime-info.json').write_text(json.dumps(runtime, indent=2), encoding='utf-8')
        executable = ROOT / ('build/bin/goodsharing_cryptor_test.exe' if args.pinned_control else 'build/bin/goodsharing_fixed_cryptor_test.exe')
        summary['native_implementation'] = 'original-pinned-archive' if args.pinned_control else 'repaired-pinned-source'
        if not args.pinned_control:
            summary['repaired_source_sha256'] = digest(ROOT / '.deps/sources/webrtc-89d790b/frame_crypto_transformer.fixed.cc')
            assert summary['repaired_source_sha256'] == runtime['installed_sdk']['engine-build-info']['repaired_cryptor_source_sha256']
        before = digest(executable)
        summary['executable_sha256'] = before
        summary['linked_webrtc_library_sha256'] = digest(ROOT / '.deps/webrtc-89d790b/win-x64-release/lib/webrtc.lib')
        for video in (False, True):
            for control in (True, False):
                name = ('video' if video else 'audio') + ('-upstream-control' if control else '-strict')
                arguments = [str(executable)] + (['--upstream-control'] if control else []) + (['--video'] if video else [])
                child = subprocess.run(arguments, capture_output=True, text=True, timeout=15,
                                       creationflags=subprocess.CREATE_NO_WINDOW)
                (folder / (name + '.log')).write_text(child.stdout + child.stderr, encoding='utf-8')
                records = [json.loads(line) for line in child.stdout.splitlines() if line.startswith('{')]
                assert child.returncode == 0 and len(records) == 1, name
                record = records[0]
                assert record['passed'] and record['strict'] is not control and record['video'] is video, name
                assert record['valid_roundtrip'] and record['revocation_checked'] and record['missing_sender_key_rejected'], name
                if control:
                    assert record['unauthenticated_forwarded'] == 2 and record['tagged_plaintext_forwarded'], name
                    assert record['disabled_sender_plaintext_forwarded'] and record['disabled_receiver_plaintext_forwarded'], name
                else:
                    assert record['unauthenticated_forwarded'] == 0 and not record['tagged_plaintext_forwarded'], name
                    assert not record['disabled_sender_plaintext_forwarded'] and not record['disabled_receiver_plaintext_forwarded'], name
                    if not args.pinned_control:
                        assert record['malformed_samples'] == 128, name
                summary['runs'].append({'name': name, 'result': record})
        summary['executable_unchanged'] = before == digest(executable)
        production = [p.relative_to(ROOT).as_posix() for p in (ROOT / 'src').rglob('*')
                      if p.is_file() and p.suffix in ('.h', '.cpp') and
                      any(term in p.read_text(encoding='utf-8') for term in ('DiagnosticRaw', 'DiagnosticTrackReaders'))]
        summary['production_raw_receiver_references'] = production
        summary['policy_rejection_passed'] = len(summary['runs']) == 4 and summary['executable_unchanged'] and not production
        if args.with_edge_diagnostics:
            for flags in (['--rtp-edge'], ['--rtp-wrap', '--ssrc-edge'], ['--long-header']):
                for control in (True, False):
                    name = 'video-' + flags[0].removeprefix('--') + ('-upstream' if control else '-strict')
                    arguments = [str(executable), '--video', *flags] + (['--upstream-control'] if control else [])
                    child = subprocess.run(arguments, capture_output=True, text=True, timeout=15,
                                           creationflags=subprocess.CREATE_NO_WINDOW)
                    (folder / (name + '.log')).write_text(child.stdout + child.stderr, encoding='utf-8')
                    summary['edge_cases'].append({'name': name, 'arguments': arguments[1:], 'exit_code': child.returncode,
                                                  'passed': child.returncode == 0, 'log': name + '.log'})
        summary['all_observed_roundtrips_passed'] = all(case['passed'] for case in summary['edge_cases'])
        summary['passed'] = summary['policy_rejection_passed'] and summary['all_observed_roundtrips_passed']
    except Exception as error:
        summary['error'] = str(error)
    (folder / 'summary.json').write_text(json.dumps(summary, indent=2), encoding='utf-8')
    print(json.dumps(summary), flush=True)
    print('Reports: ' + str(folder), flush=True)
    return 0 if summary['passed'] else 1

if __name__ == '__main__':
    raise SystemExit(main())
