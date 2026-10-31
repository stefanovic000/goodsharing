"""Offline phase/counter analysis for an existing owned network fixture.

These weighted means are not frame-age percentiles or an exact latency-stage
decomposition. Counter intervals crossing the cap/restore boundary are excluded.
The source-to-decoded latency gate remains in test_network_media.py.
"""
import argparse
import json
from pathlib import Path


def analyze(folder):
    folder=Path(folder)
    summary=json.loads((folder/'summary.json').read_text(encoding='utf-8'))
    native=json.loads((folder/'media.json').read_text(encoding='utf-8'))
    cap,restore=summary.get('cap_monotonic_s'),summary.get('restore_monotonic_s')

    def phase(us):
        if cap is None or us/1e6<cap:
            return 'baseline'
        return 'pressure' if restore is None or us/1e6<restore else 'recovery'

    totals={}
    skipped=0

    def accumulate(kind,stream,numerator,denominator,samples,scale):
        nonlocal skipped
        previous={}
        for sample in samples:
            for record in sample['transport'].get(stream,[]):
                height=record.get('height','audio')
                if height==0:
                    continue
                old=previous.get(height)
                previous[height]=(sample['sample_us'],record)
                if old is None:
                    continue
                old_us,old_record=old
                current_phase=phase(sample['sample_us'])
                if current_phase!=phase(old_us):
                    skipped+=1
                    continue
                count=record[denominator]-old_record[denominator]
                value=record[numerator]-old_record[numerator]
                if count<=0 or value<0:
                    continue
                key=f'{kind}/{height}/{current_phase}'
                total=totals.setdefault(key,[0.,0.])
                total[0]+=value*scale
                total[1]+=count

    sender=native.get('network_stats',[])
    receiver=native.get('receiver_network_stats',[])
    accumulate('sender_packet_queue_ms','outgoing_video','packet_send_delay_seconds','packets_sent',sender,1000)
    accumulate('receiver_video_jitter_ms','incoming_video','jitter_buffer_seconds','jitter_buffer_frames',receiver,1000)
    accumulate('receiver_audio_jitter_ms','incoming_audio','jitter_buffer_seconds','jitter_buffer_samples',receiver,1000)
    accumulate('receiver_audio_concealed_fraction','incoming_audio','concealed_samples','total_samples_received',receiver,1)
    timeline=[]
    for sample in sender:
        outputs=sample.get('publisher',{}).get('outputs',[])
        timeline.append({'phase':phase(sample['sample_us']),
                         'seconds_from_cap':(sample['sample_us']/1e6-cap) if cap else None,
                         'available_outgoing_bitrate_bps':[pair['available_outgoing_bitrate_bps'] for pair in sample['transport'].get('selected_ice',[])],
                         'outputs':[{key:output[key] for key in ('height','target_bitrate_bps','configured_encoder_bitrate_bps','frames_handed_off','keyframes_handed_off','periodic_keyframe_requests','bitrate_cut_keyframe_requests','rate_limited_raw_frames','raw_rate_debt_bytes') if key in output} for output in outputs]})
    return {'method':'Cumulative-counter deltas; weighted means, not percentiles. Boundary-crossing intervals excluded. Concealment fraction uses received-sample counter denominator and is not an audible continuity score.',
            'source_report':str(folder/'summary.json'),'fixture_passed':summary['passed'],
            'excluded_boundary_intervals':skipped,
            'stages':{key:{'weighted_mean':value/count,'denominator_units':count} for key,(value,count) in totals.items()},
            'sender_timeline':timeline}


if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('folder',type=Path)
    args=parser.parse_args()
    result=analyze(args.folder)
    path=args.folder/'phase-analysis.json'
    path.write_text(json.dumps(result,indent=2),encoding='utf-8')
    print(json.dumps({'report':str(path),'fixture_passed':result['fixture_passed'],'stages':result['stages']}))
