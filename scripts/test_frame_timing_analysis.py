"""Offline controls for causal boundaries in frame timing analysis."""
from analyze_frame_timing import analyze_run


def reader_event(at, rtp=0, capture=0, frame_id=0, samples=0):
    return dict(read_begin_us=at-100, read_end_us=at, callback_end_us=at+50,
        sender_us=at, rtp=rtp, has_rtp=True, epoch=1, reports=2,
        capture_us=capture, frame_id=frame_id, has_identity=bool(capture), priming=False, samples=samples)


video = [reader_event(100000, capture=70000, frame_id=1), reader_event(260000, capture=80000, frame_id=2)]
video[1]['read_begin_us'] = 100050
audio = [reader_event(100000, rtp=2**32-200, samples=480),
         reader_event(110000, rtp=280, samples=480), reader_event(120000, rtp=542, samples=480)]
completed = [dict(sid='v', kind='video', test_read_timing=video, test_trace_capacity=8192, test_trace_omitted=0),
             dict(sid='a', kind='audio', test_read_timing=audio, test_trace_capacity=8192, test_trace_omitted=0)]
run = dict(case='offline', passed=False, viewer=dict(test_completed_watches=[dict(receiver=dict(completed_readers=completed))],
    # Repeated references in snapshots must not count a reader twice.
    after_stop=dict(last_selection=dict(watch=dict(receiver=dict(completed_readers=completed))))),
    sender=dict(after_stop=dict(publication=dict(outputs=[dict(height=720, test_handoffs=[
        dict(capture_us=70000, handoff_begin_us=80000, handoff_end_us=81000, frame_id=1, priming=False),
        dict(capture_us=80000, handoff_begin_us=90000, handoff_end_us=91000, frame_id=2, priming=False)])]))))
result = analyze_run(run)
assert not result['original_passed'] and len(result['tracks']) == 2
v, a = result['tracks']
gap = v['maximum_read_gap']
assert gap['read_gap_us'] == 160000 and gap['read_wait_us'] == 159950 and gap['previous_callback_us'] == 50
assert gap['sender_matches'] == [dict(height=720, handoff_gap_us=10000, capture_gap_us=10000,
    handoff_to_read_us=169000, previous_handoff_to_read_us=19000)]
assert a['trained_audio_strides'] == 2 and a['non_nominal_audio_strides'] == 1
assert a['minimum_rtp_advance'] == 262 and a['maximum_rtp_advance'] == 480
assert not a['first_fault_audio_strides'] and not v['first_fault_nearby']
run['viewer']['playback'] = dict(audio=dict(test_first_playback_fault={}))
run['viewer']['after_stop']['last_selection']['playback'] = dict(audio=dict(test_first_playback_fault=dict(event=dict(receiver_us=260000))))
assert analyze_run(run)['first_fault']['receiver_us'] == 260000, 'Compact live snapshot hid retained final history'
run['viewer'] = {};run['sender'] = {}
assert analyze_run(run)['tracks'] == []
print('PASS: reader deduplication, late SDK read versus healthy handoff, RTP wrap, variable source stride, missing evidence, failure retained')
