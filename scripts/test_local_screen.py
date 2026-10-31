"""Capture only this script's owned moving-pattern window using WGC."""
import json
import os
from pathlib import Path
import subprocess
import time

ROOT = Path(__file__).resolve().parents[1]


def main():
    folder = ROOT / "artifacts" / (time.strftime("%Y%m%d-%H%M%S") + "-screen")
    folder.mkdir(parents=True)
    env = os.environ.copy()
    env["PATH"] = str(ROOT / ".deps/qt/6.8.3/msvc2022_64/bin") + os.pathsep + env["PATH"]
    ready = folder / "fixture.json"
    fixture = None
    result = {"passed": False}
    try:
        with (folder / "fixture.log").open("w") as log:
            fixture = subprocess.Popen([str(ROOT / "build/bin/goodsharing_fixture.exe"), "--seconds", "15", "--ready-file", str(ready)], env=env, stdout=log, stderr=subprocess.STDOUT)
            deadline = time.monotonic() + 8
            while not ready.exists():
                if fixture.poll() is not None or time.monotonic() > deadline:
                    raise RuntimeError("Fixture readiness failed")
                time.sleep(.1)
            info = json.loads(ready.read_text())
            time.sleep(.5)
            with (folder / "capture.log").open("w") as output:
                completed = subprocess.run([str(ROOT / "build/bin/goodsharing_screen_probe.exe"), "--pid", str(fixture.pid), "--hwnd", info["hwnd"], "--report", str(folder / "screen.json")], env=env, stdout=output, stderr=subprocess.STDOUT, timeout=12, creationflags=subprocess.CREATE_NO_WINDOW)
            result = {"passed": completed.returncode == 0, "exit_code": completed.returncode, "report": str((folder / "screen.json").relative_to(ROOT))}
            print(json.dumps(result))
    finally:
        if fixture is not None and fixture.poll() is None:
            fixture.terminate()
            try:
                fixture.wait(timeout=5)
            except subprocess.TimeoutExpired:
                fixture.kill()
                fixture.wait(timeout=5)
        result["owned_fixture_stopped"] = fixture is not None and fixture.poll() is not None
        (folder / "summary.json").write_text(json.dumps(result, indent=2))
        print("Reports: " + str(folder))
    return 0 if result["passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
