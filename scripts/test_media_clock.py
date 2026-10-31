"""Offline calibration and negative controls for candidate SDK media clocks."""
import json
from pathlib import Path
import tempfile
import unittest
import numpy as np
from analyze_media_clock import analyze_clock
import test_audio_timing as fixtures
from analyze_audio_timing import RATE, BLOCK


class MediaClockTest(unittest.TestCase):
    def fixture(self, folder, *, audio_shift_ms=0, missing=False, bad_video=False):
        timing = fixtures.AudioTimingTest().fixture(folder)
        # A separate receiver clock, two hours away from the source app clock.
        clock_offset = 7_200_000_000
        begin = 31 * BLOCK + 312
        for entry in timing['received']:
            source_at = 10_000_000 + (begin + entry['sample_frame']) * 1e6 / RATE
            entry['media_capture_ms'] = None if missing else int((source_at + clock_offset) / 1000 + audio_shift_ms)
            entry['native_sink_us'] = source_at + clock_offset + 400_000
        (folder / 'timing.json').write_text(json.dumps(timing))
        videos = []
        for height in (1080, 720):
            frames = [dict(capture_us=float(source_at), sdk_timestamp_us=float(source_at + clock_offset + (-3_600_000_000 if bad_video else 0)))
                      for source_at in 10_000_000 + np.arange(360) * 1e6 / 60]
            videos.append(dict(height=height, timings=frames))
        return dict(receive=dict(videos=videos))

    def check(self, **kwargs):
        with tempfile.TemporaryDirectory(prefix='goodsharing-media-clock-') as tmp:
            folder = Path(tmp)
            media = self.fixture(folder, **kwargs)
            return analyze_clock(folder, media)

    def test_shared_receiver_clock_with_different_source_epoch(self):
        result = self.check()
        self.assertTrue(result['clock_mapping_usable'], result)
        self.assertFalse(result['synchronized_playback_verified'])
        self.assertTrue(all(q['p95_absolute_clock_offset_ms'] < 10 for q in result['qualities']))

    def test_unrelated_video_clock_is_rejected(self):
        self.assertFalse(self.check(bad_video=True)['clock_mapping_usable'])

    def test_offset_audio_is_rejected(self):
        result = self.check(audio_shift_ms=300)
        self.assertFalse(result['clock_mapping_usable'])
        self.assertTrue(all(q['p95_absolute_clock_offset_ms'] > 280 for q in result['qualities']))

    def test_absent_timing_is_not_replaced_with_callback_arrival(self):
        result = self.check(missing=True)
        self.assertFalse(result['clock_mapping_usable'])
        self.assertEqual(result['timestamp_present_fraction'], 0)
        self.assertTrue(all(q['mapped_windows'] == 0 for q in result['qualities']))


if __name__ == '__main__':
    unittest.main()
