"""Test-owned host-local UDP relay to loopback; no OS traffic rules."""
from collections import deque
import random
import selectors
import socket
import threading
import time


class LoopbackRelay:
    def __init__(self, port=8765, *, front_host='127.0.0.2', queue_age_ms=1000, queue_bytes=2*1024*1024):
        if not 10 <= queue_age_ms <= 2000 or not 65536 <= queue_bytes <= 4*1024*1024:
            raise ValueError('Use bounded relay queues')
        self.port, self.age_limit, self.byte_limit = port, queue_age_ms / 1000, queue_bytes
        self.selector = selectors.DefaultSelector()
        self.front = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.front.setsockopt(socket.SOL_SOCKET, socket.SO_EXCLUSIVEADDRUSE, 1)
        if hasattr(socket, 'SIO_UDP_CONNRESET'):
            self.front.ioctl(socket.SIO_UDP_CONNRESET, False)
        self.front.bind((front_host, port))  # Winsock requires this address to be local.
        self.front.setblocking(False)
        self.wake_read, self.wake_write = socket.socketpair()
        self.wake_read.setblocking(False)
        self.wake_write.setblocking(False)
        self.selector.register(self.front, selectors.EVENT_READ, ('front', None))
        self.selector.register(self.wake_read, selectors.EVENT_READ, ('wake', None))
        self.lock = threading.Lock()
        self.rate = 0
        self.done = False
        self.queue = deque()
        self.queued_bytes = 0
        self.next_send = 0.
        self.flows = {}
        self.failure = ''
        self.stats = dict(up_received_packets=0, up_received_bytes=0, up_sent_packets=0,
                          up_sent_bytes=0, down_sent_packets=0, down_sent_bytes=0,
                          age_dropped_packets=0, capacity_dropped_packets=0,
                          queue_high_water_bytes=0, queue_high_water_packets=0,
                          maximum_sent_age_ms=0., rejected_flows=0, udp_reset_events=0,
                          loss_dropped_up=0, loss_dropped_down=0)
        self.history = []
        self.started = time.monotonic()
        self.worker = threading.Thread(target=self.run, name='owned-loopback-UDP-relay')
        self.worker.start()

    def configure(self, bitrate_bps=0):
        if not 0 <= bitrate_bps <= 100_000_000:
            raise ValueError('Use zero (unlimited) or a bounded test bitrate')
        with self.lock:
            self.rate = bitrate_bps
            self.next_send = time.monotonic()
            if len(self.history) < 64:
                self.history.append(dict(elapsed_s=time.monotonic()-self.started, bitrate_bps=bitrate_bps))
        try:
            self.wake_write.send(b'w')
        except BlockingIOError:
            pass

    def configure_loss(self, percent=0.0):
        """Drop that share of datagrams in each direction; 0 disables loss.

        This is real network loss: it happens before depacketisation, so FEC,
        RED and NACK can recover it, unlike a post-decode frame drop."""
        if not 0 <= percent <= 50:
            raise ValueError("Use 0 to 50 percent loss")
        with self.lock:
            self.loss = percent / 100.0
            self.history.append(dict(elapsed_s=time.monotonic()-self.started, loss_percent=percent))

    def snapshot(self):
        with self.lock:
            return dict(self.stats, error=self.failure, flows=len(self.flows),
                        queued_bytes=self.queued_bytes, queued_packets=len(self.queue),
                        rate_bps=self.rate, elapsed_s=time.monotonic()-self.started,
                        queue_age_limit_ms=self.age_limit*1000, queue_byte_limit=self.byte_limit,
                        configurations=list(self.history))

    def run(self):
        try:
            while True:
                with self.lock:
                    if self.done:
                        break
                    now = time.monotonic()
                    if self.queue:
                        wait = max(0., min(self.next_send-now if self.rate else 0.,
                                           self.queue[0][0]+self.age_limit-now))
                    else:
                        wait = None
                for selected, _ in self.selector.select(wait):
                    kind, client = selected.data
                    if kind == 'wake':
                        self.wake_read.recv(4096)
                        continue
                    try:
                        data, address = selected.fileobj.recvfrom(65536)
                    except ConnectionResetError as error:
                        if error.winerror != 10054:
                            raise
                        # A test-owned UDP endpoint may already have closed.
                        # Count its ICMP reset without terminating other flows.
                        with self.lock:
                            self.stats['udp_reset_events'] += 1
                        continue
                    with self.lock:
                        if kind == 'front':
                            upstream = self.flows.get(address)
                            if upstream is None:
                                try:
                                    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as ownership:
                                        ownership.bind((address[0], 0))
                                except OSError:
                                    self.stats['rejected_flows'] += 1
                                    continue  # Never relay a non-local client's traffic.
                                if len(self.flows) >= 16:
                                    self.stats['rejected_flows'] += 1
                                    continue
                                upstream = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
                                if hasattr(socket, 'SIO_UDP_CONNRESET'):
                                    upstream.ioctl(socket.SIO_UDP_CONNRESET, False)
                                upstream.bind(('127.0.0.1', 0))
                                upstream.connect(('127.0.0.1', self.port))
                                upstream.setblocking(False)
                                self.flows[address] = upstream
                                self.selector.register(upstream, selectors.EVENT_READ, ('back', address))
                            if self.loss and random.random() < self.loss:
                                self.stats['loss_dropped_up'] += 1
                                continue
                            self.stats['up_received_packets'] += 1
                            self.stats['up_received_bytes'] += len(data)
                            if self.queued_bytes + len(data) > self.byte_limit or len(self.queue) >= 8192:
                                self.stats['capacity_dropped_packets'] += 1
                                continue
                            self.queue.append((time.monotonic(), upstream, data))
                            self.queued_bytes += len(data)
                            self.stats['queue_high_water_bytes'] = max(self.stats['queue_high_water_bytes'], self.queued_bytes)
                            self.stats['queue_high_water_packets'] = max(self.stats['queue_high_water_packets'], len(self.queue))
                        else:
                            if self.loss and random.random() < self.loss:
                                self.stats['loss_dropped_down'] += 1
                                continue
                            try:
                                self.front.sendto(data, client)
                            except ConnectionResetError as error:
                                if error.winerror != 10054:
                                    raise
                                self.stats['udp_reset_events'] += 1
                                continue
                            self.stats['down_sent_packets'] += 1
                            self.stats['down_sent_bytes'] += len(data)
                # Bound each drain batch so control/return traffic cannot starve.
                for _ in range(128):
                    with self.lock:
                        now = time.monotonic()
                        if not self.queue:
                            break
                        received, upstream, data = self.queue[0]
                        if now - received >= self.age_limit:
                            self.queue.popleft()
                            self.queued_bytes -= len(data)
                            self.stats['age_dropped_packets'] += 1
                            continue
                        if self.rate and now < self.next_send:
                            break
                        try:
                            upstream.send(data)
                        except ConnectionResetError as error:
                            if error.winerror != 10054:
                                raise
                            self.queue.popleft()
                            self.queued_bytes -= len(data)
                            self.stats['udp_reset_events'] += 1
                            continue
                        self.queue.popleft()
                        self.queued_bytes -= len(data)
                        self.stats['up_sent_packets'] += 1
                        self.stats['up_sent_bytes'] += len(data)
                        self.stats['maximum_sent_age_ms'] = max(self.stats['maximum_sent_age_ms'], (now-received)*1000)
                        # No accumulated credit after idle or a rate change.
                        self.next_send = now + len(data)*8/self.rate if self.rate else now
        except Exception as error:
            with self.lock:
                self.failure = str(error)

    def close(self):
        with self.lock:
            self.done = True
        try:
            self.wake_write.send(b'x')
        except OSError:
            pass
        self.worker.join(timeout=3)
        if self.worker.is_alive():
            raise RuntimeError('Owned relay did not stop')
        result = self.snapshot()
        self.selector.close()
        for item in [self.front, self.wake_read, self.wake_write, *self.flows.values()]:
            item.close()
        self.queue.clear()
        return result
