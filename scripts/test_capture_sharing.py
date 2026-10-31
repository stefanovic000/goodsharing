"""Actual owned-window/app-audio -> GoodSharing GUI -> independent viewer.

Uses WGC and process loopback, never arbitrary user windows/audio. Only owned
test processes are stopped. No VPS or OS-wide network settings are involved.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import struct
import subprocess
import time
import math
from local_media_fixture import LocalMediaFixture,ROOT
from process_footprint import ProcessFootprint


def stop(child):
    if child is not None and child.poll() is None:
        child.terminate()
        try:child.wait(timeout=5)
        except subprocess.TimeoutExpired:child.kill();child.wait(timeout=5)


def tone_amplitude(path,hz,channel=0):
    raw=path.read_bytes()
    pcm=struct.unpack('<'+'h'*(len(raw)//2),raw)
    count=len(pcm)//2
    if not count:return 0.
    sine=cosine=0.
    for i in range(count):
        angle=i*hz*2*math.pi/48000
        sine+=pcm[i*2+channel]*math.sin(angle);cosine+=pcm[i*2+channel]*math.cos(angle)
    return 2*math.hypot(sine,cosine)/count


def run_case(folder,case,profile_resources=False,trace_motion=False):
    folder.mkdir()
    fixture=LocalMediaFixture(folder/'server');children=[];handles=[];profiles=[]
    result={'case':case,'passed':False,'actual_window_capture':True,'local_only':True,'unrelated_processes_stopped':False}
    try:
        fixture.start()
        env=fixture.client_env('capture-'+case)
        if trace_motion:env['GOODSHARING_TEST_MOTION_TRACE']='1'
        env['QML_IMPORT_PATH']=str(ROOT/'.deps/qt/6.8.3/msvc2022_64/qml')
        source=folder/'source.json'
        def launch(name,command,**kwargs):
            output=(folder/(name+'.log')).open('w');handles.append(output)
            child=subprocess.Popen(command,env=env,stdout=output,stderr=subprocess.STDOUT,**kwargs);children.append(child);return child
        source_exit=case=='source-exit-720'
        owned=launch('source',[str(ROOT/'build/bin/goodsharing_fixture.exe'),'--hide-cursor','--seconds','6' if source_exit else '30','--right-tone','880','--ready-file',str(source)])
        deadline=time.monotonic()+8
        while not source.exists():
            if owned.poll() is not None or time.monotonic()>deadline:raise RuntimeError('Owned source readiness failed')
            time.sleep(.1)
        source_info=json.loads(source.read_text())
        launch('unwanted-source',[str(ROOT/'build/bin/goodsharing_fixture.exe'),'--seconds','30','--tone','660','--no-window'])
        audio=case!='off-1080';wrong=case=='wrong-key';quality=1080 if case=='off-1080' else 720
        viewer_exe=ROOT/'build/bin/goodsharing_capture_viewer_probe.exe';app_exe=ROOT/'build/bin/GoodSharing.exe'
        result['executable_sha256']={path.name:hashlib.sha256(path.read_bytes()).hexdigest() for path in (viewer_exe,app_exe,ROOT/'build/bin/goodsharing_fixture.exe')}
        command=[str(viewer_exe),'--quality',str(quality),'--report',str(folder/'viewer.json'),'--pcm',str(folder/'received.s16le'),'--screenshot',str(folder/'received.png')]
        if wrong:command+=['--wrong-key']
        if not audio:command+=['--audio-off']
        viewer=launch('viewer',command,creationflags=subprocess.CREATE_NO_WINDOW)
        command=[str(app_exe),'--smoke-share-hwnd',source_info['hwnd'],'--smoke-pid',str(owned.pid),'--report',str(folder/'app.json'),'--screenshot',str(folder/'app.png')]
        if not audio:command+=['--audio-off']
        app=launch('app',command)
        if profile_resources:
            profiles=[ProcessFootprint(app,'GUI sender: WGC + two hardware encoders + local preview').start(),ProcessFootprint(viewer,'independent selected-quality decoder inspector; no playback').start()]
        app.wait(timeout=20);viewer.wait(timeout=23)
        app_report=json.loads((folder/'app.json').read_text());view_report=json.loads((folder/'viewer.json').read_text())
        tone=tone_amplitude(folder/'received.s16le',440)
        right=tone_amplitude(folder/'received.s16le',880,1)
        leftCross=tone_amplitude(folder/'received.s16le',880)
        rightCross=tone_amplitude(folder/'received.s16le',440,1)
        unwanted=max(tone_amplitude(folder/'received.s16le',660),tone_amplitude(folder/'received.s16le',660,1))
        stereo_ok=tone>100 and right>100 and leftCross<tone*.03 and rightCross<right*.03 and unwanted<min(tone,right)*.03
        result.update(app_exit=app.returncode,viewer_exit=viewer.returncode,app=app_report,viewer=view_report,
                      known_audio_tone_amplitude=tone,right_audio_tone_amplitude=right,left_crosstalk_amplitude=leftCross,right_crosstalk_amplitude=rightCross,
                      unwanted_audio_tone_amplitude=unwanted,stereo_and_isolation_passed=stereo_ok,selected_quality=quality)
        result['executable_unchanged']=all(hashlib.sha256((ROOT/'build/bin'/name).read_bytes()).hexdigest()==expected for name,expected in result['executable_sha256'].items())
        stopped=app_report['after_stop'];publisher=stopped['publication'];capture=app_report['capture']
        result['expected_source_exit']=source_exit
        result['passed']=bool(app.returncode==(3 if source_exit else 0) and viewer.returncode==0 and view_report['passed'] and result['executable_unchanged']
            and app_report['badge_when_requested']=='PREPARING SHARE' and (source_exit or app_report['badge_at_capture_check']=='ENCRYPTED SHARE')
            and capture['video_pairs_admitted']>100 and ('source was closed' in capture['error'] if source_exit else capture['error']=='') and capture['pending_raw_frames']<=1
            and not stopped['share_active'] and publisher.get('state')==(4 if source_exit else 3) and publisher.get('gate',{}).get('closed')
            and ((capture['audio_blocks_admitted']>100 and (tone<40 and right<40 and unwanted<40 if wrong else stereo_ok)) if audio else capture['audio_blocks_admitted']==0))
        participants=fixture.participants('capture-'+case)
        result['remaining_participants']=len(participants)
        result['passed'] &= not participants
    except Exception as error:
        result['error']=str(error)
    finally:
        if profiles:
            result['footprint']=[profile.stop() for profile in profiles]
            result['resource_sampling_passed']=all(not p['errors'] and p['sampled_lifetime'] for p in result['footprint'])
            result['passed'] &= result['resource_sampling_passed']
        for child in reversed(children):stop(child)
        for handle in handles:handle.close()
        result['owned_clients_stopped']=all(child.poll() is not None for child in children)
        result['owned_server_stopped']=fixture.stop()
        result['passed'] &= result['owned_clients_stopped'] and result['owned_server_stopped']
        (folder/'summary.json').write_text(json.dumps(result,indent=2),encoding='utf-8')
    return result


if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--case',choices=['all','application-720','off-1080','wrong-key','source-exit-720'],default='all')
    parser.add_argument('--profile-resources',action='store_true')
    parser.add_argument('--trace-motion',action='store_true',help='Retain bounded source-frame/capture/receive records for motion cadence analysis')
    args=parser.parse_args()
    folder=ROOT/'artifacts'/(time.strftime('%Y%m%d-%H%M%S')+'-capture-sharing');folder.mkdir()
    cases=['application-720','off-1080','wrong-key','source-exit-720'] if args.case=='all' else [args.case]
    results=[]
    for case in cases:
        result=run_case(folder/case,case,args.profile_resources,args.trace_motion);results.append(result)
        print(json.dumps({'case':case,'passed':result['passed'],'error':result.get('error')}),flush=True)
    summary={'passed':all(r['passed'] for r in results),'local_only':True,'runs':results}
    (folder/'summary.json').write_text(json.dumps(summary,indent=2),encoding='utf-8')
    print('Reports: '+str(folder),flush=True)
    raise SystemExit(0 if summary['passed'] else 1)
