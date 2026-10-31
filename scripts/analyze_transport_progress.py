"""Correlate SDK RTP counters with decoded delivery, without equating handoff to send."""
import argparse
import hashlib
import json
from pathlib import Path


def samples(path, prefix, time_key):
    rows = []
    if path.exists():
        for line in path.read_text(encoding='utf-8-sig').splitlines():
            if line.startswith(prefix):
                row = json.loads(line[len(prefix):])
                rows.append(dict(local_us=row[time_key], tracks=row['tracks']))
    return rows


def intervals(rows, counters):
    last, result = {}, []
    for row in rows:
        for track in row['tracks']:
            key = (track['kind'], track['ssrc'])
            previous = last.get(key)
            last[key] = (row['local_us'], track)
            if not previous or row['local_us'] <= previous[0]:
                continue
            values = {field: track[field] - previous[1][field] for field in counters
                      if field in track and field in previous[1]}
            result.append(dict(kind=key[0], ssrc=key[1], height=track.get('height'),
                start_us=previous[0], end_us=row['local_us'], deltas=values,
                counter_reset=any(value < 0 for value in values.values())))
    return result


def analyze(path):
    suite = json.loads(path.read_text(encoding='utf-8')); runs = []
    for run in suite['runs']:
        folder = path.parent / run['case']
        view = run.get('viewer', {})
        audio = view.get('after_stop', {}).get('last_selection', {}).get('playback', {}).get('audio', {})
        fault = audio.get('test_first_playback_fault', {}).get('event', {})
        at = fault.get('receiver_us', 0)
        outgoing = intervals(samples(folder / 'sender.log', 'GoodSharing test outbound stats: ', 'local_us'),
                             ('packets_sent', 'bytes_sent', 'frames_sent', 'frames_encoded'))
        incoming = intervals(samples(folder / 'viewer.log', 'GoodSharing test track stats: ', 'receiver_us'),
                             ('packets_received', 'bytes_received', 'frames_received', 'frames_decoded', 'received_samples', 'concealed_samples', 'emitted'))
        def nearby(rows):
            return [row for row in rows if at and row['start_us'] - 1000000 <= at <= row['end_us'] + 1000000]
        publisher = run.get('sender', {}).get('after_stop', {}).get('publication', {})
        runs.append(dict(case=run['case'], original_passed=run['passed'],
            first_publication_failure=publisher.get('first_publication_failure', {}),
            first_playback_fault=fault, outgoing_intervals=outgoing, incoming_intervals=incoming,
            outgoing_near_first_fault=nearby(outgoing), incoming_near_first_fault=nearby(incoming)))
    return dict(source_summary=str(path), source_sha256=hashlib.sha256(path.read_bytes()).hexdigest(),
        original_suite_passed=suite['passed'], same_pc_clock_required=True,
        packet_arrival_timing_measured=False, physical_sync_measured=False, runs=runs)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--summary', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    assert not args.output.exists(), 'Refusing to overwrite transport analysis'
    result = analyze(args.summary)
    args.output.write_text(json.dumps(result, indent=2) + '\n', encoding='utf-8')
    print(json.dumps(dict(output=str(args.output), runs=len(result['runs']))))
