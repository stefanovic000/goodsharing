"""Offline calibration/negative controls for the synthetic content analyser."""
import json
from pathlib import Path
import tempfile
import unittest
import numpy as np
from analyze_audio_timing import analyze,analyze_av_sync,amplitude,RATE,BLOCK


class AudioTimingTest(unittest.TestCase):
    def fixture(self,folder,delay_ms=50,kind='coded'):
        source_frames=RATE*6
        begin=31*BLOCK+312 # non-aligned codec-like lookahead
        offsets=np.arange(begin,begin+RATE*4)
        level=amplitude(offsets) if kind in ('coded','damaged') else np.full(len(offsets),4000.)
        signal=np.column_stack([level*np.sin(offsets*hz*2*np.pi/RATE) for hz in (440,880)])*.72
        if kind=='silence':
            signal[:]=0
        if kind=='damaged':
            signal[RATE:RATE*3]=0
        signal.astype('<i2').tofile(folder/'received.s16le')
        base=10_000_000
        source=[dict(sample_frame=i,capture_us=base+i*1e6/RATE) for i in range(0,source_frames,BLOCK)]
        received=[dict(sample_frame=i,frames=BLOCK,received_us=base+(begin+i)*1e6/RATE+delay_ms*1000) for i in range(0,len(offsets),BLOCK)]
        metadata=dict(overflow=False,invalid_format=False,code='am20-v1',sample_rate=RATE,channels=2,
                      format='s16le',pcm_file='received.s16le',pcm_sample_frames=len(offsets),source=source,received=received)
        (folder/'timing.json').write_text(json.dumps(metadata))
        return metadata

    def run_fixture(self,**kwargs):
        with tempfile.TemporaryDirectory(prefix='goodsharing-audio-timing-') as temporary:
            folder=Path(temporary)
            self.fixture(folder,**kwargs)
            return analyze(folder)

    def test_known_delay_gain_and_fractional_alignment(self):
        result=self.run_fixture()
        baseline=result['phases']['baseline']
        self.assertTrue(result['latency_target_met'],baseline)
        self.assertGreater(baseline['matched_fraction'],.95)
        self.assertAlmostEqual(baseline['p95_window_age_ms'],50,delta=10)

    def test_old_audio_is_not_fitted_to_a_fresh_timestamp(self):
        result=self.run_fixture(delay_ms=350)
        self.assertFalse(result['latency_target_met'])
        self.assertGreater(result['phases']['baseline']['matched_fraction'],.95)
        self.assertAlmostEqual(result['phases']['baseline']['p95_window_age_ms'],350,delta=10)

    def test_silence_and_constant_tone_do_not_prove_fresh_audio(self):
        for kind in ('silence','constant'):
            with self.subTest(kind=kind):
                result=self.run_fixture(kind=kind)
                self.assertFalse(result['latency_target_met'])
                self.assertEqual(result['phases']['baseline']['matched_windows'],0)

    def test_dropout_reduces_coverage(self):
        result=self.run_fixture(kind='damaged')
        self.assertFalse(result['latency_target_met'])
        self.assertLess(result['phases']['baseline']['matched_fraction'],.6)
        self.assertGreater(result['phases']['baseline']['matched_fraction'],.2)

    def test_trace_truncation_and_missing_source_are_rejected(self):
        for mode in ('overflow','missing-source','pcm-size','out-of-order'):
            with self.subTest(mode=mode),tempfile.TemporaryDirectory(prefix='goodsharing-audio-timing-') as temporary:
                folder=Path(temporary);metadata=self.fixture(folder)
                if mode=='overflow':metadata['overflow']=True
                elif mode=='missing-source':metadata['source']=[]
                elif mode=='pcm-size':metadata['pcm_sample_frames']+=1
                else:metadata['source'][2]['capture_us']=0
                (folder/'timing.json').write_text(json.dumps(metadata))
                with self.assertRaises(ValueError):analyze(folder)

    def test_unadmitted_source_is_not_counted_as_a_valid_match(self):
        with tempfile.TemporaryDirectory(prefix='goodsharing-audio-timing-') as temporary:
            folder=Path(temporary);metadata=self.fixture(folder)
            metadata['source']=[s for s in metadata['source'] if not RATE<=s['sample_frame']<RATE*3]
            (folder/'timing.json').write_text(json.dumps(metadata))
            result=analyze(folder)
            self.assertFalse(result['latency_target_met'])
            self.assertTrue(any(w['reason']=='source_window_not_admitted' for w in result['windows']))

    def test_future_content_is_rejected(self):
        result=self.run_fixture(delay_ms=-100)
        self.assertFalse(result['latency_target_met'])
        self.assertEqual(result['phases']['baseline']['matched_windows'],0)
        self.assertTrue(any(w['reason']=='noncausal_match' for w in result['windows']))

    def test_av_sync_detects_held_image_and_does_not_select_future_video(self):
        audio={'phases':{'baseline':{'windows':20}},'windows':[
            dict(phase='baseline',matched=True,receive_center_us=1_000_000+i*100_000,
                 source_center_us=950_000+i*100_000) for i in range(20)]}
        frames=[dict(received_us=t,capture_us=t-30_000) for t in range(900_000,3_100_000,16_667)]
        def media(timings):return {'receive':{'videos':[dict(height=h,timings=timings) for h in (1080,720)]}}
        normal=analyze_av_sync(audio,media(frames))
        self.assertTrue(normal['latency_target_met'])
        # Both streams have a fresh future frame; selecting the nearest frame
        # would conceal the long held-image interval between the two frames.
        frozen=analyze_av_sync(audio,media([frames[0],frames[-1]]))
        self.assertFalse(frozen['latency_target_met'])
        self.assertGreater(frozen['qualities'][0]['phases']['baseline']['maximum_skew_ms'],1500)

    def test_av_sync_requires_audio_match_coverage(self):
        audio={'phases':{'baseline':{'windows':10}},'windows':[
            dict(phase='baseline',matched=False,receive_center_us=1_000_000+i*100_000) for i in range(10)]}
        media={'receive':{'videos':[dict(height=h,timings=[dict(received_us=900_000,capture_us=870_000)]) for h in (1080,720)]}}
        result=analyze_av_sync(audio,media)
        self.assertFalse(result['latency_target_met'])
        self.assertEqual(result['qualities'][0]['phases']['baseline']['matched_fraction'],0)


if __name__=='__main__':
    unittest.main(verbosity=2)
