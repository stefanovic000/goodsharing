import unittest
from analyze_sender_audio_clock import analyze_sender_clock


class SenderClockTests(unittest.TestCase):
    def fixture(self):
        lines, source = [], []
        for sequence in (1, 2, 3, 4, 60, 300):
            capture = 1000000000 + sequence * 10000
            age = 25000
            native = capture + 7200000000 + age
            source.append(dict(capture_us=capture))
            lines.append(f'GoodSharing sender audio clock: sequence={sequence} source_us={capture} steady_us={capture+age} native_us={native} capture_ms={(native-age)//1000} age_us={age}')
        return '\n'.join(lines), dict(source=source)

    def test_different_epoch_preserves_capture_age(self):
        log, timing = self.fixture()
        self.assertTrue(analyze_sender_clock(log, timing)['passed'])

    def test_arrival_time_substitution_rejected(self):
        log, timing = self.fixture()
        log = log.replace('capture_ms=8200010', 'capture_ms=8200035')
        self.assertFalse(analyze_sender_clock(log, timing)['passed'])

    def test_unadmitted_capture_time_rejected(self):
        log, timing = self.fixture()
        timing['source'].pop()
        self.assertFalse(analyze_sender_clock(log, timing)['passed'])

    def test_missing_native_branch_samples_rejected(self):
        log, timing = self.fixture()
        self.assertFalse(analyze_sender_clock(log.split('\n', 1)[1], timing)['passed'])

    def sample_grid_fixture(self):
        log, timing = self.fixture()
        timing['source_timestamp_model'] = 'synthetic-48000hz-sample-grid'
        for row, sequence in zip(timing['source'], (1, 2, 3, 4, 60, 300)):
            row['sample_frame'] = sequence * 480
        return log, timing

    def test_generated_sample_clock_is_checked_separately_from_native_samples(self):
        log, timing = self.sample_grid_fixture()
        result = analyze_sender_clock(log, timing)
        self.assertTrue(result['passed'], result)
        self.assertEqual(result['source_sample_grid_blocks_checked'], 6)
        self.assertFalse(result['all_blocks_measured'])

    def test_wakeup_drift_in_an_unsampled_source_block_is_rejected(self):
        log, timing = self.sample_grid_fixture()
        timing['source'].insert(4, dict(sample_frame=5*480, capture_us=1000000000+50000+1500))
        result = analyze_sender_clock(log, timing)
        self.assertFalse(result['passed'])
        self.assertEqual(result['sampled_blocks'], 6)


if __name__ == '__main__':
    unittest.main()
