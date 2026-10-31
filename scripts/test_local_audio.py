"""Verify application isolation and desktop feedback exclusion with owned tones."""
import json
import os
from pathlib import Path
import subprocess
import time

ROOT = Path(__file__).resolve().parents[1]


def main():
    folder = ROOT / "artifacts" / (time.strftime("%Y%m%d-%H%M%S") + "-audio")
    folder.mkdir(parents=True)
    env = os.environ.copy()
    env["PATH"] = str(ROOT / ".deps/qt/6.8.3/msvc2022_64/bin") + os.pathsep + env["PATH"]
    env["QT_LOGGING_RULES"] = "qt.multimedia.ffmpeg=false"
    fixtures = []
    results = []
    try:
        for hz in (440, 880):
            p = subprocess.Popen([str(ROOT / "build/bin/goodsharing_fixture.exe"), "--tone", str(hz), "--seconds", "25", "--no-window"], env=env, stdout=subprocess.PIPE, stderr=(folder / (str(hz) + ".log")).open("w"), text=True, creationflags=subprocess.CREATE_NO_WINDOW)
            fixtures.append(p)
        # Fixture stdout is a ready record; wait through polling to keep startup bounded.
        time.sleep(2)
        if any(p.poll() is not None for p in fixtures):
            raise RuntimeError("Audio fixture startup failed; inspect logs")
        for mode in ("application", "desktop"):
            report = folder / (mode + ".json")
            with (folder / (mode + ".log")).open("w") as log:
                c = subprocess.run([str(ROOT / "build/bin/goodsharing_audio_probe.exe"), "--mode", mode, "--pid", str(fixtures[0].pid), "--seconds", "5", "--report", str(report)], env=env, stdout=log, stderr=subprocess.STDOUT, timeout=20, creationflags=subprocess.CREATE_NO_WINDOW)
            data = json.loads(report.read_text()) if report.exists() else {"error": "Missing report"}
            tones = data.get("tone_amplitude", {})
            wanted = tones.get("440", 0) > 200
            passed = c.returncode == 0 and wanted and (tones.get("880", 0) < tones.get("440", 0) * .03 if mode == "application" else tones.get("880", 0) > 200 and tones.get("660", 0) < min(tones.get("440", 0), tones.get("880", 0)) * .03)
            result = {"mode": mode, "passed": passed, "exit_code": c.returncode, "report": str(report.relative_to(ROOT)), "tone_amplitude": tones}
            results.append(result)
            print(json.dumps(result), flush=True)
    finally:
        for p in fixtures:
            if p.poll() is None:
                p.terminate()
                try:
                    p.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    p.kill()
                    p.wait(timeout=5)
        (folder / "summary.json").write_text(json.dumps({"runs": results, "owned_fixtures_stopped": all(p.poll() is not None for p in fixtures), "saved_audio_content": False}, indent=2))
        print("Reports: " + str(folder), flush=True)
    return 0 if len(results) == 2 and all(r["passed"] for r in results) else 1


if __name__ == "__main__":
    raise SystemExit(main())
