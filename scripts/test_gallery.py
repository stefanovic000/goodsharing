"""Generalized gallery test: 2 or 3 owned publishers, one viewer.

Usage: python scripts/test_gallery.py [--publishers 2|3]

Phase 1: all publishers are live and the viewer holds one tile per share (extra
tiles carry their own admitted video). Phase 2: the last publisher stops on its
own; its tile is released while the remaining tiles keep painting.
"""
import argparse
import atexit
import json
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / 'scripts'))
from local_media_fixture import LocalMediaFixture  # noqa: E402

parser = argparse.ArgumentParser()
parser.add_argument('--publishers', type=int, default=2, choices=[2, 3])
parser.add_argument('--wrong-key-index', type=int, default=0, help='1-based publisher that uses a mismatched room key')
args = parser.parse_args()

APP = ROOT / 'build/bin/GoodSharing.exe'
FIXTURE = ROOT / 'build/bin/goodsharing_fixture.exe'
folder = ROOT / 'artifacts' / (time.strftime('%Y%m%d-%H%M%S') + f'-gallery{args.publishers}')
folder.mkdir(parents=True)
logs = []
children = []


def launch(name, command, env):
    log = (folder / (name + '.log')).open('w')
    logs.append(log)
    child = subprocess.Popen(command, env=env, stdout=log, stderr=subprocess.STDOUT)
    children.append(child)
    return child


fixture = LocalMediaFixture(folder / 'server')
fixture.start()
atexit.register(fixture.stop)
room = 'gallery-test'
env = fixture.client_env(room)
watch_ms = 28000

sources = []
senders = []
for index in range(1, args.publishers + 1):
    ready = folder / f'source{index}.json'
    source = launch(f'source{index}', [str(FIXTURE), '--hide-cursor', '--seconds', '40',
                                       '--ready-file', str(ready)], env)
    deadline = time.monotonic() + 8
    while not ready.exists():
        if source.poll() is not None or time.monotonic() > deadline:
            raise RuntimeError(f'Source {index} did not become ready')
        time.sleep(.1)
    info = json.loads(ready.read_text())
    sources.append(source)
    sender_env = env.copy()
    sender_env['GOODSHARING_TEST_PUBLISHER_TOKEN'] = fixture.token(f'publisher{index}', room, can_publish=True)
    if args.wrong_key_index == index:
        key = bytearray.fromhex(sender_env['GOODSHARING_TEST_E2EE_KEY']); key[0] ^= 0xff
        sender_env['GOODSHARING_TEST_E2EE_KEY'] = key.hex()
    senders.append(launch(f'sender{index}', [str(APP), '--smoke-share-hwnd', info['hwnd'],
                                             '--smoke-pid', str(info['pid']),
                                             '--smoke-duration-ms', str(15000 if index == args.publishers else watch_ms + 6000),
                                             '--report', str(folder / f'sender{index}.json'),
                                             '--screenshot', str(folder / f'sender{index}.png'),
                                             '--test-window-minimized'], sender_env))

viewer_env = env.copy()
viewer_env['GOODSHARING_TEST_GALLERY'] = '1'
checkpoints = folder / 'checkpoints.json'
viewer = launch('viewer', [str(APP), '--smoke-watch', '--test-av-playback', '--quality', '720',
                           '--test-av-checkpoints', '--test-av-checkpoint-file', str(checkpoints),
                           '--report', str(folder / 'viewer.json'), '--screenshot', str(folder / 'viewer.png'),
                           '--watch-duration-ms', str(watch_ms), '--test-window-minimized'], viewer_env)

deadline = time.monotonic() + 30
while time.monotonic() < deadline:
    if checkpoints.exists():
        try:
            if len(json.loads(checkpoints.read_text())) >= 1:
                break
        except json.JSONDecodeError:
            pass
    time.sleep(.2)

try:
    viewer.wait(timeout=45)
except subprocess.TimeoutExpired:
    viewer.terminate()

report = json.loads((folder / 'viewer.json').read_text()) if (folder / 'viewer.json').exists() else {}
observations = json.loads(checkpoints.read_text()) if checkpoints.exists() else []
first = observations[0] if observations else {}
extras = first.get('extras') or []
primary_drawn = report.get('video', {}).get('drawn_distinct_frames', 0)
extra_drawn = [item.get('video', {}).get('drawn_distinct_frames', 0) for item in extras]
extra_admitted = [item.get('video', {}).get('admitted_frames', 0) for item in extras]
expected_before = args.publishers
if args.wrong_key_index:
    tile_drawn = [primary_drawn] + extra_drawn
    checks = {
        'three_slots': first.get('slot_count') == expected_before,
        'healthy_tiles_painting': sum(1 for value in tile_drawn if value > 10) >= expected_before - 1,
        'mismatched_tile_silent': any(value == 0 for value in tile_drawn),
        'no_room_wide_invalidation': report.get('watching') in (True, 1),
    }
else:
    checks = {
    f'{expected_before}_slots_before_exit': first.get('slot_count') == expected_before,
    f'{expected_before - 1}_extra_tiles': len(extras) == expected_before - 1,
    'all_extra_tiles_admitted': all(value > 30 for value in extra_admitted) if not args.wrong_key_index else True,
    'all_extra_tiles_drawn': all(value > 10 for value in extra_drawn) if not args.wrong_key_index else True,
    'primary_painted': primary_drawn > 60,
    'released_after_exit': report.get('slot_count') == expected_before - 1
                          and len(report.get('extras') or []) == expected_before - 2,
}
summary = {'case': f'gallery{args.publishers}', 'passed': all(checks.values()), 'checks': checks,
           'slots_before': first.get('slot_count'), 'slots_after': report.get('slot_count'),
           'primary_drawn': primary_drawn, 'extra_admitted': extra_admitted, 'extra_drawn': extra_drawn}
(folder / 'summary.json').write_text(json.dumps(summary, indent=2), encoding='utf-8')
print(json.dumps({'case': summary['case'], 'passed': summary['passed'],
                  'failed_checks': [name for name, value in checks.items() if not value]}))
print('Reports:', folder)

for child in children:
    if child.poll() is None:
        child.terminate()
fixture.stop()
for log in logs:
    log.close()
raise SystemExit(0 if summary['passed'] else 1)
