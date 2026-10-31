"""Validate a candidate RTCP sender clock using independently matched A/V content.

One-PC synthetic diagnostic before playback. Sender NTP is a distinct clock
domain; it is never compared with receiver callback/native sink epochs.
"""
import argparse
import json
from pathlib import Path
import numpy as np
from analyze_audio_timing import analyze, RATE, BLOCK, WINDOW, STRIDE


def observation_span(indices, positions):
    """Longest ordered content interval; an isolated bad match cannot create it."""
    if not positions: return 0., 0
    start = previous = positions[0]
    longest, breaks = 0., 0
    for at in range(1, len(positions)):
        position = positions[at]
        callback_window_us = (indices[at] - indices[at - 1]) * STRIDE * BLOCK * 1e6 / RATE
        if not 0 < position - previous <= callback_window_us + 200000:
            start = position
            breaks += 1
        longest = max(longest, position - start)
        previous = position
    return longest / 1e6, breaks


def analyze_rtcp_clock(folder, media, *, minimum_seconds=8, maximum_warmup_seconds=5):
    folder = Path(folder)
    content = analyze(folder)
    timing = json.loads((folder / 'timing.json').read_text())
    times = np.full(timing['pcm_sample_frames'], np.nan)
    epochs = np.zeros(timing['pcm_sample_frames'], dtype=np.uint32)
    missing_rtp = missing_ntp = invalid = 0
    for entry in timing['received']:
        rtp, ntp, reports, epoch = (entry.get(k) for k in ('rtp_timestamp', 'sender_ntp_us', 'rtcp_reports', 'sender_clock_epoch'))
        missing_rtp += rtp is None
        missing_ntp += ntp is None
        if ntp is None: continue
        if (rtp is None or not isinstance(rtp, int) or not 0 <= rtp <= 0xffffffff or
                not isinstance(ntp, (int, float)) or not np.isfinite(ntp) or not 0 < ntp <= 5e15 or
                not isinstance(reports, int) or reports < 2 or not isinstance(epoch, int) or epoch <= 0):
            invalid += 1; continue
        start, frames = entry['sample_frame'], entry['frames']
        times[start:start + frames] = ntp + np.arange(frames) * 1e6 / RATE
        epochs[start:start + frames] = epoch
    videos = media.get('receive', {}).get('videos', [])
    if len(videos) != 2 or {v['height'] for v in videos} != {720, 1080}: raise ValueError('Expected two measured video qualities')
    qualities = []
    for video in videos:
        frames = video['timings']
        mapped = [f for f in frames if f.get('sender_ntp_us') is not None and f.get('rtp_timestamp') is not None and
                  isinstance(f.get('rtcp_reports'), int) and f['rtcp_reports'] >= 2 and
                  isinstance(f.get('sender_clock_epoch'), int) and f['sender_clock_epoch'] > 0]
        source = np.array([f['capture_us'] for f in mapped], dtype=np.float64)
        ntps = np.array([f['sender_ntp_us'] for f in mapped], dtype=np.float64)
        video_epochs = np.array([f['sender_clock_epoch'] for f in mapped], dtype=np.uint32)
        valid_video = bool(len(mapped) > 2 and np.all(np.isfinite(ntps)) and np.all(ntps > 0) and np.all(ntps <= 5e15) and
                           np.all(np.diff(source) > 0))
        offsets, indices, source_positions = [], [], []
        if valid_video:
            for index, window in enumerate(content['windows']):
                if not window['matched']: continue
                position = window['source_center_us']
                right = int(np.searchsorted(source, position))
                if right < 1 or right >= len(source): continue
                left = right - 1
                if (video_epochs[left] != video_epochs[right] or source[right] - source[left] > 200000 or
                        ntps[right] <= ntps[left]): continue
                samples = index * STRIDE * BLOCK + np.arange(WINDOW) * BLOCK
                audio = times[samples]
                if not np.all(np.isfinite(audio)) or epochs[samples].min() == 0 or np.ptp(epochs[samples]) != 0: continue
                value = float(np.interp(position, source[left:right+1], ntps[left:right+1]))
                offsets.append(float(audio.mean() - value) / 1000)
                indices.append(index); source_positions.append(position)
        total = len(content['windows'])
        # Missing startup timing remains visible. Evaluate continuity only after
        # training, with an explicit <=5 s warmup and >=8 s observation gate.
        after_ready = total - indices[0] if indices else total
        coverage = len(offsets) / after_ready if after_ready else 0
        span, content_breaks = observation_span(indices, source_positions)
        warmup = indices[0] * STRIDE * BLOCK / RATE if indices else None
        p95 = float(np.percentile(np.abs(offsets), 95, method='lower')) if offsets else None
        trained_valid = bool(valid_video and coverage >= .9 and span >= minimum_seconds and p95 is not None and p95 <= 80)
        startup_met = bool(warmup is not None and warmup <= maximum_warmup_seconds)
        usable = trained_valid and startup_met
        qualities.append(dict(height=video['height'], video_frames=len(frames), frames_with_sender_ntp=len(mapped),
            video_clock_valid=valid_video, windows=total, mapped_windows=len(offsets),
            whole_run_mapped_fraction=len(offsets)/total if total else 0, after_training_mapped_fraction=coverage,
            warmup_seconds=warmup, observed_source_seconds=span, required_observation_seconds=minimum_seconds,
            observation_method='longest ordered source-content interval; maximum extra step 200 ms',
            content_observation_breaks=content_breaks,
            median_signed_clock_offset_ms=float(np.median(offsets)) if offsets else None,
            p95_absolute_clock_offset_ms=p95, mapping_valid_after_training=trained_valid,
            startup_target_met=startup_met, maximum_warmup_seconds=maximum_warmup_seconds, usable=usable))
    return dict(method=__doc__.strip(), received_blocks=len(timing['received']), missing_audio_rtp=missing_rtp,
        missing_audio_sender_ntp=missing_ntp, invalid_audio_timing=invalid, qualities=qualities,
        clock_domain='RTCP sender-report NTP; infrastructure supplied, not E2EE-authenticated',
        mapping_valid_after_training=bool(not invalid and all(q['mapping_valid_after_training'] for q in qualities)),
        startup_target_met=bool(all(q['startup_target_met'] for q in qualities)),
        rtcp_clock_mapping_usable=bool(not invalid and all(q['usable'] for q in qualities)),
        audio_content_match_latency_passed=content['latency_target_met'],
        synchronized_playback_verified=False, physical_playback_measured=False)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--folder', type=Path, required=True); parser.add_argument('--media', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    result = analyze_rtcp_clock(args.folder, json.loads(args.media.read_text()))
    args.output.write_text(json.dumps(result, indent=2))
    print(json.dumps(result))
    raise SystemExit(0 if result['rtcp_clock_mapping_usable'] else 1)
