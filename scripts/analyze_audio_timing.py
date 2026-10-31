"""Synthetic one-PC audio content timing, before playback. Requires NumPy.

480 ms amplitude-code windows are matched every 100 ms, independently of
timestamps. Report unmatched/ambiguous windows explicitly, including PLC or
time stretching that destroys the code. Never infer continuous audio from a
tone, regular callbacks, or matched windows alone. Window ages are NOT a
per-packet latency distribution or physical capture-to-speaker measurement.
"""
import argparse
import bisect
import hashlib
import json
from pathlib import Path
import numpy as np

RATE=48000
BLOCK=480
WINDOW=48
STRIDE=10
MIN_SCORE=.80
MIN_MARGIN=.12


def amplitude(sample_frames):
    x=(np.asarray(sample_frames,dtype=np.uint64)//960+1).astype(np.uint32)*np.uint32(0x9e3779b9)
    x^=x>>16
    x*=np.uint32(0x7feb352d)
    x^=x>>15
    x*=np.uint32(0x846ca68b)
    x^=x>>16
    return np.where(x&1,6000.,2000.)


def envelope(pcm):
    """Fit sine/cosine quadratures, avoiding non-integer-cycle bias."""
    blocks=pcm[:len(pcm)//BLOCK*BLOCK].reshape(-1,BLOCK,2).astype(np.float64)
    result=[]
    for channel,hz in enumerate((440,880)):
        angle=np.arange(BLOCK)*hz*2*np.pi/RATE
        basis=np.column_stack((np.sin(angle),np.cos(angle),np.ones(BLOCK)))
        fitted=blocks[:,:,channel]@np.linalg.pinv(basis).T
        result.append(np.hypot(fitted[:,0],fitted[:,1]))
    return np.mean(result,axis=0)


def match_windows(values,source_frames):
    reference=amplitude(np.arange(source_frames//BLOCK)*BLOCK)
    if len(reference)<WINDOW or len(values)<WINDOW:
        return []
    candidates=np.lib.stride_tricks.sliding_window_view(reference,WINDOW)
    centered=candidates-candidates.mean(axis=1,keepdims=True)
    norms=np.linalg.norm(centered,axis=1)
    normalized=np.divide(centered,norms[:,None],out=np.zeros_like(centered),where=norms[:,None]>0)
    result=[]
    for start in range(0,len(values)-WINDOW+1,STRIDE):
        observed=values[start:start+WINDOW]
        observed=observed-observed.mean()
        norm=float(np.linalg.norm(observed))
        if norm<100:
            result.append(dict(start=start,matched=False,reason='no_code_variation'))
            continue
        scores=normalized@observed/norm
        best=int(np.argmax(scores))
        # A fractional 10 ms shift accounts for codec lookahead and block
        # alignment. It is fitted from amplitude, never from expected age.
        score=float(scores[best]);position=float(best)
        for candidate in range(max(0,best-2),min(len(candidates)-1,best+2)+1):
            if candidate+WINDOW>=len(reference):
                continue
            for fraction in (.1,.2,.3,.4,.5,.6,.7,.8,.9):
                shifted=(1-fraction)*reference[candidate:candidate+WINDOW]+fraction*reference[candidate+1:candidate+WINDOW+1]
                shifted=shifted-shifted.mean()
                divisor=float(np.linalg.norm(shifted))*norm
                trial=float(shifted@observed)/divisor if divisor else 0.
                if trial>score:
                    score,position=trial,candidate+fraction
        others=scores.copy()
        others[max(0,int(position)-4):min(len(others),int(position)+5)]=-1
        runner=float(np.max(others)) if len(others) else -1.
        matched=score>=MIN_SCORE and score-runner>=MIN_MARGIN
        result.append(dict(start=start,matched=bool(matched),score=score,margin=score-runner,
                           source_start_block=position,
                           reason='matched' if matched else 'weak_or_ambiguous_code'))
    return result


def analyze(folder,cap_us=None,restore_us=None):
    folder=Path(folder)
    timing_path=folder/'timing.json'
    timing=json.loads(timing_path.read_text(encoding='utf-8'))
    if (timing.get('overflow') or timing.get('invalid_format') or timing.get('code')!='am20-v1'
            or timing.get('sample_rate')!=RATE or timing.get('channels')!=2 or timing.get('format')!='s16le'):
        raise ValueError('Invalid or truncated synthetic timing trace')
    pcm_path=folder/timing['pcm_file']
    if pcm_path.resolve().parent!=folder.resolve():
        raise ValueError('PCM path must stay inside the trace folder')
    size=pcm_path.stat().st_size
    if size>RATE*2*2*24 or size%4 or size!=timing['pcm_sample_frames']*4:
        raise ValueError('Unexpected PCM size')
    pcm=np.fromfile(pcm_path,dtype='<i2').reshape(-1,2)
    source=timing['source'];received=timing['received']
    if not source or not received or len(source)>2400 or len(received)>2400:
        raise ValueError('Missing or oversized timing map')
    source_offsets=np.array([s['sample_frame'] for s in source],dtype=np.int64)
    source_times=np.array([s['capture_us'] for s in source],dtype=np.float64)
    if (np.any(np.diff(source_offsets)<=0) or np.any(np.diff(source_times)<=0)
            or source_offsets[0]<0 or source_offsets[-1]>RATE*24):
        raise ValueError('Invalid source ordering or range')
    callback_times=np.empty(len(pcm),dtype=np.float64)
    next_frame=0;previous_time=0
    for block in received:
        begin,count,at=block['sample_frame'],block['frames'],block['received_us']
        if begin!=next_frame or count<=0 or begin+count>len(pcm) or at<previous_time:
            raise ValueError('Invalid receive timing map')
        callback_times[begin:begin+count]=at
        next_frame+=count;previous_time=at
    if next_frame!=len(pcm):
        raise ValueError('Incomplete receive timing map')
    values=envelope(pcm)
    matches=match_windows(values,int(source_offsets[-1])+BLOCK)
    phases={name:[] for name in ('baseline','pressure','recovery')}
    excluded=0

    def phase(at):
        return 'baseline' if cap_us is None or at<cap_us else 'pressure' if restore_us is None or at<restore_us else 'recovery'

    for window in matches:
        begin=window.pop('start')*BLOCK
        end=begin+WINDOW*BLOCK
        first,last=float(callback_times[begin]),float(callback_times[end-1])
        center=float(callback_times[begin:end].mean())
        window.update(receive_start_us=first,receive_end_us=last,receive_center_us=center)
        window['phase']=phase(center)
        if phase(first)!=phase(last):
            window['phase']='boundary';excluded+=1
        if window['matched']:
            position=window['source_start_block']*BLOCK
            # Every source block covered by the match must actually have been
            # admitted, rather than merely generated before crypto readiness.
            lower=int(position//BLOCK)*BLOCK
            needed=np.arange(lower,int(np.ceil(position/BLOCK)+WINDOW)*BLOCK,BLOCK,dtype=np.int64)
            indexes=np.searchsorted(source_offsets,needed)
            if np.any(indexes>=len(source_offsets)) or np.any(source_offsets[np.minimum(indexes,len(source_offsets)-1)]!=needed):
                window.update(matched=False,reason='source_window_not_admitted')
            else:
                # Average source block creation time; fractional shift in the
                # 10 ms block grid. Both endpoints use this PC's steady clock.
                positions=position+np.arange(WINDOW)*BLOCK
                source_at=float(np.interp(positions,source_offsets,source_times).mean())
                age=(center-source_at)/1000
                window.update(source_center_us=source_at,age_ms=age)
                if age<-10:
                    window.update(matched=False,reason='noncausal_match')
        if window['phase'] in phases:
            phases[window['phase']].append(window)
    phase_results={}
    for name,items in phases.items():
        matched=[w for w in items if w['matched']]
        ages=[w['age_ms'] for w in matched]
        times=[b['received_us'] for b in received if phase(b['received_us'])==name]
        gaps=np.diff(times)/1000 if len(times)>1 else np.array([])
        coverage=len(matched)/len(items) if items else None
        phase_results[name]=dict(windows=len(items),matched_windows=len(matched),unmatched_windows=len(items)-len(matched),
            matched_fraction=coverage,p95_window_age_ms=float(np.percentile(ages,95,method='lower')) if ages else None,
            maximum_window_age_ms=max(ages) if ages else None,
            maximum_callback_gap_ms=float(max(gaps)) if len(gaps) else None,
            target_met=bool(items and coverage>=.9 and ages and max(0,float(np.percentile(ages,95,method='lower')))<=150 and (not len(gaps) or max(gaps)<=100)))
    required=('baseline',) if cap_us is None else ('baseline','pressure','recovery')
    return dict(method=__doc__.strip(),window_ms=480,stride_ms=100,minimum_correlation=MIN_SCORE,minimum_margin=MIN_MARGIN,
        measurement_valid=True,latency_target_met=all(phase_results[p]['target_met'] for p in required),
        boundary_windows_excluded=excluded,phases=phase_results,windows=matches,
        timing_sha256=hashlib.sha256(timing_path.read_bytes()).hexdigest(),pcm_sha256=hashlib.sha256(pcm_path.read_bytes()).hexdigest(),
        analyzer_sha256=hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),numpy_version=np.__version__,
        production_audio_changed=False,physical_playback_measured=False)


def analyze_av_sync(audio,media):
    """Estimate content skew if decoded callbacks were presented immediately.

    Use the most recently delivered image at each matched audio window's
    callback midpoint. This retains a frozen image's growing skew instead of
    selecting a future frame or omitting sparse video. Actual speaker/display
    presentation and common playout-clock scheduling remain separate gates.
    Positive skew means the audio content is ahead of the held video image.
    """
    qualities=[]
    videos=media.get('receive',{}).get('videos',[])
    if len(videos)!=2 or {v.get('height') for v in videos}!={1080,720}:
        raise ValueError('A/V timing requires both measured video qualities')
    for video in videos:
        frames=video.get('timings',[])
        arrivals=[f['received_us'] for f in frames]
        if not frames or any(b<a for a,b in zip(arrivals,arrivals[1:])):
            raise ValueError('Missing or out-of-order video callback timing')
        phases={name:[] for name in ('baseline','pressure','recovery')}
        for audio_window in audio['windows']:
            phase=audio_window['phase']
            if phase not in phases:continue
            item={'matched':False}
            if audio_window['matched']:
                at=audio_window['receive_center_us']
                index=bisect.bisect_right(arrivals,at)-1
                if index>=0:
                    frame=frames[index]
                    item={'matched':True,'skew_ms':(audio_window['source_center_us']-frame['capture_us'])/1000,
                          'held_video_age_ms':(at-frame['capture_us'])/1000}
            phases[phase].append(item)
        results={}
        for phase,items in phases.items():
            matched=[item for item in items if item['matched']]
            skew=[item['skew_ms'] for item in matched]
            absolute=[abs(value) for value in skew]
            coverage=len(matched)/len(items) if items else None
            p95=float(np.percentile(absolute,95,method='lower')) if absolute else None
            results[phase]=dict(windows=len(items),matched_windows=len(matched),matched_fraction=coverage,
                p95_absolute_skew_ms=p95,minimum_skew_ms=min(skew) if skew else None,
                maximum_skew_ms=max(skew) if skew else None,
                maximum_held_video_age_ms=max(item['held_video_age_ms'] for item in matched) if matched else None,
                target_met=bool(items and coverage>=.9 and p95 is not None and p95<=80))
        qualities.append(dict(height=video['height'],phases=results))
    required=[phase for phase,value in audio['phases'].items() if value['windows']]
    return dict(method=analyze_av_sync.__doc__.strip(),actual_playback_measured=False,skew_target_ms=80,
                latency_target_met=bool(required) and all(q['phases'][phase]['target_met'] for q in qualities for phase in required),
                qualities=qualities)


if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('folder',type=Path)
    parser.add_argument('--cap-us',type=float)
    parser.add_argument('--restore-us',type=float)
    parser.add_argument('--media-report',type=Path)
    args=parser.parse_args()
    result=analyze(args.folder,args.cap_us,args.restore_us)
    if args.media_report:
        result['av_sync']=analyze_av_sync(result,json.loads(args.media_report.read_text(encoding='utf-8')))
    (args.folder/'analysis.json').write_text(json.dumps(result,indent=2),encoding='utf-8')
    print(json.dumps({key:result[key] for key in ('measurement_valid','latency_target_met','phases')}))
