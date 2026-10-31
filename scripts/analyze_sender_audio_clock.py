"""Check bounded native-source samples against the admitted synthetic PCM map.

This proves sender submission timing only, before Opus/network/remote playback.
"""
import re


def analyze_sender_clock(log, timing):
    rows = [dict(zip(('sequence', 'source_us', 'steady_us', 'native_us', 'capture_ms', 'age_us'), map(int, values)))
            for values in re.findall(r'GoodSharing sender audio clock: sequence=(\d+) source_us=(-?\d+) steady_us=(-?\d+) native_us=(-?\d+) capture_ms=(-?\d+) age_us=(-?\d+)', log)]
    admitted = {entry['capture_us'] for entry in timing['source']}
    errors = []
    sample_grid = timing.get('source_timestamp_model') == 'synthetic-48000hz-sample-grid'
    if sample_grid:
        source = timing['source']
        if not source or any(entry['capture_us'] != source[0]['capture_us'] +
                (entry['sample_frame'] - source[0]['sample_frame']) * 1000000 // 48000 for entry in source):
            errors.append('Generated source timestamps do not follow their exact 48 kHz sample clock')
    if [row['sequence'] for row in rows] != [1, 2, 3, 4, 60, 300]:
        errors.append('Expected all six bounded native source samples')
    for row in rows:
        if row['source_us'] not in admitted:
            errors.append('Native source timestamp was not an admitted PCM timestamp')
        age = row['steady_us'] - row['source_us']
        if age != row['age_us'] or not 0 <= age <= 100000:
            errors.append('Capture age is invalid or changed')
        remainder = row['native_us'] - age - row['capture_ms'] * 1000
        if row['capture_ms'] <= 0 or not 0 <= remainder < 1000:
            errors.append('Native capture timestamp does not preserve source age')
    return dict(passed=not errors, method=__doc__.strip(), sampled_blocks=len(rows),
                source_sample_grid_checked=sample_grid, source_sample_grid_blocks_checked=len(timing['source']) if sample_grid else 0,
                samples=rows, errors=errors, all_blocks_measured=False,
                encoded_timing_verified=False, synchronized_playback_verified=False)
