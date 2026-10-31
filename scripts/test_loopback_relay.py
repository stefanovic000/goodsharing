"""Known UDP echo workload verifies relay isolation, rate and queue bounds."""
import json
from pathlib import Path
import socket
import threading
import time
from loopback_relay import LoopbackRelay


def main():
    echo = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    echo.bind(('127.0.0.1', 0))
    echo.settimeout(.05)
    stop = threading.Event()
    failures = []

    def answer():
        while not stop.is_set():
            try:
                data, peer = echo.recvfrom(65536)
                echo.sendto(data, peer)
            except socket.timeout:
                pass
            except OSError as error:
                if not stop.is_set():
                    failures.append(str(error))
                break

    worker = threading.Thread(target=answer, name='owned-UDP-echo')
    worker.start()
    relay = None
    clients = []
    result = {'passed': False, 'local_only': True}
    try:
        port = echo.getsockname()[1]
        relay = LoopbackRelay(port, queue_age_ms=150, queue_bytes=65536)
        for _ in range(2):
            client = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            client.bind(('127.0.0.1', 0))
            client.settimeout(1)
            clients.append(client)
        for i, client in enumerate(clients):
            body = ('owned-flow-' + str(i)).encode()
            client.sendto(body, ('127.0.0.2', port))
            assert client.recv(65536) == body, 'Relay crossed client flows'
        before = relay.snapshot()
        relay.configure(8000)
        began = time.monotonic()
        for _ in range(100):
            clients[0].sendto(b'G'*1200, ('127.0.0.2', port))
            time.sleep(.001)
        time.sleep(.25)
        after = relay.snapshot()
        elapsed = time.monotonic()-began
        bits = (after['up_sent_bytes']-before['up_sent_bytes'])*8
        assert bits <= 8000*elapsed+1200*8, 'Configured rate did not constrain transmission'
        assert after['age_dropped_packets'] > 0 and after['capacity_dropped_packets'] > 0, 'Queue bounds were not exercised'
        assert after['queue_high_water_bytes'] <= 65536 and after['maximum_sent_age_ms'] <= 150, 'Queue limits exceeded'
        relay.configure(0)
        clients[1].sendto(b'fresh-after-pressure', ('127.0.0.2', port))
        assert clients[1].recv(65536) == b'fresh-after-pressure', 'Relay did not recover'
        result.update(passed=True, restricted_seconds=elapsed, restricted_sent_bits=bits, relay=relay.snapshot())
    except Exception as error:
        result['error'] = str(error)
    finally:
        stop.set()
        worker.join(timeout=1)
        if relay:
            result['final_relay'] = relay.close()
            result['passed'] &= not result['final_relay']['error']
        for client in clients:
            client.close()
        echo.close()
        result['passed'] &= not failures and not worker.is_alive()
        result['echo_errors'] = failures
        path = Path(__file__).resolve().parents[1] / 'artifacts' / (time.strftime('%Y%m%d-%H%M%S')+'-relay-self-check.json')
        path.parent.mkdir(exist_ok=True)
        path.write_text(json.dumps(result, indent=2), encoding='utf-8')
        print(json.dumps({'passed': result['passed'], 'report': str(path), 'error': result.get('error')}), flush=True)
    return 0 if result['passed'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
