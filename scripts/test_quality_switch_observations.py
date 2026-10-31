"""Offline negative controls for the bounded quality-switch observations."""
import argparse
import copy
import json
from pathlib import Path
from test_remote_av import check_quality_switch


def controls(view):
    baseline = check_quality_switch(view, 'switch')
    assert all(baseline.values()), baseline
    results = {'valid_round_trip_accepted': True}

    def reject(name, key, mutate):
        changed = copy.deepcopy(view)
        mutate(changed)
        results[name] = check_quality_switch(changed, 'switch')[key] is False

    reject('missing_interval_rejected', 'switch_audio_observed',
           lambda v: v['audio_observer']['intervals'].pop())
    reject('quiet_gap_rejected', 'switch_audio_observed',
           lambda v: v['audio_observer']['intervals'][0].update(longest_quiet_ms=40.01))
    reject('missing_audio_samples_rejected', 'switch_audio_observed',
           lambda v: v['audio_observer']['intervals'][0].update(observed_frame_coverage=.94))
    reject('new_audio_session_rejected', 'persistent_audio_identity',
           lambda v: v['switch_observations'][-1]['playback'].update(session_id=-1))
    reject('extra_worker_rejected', 'one_output_worker',
           lambda v: v['switch_observations'][-1]['playback'].update(worker_starts=2))
    reject('post_commit_clock_reset_rejected', 'clock_preserved_through_observation',
           lambda v: v['switch_observations'][-1]['playback']['audio'].update(reset_serial=-1))
    reject('post_commit_device_reset_rejected', 'clock_preserved_through_observation',
           lambda v: v['switch_observations'][-1]['playback']['audio'].update(device_resets=-1))
    reject('missing_clock_counter_rejected', 'clock_preserved_through_observation',
           lambda v: [r['playback']['audio'].pop('reset_serial') for r in v['switch_observations']])
    reject('unqueued_picture_rejected', 'two_video_commits',
           lambda v: v['handover_history'][0]['new_video'].update(queued_presentations=0))
    reject('multiple_warming_images_rejected', 'one_warming_image',
           lambda v: v['handover_history'][0]['new_video'].update(warm_queue_high_water=2))
    reject('commit_counter_mismatch_rejected', 'native_and_gui_agree',
           lambda v: v['after_stop']['last_selection'].update(committed_handovers=3))
    assert all(results.values()), results
    return results


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--summary', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    assert not args.output.exists(), 'Refusing to overwrite control evidence'
    suite = json.loads(args.summary.read_text(encoding='utf-8'))
    view = next(r['viewer'] for r in suite['runs'] if r['case'] == 'switch' and r['passed'])
    result = dict(passed=True, source_summary=str(args.summary), checks=controls(view))
    args.output.write_text(json.dumps(result, indent=2) + '\n', encoding='utf-8')
    print(json.dumps(result))
