"""Room cleanup, partial setup failure, and cancellation with an owned stall."""
import argparse
import json
import socket
import subprocess
import threading
import time
from local_media_fixture import LocalMediaFixture, ROOT


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--cycles', type=int, default=20)
    parser.add_argument('--idle-seconds', type=int, default=0, help='Diagnostic samples only; do not replace the immediate resource gate')
    args = parser.parse_args()
    if not 20 <= args.cycles <= 100 or not 0 <= args.idle_seconds <= 15:
        parser.error('Use 20 to 100 cycles and 0 to 15 idle diagnostic seconds')
    folder = ROOT / 'artifacts' / (time.strftime('%Y%m%d-%H%M%S') + '-lifecycle')
    fixture = LocalMediaFixture(folder)
    summary = {'local_only': True, 'passed': False}
    stop = threading.Event()
    listener = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    listener.bind(('127.0.0.1', 0))
    listener.listen(2)
    listener.settimeout(.2)
    connections = []
    accepted = folder / 'stall-accepted'

    def stall():
        while not stop.is_set():
            try:
                connection, _ = listener.accept()
                connections.append(connection)
                accepted.write_text('Owned stall accepted the join handshake')
                # Deliberately withhold HTTP/WebSocket response, bounded by cleanup.
            except socket.timeout:
                pass

    worker = None
    try:
        fixture.start()
        env = fixture.client_env('lifecycle', 'off')
        env['GOODSHARING_LIFECYCLE_CYCLES'] = str(args.cycles)
        env['GOODSHARING_LIFECYCLE_IDLE_SECONDS'] = str(args.idle_seconds)
        env['GOODSHARING_CANCEL_URL'] = 'ws://127.0.0.1:' + str(listener.getsockname()[1])
        env['GOODSHARING_CANCEL_ACCEPTED'] = str(accepted)
        worker = threading.Thread(target=stall)
        worker.start()
        report = folder / 'lifecycle.json'
        log = folder / 'lifecycle.log'
        with log.open('w') as output:
            child = subprocess.run([str(ROOT / 'build/bin/goodsharing_lifecycle_probe.exe'), '--report', str(report)], env=env, stdout=output, stderr=subprocess.STDOUT, timeout=90, creationflags=subprocess.CREATE_NO_WINDOW)
        data = json.loads(report.read_text()) if report.exists() else {}
        cycles = data.get('cycles', [])
        # Sample after full destruction, allowing five native-library warmups.
        baseline = cycles[4].get('resources', {}) if len(cycles) > 4 else {}
        tail = [c.get('resources', {}) for c in cycles[-3:]]
        growth = {key: max(r[key] for r in tail) - baseline[key] if key in baseline and tail and all(key in r for r in tail) else None for key in ('handles', 'threads', 'private_bytes')}
        resources_ok = all(value is not None for value in growth.values()) and growth['handles'] <= 8 and growth['threads'] <= 2 and growth['private_bytes'] <= 32 * 1024 * 1024
        text = log.read_text(errors='replace')
        log_error = any(message in text for message in ('failed to negotiate', 'wrong state: closed', 'panicked at', 'peer connection is closed, cannot create offer', 'Attempted to drop unknown FFI handle'))
        deadline = time.monotonic() + 3
        participants = fixture.participants('lifecycle')
        while participants and time.monotonic() < deadline:
            time.sleep(.1)
            participants = fixture.participants('lifecycle')
        passed = child.returncode == 0 and not data.get('error') and len(cycles) == args.cycles and all(c['disconnected'] and c['stop_ms'] < 2000 and not c['error'] for c in cycles) and data.get('invalid_token_rejected') is True and 0 < data.get('cancel_during_connect_ms', 99999) < 2000 and data.get('cancelled_state_disconnected') is True and not log_error and not participants
        summary.update({'passed': passed, 'completed_cycles': len(cycles), 'lifecycle_log_error': log_error, 'remaining_participants': len(participants), 'report': str(report.relative_to(ROOT)), 'cancel_during_connect_ms': data.get('cancel_during_connect_ms'), 'max_stop_ms': max((c['stop_ms'] for c in cycles), default=None)})
        summary['resource_growth_after_warmup'] = growth
        summary['resource_bounds_passed'] = resources_ok
        summary['exception_cleanup_cycles'] = data.get('exception_cleanup_cycles', 0)
        summary['configuration'] = vars(args)
        summary['idle_resources'] = data.get('idle_resources', [])
        summary['passed'] &= resources_ok and summary['exception_cleanup_cycles'] == 3
        print(json.dumps(summary), flush=True)
    finally:
        stop.set()
        if worker is not None:
            worker.join(timeout=2)
        for connection in connections:
            connection.close()
        listener.close()
        summary['owned_server_stopped'] = fixture.stop()
        summary['stall_thread_stopped'] = worker is None or not worker.is_alive()
        folder.mkdir(parents=True, exist_ok=True)
        (folder / 'summary.json').write_text(json.dumps(summary, indent=2))
        print('Reports: ' + str(folder), flush=True)
    return 0 if summary['passed'] and summary['owned_server_stopped'] and summary['stall_thread_stopped'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
