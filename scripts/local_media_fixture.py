"""Owned loopback fixture shared by bounded local integration checks."""
import base64
import hashlib
import hmac
import json
import os
from pathlib import Path
import secrets
import socket
import subprocess
import time
import urllib.request
from verify_livekit_sdk import runtime_info

ROOT = Path(__file__).resolve().parents[1]


class LocalMediaFixture:
    def __init__(self, folder):
        self.folder = Path(folder)
        self.secret = secrets.token_hex(32)
        self.server = None
        self.log = None
        self.server_info = None

    def start(self, *, relay=False, relay_address='127.0.0.2', sfu_rtcp_report_ms=0, clock_server=False, server_debug=False, diagnostic_port=0):
        if diagnostic_port and not 1024 <= diagnostic_port <= 65535: raise ValueError('Invalid owned diagnostic port')
        if sfu_rtcp_report_ms not in (0, 500): raise ValueError('Use upstream SFU defaults or the isolated 500 ms experiment')
        executable = ROOT / '.deps/livekit-server-1.13.10/livekit-server.exe'
        self.server_info = dict(variant='upstream-release', version='1.13.10', rtcp_report_ms=0)
        if sfu_rtcp_report_ms or clock_server:
            from verify_clock_server import verify, OUTPUT
            self.server_info = dict(variant='isolated-clock-experiment', rtcp_report_ms=sfu_rtcp_report_ms, build=verify())
            executable = OUTPUT / 'livekit-server.exe'
        self.server_info['binary_sha256'] = hashlib.sha256(executable.read_bytes()).hexdigest()
        for kind in (socket.SOCK_STREAM, socket.SOCK_DGRAM):
            with socket.socket(socket.AF_INET, kind) as check:
                check.bind(('127.0.0.1', 8765))
        self.folder.mkdir(parents=True, exist_ok=False)
        (self.folder / 'runtime-info.json').write_text(json.dumps(runtime_info(), indent=2), encoding='utf-8')
        (self.folder / 'server-runtime.json').write_text(json.dumps(self.server_info, indent=2), encoding='utf-8')
        config = self.folder / 'server.yaml'
        if relay:
            with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as local_check:
                local_check.bind((relay_address, 0))
        media_ip = relay_address if relay else '127.0.0.1'
        # HTTP bind_addresses/node_ip do not restrict ICE gathering. In
        # particular, IPv6 VPN adapters can otherwise win a same-PC session
        # and later lose media while loopback signalling stays healthy.
        candidate_filter = '  ips:\n    includes: [127.0.0.0/8]\n'
        self.server_info['ice_candidate_scope'] = 'ipv4-loopback-only'
        (self.folder / 'server-runtime.json').write_text(json.dumps(self.server_info, indent=2), encoding='utf-8')
        config.write_text('port: 8765\nbind_addresses: [127.0.0.1]\nrtc:\n  tcp_port: 0\n  udp_port: 8765\n  node_ip: ' + media_ip + '\n  enable_loopback_candidate: true\n  use_external_ip: false\n' + candidate_filter + 'logging:\n  level: ' + ('debug' if server_debug else 'warn') + '\n')
        if diagnostic_port:
            with socket.socket(socket.AF_INET,socket.SOCK_STREAM) as check: check.bind(('127.0.0.1',diagnostic_port))
            with config.open('a') as output: output.write('debug_handler:\n  port: '+str(diagnostic_port)+'\n')
        env = os.environ.copy()
        env.pop('GOODSHARING_TEST_SFU_RTCP_MS', None)
        if sfu_rtcp_report_ms: env['GOODSHARING_TEST_SFU_RTCP_MS'] = str(sfu_rtcp_report_ms)
        env['LIVEKIT_KEYS'] = 'local-test: ' + self.secret
        self.log = (self.folder / 'server.log').open('w')
        self.server = subprocess.Popen([str(executable), '--config', str(config)], env=env, stdout=self.log, stderr=subprocess.STDOUT, creationflags=subprocess.CREATE_NO_WINDOW)
        deadline = time.monotonic() + 15
        while time.monotonic() < deadline:
            if self.server.poll() is not None:
                raise RuntimeError('Owned SFU exited during startup')
            try:
                with urllib.request.urlopen('http://127.0.0.1:8765', timeout=1) as response:
                    if response.status == 200:
                        return self
            except OSError:
                time.sleep(.1)
        raise TimeoutError('Owned SFU readiness deadline exceeded')

    def token(self, identity, room, *, admin=False, can_publish=None):
        def enc(value):
            return base64.urlsafe_b64encode(json.dumps(value, separators=(',', ':')).encode()).rstrip(b'=')
        publish = identity == 'publisher' if can_publish is None else bool(can_publish)
        claims = {'iss': 'local-test', 'sub': identity, 'nbf': int(time.time()) - 5, 'exp': int(time.time()) + 600, 'video': {'roomJoin': True, 'room': room, 'canPublish': publish, 'canPublishData': True, 'canSubscribe': True}}
        if admin:
            claims['video'] = {'roomAdmin': True, 'room': room}
        body = b'.'.join([enc({'alg': 'HS256', 'typ': 'JWT'}), enc(claims)])
        return (body + b'.' + base64.urlsafe_b64encode(hmac.new(self.secret.encode(), body, hashlib.sha256).digest()).rstrip(b'=')).decode()

    def client_env(self, room, mode='shared-key'):
        env = os.environ.copy()
        env['PATH'] = str(ROOT / '.deps/qt/6.8.3/msvc2022_64/bin') + os.pathsep + env['PATH']
        plugins = ROOT / '.deps/qt/6.8.3/msvc2022_64/plugins'
        env['QT_PLUGIN_PATH'] = str(plugins)
        env['QT_QPA_PLATFORM_PLUGIN_PATH'] = str(plugins / 'platforms')
        env['GOODSHARING_TEST_URL'] = 'ws://127.0.0.1:8765'
        env['GOODSHARING_LOCAL_LOOPBACK'] = '1'
        env['GOODSHARING_TEST_PUBLISHER_TOKEN'] = self.token('publisher', room)
        env['GOODSHARING_TEST_SUBSCRIBER_TOKEN'] = self.token('subscriber', room)
        env['GOODSHARING_TEST_ENCRYPTION'] = mode
        env['GOODSHARING_TEST_E2EE_KEY'] = secrets.token_hex(32)  # Client only.
        return env

    def participants(self, room):
        request = urllib.request.Request('http://127.0.0.1:8765/twirp/livekit.RoomService/ListParticipants', json.dumps({'room': room}).encode(), {'Content-Type': 'application/json', 'Authorization': 'Bearer ' + self.token('test-admin', room, admin=True)})
        with urllib.request.urlopen(request, timeout=2) as response:
            return json.load(response).get('participants', [])

    def stop(self):
        if self.server is not None and self.server.poll() is None:
            self.server.terminate()
            try:
                self.server.wait(timeout=10)
            except subprocess.TimeoutExpired:
                self.server.kill()
                self.server.wait(timeout=5)
        if self.log is not None:
            self.log.close()
        return self.server is not None and self.server.poll() is not None

