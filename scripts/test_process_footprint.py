"""Cross-check resource units against one owned Python child's known workload."""
import json
from pathlib import Path
import subprocess
import sys
import time
from process_footprint import ProcessFootprint


def main():
    program = '''
import json,time
time.sleep(.5)
memory=bytearray(48*1024*1024)
for page in range(0,len(memory),4096): memory[page]=1
time.sleep(.5)
until=time.monotonic()+2
value=0
while time.monotonic()<until: value=(value+1)%1000003
print(json.dumps({'cpu_seconds':time.process_time(),'retained_bytes':len(memory)}),flush=True)
time.sleep(.5)
'''
    with subprocess.Popen([sys.executable, '-c', program], stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                          text=True, creationflags=subprocess.CREATE_NO_WINDOW) as child:
        profile = None
        try:
            profile = ProcessFootprint(child, 'sampler self-check: known 48 MiB allocation and CPU work').start()
            output, error = child.communicate(timeout=10)
        finally:
            if child.poll() is None:
                child.kill()
                child.wait(timeout=5)
            if profile:
                result = profile.stop()
    native = json.loads(output)
    samples = result['samples']
    # The child sleeps at both ends, so sampled CPU should closely match its
    # own independent time.process_time() reading after the busy section.
    cpu = samples[-1]['cpu_s'] - samples[0]['cpu_s']
    private_delta = max(s['private_bytes'] for s in samples) - min(s['private_bytes'] for s in samples)
    success = child.returncode == 0 and not error and not result['errors'] and len(samples) >= 8
    success &= abs(cpu - native['cpu_seconds']) < .15 and cpu > 0
    success &= private_delta >= 40 * 1024 * 1024
    success &= all(b['cpu_s'] >= a['cpu_s'] for a, b in zip(samples, samples[1:]))
    result.update({'passed': bool(success), 'child': native, 'sampled_cpu_seconds': cpu,
                   'private_delta_bytes': private_delta})
    report = Path(__file__).resolve().parents[1] / 'artifacts' / (time.strftime('%Y%m%d-%H%M%S') + '-footprint-self-check.json')
    report.parent.mkdir(exist_ok=True)
    report.write_text(json.dumps(result, indent=2), encoding='utf-8')
    print(json.dumps({'passed': bool(success), 'report': str(report), 'sampled_cpu_seconds': cpu,
                      'child_cpu_seconds': native['cpu_seconds'], 'private_delta_bytes': private_delta}), flush=True)
    return 0 if success else 1


if __name__ == '__main__':
    raise SystemExit(main())
