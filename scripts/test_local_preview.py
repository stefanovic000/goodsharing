"""Smoke-test QML rendering with this script's own capture window."""
import json
import argparse
import os
from pathlib import Path
import subprocess
import time

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--scenario", choices=["normal", "restart", "source-exit"], default="normal")
    args = parser.parse_args()
    folder = ROOT / "artifacts" / (time.strftime("%Y%m%d-%H%M%S") + "-preview-" + args.scenario)
    folder.mkdir(parents=True)
    env = os.environ.copy()
    env["PATH"] = str(ROOT / ".deps/qt/6.8.3/msvc2022_64/bin") + os.pathsep + env["PATH"]
    env["QML_IMPORT_PATH"] = str(ROOT / ".deps/qt/6.8.3/msvc2022_64/qml")
    ready = folder / "fixture.json"
    fixture = None
    result = {"passed": False}
    try:
        with (folder / "fixture.log").open("w") as log:
            fixture = subprocess.Popen([str(ROOT / "build/bin/goodsharing_fixture.exe"), "--seconds", "3" if args.scenario == "source-exit" else "18", "--ready-file", str(ready)], env=env, stdout=log, stderr=subprocess.STDOUT)
            deadline = time.monotonic() + 8
            while not ready.exists():
                if fixture.poll() is not None or time.monotonic() > deadline:
                    raise RuntimeError("Fixture readiness failed")
                time.sleep(.1)
            info = json.loads(ready.read_text())
            with (folder / "app.log").open("w") as output:
                completed = subprocess.run([str(ROOT / "build/bin/GoodSharing.exe"), "--smoke-scenario", args.scenario, "--smoke-pid", str(fixture.pid), "--smoke-hwnd", info["hwnd"], "--report", str(folder / "preview.json"), "--screenshot", str(folder / "preview.png")], env=env, stdout=output, stderr=subprocess.STDOUT, timeout=15)
            result = {"passed": completed.returncode == 0, "exit_code": completed.returncode, "report": str((folder / "preview.json").relative_to(ROOT)), "screenshot": str((folder / "preview.png").relative_to(ROOT))}
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
