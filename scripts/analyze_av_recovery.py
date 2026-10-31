"""Summarize bounded local recovery traces; never rewrite original results."""
import argparse
import hashlib
import json
from pathlib import Path


def classify_fault(event):
    """Describe retained evidence without assigning unobserved upstream causes."""
    if not event:
        return 'no_recorded_fault'
    if event.get('kind') == 4:
        return 'endpoint_discontinuity'
    if event.get('kind') == 5:
        return 'presented_video_expired'
    if event.get('sustained_headroom_reset'):
        return 'sustained_audio_headroom_loss'
    if event.get('clock_unavailable'):
        return 'source_clock_unavailable'
    if event.get('kind') == 1 and event.get('last_video_arrival_us', 0) > 0 and event['receiver_us']-event['last_video_arrival_us'] > 150000:
        return 'video_ingress_expired_at_audio'
    if event.get('kind') == 2 and event.get('was_anchored') and event.get('last_video_arrival_us', 0) > 0 and event['receiver_us']-event['last_video_arrival_us'] > 150000:
        return 'video_arrival_gap_with_clock_reanchor'
    if event.get('output_window_overlaps'):
        return 'overlapping_output_window'
    if event.get('output_window_already_due'):
        return 'expired_output_window'
    return 'unclassified_timing_reset'


def analyze(summary):
    suite = json.loads(summary.read_text(encoding='utf-8'))
    rows = []
    for run in suite['runs']:
        folder = summary.parent / run['case']
        view = run.get('viewer', run.get('diagnostic_viewer', {}))
        playback = view.get('after_stop', {}).get('last_selection', {}).get('playback') or view.get('playback') or view.get('last_selection', {}).get('playback') or {}
        audio = playback.get('audio', {})
        events = audio.get('test_reset_trace', [])
        first_fault = audio.get('test_first_playback_fault', {})
        first_event = first_fault.get('event', {})
        preceding = first_fault.get('preceding', [])
        requirements = []
        for event in events:
            if (event.get('kind') == 1 and event.get('was_anchored') and event.get('has_rtp') and
                    event.get('reports', 0) >= 2 and event.get('previous_video_sender_us', 0) > 0):
                requirements.append((event['receiver_us'] - event['last_video_arrival_us'] + 20000 +
                                     event['previous_video_sender_us'] - event['sender_us']) / 1000)
        last, delays, snapshots = {}, [], 0
        viewer_events, sender_events = [], []
        path = folder / 'viewer.log'
        for line in path.read_text(encoding='utf-8-sig').splitlines() if path.exists() else []:
            if line.startswith('GoodSharing test room event: '):
                viewer_events.append(json.loads(line[len('GoodSharing test room event: '):]))
            prefix = 'GoodSharing test track stats: '
            if not line.startswith(prefix):
                continue
            snapshot = json.loads(line[len(prefix):]);snapshots += 1
            for track in snapshot['tracks']:
                if track['kind'] != 'audio':
                    continue
                old = last.get(track['ssrc']);last[track['ssrc']] = track
                if not old or track['emitted'] <= old['emitted']:
                    continue
                count = track['emitted'] - old['emitted']
                values = {key: 1000 * (track[key] - old[key]) / count for key in
                          ('jitter_buffer_seconds', 'jitter_target_seconds', 'jitter_minimum_seconds')}
                if all(value >= 0 for value in values.values()):
                    delays.append(dict(receiver_us=snapshot['receiver_us'], actual_ms=values['jitter_buffer_seconds'],
                                       target_ms=values['jitter_target_seconds'], minimum_ms=values['jitter_minimum_seconds']))
        rates = []
        path = folder / 'sender.log'
        for line in path.read_text(encoding='utf-8-sig').splitlines() if path.exists() else []:
            if line.startswith('GoodSharing test room event: '):
                sender_events.append(json.loads(line[len('GoodSharing test room event: '):]))
            prefix = 'GoodSharing test sender rates: '
            if line.startswith(prefix):
                sample = json.loads(line[len(prefix):])
                rates.append(dict(local_us=sample['local_us'], input=sample.get('input', {}),
                    state=sample.get('state'), submitted_audio_blocks=sample.get('submitted_audio_blocks'),
                    submitted_video_pairs=sample.get('submitted_video_pairs'), outputs=[
                    {key: out.get(key) for key in ('height', 'target_bitrate_bps', 'configured_encoder_bitrate_bps', 'frames_handed_off')}
                    for out in sample['outputs']]))
        rows.append(dict(case=run['case'], original_passed=run['passed'],
            failed_checks=[key for key, value in run.get('checks', {}).items() if not value],
            sampled_reset_serial=audio.get('reset_serial'), alignment_rejections=audio.get('common_clock_alignment_rejected'),
            early_video_drops=audio.get('early_video_drops'),
            sustained_headroom_resets=audio.get('sustained_low_headroom_resets'),
            traced_events=len(events), traced_alignment_rejections=sum(bool(e.get('alignment_rejected')) for e in events),
            traced_rejections_preserving_serial=sum(bool(e.get('alignment_rejected')) and e['serial_before'] == e['serial_after'] for e in events),
            traced_early_drops_preserving_serial=sum(bool(e.get('early_video_dropped')) and e['serial_before'] == e['serial_after'] for e in events),
            required_video_lead_for_20ms_audio_max_ms=max(requirements) if requirements else None,
            per_track_snapshots=snapshots, interval_jitter=delays,
            maximum_interval_mean_audio_jitter_ms=max((d['actual_ms'] for d in delays), default=None),
            last_audio_statistics=[{key: track.get(key) for key in ('ssrc', 'emitted', 'concealed_samples', 'received_samples', 'packets_lost')}
                for track in last.values()],
            sender_room_events=sender_events, viewer_room_events=viewer_events,
            first_sender_pressure_us=min((s['input']['first_audio_pressure_us'] for s in rates
                if s['input'].get('first_audio_pressure_us', 0) > 0), default=None),
            first_playback_fault=first_event,
            first_fault_inferred_reason=classify_fault(first_event),
            first_fault_preceding_events=len(preceding),
            first_fault_preceding_span_ms=(preceding[-1]['now']-preceding[0]['now'])/1000 if preceding else None,
            first_fault_last_events=preceding[-12:],
            first_fault_nearest_jitter=min(delays, key=lambda d: abs(d['receiver_us']-first_event['receiver_us']))
                if delays and first_event else None,
            first_fault_nearest_sender=min(rates, key=lambda d: abs(d['local_us']-first_event['receiver_us']))
                if rates and first_event else None,
            sender_rates=rates))
    return dict(source_summary=str(summary), source_sha256=hashlib.sha256(summary.read_bytes()).hexdigest(),
                original_suite_passed=suite['passed'], physical_sync_measured=False, runs=rows)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--summary', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    assert not args.output.exists(), 'Refusing to overwrite recovery analysis'
    result = analyze(args.summary)
    args.output.write_text(json.dumps(result, indent=2) + '\n', encoding='utf-8')
    print(json.dumps({key: value for key, value in result.items() if key != 'runs'}))
