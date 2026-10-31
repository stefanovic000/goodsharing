"""Correlate bounded test-only handoff/read records on this one PC.

SDK read completion is after decoding/queueing, not packet arrival. A successful
captureFrame is an SDK handoff, not proof of encoder RTP transmission. Neither
source-clock metadata nor this analysis grants media permission.
"""
import argparse
import hashlib
import json
from pathlib import Path


def readers(value):
    found = {}
    def walk(node):
        if isinstance(node, dict):
            for item in node.get('receiver', {}).get('completed_readers', []):
                if item.get('kind') == 'video' and item.get('sid') == node.get('video_sid') and node.get('quality') in (720, 1080):
                    item = dict(item, height=node['quality'])
                    if len(item.get('test_read_timing', [])) >= len(found.get(item['sid'], {}).get('test_read_timing', [])):
                        found[item['sid']] = item
            for item in node.get('completed_readers', []):
                sid = item.get('sid')
                if sid and len(item.get('test_read_timing', [])) > len(found.get(sid, {}).get('test_read_timing', [])):
                    found[sid] = item
            for child in node.values():
                walk(child)
        elif isinstance(node, list):
            for child in node:
                walk(child)
    walk(value)
    return list(found.values())


def analyze_run(run):
    view = run.get('viewer', {})
    playback = (view.get('after_stop', {}).get('last_selection', {}).get('playback') or view.get('playback') or
                view.get('last_selection', {}).get('playback') or {})
    fault = playback.get('audio', {}).get('test_first_playback_fault', {}).get('event', {})
    fault_us = fault.get('receiver_us', 0)
    observation_end = playback.get('audio', {}).get('sampled_receiver_us', 0)
    send = run.get('sender', {}).get('after_stop', {}).get('publication', {})
    handoffs = {(o['height'], e['capture_us'], e['frame_id']): e
                for o in send.get('outputs', []) for e in o.get('test_handoffs', []) if not e['priming']}
    tracks = []
    for reader in readers(view):
        events = reader.get('test_read_timing', [])
        video = reader.get('kind') == 'video'
        media = [e for e in events if not e['priming']] if video else events
        gaps, strides = [], []
        for before, event in zip(media, media[1:]):
            gap = dict(read_gap_us=event['read_end_us']-before['read_end_us'],
                       read_wait_us=event['read_end_us']-event['read_begin_us'],
                       previous_callback_us=before['callback_end_us']-before['read_end_us'],
                       callback_us=event['callback_end_us']-event['read_end_us'], receiver_us=event['read_end_us'])
            if video:
                # The stopped watch manifest maps this SID to a quality. Older
                # evidence without that mapping retains both labelled matches.
                for height in ((reader['height'],) if reader.get('height') else (720, 1080)):
                    a = handoffs.get((height, before['capture_us'], before['frame_id']))
                    b = handoffs.get((height, event['capture_us'], event['frame_id']))
                    if a and b:
                        gap.setdefault('sender_matches', []).append(dict(height=height,
                            handoff_gap_us=b['handoff_begin_us']-a['handoff_begin_us'],
                            capture_gap_us=b['capture_us']-a['capture_us'],
                            handoff_to_read_us=event['read_end_us']-b['handoff_end_us'],
                            previous_handoff_to_read_us=before['read_end_us']-a['handoff_end_us']))
                gap.update(frame_id=event['frame_id'], capture_us=event['capture_us'])
            elif before['has_rtp'] and event['has_rtp'] and event['epoch'] == before['epoch'] and event['reports'] >= 2 and before['reports'] >= 2:
                delta = ((event['rtp']-before['rtp']+2**31) % 2**32)-2**31
                if 0 < delta <= 48000:
                    strides.append(dict(receiver_us=event['read_end_us'], rtp_advance=delta,
                        decoded_samples=before['samples'], read_gap_us=gap['read_gap_us'],
                        sender_advance_us=event['sender_us']-before['sender_us']))
            gaps.append(gap)
        unusual = [e for e in strides if abs(e['rtp_advance']-e['decoded_samples']) > 48]
        tracks.append(dict(sid=reader['sid'], kind=reader['kind'], records=len(events),
            height=reader.get('height'), first_read_us=events[0]['read_end_us'] if events else None,
            last_read_us=events[-1]['read_end_us'] if events else None,
            last_read_age_at_observation_end_us=observation_end-events[-1]['read_end_us'] if events and observation_end else None,
            handoffs_after_last_read=sum(e['handoff_begin_us'] > events[-1]['read_end_us'] for (h, _, _), e in handoffs.items()
                if not reader.get('height') or h == reader['height']) if video and events else None,
            capacity=reader.get('test_trace_capacity'), omitted=reader.get('test_trace_omitted'),
            maximum_read_gap=max(gaps, key=lambda e: e['read_gap_us']) if gaps else None,
            first_fault_nearby=[e for e in gaps if fault_us and abs(e['receiver_us']-fault_us) <= 300000],
            trained_audio_strides=len(strides), non_nominal_audio_strides=len(unusual),
            minimum_rtp_advance=min((e['rtp_advance'] for e in strides), default=None),
            maximum_rtp_advance=max((e['rtp_advance'] for e in strides), default=None),
            first_non_nominal_audio_strides=unusual[:12],
            first_fault_audio_strides=[e for e in strides if fault_us and abs(e['receiver_us']-fault_us) <= 300000]))
    sender_nearby = []
    if fault_us:
        for output in send.get('outputs', []):
            events = [e for e in output.get('test_handoffs', []) if not e['priming']]
            sender_nearby.append(dict(height=output['height'], handoffs=[dict(event,
                gap_us=event['handoff_begin_us']-before['handoff_begin_us'])
                for before, event in zip(events, events[1:]) if abs(event['handoff_begin_us']-fault_us) <= 300000]))
    return dict(case=run['case'], original_passed=run['passed'], first_fault=fault, sender_near_first_fault=sender_nearby,
        sender_trace_outputs=[dict(height=o['height'], records=len(o.get('test_handoffs', [])),
            omitted=o.get('test_handoffs_omitted')) for o in send.get('outputs', [])], tracks=tracks)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--summary', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    assert not args.output.exists(), 'Refusing to overwrite analysis'
    suite = json.loads(args.summary.read_text(encoding='utf-8'))
    result = dict(source_summary=str(args.summary), source_sha256=hashlib.sha256(args.summary.read_bytes()).hexdigest(),
        original_suite_passed=suite['passed'], same_pc_clock_required=True, packet_arrival_measured=False,
        physical_sync_measured=False, runs=[analyze_run(run) for run in suite['runs']])
    args.output.write_text(json.dumps(result, indent=2)+'\n', encoding='utf-8')
    print(json.dumps(dict(output=str(args.output), tracks=[(r['case'], len(r['tracks'])) for r in result['runs']])))
