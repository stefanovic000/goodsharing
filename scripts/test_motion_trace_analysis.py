"""Offline controls for visible source-frame cadence analysis."""
from analyze_motion_trace import analyze


trace = []
at = 0
source = 10
for step, gap in [(1, 16667), (1, 16667), (2, 33334), (1, 16667), (1, 8000), (1, 8000), (0, 16667)]:
    at += gap
    source += step
    trace.append(dict(source_frame_id=source, drawn_us=at, deadline_us=at, revision=len(trace) + 1))

result = analyze(dict(video=dict(motion_trace=trace, motion_trace_omitted=3)))
assert result['frames'] == 7
assert result['duplicate_source_frames'] == 1
assert result['maximum_source_step'] == 2
assert result['catchup_frames'] == 2
assert result['trace_omitted'] == 3
assert result['draw_interval_ms']['minimum'] == 8
try:
    analyze(dict(video=dict(motion_trace=[])))
except ValueError:
    pass
else:
    raise AssertionError('Empty motion trace was accepted')
print('PASS: drawn source-frame steps, duplicates, skips, catch-up bursts and missing evidence')

