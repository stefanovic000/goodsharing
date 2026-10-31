"""Analyze actually drawn source-frame IDs without retaining frame pixels."""
import argparse
import hashlib
import json
import statistics
from pathlib import Path


def find_video(value):
    if isinstance(value, dict):
        trace = value.get('motion_trace')
        if isinstance(trace, list):
            return value
        for child in value.values():
            found = find_video(child)
            if found is not None:
                return found
    elif isinstance(value, list):
        for child in value:
            found = find_video(child)
            if found is not None:
                return found
    return None


def analyze(viewer):
    video = find_video(viewer)
    if video is None:
        raise ValueError('Viewer report has no motion trace')
    trace = video['motion_trace']
    if len(trace) < 2:
        raise ValueError('Motion trace requires at least two frames')
    time_key = 'drawn_us' if 'drawn_us' in trace[0] else 'received_us'
    deadline_key = 'deadline_us' if 'deadline_us' in trace[0] else None

    intervals = []
    resets = 0
    for before, current in zip(trace, trace[1:]):
        source_step = (current['source_frame_id'] - before['source_frame_id']) & 0xffff
        if source_step >= 0x8000:
            resets += 1
            continue
        draw_gap = current[time_key] - before[time_key]
        deadline_gap = (current[deadline_key] - before[deadline_key]) if deadline_key else 0
        intervals.append(dict(
            source_frame_id=current['source_frame_id'],
            source_step=source_step,
            drawn_us=current[time_key],
            draw_gap_us=draw_gap,
            deadline_gap_us=deadline_gap,
        ))

    if len(intervals) < 2:
        raise ValueError('Motion trace has too few forward intervals')

    elapsed = intervals[-1]['drawn_us'] - intervals[0]['drawn_us']
    source_advance = sum(item['source_step'] for item in intervals)
    nominal_source_rate = source_advance * 1000000 / elapsed if elapsed > 0 else 0
    draw_gaps = [item['draw_gap_us'] for item in intervals]
    median_draw_gap = statistics.median(draw_gaps)

    for item in intervals:
        gap = item['draw_gap_us']
        step = item['source_step']
        item['source_fps'] = step * 1000000 / gap if gap > 0 else None
        item['speed_ratio'] = item['source_fps'] / nominal_source_rate if nominal_source_rate and gap > 0 else None
        item['catchup'] = step > 0 and gap < median_draw_gap * 0.65

    ranked = sorted(
        (item for item in intervals if item['source_fps'] is not None),
        key=lambda item: abs(item['source_fps'] - nominal_source_rate),
        reverse=True,
    )[:24]

    return dict(
        time_basis=time_key,
        frames=len(trace),
        intervals_analyzed=len(intervals),
        backward_resets=resets,
        trace_omitted=video.get('motion_trace_omitted', 0),
        duration_ms=elapsed / 1000,
        source_frames_advanced=source_advance,
        effective_source_fps=nominal_source_rate,
        duplicate_source_frames=sum(item['source_step'] == 0 for item in intervals),
        skipped_source_frames=max(0, source_advance - sum(item['source_step'] > 0 for item in intervals)),
        maximum_source_step=max(item['source_step'] for item in intervals),
        catchup_frames=sum(item['catchup'] for item in intervals),
        draw_interval_ms=dict(
            minimum=min(draw_gaps) / 1000,
            p05=sorted(draw_gaps)[int((len(draw_gaps) - 1) * 0.05)] / 1000,
            median=median_draw_gap / 1000,
            p95=sorted(draw_gaps)[int((len(draw_gaps) - 1) * 0.95)] / 1000,
            maximum=max(draw_gaps) / 1000,
        ),
        top_speed_anomalies=ranked,
    )


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--viewer', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    assert not args.output.exists(), 'Refusing to overwrite analysis'
    viewer = json.loads(args.viewer.read_text(encoding='utf-8'))
    result = analyze(viewer)
    result['source_viewer'] = str(args.viewer)
    result['source_sha256'] = hashlib.sha256(args.viewer.read_bytes()).hexdigest()
    args.output.write_text(json.dumps(result, indent=2) + '\n', encoding='utf-8')
    print(json.dumps({key: result[key] for key in (
        'frames', 'effective_source_fps', 'duplicate_source_frames',
        'skipped_source_frames', 'catchup_frames', 'draw_interval_ms')}))


if __name__ == '__main__':
    main()

