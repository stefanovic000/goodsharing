"""Bounded native-media fixture. Only processes started here are stopped."""
import argparse
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
import threading
from verify_livekit_sdk import runtime_info
from verify_recordings import verify_recordings

ROOT = Path(__file__).resolve().parents[1]


def token(secret, identity, room):
    def enc(value):
        return base64.urlsafe_b64encode(json.dumps(value, separators=(",", ":")).encode()).rstrip(b"=")
    body = b".".join([enc({"alg": "HS256", "typ": "JWT"}), enc({
        "iss": "local-test", "sub": identity, "nbf": int(time.time()) - 5,
        "exp": int(time.time()) + 600,
        "video": {"roomJoin": True, "room": room, "canPublish": identity == "publisher", "canSubscribe": True},
    })])
    return (body + b"." + base64.urlsafe_b64encode(hmac.new(secret.encode(), body, hashlib.sha256).digest()).rstrip(b"=")).decode()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--seconds", type=int, default=15)
    parser.add_argument("--encryption", choices=["off", "shared-key", "wrong-key", "all"], default="all")
    parser.add_argument("--encoder", choices=["sdk", "mf"], default="mf")
    parser.add_argument("--marker", choices=["sequential", "stress"], default="sequential")
    parser.add_argument("--local-decode", action="store_true", help="Decode and verify hardware output before RTP/E2EE for diagnosis")
    parser.add_argument("--bitrate-step-at", type=int, default=-1, help="Diagnostic: force a two-second 1 Mbps bitrate step at this source frame")
    parser.add_argument("--stall-ms", type=int, default=0, help="Pause only the test video producer once to simulate a busy capture thread")
    parser.add_argument("--stall-at", type=int, default=1000)
    parser.add_argument("--record-encoded", action="store_true", help="Save only synthetic encoded access units and independently decode them after the live run")
    parser.add_argument("--cpu-burst", type=int, default=0, help="Use 0 to 4 owned hashing threads for a bounded three-second diagnostic load burst")
    parser.add_argument("--burst-frames", type=int, default=0, help="Deliberately overproduce up to 120 test frames at --stall-at to reproduce encode-delivery backlog")
    parser.add_argument("--tamper-frame-id-at", type=int, default=-1, help="Negative control: deliberately send a wrong metadata frame ID; normal verification must fail")
    parser.add_argument("--unsafe-timestamp", action="store_true", help="Probe metadata byte framing with a timestamp whose inverted bytes contain an H.264 start code")
    parser.add_argument("--unsafe-timestamp-at", type=int, default=-1, help="Apply the framing probe to one marker ID, or -1 for every frame")
    parser.add_argument("--metadata-codec", choices=["safe", "legacy"], default="safe", help="Use the fixed framing by default; legacy is only for reproducing the original failure")
    parser.add_argument("--legacy-pacing", action="store_true", help="Diagnostic only: restore unbounded encoded bursts and the original deadline catch-up loop")
    args = parser.parse_args()
    if not 3 <= args.seconds <= 300:
        parser.error("Use a bounded duration of 3 to 300 seconds")
    if not 0 <= args.stall_ms <= 1000 or args.stall_at < 0:
        parser.error("Use a stall of 0 to 1000 ms and a non-negative source frame")
    if not 0 <= args.cpu_burst <= 4:
        parser.error("Use 0 to 4 diagnostic load threads")
    if not 0 <= args.burst_frames <= 120:
        parser.error("Use a bounded overproduction burst of 0 to 120 frames")
    if args.tamper_frame_id_at < -1:
        parser.error("Use -1 to disable deliberate metadata corruption or a non-negative marker ID")
    # Never displace an existing listener, even on our intended development port.
    for kind in (socket.SOCK_STREAM, socket.SOCK_DGRAM):
        with socket.socket(socket.AF_INET, kind) as check:
            check.bind(("127.0.0.1", 8765))
    folder = ROOT / "artifacts" / (time.strftime("%Y%m%d-%H%M%S") + "-local-media")
    folder.mkdir(parents=True)
    (folder / 'runtime-info.json').write_text(json.dumps(runtime_info(), indent=2), encoding='utf-8')
    api_secret = secrets.token_hex(32)
    server_env = os.environ.copy()
    server_env["LIVEKIT_KEYS"] = "local-test: " + api_secret
    config = """port: 8765
bind_addresses: [127.0.0.1]
rtc:
  tcp_port: 0
  udp_port: 8765
  node_ip: 127.0.0.1
  enable_loopback_candidate: true
  use_external_ip: false
logging:
  level: warn
"""
    config_path = folder / "server.yaml"
    config_path.write_text(config)
    summary = {"scenario": "one Windows publisher, two exact video tracks, one shared-audio track, one subscriber, loopback SFU", "internet_test": False, "encoder_mode": args.encoder, "seconds_per_run": args.seconds, "configuration": vars(args), "runs": []}
    server = None
    try:
        with (folder / "server.log").open("w") as log:
            server = subprocess.Popen([str(ROOT / ".deps/livekit-server-1.13.10/livekit-server.exe"), "--config", str(config_path)], env=server_env, stdout=log, stderr=subprocess.STDOUT, creationflags=subprocess.CREATE_NO_WINDOW)
            deadline = time.monotonic() + 15
            while True:
                if server.poll() is not None:
                    raise RuntimeError("Local server exited; inspect server.log")
                try:
                    with urllib.request.urlopen("http://127.0.0.1:8765", timeout=1) as response:
                        if response.status == 200:
                            break
                except OSError:
                    if time.monotonic() > deadline:
                        raise RuntimeError("Local server readiness timed out")
                    time.sleep(.2)
            modes = ["off", "shared-key", "wrong-key"] if args.encryption == "all" else [args.encryption]
            for mode in modes:
                client_env = os.environ.copy()
                client_env["PATH"] = str(ROOT / ".deps/qt/6.8.3/msvc2022_64/bin") + os.pathsep + client_env["PATH"]
                client_env["GOODSHARING_TEST_URL"] = "ws://127.0.0.1:8765"
                client_env["GOODSHARING_TEST_PUBLISHER_TOKEN"] = token(api_secret, "publisher", mode)
                client_env["GOODSHARING_TEST_SUBSCRIBER_TOKEN"] = token(api_secret, "subscriber", mode)
                # This key goes to the native clients only, never the SFU process/config/JWT.
                client_env["GOODSHARING_TEST_E2EE_KEY"] = secrets.token_hex(32)
                client_env["GOODSHARING_TEST_ENCRYPTION"] = mode
                client_env["GOODSHARING_TEST_SECONDS"] = str(args.seconds)
                client_env["GOODSHARING_TEST_ENCODER"] = args.encoder
                client_env["GOODSHARING_TEST_MARKER"] = args.marker
                client_env["GOODSHARING_TEST_LOCAL_DECODE"] = "1" if args.local_decode else "0"
                client_env["GOODSHARING_TEST_BITRATE_STEP_AT"] = str(args.bitrate_step_at)
                client_env["GOODSHARING_TEST_STALL_MS"] = str(args.stall_ms)
                client_env["GOODSHARING_TEST_STALL_AT"] = str(args.stall_at)
                client_env["GOODSHARING_TEST_BURST_FRAMES"] = str(args.burst_frames)
                client_env["GOODSHARING_TEST_TAMPER_AT"] = str(args.tamper_frame_id_at)
                client_env["GOODSHARING_TEST_UNSAFE_TIMESTAMP"] = "1" if args.unsafe_timestamp else "0"
                client_env["GOODSHARING_TEST_UNSAFE_TIMESTAMP_AT"] = str(args.unsafe_timestamp_at)
                client_env["GOODSHARING_TEST_METADATA_CODEC"] = args.metadata_codec
                client_env["GOODSHARING_TEST_LEGACY_PACING"] = "1" if args.legacy_pacing else "0"
                diagnostics = folder / (mode + "-diagnostics")
                diagnostics.mkdir()
                client_env["GOODSHARING_TEST_DIAGNOSTICS"] = str(diagnostics)
                if args.record_encoded:
                    client_env["GOODSHARING_TEST_RECORD_ENCODED"] = str(diagnostics)
                report = folder / (mode + ".json")
                load_stop = threading.Event()
                def load_burst():
                    if load_stop.wait(15):
                        return
                    block = b"G" * (8 * 1024 * 1024)
                    until = time.monotonic() + 3
                    while time.monotonic() < until and not load_stop.is_set():
                        hashlib.sha256(block).digest()
                load_threads = [threading.Thread(target=load_burst) for _ in range(args.cpu_burst)]
                for thread in load_threads:
                    thread.start()
                try:
                    with (folder / (mode + ".log")).open("w") as output:
                        completed = subprocess.run([str(ROOT / "build/bin/goodsharing_probe.exe"), "--media", "--report", str(report)], env=client_env, stdout=output, stderr=subprocess.STDOUT, timeout=args.seconds + 60, creationflags=subprocess.CREATE_NO_WINDOW)
                finally:
                    load_stop.set()
                    for thread in load_threads:
                        thread.join(timeout=5)
                result = json.loads(report.read_text()) if report.exists() else {"error": "Native probe produced no report"}
                tracks = result.get("media", {}).get("inbound", [])
                outbound = result.get("media", {}).get("outbound", [])
                decoded = len(tracks) == 2 and all(t["frames"] > 0 and t.get("pixel_markers",0)==t["frames"] for t in tracks)
                sizes = {(t["width"], t["height"]) for t in tracks}
                exact = sizes == {(1920, 1080), (1280, 720)}
                encryption_marked = len(tracks) == 2 and all(t["encrypted_publication"] for t in tracks)
                blocked = len(tracks) == 2 and all(t["frames"] == 0 for t in tracks) and sum(t["frames_encoded"] for t in outbound) > 0
                audio = result.get("media", {}).get("audio", {})
                # WebRTC can emit concealment/comfort noise when decryption fails.
                # Require the known transmitted tone to be absent, plus an SDK
                # decryption failure; PCM callback count alone is not meaningful.
                audio_passed = audio.get("producer_error") == "" and (audio.get("tone_440_amplitude", 9999) < 40 and audio.get("max_100ms_tone_440_amplitude", 9999) < 200 and audio.get("encrypted_publication") and result.get("media", {}).get("decryption_failures", 0) > 0 if mode == "wrong-key" else audio.get("tone_440_amplitude", 0) > 200 and audio.get("sample_rate") == 48000 and (mode == "off" or audio.get("encrypted_publication")))
                encryption_ready = mode == "off" or result.get("media", {}).get("publisher_encryption_ok_events", 0) >= 3
                passed = completed.returncode == 0 and not result.get("error") and encryption_ready and audio_passed and ((blocked and encryption_marked) if mode == "wrong-key" else (decoded and exact and (mode == "off" or encryption_marked)))
                run = {"mode": mode, "exit_code": completed.returncode, "media_correctness_passed": passed, "audio_correctness_passed": audio_passed, "report": str(report.relative_to(ROOT)), "decoded_fps": [t["decoded_fps"] for t in tracks], "encoders": [t["encoder"] for t in outbound]}
                run["exploratory_59_fps_target_met"] = None if mode == "wrong-key" else len(tracks) == 2 and all(t["frames"] > 0 and t["decoded_fps"] >= 59 for t in tracks)
                run["five_minute_rate_gate_passed"] = None if mode == "wrong-key" or args.seconds < 300 else run["exploratory_59_fps_target_met"]
                run["five_minute_media_gate_passed"] = None if mode == "wrong-key" or args.seconds < 300 else passed and run["exploratory_59_fps_target_met"]
                run["external_hardware_encoders"] = result.get("media", {}).get("external_hardware_encoders", [])
                run["encoded_handoffs_preserved"] = None if args.encoder != "mf" else len(run["external_hardware_encoders"]) == 2 and all(e["frames_encoded"] > 0 and e["frames_encoded"] == e.get("frames_handed_off") for e in run["external_hardware_encoders"])
                run["latency_target_met"] = None if mode == "wrong-key" or args.unsafe_timestamp else len(tracks) == 2 and all(0 < t.get("capture_to_decoded_p95_ms", 0) <= 150 for t in tracks)
                if run["encoded_handoffs_preserved"] is False:
                    run["media_correctness_passed"] = False
                if args.seconds >= 300 and mode != "wrong-key":
                    run["five_minute_media_gate_passed"] = run["media_correctness_passed"] and run["five_minute_rate_gate_passed"] and run["latency_target_met"] is not False
                if args.record_encoded:
                    # Offline analysis scales with the amount of retained media;
                    # it does not change the live rate/latency/correctness gates.
                    checks = verify_recordings(diagnostics, client_env, max(60, args.seconds))
                    run["independent_decode"] = checks
                    run["independent_decode_passed"] = len(checks) == 2 and all(c["passed"] for c in checks)
                    if not run["independent_decode_passed"]:
                        run["media_correctness_passed"] = False
                        if args.seconds >= 300 and mode != "wrong-key":
                            run["five_minute_media_gate_passed"] = False
                summary["runs"].append(run)
                print(json.dumps(run), flush=True)
    finally:
        if server is not None and server.poll() is None:
            server.terminate()
            try:
                server.wait(timeout=10)
            except subprocess.TimeoutExpired:
                server.kill()
                server.wait(timeout=5)
        summary["owned_server_stopped"] = server is not None and server.poll() is not None
        (folder / "summary.json").write_text(json.dumps(summary, indent=2))
        print("Reports: " + str(folder), flush=True)
    return 0 if summary["runs"] and all(r["media_correctness_passed"] and r["five_minute_media_gate_passed"] is not False for r in summary["runs"]) else 1


if __name__ == "__main__":
    raise SystemExit(main())
