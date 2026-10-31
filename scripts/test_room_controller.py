"""Bounded Qt room-control integration; synthetic silence and owned sockets only."""
import json
import argparse
import socket
import subprocess
import threading
import time
from local_media_fixture import LocalMediaFixture, ROOT


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--with-publisher', action='store_true')
    parser.add_argument('--idle-seconds', type=int, default=0, help='Zero or 3 to 30 seconds of joined idle-room CPU measurement')
    args = parser.parse_args()
    if args.idle_seconds != 0 and not 3 <= args.idle_seconds <= 30:
        parser.error('Use zero or 3 to 30 idle seconds')
    folder = ROOT / 'artifacts' / (time.strftime('%Y%m%d-%H%M%S') + '-room-controller')
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
                accepted.write_text('Owned stall accepted handshake')
            except socket.timeout:
                pass

    worker = None
    try:
        fixture.start()
        env = fixture.client_env('room-controller')
        env['GOODSHARING_CONTROLLER_PUBLISHER'] = '1' if args.with_publisher else '0'
        env['GOODSHARING_CONTROLLER_IDLE_SECONDS'] = str(args.idle_seconds)
        env['GOODSHARING_CANCEL_URL'] = 'ws://127.0.0.1:' + str(listener.getsockname()[1])
        env['GOODSHARING_CANCEL_ACCEPTED'] = str(accepted)
        worker = threading.Thread(target=stall)
        worker.start()
        report, log = folder / 'controller.json', folder / 'controller.log'
        with log.open('w') as output:
            child = subprocess.run([str(ROOT / 'build/bin/goodsharing_room_controller_probe.exe'), '--report', str(report)], env=env, stdout=output, stderr=subprocess.STDOUT, timeout=60 + args.idle_seconds, creationflags=subprocess.CREATE_NO_WINDOW)
        data = json.loads(report.read_text()) if report.exists() else {}
        text = log.read_text(errors='replace')
        log_error = any(message in text for message in ('failed to negotiate', 'wrong state: closed', 'panicked at', 'peer connection is closed, cannot create offer', 'Attempted to drop unknown FFI handle'))
        participants = fixture.participants('room-controller')
        deadline = time.monotonic() + 3
        while participants and time.monotonic() < deadline:
            time.sleep(.1)
            participants = fixture.participants('room-controller')
        cycles = data.get('cycles', [])
        success = child.returncode == 0 and data.get('passed') is True and not data.get('error') and len(cycles) == 8
        success &= all(c.get('join_api_ms', 9999) < 100 and c.get('leave_api_ms', 9999) < 100 and c.get('leave_complete_ms', 9999) < 2000 for c in cycles)
        success &= data.get('announcement_subscription_cleanup_passed') is True and data.get('late_join_snapshot_passed') is True and data.get('invalid_token_rejected') is True
        success &= 0 < data.get('cancel_complete_ms', 9999) < 2000 and data.get('cancel_api_ms', 9999) < 100 and data.get('qt_ticks_during_cancel', 0) >= 10
        success &= not log_error and not participants
        if args.with_publisher:
            success &= data.get('owned_publisher_passed') is True and data.get('owned_publisher_max_stop_ms', 9999) < 1000 and data.get('audio_idle_wake_passed') is True
        if args.idle_seconds:
            idle = data.get('idle_room', {})
            success &= idle.get('duration_s', 0) >= args.idle_seconds * .9 and 0 <= idle.get('cpu_machine_percent', -1) <= 100 and idle.get('wake_command_complete_ms', 9999) < 100
        summary.update({'passed': bool(success), 'completed_cycles': len(cycles), 'lifecycle_log_error': log_error, 'remaining_participants': len(participants), 'report': str(report.relative_to(ROOT)), 'native': data})
    except Exception as error:
        summary['error'] = str(error)
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
        print(json.dumps(summary), flush=True)
        print('Reports: ' + str(folder), flush=True)
    return 0 if summary['passed'] and summary['owned_server_stopped'] and summary['stall_thread_stopped'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
