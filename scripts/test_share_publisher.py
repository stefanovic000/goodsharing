"""Encrypted publisher integration and failure cases using synthetic media only."""
import argparse
import hashlib
import json
import os
import subprocess
import threading
import time
from local_media_fixture import LocalMediaFixture, ROOT
from verify_recordings import verify_recordings
from process_footprint import ProcessFootprint


def blocked_tones(audio):
    return audio.get('max_100ms_tone_amplitude', 9999) < 200 and all(max(ch.get('440', 9999), ch.get('880', 9999)) < 40 for ch in audio.get('tones', []))


def main():
    parser = argparse.ArgumentParser()
    choices = ['stereo', 'audio-off', 'wrong-key', 'receiver-unconfigured', 'missing-handler', 'partial-failure', 'cancel-start', 'cancel-before-publish', 'revoke', 'revoke-in-output', 'disable-encryption']
    parser.add_argument('--case', choices=['all'] + choices, default='all')
    parser.add_argument('--seconds', type=int, default=5)
    parser.add_argument('--record-encoded', action='store_true')
    parser.add_argument('--clock-bootstrap', action='store_true', help='Use the opt-in 500 ms client/SFU report experiment with bounded synthetic timing traces')
    parser.add_argument('--profile-resources', action='store_true', help='Sample only the owned synthetic client and SFU; excludes this Python driver')
    parser.add_argument('--cpu-burst', type=int, default=0)
    parser.add_argument('--reader-stall-ms', type=int, default=0, help='Pause each synthetic video consumer once to verify bounded decoded queues')
    parser.add_argument('--owner-stall-ms', type=int, default=0, help='Pause only the synthetic video owner once; audio must keep draining')
    parser.add_argument('--encoded-stall-ms', type=int, default=0, help='Hold one synthetic encoded output before age checks; exercise fresh-keyframe recovery')
    parser.add_argument('--audio-worker-stall-ms', type=int, default=0, help='Pause the synthetic audio publisher once; verify bounded old-input drops and fresh recovery')
    args = parser.parse_args()
    if not 3 <= args.seconds <= 300 or not 0 <= args.cpu_burst <= 4:
        parser.error('Use 3 to 300 seconds and 0 to 4 owned load threads')
    if args.case == 'all' and args.seconds > 15:
        parser.error('Use a specific case for sustained checks')
    if args.clock_bootstrap and args.seconds > 20:
        parser.error('Use at most 20 seconds with the bounded clock experiment')
    if not 0 <= args.reader_stall_ms <= 250 or (args.reader_stall_ms and args.case not in ('stereo', 'audio-off')):
        parser.error('Use 0 to 250 ms reader stall with stereo or audio-off')
    if not 0 <= args.owner_stall_ms <= 250 or (args.owner_stall_ms and args.case != 'stereo'):
        parser.error('Use 0 to 250 ms owner stall with stereo')
    if not 0 <= args.encoded_stall_ms <= 250 or (args.encoded_stall_ms and args.case != 'stereo'):
        parser.error('Use 0 to 250 ms encoded output stall with stereo')
    if not 110 <= args.audio_worker_stall_ms <= 250 and args.audio_worker_stall_ms != 0:
        parser.error('Use 0 or 110 to 250 ms audio worker stall')
    if args.audio_worker_stall_ms and args.case not in ('stereo', 'wrong-key'):
        parser.error('Use an audio worker stall with stereo or wrong-key')
    folder = ROOT / 'artifacts' / (time.strftime('%Y%m%d-%H%M%S') + '-share-publisher')
    fixture = LocalMediaFixture(folder)
    results = []
    summary = {'local_only': True, 'actual_capture': False, 'configuration': vars(args), 'passed': False, 'runs': results}
    server_profile = None
    try:
        fixture.start(sfu_rtcp_report_ms=500 if args.clock_bootstrap else 0)
        summary['server_runtime'] = fixture.server_info
        if args.profile_resources:
            server_profile = ProcessFootprint(fixture.server, 'loopback forwarding SFU').start()
        for case in choices if args.case == 'all' else [args.case]:
            room = 'publisher-' + case
            env = fixture.client_env(room, 'wrong-key' if case == 'wrong-key' else 'shared-key')
            env['GOODSHARING_PUBLISHER_CASE'] = case
            env['GOODSHARING_TEST_SECONDS'] = str(args.seconds)
            env['GOODSHARING_TEST_READER_STALL_MS'] = str(args.reader_stall_ms)
            env['GOODSHARING_TEST_OWNER_STALL_MS'] = str(args.owner_stall_ms)
            env['GOODSHARING_TEST_ENCODED_STALL_MS'] = str(args.encoded_stall_ms)
            env['GOODSHARING_TEST_AUDIO_WORKER_STALL_MS'] = str(args.audio_worker_stall_ms)
            diagnostics = folder / (case + '-recordings')
            diagnostics.mkdir()
            env.pop('GOODSHARING_TEST_RTCP_REPORT_MS', None)
            if args.clock_bootstrap:
                trace = diagnostics / 'audio-timing'
                trace.mkdir()
                env.update(GOODSHARING_TEST_AUDIO_TRACE=str(trace), GOODSHARING_TEST_RTCP_REPORT_MS='500')
            env['GOODSHARING_TEST_DIAGNOSTICS'] = str(diagnostics)
            executable = ROOT / 'build/bin/goodsharing_publisher_probe.exe'
            executable_hash = hashlib.sha256(executable.read_bytes()).hexdigest()
            recorded = args.record_encoded and case in ('stereo', 'audio-off', 'wrong-key')
            if recorded:
                env['GOODSHARING_TEST_RECORD_ENCODED'] = str(diagnostics)
            report, log = folder / (case + '.json'), folder / (case + '.log')
            stop_load = threading.Event()

            def load():
                if stop_load.wait(15):
                    return
                block = b'G' * (8 * 1024 * 1024)
                until = time.monotonic() + 3
                while time.monotonic() < until and not stop_load.is_set():
                    hashlib.sha256(block).digest()

            workers = [threading.Thread(target=load) for _ in range(args.cpu_burst)]
            for worker in workers:
                worker.start()
            footprint = None
            try:
                with log.open('w') as output:
                    with subprocess.Popen([str(executable), '--report', str(report)], env=env, stdout=output, stderr=subprocess.STDOUT, creationflags=subprocess.CREATE_NO_WINDOW) as child:
                        try:
                            if args.profile_resources:
                                footprint = ProcessFootprint(child, 'synthetic sender plus receiver of BOTH qualities; no capture/render').start()
                            child.wait(timeout=args.seconds + 45)
                        finally:
                            if child.poll() is None:
                                child.kill()  # This fixture child only, including timeout/error paths.
                                child.wait(timeout=5)
                            if footprint:
                                (folder / (case + '-footprint.json')).write_text(json.dumps(footprint.stop(), indent=2), encoding='utf-8')
            finally:
                stop_load.set()
                for worker in workers:
                    worker.join(timeout=5)
            data = json.loads(report.read_text(encoding='utf-8')) if report.exists() else {}
            receive = data.get('receive', {})
            videos = receive.get('videos', [])
            audio = receive.get('audio', {})
            ready = data.get('ready', {})
            stopped = data.get('stopped', {})
            manifest = ready.get('gate', {}).get('tracks', [])
            events = data.get('sender_events', {})
            success = child.returncode == 0 and bool(data) and not data.get('error') and data.get('close_error') == '' and receive.get('reader_error') == ''
            if args.clock_bootstrap:
                success &= 'GoodSharing RTCP report experiment: interval_ms=500' in log.read_text(errors='replace')
                success &= 'GoodSharing SFU RTCP experiment: interval_ms=500' in (folder / 'server.log').read_text(errors='replace')
            success &= data.get('remaining_local_publications', 9999) == 0 and data.get('old_input_closed') is True and data.get('old_input_rejected') is True and data.get('stop_ms', 9999) < 1000
            success &= stopped.get('priming_buffer_capacity_bytes', -1) == 0
            success &= data.get('video_producer_error') == '' and data.get('audio_producer_error') == '' and events.get('missing_sid_events', 9999) == 0
            verification = receive.get('verification', {})
            success &= verification.get('verification_required') is True
            if args.owner_stall_ms:
                mailbox = data.get('before_stop', {}).get('input', {})
                success &= mailbox.get('replaced_raw_frames', 0) > 0 and mailbox.get('audio_high_water_ms', 9999) <= 100 and data.get('before_stop', {}).get('submitted_audio_blocks', 0) >= args.seconds * 95
            if args.reader_stall_ms:
                readers = verification.get('completed_readers', [])
                success &= len(readers) == 2 and all(r.get('frames_dropped', 0) > 0 and 0 < r.get('queue_high_water', 9999) <= r.get('queue_capacity', 0) and r.get('frames_read', 0) > 0 for r in readers)
            pressure_checks = {}
            if args.audio_worker_stall_ms:
                stall = data.get('audio_worker_stall', {})
                mailbox = data.get('before_stop', {}).get('input', {})
                pressure_checks = dict(stall_performed=stall.get('performed') is True,
                    requested_stall_observed=stall.get('end_us', 0)-stall.get('begin_us', 0) >= args.audio_worker_stall_ms*1000,
                    bounded_queue=mailbox.get('audio_high_water_ms', 9999) <= 100,
                    pressure_discarded_old_input=mailbox.get('overflow_audio_drops', 0)+mailbox.get('expired_audio_drops', 0) > 0,
                    held_expired_block_dropped=data.get('before_stop', {}).get('expired_audio', 0) > 0,
                    continued_after_stall=stall.get('successful_handoffs_after_stall', 0) >= 100,
                    fresh_recovery=0 <= stall.get('first_successful_resume_us', 0)-stall.get('end_us', 0) <= 200000,
                    unchanged_handoff_age=stall.get('maximum_handoff_start_age_us', 999999) <= 100000,
                    publication_remains_open=not data.get('before_stop', {}).get('gate', {}).get('closed', True))
                success &= all(pressure_checks.values())
            success &= all(not v.get('marker_errors') and v.get('metadata_errors', 1) == 0 and v.get('dimension_errors', 1) == 0 and v.get('nonblack_priming', 1) == 0 for v in videos)
            normal = case in ('stereo', 'audio-off', 'wrong-key', 'receiver-unconfigured', 'revoke', 'revoke-in-output', 'disable-encryption')
            if normal:
                expected = 2 if case == 'audio-off' else 3
                success &= ready.get('state') == 1 and len(manifest) == expected and all(t.get('ready') for t in manifest)
                success &= ready.get('priming_buffer_capacity_bytes', -1) == 0
                success &= {t['sid'] for t in manifest} <= set(events.get('ok_sids', []))
                success &= receive.get('all_encrypted') is True and len(videos) == 2 and {v.get('height') for v in videos} == {1080, 720}
                success &= data.get('video_admitted', 0) > 0 and data.get('real_video_handoffs', 0) > 0
                outputs = data.get('before_stop', {}).get('outputs', [])
                success &= len(outputs) == 2 and all(o.get('frames_encoded', 0) > 0 and o.get('frames_encoded') == o.get('frames_handed_off', -1)+o.get('stale_encoded_drops', 0)+o.get('dependent_recovery_drops', 0)+o.get('revoked_encoded_drops',0) and o.get('age_budget_ms') == 150 and o.get('maximum_handoff_start_age_ms', 9999) <= 150 for o in outputs)
                if case not in ('revoke','revoke-in-output','disable-encryption'):
                    success &= all(o.get('revoked_encoded_drops',0)==0 for o in outputs)
                if case=='revoke-in-output':
                    success &= sum(o.get('revoked_encoded_drops',0) for o in outputs)>=1
                if args.encoded_stall_ms > 150:
                    success &= sum(o.get('stale_encoded_drops', 0) for o in outputs) > 0 and sum(o.get('keyframe_recoveries', 0) for o in outputs) > 0 and all(not o.get('recovering', True) for o in outputs)
                    success &= data.get('before_stop', {}).get('submitted_audio_blocks', 0) >= args.seconds*95 and data.get('before_stop', {}).get('input', {}).get('audio_high_water_ms', 9999) <= 100
                if case not in ('revoke', 'revoke-in-output', 'disable-encryption'):
                    success &= all(o.get('pending_frames', 9999) == 0 for o in outputs)
                if case in ('wrong-key','receiver-unconfigured'):
                    success &= verification.get('verified_tracks') == 0
                    success &= audio.get('frames') == 0 and all(v.get('priming_frames') == 0 for v in videos)
                    success &= all(v.get('frames') == 0 for v in videos) and receive.get('audio_tracks') == 1 and data.get('audio_admitted', 0) > 0 and blocked_tones(audio)
                    if case=='wrong-key':
                        success &= receive.get('decryption_failures', 0)>0
                    else:
                        success &= receive.get('decryption_failures', -1)==0
                else:
                    success &= verification.get('verified_tracks') == expected
                    success &= {t['sid'] for t in manifest} == {t['sid'] for t in verification.get('tracks', []) if t.get('verified') and t.get('owner') == 'publisher'}
                    success &= all(v.get('frames', 0) > 0 and 0 < v.get('p95_ms', 9999) <= 150 for v in videos)
                    if case == 'audio-off':
                        success &= receive.get('audio_tracks') == 0 and data.get('audio_admitted') == 0
                    else:
                        success &= receive.get('audio_tracks') == 1 and receive.get('sample_rate') == 48000 and audio.get('stereo_passed') is True
                success &= stopped.get('state') == (4 if case in ('revoke', 'revoke-in-output', 'disable-encryption') else 3)
                if case in ('revoke', 'revoke-in-output', 'disable-encryption'):
                    success &= data.get('handoffs_after_revocation') == 0 and stopped.get('gate', {}).get('failed') is True
                if case == 'disable-encryption':
                    success &= data.get('disable_performed') is True and data.get('manager_disabled_observed') is True and data.get('reenable_did_not_resume_share') is True and data.get('before_stop', {}).get('state') == 4
                elif args.seconds >= 300 and case not in ('wrong-key', 'revoke', 'revoke-in-output'):
                    success &= all(v.get('fps', 0) >= 59 for v in videos)
            else:
                success &= data.get('video_admitted') == 0 and data.get('audio_admitted') == 0 and data.get('real_video_handoffs') == 0
                success &= all(v.get('frames') == 0 for v in videos) and blocked_tones(audio)
                success &= stopped.get('state') == (4 if case in ('missing-handler', 'partial-failure') else 3)
                if case == 'missing-handler':
                    success &= len(manifest) == 3 and sum(bool(t.get('ready')) for t in manifest) == 2 and len(events.get('ok_sids', [])) == 2
                else:
                    success &= data.get('published_before_cleanup') == (0 if case == 'cancel-before-publish' else 1)
            text = log.read_text(errors='replace')
            log_error = any(message in text for message in ('failed to negotiate', 'wrong state: closed', 'panicked at', 'peer connection is closed, cannot create offer', 'Attempted to drop unknown FFI handle'))
            success &= not log_error
            deadline = time.monotonic() + 3
            participants = fixture.participants(room)
            while participants and time.monotonic() < deadline:
                time.sleep(.1)
                participants = fixture.participants(room)
            success &= not participants
            check = {'case': case, 'passed': bool(success), 'report': str(report.relative_to(ROOT)), 'remaining_participants': len(participants), 'lifecycle_log_error': log_error, 'decoded_fps': [v.get('fps') for v in videos], 'stop_ms': data.get('stop_ms')}
            check['executable_sha256'] = executable_hash
            if pressure_checks:
                check['audio_pressure_checks'] = pressure_checks
            check['executable_unchanged'] = executable_hash == hashlib.sha256(executable.read_bytes()).hexdigest()
            check['passed'] &= check['executable_unchanged']
            if footprint:
                profile = footprint.report()
                check['footprint'] = str((folder / (case + '-footprint.json')).relative_to(ROOT))
                check['resource_sampling_passed'] = not profile['errors'] and profile['sampled_lifetime'] is not None
                check['passed'] &= check['resource_sampling_passed']
            if recorded:
                independent = verify_recordings(diagnostics, env, max(60, args.seconds))
                check['independent_decode'] = independent
                check['passed'] &= len(independent) == 2 and all(r['passed'] for r in independent)
            results.append(check)
            print(json.dumps(check), flush=True)
        summary['passed'] = bool(results) and all(r['passed'] for r in results)
    except Exception as error:
        summary['error'] = str(error)
    finally:
        if server_profile:
            (folder / 'sfu-footprint.json').write_text(json.dumps(server_profile.stop(), indent=2), encoding='utf-8')
        summary['owned_server_stopped'] = fixture.stop()
        folder.mkdir(parents=True, exist_ok=True)
        (folder / 'summary.json').write_text(json.dumps(summary, indent=2), encoding='utf-8')
        print('Reports: ' + str(folder), flush=True)
    return 0 if summary['passed'] and summary['owned_server_stopped'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
