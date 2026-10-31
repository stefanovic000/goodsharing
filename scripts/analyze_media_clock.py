"""Check optional SDK media timestamps against independently identified content.

Synthetic one-PC diagnostic, before presentation. This verifies a candidate
clock mapping; it is not an audio/video synchronization or playback test.
"""
import argparse
import json
from pathlib import Path
import numpy as np
from analyze_audio_timing import analyze, RATE, BLOCK, WINDOW, STRIDE


def analyze_clock(folder, media):
    folder = Path(folder)
    content = analyze(folder)
    timing = json.loads((folder / 'timing.json').read_text())
    received = timing['received']
    times = np.full(timing['pcm_sample_frames'], np.nan)
    missing = invalid = missing_capture = missing_sink = 0
    for entry in received:
        capture = entry.get('media_capture_ms')
        sink = entry.get('native_sink_us')
        missing_capture += capture is None
        missing_sink += sink is None
        if capture is None or sink is None:
            missing += 1
            continue
        if (not isinstance(capture, (int, float)) or not isinstance(sink, (int, float)) or
                not np.isfinite(capture) or not np.isfinite(sink) or capture <= 0 or sink <= 0 or
                capture * 1000 > sink + 50_000):
            invalid += 1
            continue
        begin, count = entry['sample_frame'], entry['frames']
        times[begin:begin + count] = capture * 1000 + np.arange(count) * 1e6 / RATE
    videos = media.get('receive', {}).get('videos', [])
    if len(videos) != 2 or {v.get('height') for v in videos} != {1080, 720}:
        raise ValueError('Expected both measured video qualities')
    qualities = []
    for video in videos:
        frames = video.get('timings', [])
        source = np.array([v['capture_us'] for v in frames], dtype=np.float64)
        sdk = np.array([v.get('sdk_timestamp_us', np.nan) for v in frames], dtype=np.float64)
        video_valid = bool(len(source) > 2 and np.all(np.isfinite(sdk)) and np.all(sdk > 0) and
                           np.all(np.diff(source) > 0) and np.all(np.diff(sdk) > 0))
        offsets = []
        for index, window in enumerate(content['windows']):
            if not window['matched'] or not video_valid:
                continue
            source_at = window['source_center_us']
            if not source[0] <= source_at <= source[-1]:
                continue  # Never extrapolate across a missing video timeline.
            positions = index * STRIDE * BLOCK + np.arange(WINDOW) * BLOCK
            audio_pts = times[positions]
            if np.any(~np.isfinite(audio_pts)):
                continue
            video_pts = float(np.interp(source_at, source, sdk))
            offsets.append(float(audio_pts.mean() - video_pts) / 1000)
        total = len(content['windows'])
        coverage = len(offsets) / total if total else 0
        p95 = float(np.percentile(np.abs(offsets), 95, method='lower')) if offsets else None
        qualities.append(dict(height=video['height'], video_timing_valid=video_valid,
                              video_frames=len(source), zero_video_timestamps=int(np.sum(sdk == 0)),
                              windows=total, mapped_windows=len(offsets), mapped_fraction=coverage,
                              signed_median_clock_offset_ms=float(np.median(offsets)) if offsets else None,
                              p95_absolute_clock_offset_ms=p95,
                              usable=bool(video_valid and coverage >= .9 and p95 is not None and p95 <= 80)))
    return dict(method=__doc__.strip(), received_blocks=len(received),
                missing_timing_blocks=missing, invalid_timing_blocks=invalid,
                missing_capture_timestamps=missing_capture, missing_native_sink_timestamps=missing_sink,
                timestamp_present_fraction=(len(received) - missing) / len(received) if received else 0,
                qualities=qualities, clock_mapping_usable=bool(not invalid and all(q['usable'] for q in qualities)),
                content_match_latency_passed=content['latency_target_met'],
                audio_content_matching=content['phases'],
                synchronized_playback_verified=False, physical_playback_measured=False)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--folder', type=Path, required=True)
    parser.add_argument('--media', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    result = analyze_clock(args.folder, json.loads(args.media.read_text()))
    args.output.write_text(json.dumps(result, indent=2), encoding='utf-8')
    print(json.dumps(result))
    raise SystemExit(0 if result['clock_mapping_usable'] else 1)
