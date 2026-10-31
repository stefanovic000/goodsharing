"""Repeated bounded synthetic startup checks with a widened setup interval."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import sys
import time
from local_media_fixture import ROOT
from startup_diagnostics import analyze_startup
from verify_livekit_sdk import runtime_info
import os


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--runs',type=int,default=8)
    parser.add_argument('--setup-stall-ms',type=int,default=200)
    args=parser.parse_args()
    if not 1<=args.runs<=16 or not 0<=args.setup_stall_ms<=250:
        parser.error('Use 1 to 16 runs and a setup stall of 0 to 250 ms')
    folder=ROOT/'artifacts'/(time.strftime('%Y%m%d-%H%M%S')+'-receiver-startup')
    folder.mkdir(exist_ok=False)
    runtime=runtime_info()
    (folder/'runtime-info.json').write_text(json.dumps(runtime,indent=2),encoding='utf-8')
    executable=ROOT/'build/bin/goodsharing_publisher_probe.exe'
    expected_hash=hashlib.sha256(executable.read_bytes()).hexdigest()
    env=os.environ.copy()
    env.update(GOODSHARING_TEST_NETWORK_TELEMETRY='1',GOODSHARING_TEST_RECEIVER_SETUP_STALL_MS=str(args.setup_stall_ms))
    results=[]
    for index in range(args.runs):
        output_path=folder/f'run-{index+1}.log'
        with output_path.open('w',encoding='utf-8') as output:
            # The child harness bounds, tears down and audits its own SFU/probe.
            child=subprocess.run([sys.executable,str(ROOT/'scripts/test_share_publisher.py'),'--case','stereo','--seconds','3','--record-encoded'],cwd=ROOT,env=env,stdout=output,stderr=subprocess.STDOUT,creationflags=subprocess.CREATE_NO_WINDOW)
        paths=[Path(line.removeprefix('Reports: ')) for line in output_path.read_text(encoding='utf-8').splitlines() if line.startswith('Reports: ')]
        item={'run':index+1,'exit_code':child.returncode,'passed':False}
        if len(paths)==1 and (paths[0]/'summary.json').is_file():
            native_path=paths[0]
            summary=json.loads((native_path/'summary.json').read_text(encoding='utf-8'))
            native=json.loads((native_path/'stereo.json').read_text(encoding='utf-8'))
            analysis=analyze_startup((native_path/'stereo.log').read_text(encoding='utf-8',errors='replace'),args.setup_stall_ms)
            observed_runtime=json.loads((native_path/'runtime-info.json').read_text(encoding='utf-8'))
            item.update(report=str((native_path/'summary.json').relative_to(ROOT)),startup=analysis,
                        inspected_frames={str(video['height']):video['frames'] for video in native['receive']['videos']},
                        pixel_error_frames=sum(video['pixel_error_frames'] for video in native['receive']['videos']))
            item['passed']=child.returncode==0 and summary['passed'] and analysis['closed_pipeline_order_verified'] and analysis['setup_stall_verified'] and analysis['receiver_count']==3 and analysis['decoder_warning_count']==0 and item['pixel_error_frames']==0
            item['passed'] &= all(case['executable_sha256']==expected_hash and case['executable_unchanged'] and case['remaining_participants']==0 for case in summary['runs'])
            item['passed'] &= observed_runtime['tested_runtime_sha256']==runtime['tested_runtime_sha256'] and summary['owned_server_stopped']
        results.append(item)
        print(json.dumps(item),flush=True)
    result={'local_only':True,'actual_capture':False,'configuration':vars(args),'executable_sha256':expected_hash,'runs':results,'passed':len(results)==args.runs and all(item['passed'] for item in results)}
    result['passed'] &= hashlib.sha256(executable.read_bytes()).hexdigest()==expected_hash
    (folder/'summary.json').write_text(json.dumps(result,indent=2),encoding='utf-8')
    print('Reports: '+str(folder),flush=True)
    return 0 if result['passed'] else 1


if __name__=='__main__':
    raise SystemExit(main())
