"""Independent-content controls for the RTCP clock diagnostic."""
import json
from pathlib import Path
import tempfile
import unittest
import numpy as np
import test_audio_timing as fixtures
from analyze_rtcp_clock import analyze_rtcp_clock, observation_span
from analyze_audio_timing import RATE, BLOCK


class RtcpClockTests(unittest.TestCase):
    def test_late_false_match_does_not_erase_prior_observation(self):
        span, breaks = observation_span(list(range(102)), [i * 100000 for i in range(101)] + [0])
        self.assertEqual(span, 10)
        self.assertEqual(breaks, 1)

    def test_large_false_match_jump_cannot_fabricate_observation(self):
        span, breaks = observation_span([0, 1, 2, 3], [0, 100000, 1000000000, 1000100000])
        self.assertEqual(span, .1)
        self.assertEqual(breaks, 1)

    def test_unmatched_windows_preserve_their_elapsed_interval(self):
        self.assertEqual(observation_span([0, 1, 5], [0, 100000, 500000]), (.5, 0))

    def fixture(self, folder, *, shift_ms=0, missing=False, untrained=False, unrelated=False, warmup=0):
        timing = fixtures.AudioTimingTest().fixture(folder)
        origin = 4_000_000_000_000_000  # Sender NTP, far from either local clock.
        begin = 31 * BLOCK + 312
        for entry in timing['received']:
            at = 10_000_000 + (begin + entry['sample_frame']) * 1e6 / RATE
            entry.update(rtp_timestamp=(0xffffa000 + begin + entry['sample_frame']) % 2**32,
                sender_ntp_us=None if missing or entry['sample_frame'] / RATE < warmup else int(origin + at + shift_ms * 1000),
                rtcp_reports=1 if untrained else 2, sender_clock_epoch=1)
        (folder/'timing.json').write_text(json.dumps(timing))
        videos = []
        for height in (720, 1080):
            frames = [dict(capture_us=float(at), sender_ntp_us=None if (at - 10_000_000)/1e6 < warmup else float(origin + at + (7_200_000_000 if unrelated else 0)),
                           rtp_timestamp=(90000 * i//60 + (1111 if height == 720 else 2222)) % 2**32,
                           rtcp_reports=2, sender_clock_epoch=1)
                      for i, at in enumerate(10_000_000 + np.arange(360)*1e6/60)]
            videos.append(dict(height=height, timings=frames))
        return dict(receive=dict(videos=videos))

    def check(self, **kwargs):
        with tempfile.TemporaryDirectory(prefix='goodsharing-rtcp-clock-') as tmp:
            folder = Path(tmp)
            return analyze_rtcp_clock(folder, self.fixture(folder, **kwargs), minimum_seconds=2)

    def test_common_sender_clock_with_independent_local_epochs(self):
        result = self.check()
        self.assertTrue(result['rtcp_clock_mapping_usable'], result)
        self.assertTrue(all(q['p95_absolute_clock_offset_ms'] < 10 for q in result['qualities']))
        self.assertFalse(result['synchronized_playback_verified'])

    def test_explicit_training_warmup_preserves_missing_startup_timing(self):
        result = self.check(warmup=.5)
        self.assertTrue(result['rtcp_clock_mapping_usable'], result)
        self.assertGreater(result['missing_audio_sender_ntp'], 0)
        self.assertTrue(all(q['whole_run_mapped_fraction'] < 1 for q in result['qualities']))

    def test_missing_timing_is_not_replaced_with_arrival(self):
        self.assertFalse(self.check(missing=True)['rtcp_clock_mapping_usable'])

    def test_untrained_clock_is_rejected(self):
        self.assertFalse(self.check(untrained=True)['rtcp_clock_mapping_usable'])

    def test_unrelated_sender_clock_is_rejected(self):
        self.assertFalse(self.check(unrelated=True)['rtcp_clock_mapping_usable'])

    def test_wrong_content_time_is_rejected(self):
        self.assertFalse(self.check(shift_ms=300)['rtcp_clock_mapping_usable'])

    def test_late_training_is_separate_from_steady_mapping(self):
        with tempfile.TemporaryDirectory(prefix='goodsharing-late-clock-') as tmp:
            folder = Path(tmp)
            result = analyze_rtcp_clock(folder, self.fixture(folder, warmup=.5), minimum_seconds=2, maximum_warmup_seconds=.1)
            self.assertTrue(result['mapping_valid_after_training'])
            self.assertFalse(result['startup_target_met'])
            self.assertFalse(result['rtcp_clock_mapping_usable'])


if __name__ == '__main__': unittest.main()
