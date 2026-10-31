"""Measure only owned loopback media through a bounded UDP relay."""
import argparse
import hashlib
import json
import re
import subprocess
import socket
import ipaddress
import time
import sys
from local_media_fixture import LocalMediaFixture, ROOT
from loopback_relay import LoopbackRelay
from verify_recordings import verify_recordings
from startup_diagnostics import analyze_startup


def percentile(values, fraction=.95):
    values = sorted(values)
    return values[int((len(values)-1)*fraction)] if values else None


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--mode', choices=['baseline', 'pressure'], default='baseline')
    parser.add_argument('--ratio', type=float, default=.6)
    parser.add_argument('--cap-bps', type=int, help='Explicit total upload limit for repeatable pressure runs; otherwise use the measured baseline ratio')
    parser.add_argument('--queue-age-ms', type=int, default=1000, help='Owned relay FIFO age limit; default preserves the original bufferbloat case')
    parser.add_argument('--sdk-stall-ms', type=int, default=0, help='One bounded encoded-input stall per SDK encoder; baseline synthetic diagnostics only')
    parser.add_argument('--receiver-setup-stall-ms', type=int, default=0, help='Widen the receiver transform installation interval; baseline synthetic diagnostics only')
    parser.add_argument('--present-stall-ms', type=int, default=0, help='One bounded stall before each remote decoded-image delivery check; baseline synthetic diagnostics only')
    parser.add_argument('--record-encoded', action='store_true', help='Independently decode sender access units to distinguish source quality from transport damage')
    parser.add_argument('--audio-trace',action='store_true',help='Synthetic coded audio and bounded PCM trace; never captures user audio')
    parser.add_argument('--audio-analysis-python',default=sys.executable,help='Python with NumPy for optional offline audio trace analysis')
    args = parser.parse_args()
    if not .1 <= args.ratio <= .9:
        parser.error('Use a rate ratio between 0.1 and 0.9')
    if args.cap_bps is not None and (args.mode!='pressure' or not 100_000<=args.cap_bps<=20_000_000):
        parser.error('Use a 100,000 to 20,000,000 bps explicit cap in pressure mode')
    if not 10 <= args.queue_age_ms <= 2000:
        parser.error('Use a relay queue age of 10 to 2000 ms')
    if not 0 <= args.sdk_stall_ms <= 250 or (args.sdk_stall_ms and args.mode != 'baseline'):
        parser.error('Use a 0 to 250 ms SDK stall with baseline mode')
    if not 0<=args.receiver_setup_stall_ms<=250 or (args.receiver_setup_stall_ms and args.mode!='baseline'):
        parser.error('Use a 0 to 250 ms receiver setup stall with baseline mode')
    if not 0 <= args.present_stall_ms <= 250 or (args.present_stall_ms and args.mode != 'baseline'):
        parser.error('Use a 0 to 250 ms decoded-image delivery stall with baseline mode')
    folder = ROOT / 'artifacts' / (time.strftime('%Y%m%d-%H%M%S') + '-network-media')
    fixture = LocalMediaFixture(folder)
    relay = None
    snapshots = []
    summary = {'local_only': True, 'mode': args.mode, 'passed': False,
               'queue_age_ms': args.queue_age_ms,
               'cap_source': 'explicit' if args.cap_bps is not None else 'baseline_ratio',
               'requested_cap_bps': args.cap_bps,
               'sdk_stall_ms': args.sdk_stall_ms,
               'receiver_setup_stall_ms': args.receiver_setup_stall_ms,
               'present_stall_ms': args.present_stall_ms,
               'audio_trace_enabled':args.audio_trace,
               'actual_capture': False, 'global_network_rules_changed': False}
    try:
        local_ips = [entry[4][0] for entry in socket.getaddrinfo(socket.gethostname(), None, socket.AF_INET, socket.SOCK_DGRAM)]
        relay_address = next(ip for ip in local_ips if ipaddress.ip_address(ip).is_private and not ipaddress.ip_address(ip).is_loopback and not ipaddress.ip_address(ip).is_link_local)
        summary['relay_address'] = relay_address
        fixture.start(relay=True, relay_address=relay_address)
        relay = LoopbackRelay(front_host=relay_address, queue_age_ms=args.queue_age_ms)
        env = fixture.client_env('network-media')
        env.update(GOODSHARING_PUBLISHER_CASE='stereo', GOODSHARING_TEST_SECONDS='18',
                   GOODSHARING_TEST_NETWORK_TELEMETRY='1', GOODSHARING_TEST_SDK_STALL_MS=str(args.sdk_stall_ms),
                   GOODSHARING_TEST_RECEIVER_SETUP_STALL_MS=str(args.receiver_setup_stall_ms),
                   GOODSHARING_TEST_PRESENT_STALL_MS=str(args.present_stall_ms))
        diagnostics = folder / 'recordings'
        images=folder/'diagnostics'
        images.mkdir()
        env['GOODSHARING_TEST_DIAGNOSTICS']=str(images)
        if args.audio_trace:
            (folder/'audio-timing').mkdir()
            env['GOODSHARING_TEST_AUDIO_TRACE']=str(folder/'audio-timing')
        else:
            env.pop('GOODSHARING_TEST_AUDIO_TRACE',None)
        if args.record_encoded:
            diagnostics.mkdir()
            env['GOODSHARING_TEST_RECORD_ENCODED'] = str(diagnostics)
        executable = ROOT / 'build/bin/goodsharing_publisher_probe.exe'
        summary['executable_sha256'] = hashlib.sha256(executable.read_bytes()).hexdigest()
        snapshots = []
        report = folder / 'media.json'
        cap_at = restore_at = None
        with (folder / 'media.log').open('w') as output:
            with subprocess.Popen([str(executable), '--report', str(report)], env=env, stdout=output,
                                  stderr=subprocess.STDOUT, creationflags=subprocess.CREATE_NO_WINDOW) as child:
                began = time.monotonic()
                try:
                    while child.poll() is None:
                        elapsed = time.monotonic()-began
                        if elapsed > 60:
                            raise TimeoutError('Owned network fixture deadline exceeded')
                        sample = relay.snapshot()
                        if sample['error']:
                            raise RuntimeError(sample['error'])
                        snapshots.append(sample)
                        if args.mode == 'pressure' and cap_at is None and elapsed >= 6:
                            reference = min(snapshots, key=lambda s: abs(s['elapsed_s']-(sample['elapsed_s']-3)))
                            baseline_bps = (sample['up_sent_bytes']-reference['up_sent_bytes'])*8/(sample['elapsed_s']-reference['elapsed_s'])
                            cap_bps = args.cap_bps if args.cap_bps is not None else max(100_000, int(baseline_bps*args.ratio))
                            relay.configure(cap_bps)
                            cap_at = time.monotonic()
                            summary.update(baseline_bps=baseline_bps, restricted_bps=cap_bps,
                                           restricted_start_sent_bytes=sample['up_sent_bytes'])
                        if cap_at and restore_at is None and time.monotonic()-cap_at >= 4:
                            sent = relay.snapshot()['up_sent_bytes'] - summary['restricted_start_sent_bytes']
                            summary['restricted_sent_bytes'] = sent
                            summary['restricted_duration_s'] = time.monotonic()-cap_at
                            summary['rate_cap_verified'] = sent*8 <= summary['restricted_bps']*summary['restricted_duration_s']+1500*8
                            relay.configure(0)
                            restore_at = time.monotonic()
                        time.sleep(.1)
                finally:
                    if child.poll() is None:
                        child.kill()
                        child.wait(timeout=5)
        native = json.loads(report.read_text(encoding='utf-8')) if report.exists() else {}
        summary['preencoded_gap_warnings'] = (folder / 'media.log').read_text(errors='replace').count('GoodSharing preencoded input gap; awaiting fresh keyframe')
        native_log = (folder / 'media.log').read_text(errors='replace')
        startup=analyze_startup(native_log,args.receiver_setup_stall_ms)
        summary['receiver_startup']=startup
        summary['preencoded_chain_drops_logged'] = native_log.count('GoodSharing chain drop:')
        summary['congestion_pushback_policy_verified'] = 'GoodSharing congestion policy: window=1 pushback=1 drop_frame_only=0' in native_log and 'drop_frame_only=1' not in native_log
        summary['recovery_policy_verified'] = 'GoodSharing recovery policy: alr_probing=1 keyframe_flush=1 video_max_ms=60' in native_log
        summary['queue_policy_verified'] = 'GoodSharing queue policy: pacing_target_ms=100 sdk_input_age_ms=100' in native_log
        summary['sdk_input_age'] = [dict(stale_drops=int(stale), maximum_seen_us=int(seen), maximum_forward_us=int(forward)) for stale,seen,forward in re.findall(r'GoodSharing SDK age: stale_drops=(\d+) maximum_seen_us=(\d+) maximum_forward_us=(\d+)', native_log)]
        age_ok = len(summary['sdk_input_age']) >= 2 and all(s['maximum_forward_us'] <= 100000 for s in summary['sdk_input_age'])
        if args.sdk_stall_ms > 100:
            age_ok &= sum(s['stale_drops'] for s in summary['sdk_input_age']) >= 2
            age_ok &= native.get('before_stop', {}).get('submitted_audio_blocks', 0) >= 18*95 and native.get('before_stop', {}).get('input', {}).get('audio_high_water_ms', 9999) <= 100
        summary['sdk_input_age_passed'] = bool(age_ok)
        summary['decoded_age'] = [dict(stale_drops=int(stale), missing_packet_times=int(missing), maximum_seen_us=int(seen), maximum_forward_us=int(forward)) for stale,missing,seen,forward in re.findall(r'GoodSharing decoded age: stale_drops=(\d+) missing_packet_times=(\d+) maximum_seen_us=(\d+) maximum_forward_us=(\d+)', native_log)]
        decoded_age_ok = len(summary['decoded_age']) >= 2 and all(s['maximum_forward_us'] <= 100000 and s['missing_packet_times'] == 0 for s in summary['decoded_age'])
        if args.present_stall_ms > 100:
            decoded_age_ok &= sum(s['stale_drops'] for s in summary['decoded_age']) >= 2
        if max(args.present_stall_ms, args.sdk_stall_ms) > 100:
            decoded_age_ok &= native.get('before_stop', {}).get('submitted_audio_blocks', 0) >= 18*95 and native.get('before_stop', {}).get('input', {}).get('audio_high_water_ms', 9999) <= 100
        summary['decoded_age_passed'] = bool(decoded_age_ok)
        videos = native.get('receive', {}).get('videos', [])
        sender_route = native.get('sender_rtc', {}).get('selected_ice', [])
        receiver_route = native.get('receiver_rtc', {}).get('selected_ice', [])
        selected = sender_route + receiver_route
        route_samples = [s.get('transport', {}).get('selected_ice', []) for s in native.get('network_stats', [])]
        receiver_route_samples = [s.get('transport', {}).get('selected_ice', []) for s in native.get('receiver_network_stats', [])]
        def expected_route(route):
            return bool(route) and all(p.get('remote_address') == relay_address and p.get('protocol') == 'udp' and p.get('remote_port') == 8765 for p in route)
        route_verified = expected_route(sender_route) and expected_route(receiver_route) and bool(route_samples) and bool(receiver_route_samples) and all(expected_route(route) for route in route_samples + receiver_route_samples)
        media_ok = child.returncode == 0 and not native.get('error') and native.get('close_error') == ''
        media_ok &= len(videos) == 2 and all(v.get('frames', 0) > 0 and not v.get('marker_errors') and v.get('metadata_errors', 1) == 0 and v.get('dimension_errors', 1) == 0 for v in videos)
        media_ok &= native.get('before_stop', {}).get('state') == 1 and native.get('remaining_local_publications') == 0
        media_ok &= native.get('receive', {}).get('audio', {}).get('stereo_passed') is True
        per_quality = []
        latency_ok = True
        for video in videos:
            timings = video.get('timings', [])
            latest_sample = max((s['sample_us'] for s in native.get('network_stats', [])), default=0)
            baseline = [(t['received_us']-t['capture_us'])/1000 for t in timings if t['received_us']/1e6 < (cap_at or began+16)]
            pressure = [(t['received_us']-t['capture_us'])/1000 for t in timings if cap_at and cap_at <= t['received_us']/1e6 < (restore_at or float('inf'))]
            recovered = [(t['received_us']-t['capture_us'])/1000 for t in timings if restore_at and t['capture_us']/1e6 >= restore_at+2]
            fresh = [t for t in timings if restore_at and t['capture_us']/1e6 >= restore_at and (t['received_us']-t['capture_us'])/1000 <= 150]
            item = dict(height=video['height'], baseline_p95_ms=percentile(baseline), pressure_p95_ms=percentile(pressure),
                        pressure_max_ms=max(pressure) if pressure else None, recovered_p95_ms=percentile(recovered),
                        recovery_to_fresh_ms=(fresh[0]['received_us']/1e6-restore_at)*1000 if fresh else None)
            gaps = [(b['received_us']-a['received_us'])/1000 for a,b in zip(timings, timings[1:])]
            item.update(inspected_frames=len(timings), pressure_frames=len(pressure), recovered_frames=len(recovered),
                        maximum_inter_frame_gap_ms=max(gaps) if gaps else None,
                        tail_recent=bool(timings) and timings[-1]['capture_us'] >= latest_sample-500000)
            item['video_continues'] = item['tail_recent'] and bool(gaps) and max(gaps) <= 2000
            latency_ok &= item['baseline_p95_ms'] is not None and 0 < item['baseline_p95_ms'] <= 150
            if args.mode == 'baseline':
                latency_ok &= item['tail_recent']
            if args.mode == 'pressure':
                latency_ok &= bool(pressure) and percentile(pressure) <= 150 and bool(recovered) and percentile(recovered) <= 150
                latency_ok &= bool(fresh) and item['recovery_to_fresh_ms'] <= 2000
            per_quality.append(item)
        summary.update(media_correctness_passed=bool(media_ok), relay_path_verified=route_verified,
                       video_continues=bool(per_quality) and all(q['video_continues'] for q in per_quality),
                       relay_route_samples_verified=len(route_samples) if route_verified else 0,
                       relay_receiver_route_samples_verified=len(receiver_route_samples) if route_verified else 0,
                       latency_target_met=bool(latency_ok), qualities=per_quality,
                       cap_monotonic_s=cap_at, restore_monotonic_s=restore_at,
                       selected_ice=selected, report=str(report.relative_to(ROOT)))
        (folder / 'relay-samples.json').write_text(json.dumps(snapshots, indent=2), encoding='utf-8')
        participants = fixture.participants('network-media')
        deadline = time.monotonic()+3
        while participants and time.monotonic() < deadline:
            time.sleep(.1)
            participants = fixture.participants('network-media')
        summary['remaining_participants'] = len(participants)
        summary['executable_unchanged'] = summary['executable_sha256'] == hashlib.sha256(executable.read_bytes()).hexdigest()
        if args.record_encoded:
            summary['independent_decode'] = verify_recordings(diagnostics, env, 60)
        recordings_ok = not args.record_encoded or (len(summary['independent_decode']) == 2 and all(r['passed'] for r in summary['independent_decode']))
        audio_timing_ok=True
        if args.audio_trace:
            command=[args.audio_analysis_python,str(ROOT/'scripts/analyze_audio_timing.py'),str(folder/'audio-timing'),'--media-report',str(report)]
            if cap_at is not None:
                command+=['--cap-us',str(cap_at*1e6),'--restore-us',str(restore_at*1e6)]
            analysis_env=env.copy();analysis_env['OPENBLAS_NUM_THREADS']='1'
            analysis=subprocess.run(command,env=analysis_env,capture_output=True,text=True,timeout=60,creationflags=subprocess.CREATE_NO_WINDOW)
            (folder/'audio-timing/analysis.log').write_text(analysis.stdout+analysis.stderr,encoding='utf-8')
            if analysis.returncode:
                raise RuntimeError('Audio timing analysis failed; see audio-timing/analysis.log')
            audio_result=json.loads((folder/'audio-timing/analysis.json').read_text())
            summary['audio_timing']={key:audio_result[key] for key in ('measurement_valid','latency_target_met','phases','av_sync')}
            audio_timing_ok=audio_result['measurement_valid'] and audio_result['latency_target_met'] and audio_result['av_sync']['latency_target_met']
        summary['passed'] = bool(media_ok and route_verified and latency_ok and summary['video_continues'] and not participants and summary['executable_unchanged'] and recordings_ok and summary['congestion_pushback_policy_verified'] and summary['recovery_policy_verified'] and summary['queue_policy_verified'] and age_ok and decoded_age_ok and startup['closed_pipeline_order_verified'] and startup['setup_stall_verified'] and startup['decoder_warning_count']==0 and (args.mode == 'baseline' or summary.get('rate_cap_verified') is True))
        summary['passed'] &= audio_timing_ok
    except Exception as error:
        summary['error'] = str(error)
    finally:
        if folder.exists():
            (folder / 'relay-samples.json').write_text(json.dumps(snapshots, indent=2), encoding='utf-8')
        if relay:
            try:
                summary['relay'] = relay.close()
                summary['passed'] &= not summary['relay']['error']
            except Exception as error:
                summary['relay_cleanup_error'] = str(error)
                summary['passed'] = False
        summary['owned_server_stopped'] = fixture.stop()
        folder.mkdir(parents=True, exist_ok=True)
        (folder / 'summary.json').write_text(json.dumps(summary, indent=2), encoding='utf-8')
        print(json.dumps(summary), flush=True)
        print('Reports: ' + str(folder), flush=True)
    return 0 if summary['passed'] and summary['owned_server_stopped'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
