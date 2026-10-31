"""Bounded synthetic media-clock experiment; no desktop/audio capture or playback."""
import argparse
import hashlib
import json
import re
from pathlib import Path
import subprocess
import time
from local_media_fixture import LocalMediaFixture, ROOT
from analyze_sender_audio_clock import analyze_sender_clock
from verify_livekit_sdk import runtime_info


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--seconds', type=int, default=18)
    parser.add_argument('--clock', choices=['local', 'rtcp'], default='local', help='Candidate clock gate; keeps the preceding local-clock diagnostic separate')
    parser.add_argument('--audio-delay-ms', type=int, default=0, help='Owned synthetic producer holds original PCM for 0 to 70 ms before submission')
    parser.add_argument('--rtcp-report-ms', type=int, choices=[0, 500], default=0, help='Synthetic-only client report experiment; 0 preserves upstream defaults')
    parser.add_argument('--sfu-rtcp-report-ms', type=int, choices=[0, 500], default=0, help='Use the separately built local SFU report experiment; 0 keeps the release SFU')
    parser.add_argument('--clock-server', action='store_true', help='Use the same rebuilt SFU even with its default report interval, for a controlled comparison')
    parser.add_argument('--source-capture-clock', action='store_true', help='Legacy diagnostic for untimed synthetic submissions; normal content now supplies its original source timestamp')
    parser.add_argument('--analysis-python', default=r'C:\Users\Hello\.cache\codex-runtimes\codex-primary-runtime\dependencies\python\python.exe')
    args = parser.parse_args()
    if not 5 <= args.seconds <= 20:
        parser.error('Use 5 to 20 seconds for the bounded PCM trace')
    if not 0 <= args.audio_delay_ms <= 70: parser.error('Use 0 to 70 ms owned synthetic source delay')
    folder = ROOT / 'artifacts' / (time.strftime('%Y%m%d-%H%M%S') + '-media-clock')
    fixture = LocalMediaFixture(folder)
    result = {'passed': False, 'synthetic_only': True, 'local_only': True,
              'synchronized_playback_verified': False, 'unrelated_processes_stopped': False}
    result.update(candidate_clock=args.clock, source_audio_delay_ms=args.audio_delay_ms,
                  client_rtcp_report_ms=args.rtcp_report_ms, sfu_rtcp_report_ms=args.sfu_rtcp_report_ms,
                  clock_server=args.clock_server or bool(args.sfu_rtcp_report_ms))
    child = None
    try:
        fixture.start(sfu_rtcp_report_ms=args.sfu_rtcp_report_ms, clock_server=args.clock_server)
        result['server_runtime'] = fixture.server_info
        env = fixture.client_env('media-clock')
        trace = folder / 'audio-timing'; trace.mkdir()
        env.update(GOODSHARING_PUBLISHER_CASE='stereo', GOODSHARING_TEST_SECONDS=str(args.seconds),
                   GOODSHARING_TEST_AUDIO_DELAY_MS=str(args.audio_delay_ms),
                   GOODSHARING_TEST_NETWORK_TELEMETRY='1', GOODSHARING_TEST_AUDIO_TRACE=str(trace))
        env.pop('GOODSHARING_TEST_RTCP_REPORT_MS', None)
        if args.rtcp_report_ms: env['GOODSHARING_TEST_RTCP_REPORT_MS'] = str(args.rtcp_report_ms)
        if args.source_capture_clock: env['GOODSHARING_TEST_AUDIO_CAPTURE_CLOCK'] = '1'
        else: env.pop('GOODSHARING_TEST_AUDIO_CAPTURE_CLOCK', None)
        result['synthetic_source_capture_clock'] = args.source_capture_clock
        executable = ROOT / 'build/bin/goodsharing_publisher_probe.exe'
        result['probe_sha256'] = hashlib.sha256(executable.read_bytes()).hexdigest()
        result['runtime'] = runtime_info()
        with (folder / 'media.log').open('w') as log:
            child = subprocess.Popen([str(executable), '--report', str(folder / 'media.json')],
                                     env=env, stdout=log, stderr=subprocess.STDOUT,
                                     creationflags=subprocess.CREATE_NO_WINDOW)
            child.wait(timeout=args.seconds + 30)
        media = json.loads((folder / 'media.json').read_text())
        text = (folder / 'media.log').read_text(errors='replace')
        result['client_rtcp_experiment_observed'] = 'GoodSharing RTCP report experiment: interval_ms=500' in text
        if result['client_rtcp_experiment_observed'] != bool(args.rtcp_report_ms):
            raise RuntimeError('Native RTCP report experiment did not match the requested configuration')
        result['sfu_rtcp_experiment_observed'] = 'GoodSharing SFU RTCP experiment: interval_ms=500' in (folder / 'server.log').read_text(errors='replace')
        if result['sfu_rtcp_experiment_observed'] != bool(args.sfu_rtcp_report_ms):
            raise RuntimeError('SFU RTCP report experiment did not match the requested configuration')
        result['sender_capture_clock'] = analyze_sender_clock(text, json.loads((trace / 'timing.json').read_text()))
        result['native_video_clock'] = [dict(sequence=int(sequence), rtp=int(rtp), timestamp_us=int(stamp), ntp_ms=int(ntp), sink_us=int(sink))
            for sequence, rtp, stamp, ntp, sink in re.findall(r'GoodSharing native video clock: sequence=(\d+) rtp=(\d+) timestamp_us=(-?\d+) ntp_ms=(-?\d+) sink_us=(-?\d+)', text)]
        with (folder / 'analysis.log').open('w') as log:
            analysis = subprocess.run([args.analysis_python, str(ROOT / 'scripts/analyze_media_clock.py'),
                                       '--folder', str(trace), '--media', str(folder / 'media.json'),
                                       '--output', str(folder / 'clock.json')], stdout=log,
                                      stderr=subprocess.STDOUT, timeout=30)
        clock = json.loads((folder / 'clock.json').read_text())
        with (folder / 'rtcp-analysis.log').open('w') as log:
            rtcp_analysis = subprocess.run([args.analysis_python, str(ROOT / 'scripts/analyze_rtcp_clock.py'),
                '--folder', str(trace), '--media', str(folder / 'media.json'), '--output', str(folder / 'rtcp-clock.json')],
                stdout=log, stderr=subprocess.STDOUT, timeout=30)
        result['rtcp_clock'] = json.loads((folder / 'rtcp-clock.json').read_text())
        result['rtcp_analysis_exit'] = rtcp_analysis.returncode
        receive = media.get('receive', {})
        videos = receive.get('videos', [])
        media_passed = bool(child.returncode == 0 and not media.get('error') and media.get('close_error') == '' and
                            media.get('before_stop', {}).get('state') == 1 and media.get('remaining_local_publications') == 0 and
                            receive.get('audio', {}).get('stereo_passed') and len(videos) == 2 and
                            all(v.get('frames', 0) > 100 and v.get('metadata_errors', 1) == 0 and
                                v.get('dimension_errors', 1) == 0 and not v.get('marker_errors') for v in videos))
        result.update(native_exit=child.returncode, analysis_exit=analysis.returncode, clock=clock,
                      media_passed=media_passed, remaining_participants=len(fixture.participants('media-clock')))
        candidate_passed = (analysis.returncode == 0 and clock['clock_mapping_usable']) if args.clock == 'local' else (
            rtcp_analysis.returncode == 0 and result['rtcp_clock']['rtcp_clock_mapping_usable'] and
            result['rtcp_clock']['audio_content_match_latency_passed'])
        result['passed'] = bool(media_passed and result['sender_capture_clock']['passed'] and candidate_passed and not result['remaining_participants'])
    except Exception as error:
        result['error'] = str(error)
    finally:
        if child is not None and child.poll() is None:
            child.terminate()
            try: child.wait(timeout=5)
            except subprocess.TimeoutExpired: child.kill(); child.wait(timeout=5)
        result['owned_client_stopped'] = child is not None and child.poll() is not None
        result['owned_server_stopped'] = fixture.stop()
        result['passed'] &= result['owned_client_stopped'] and result['owned_server_stopped']
        folder.mkdir(exist_ok=True)
        (folder / 'summary.json').write_text(json.dumps(result, indent=2), encoding='utf-8')
    print(json.dumps({key: result[key] for key in ('passed', 'candidate_clock', 'source_audio_delay_ms', 'client_rtcp_report_ms', 'sfu_rtcp_report_ms', 'native_exit', 'analysis_exit', 'rtcp_analysis_exit', 'media_passed', 'error') if key in result}))
    print('Reports: ' + str(folder))
    return 0 if result['passed'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
