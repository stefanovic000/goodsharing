"""Own one loopback SFU and two interactive clients; capture starts only by click."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import secrets
import subprocess
import time
from local_media_fixture import LocalMediaFixture, ROOT
from verify_livekit_sdk import runtime_info
from test_capture_sharing import stop


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--smoke',action='store_true',help='Join both normal UI clients, verify no capture starts, then close only these test clients')
    args=parser.parse_args()
    app=ROOT/'build/bin/GoodSharing.exe'
    runtime=runtime_info()
    folder=ROOT/'artifacts'/(time.strftime('%Y%m%d-%H%M%S')+'-'+secrets.token_hex(3)+'-local-demo')
    folder.mkdir()
    fixture=LocalMediaFixture(folder/'server')
    children=[];logs=[]
    result=dict(local_only=True,loopback_ice_only=True,client_loopback_enabled=True,smoke=args.smoke,automatic_capture=False,
        app_sha256=hashlib.sha256(app.read_bytes()).hexdigest(),runtime=runtime,normal_ui_clients=True,
        unrelated_processes_stopped=False,clients=[])
    try:
        fixture.start(sfu_rtcp_report_ms=500,clock_server=True)
        result['server_runtime']=fixture.server_info
        room='local-demo-'+secrets.token_hex(6)
        key=secrets.token_hex(32)
        for role,identity in (('Client-1','demo-1'),('Client-2','demo-2')):
            env=os.environ.copy()
            for name in list(env):
                if name.startswith(('GOODSHARING_TEST_','GOODSHARING_DEMO_')):env.pop(name)
            env['PATH']=str(ROOT/'.deps/qt/6.8.3/msvc2022_64/bin')+os.pathsep+env.get('PATH','')
            env['QML_IMPORT_PATH']=str(ROOT/'.deps/qt/6.8.3/msvc2022_64/qml')
            env.update(GOODSHARING_LOCAL_LOOPBACK='1',GOODSHARING_DEMO_URL='ws://127.0.0.1:8765',
                GOODSHARING_DEMO_TOKEN=fixture.token(identity,room,can_publish=True),GOODSHARING_DEMO_KEY=key)
            command=[str(app),'--local-demo','--demo-role',role]
            if args.smoke:command+=['--demo-smoke-report',str(folder/(role.lower()+'.json'))]
            log=(folder/(role.lower()+'.log')).open('x');logs.append(log)
            child=subprocess.Popen(command,env=env,stdout=log,stderr=subprocess.STDOUT);children.append(child)
            result['clients'].append(dict(role=role,pid=child.pid))
        # The SFU never receives this key, and no secret is written to reports.
        del key,env
        print('Two local clients opened. Choose a source and click Share in either window; capture never starts automatically.',flush=True)
        deadline=time.monotonic()+25 if args.smoke else None
        while any(child.poll() is None for child in children):
            if deadline and time.monotonic()>deadline:raise TimeoutError('Local UI smoke test timed out')
            time.sleep(.2)
        result['exit_codes']=[child.returncode for child in children]
        result['passed']=all(code==0 for code in result['exit_codes'])
        if args.smoke:
            result['ui_checks']=[json.loads((folder/(role+'.json')).read_text()) for role in ('client-1','client-2')]
            result['passed'] &= all(row['passed'] and not row['capture_active'] and not row['share_active'] and not row['watching'] for row in result['ui_checks'])
    except Exception as error:
        result.update(passed=False,error=str(error));print(str(error),flush=True)
    finally:
        for child in reversed(children):stop(child)
        for log in logs:log.close()
        result['owned_clients_stopped']=all(child.poll() is not None for child in children)
        result['owned_server_stopped']=fixture.stop()
        result['passed']=bool(result.get('passed')) and result['owned_clients_stopped'] and result['owned_server_stopped']
        (folder/'summary.json').write_text(json.dumps(result,indent=2))
        print('Reports: '+str(folder),flush=True)
    return 0 if result['passed'] else 1


if __name__=='__main__':raise SystemExit(main())
