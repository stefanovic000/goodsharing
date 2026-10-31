"""Repeat lifecycle checks with one owned offline decoder as a bounded load."""
import argparse
import ctypes
from ctypes import wintypes
import json
import os
from pathlib import Path
import subprocess
import sys
import time
from verify_livekit_sdk import ROOT, runtime_info


def cpu_seconds(process):
    values = [wintypes.FILETIME() for _ in range(4)]
    query = ctypes.windll.kernel32.GetProcessTimes
    query.argtypes = [wintypes.HANDLE] + [ctypes.POINTER(wintypes.FILETIME)] * 4
    query.restype = wintypes.BOOL
    if not query(wintypes.HANDLE(int(process._handle)), *(ctypes.byref(value) for value in values)):
        raise ctypes.WinError()
    return sum((value.dwHighDateTime << 32) + value.dwLowDateTime for value in values[2:]) / 1e7


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('recording', type=Path)
    args = parser.parse_args()
    folder = ROOT / 'artifacts' / (time.strftime('%Y%m%d-%H%M%S') + '-lifecycle-decode-load')
    folder.mkdir()
    (folder / 'runtime-info.json').write_text(json.dumps(runtime_info(), indent=2))
    env = os.environ.copy()
    env['PATH'] = str(ROOT / '.deps/qt/6.8.3/msvc2022_64/bin') + os.pathsep + env['PATH']
    result = {'passed': False, 'decoder_is_load_fixture_only': True}
    decoder = None
    try:
        with (folder / 'decoder-load.log').open('w') as log:
            decoder = subprocess.Popen([str(ROOT / 'build/bin/goodsharing_probe.exe'), '--decode-recording', str(args.recording.resolve())], env=env, stdout=log, stderr=subprocess.STDOUT, creationflags=subprocess.CREATE_NO_WINDOW)
            deadline = time.monotonic() + 10
            while decoder.poll() is None and cpu_seconds(decoder) < 1 and time.monotonic() < deadline:
                time.sleep(.1)
            result['decoder_cpu_seconds_at_start'] = cpu_seconds(decoder)
            if decoder.poll() is not None or result['decoder_cpu_seconds_at_start'] < 1:
                raise RuntimeError('Decoder load did not become active')
            child = subprocess.run([sys.executable, str(ROOT / 'scripts/test_local_lifecycle.py')], cwd=ROOT, env=env, capture_output=True, text=True, timeout=110, creationflags=subprocess.CREATE_NO_WINDOW)
            (folder / 'lifecycle-runner.log').write_text(child.stdout + child.stderr)
            result['lifecycle_exit_code'] = child.returncode
            result['decoder_running_at_finish'] = decoder.poll() is None
            result['decoder_cpu_seconds_at_finish'] = cpu_seconds(decoder)
            result['passed'] = child.returncode == 0 and result['decoder_running_at_finish']
    finally:
        if decoder is not None and decoder.poll() is None:
            decoder.terminate()
            try:
                decoder.wait(timeout=5)
            except subprocess.TimeoutExpired:
                decoder.kill()
                decoder.wait(timeout=5)
        result['owned_decoder_stopped'] = decoder is not None and decoder.poll() is not None
        (folder / 'summary.json').write_text(json.dumps(result, indent=2))
        print(json.dumps(result), flush=True)
        print('Reports: ' + str(folder), flush=True)
    return 0 if result['passed'] and result['owned_decoder_stopped'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
