"""Prove unsafe framing fails, safe framing works, and checks reject wrong IDs."""
import json
from pathlib import Path
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parents[1]


def scenario(name, extra, expected_exit):
    command = [sys.executable, str(ROOT / "scripts/test_local_media.py"), "--seconds", "6", "--encryption", "shared-key", "--encoder", "mf", "--record-encoded", *extra]
    completed = subprocess.run(command, cwd=ROOT, capture_output=True, text=True, timeout=90)
    print(completed.stdout, end="", flush=True)
    if completed.returncode != expected_exit:
        raise RuntimeError(name + ": unexpected verification result; " + completed.stderr)
    report_line = next(line for line in completed.stdout.splitlines() if line.startswith("Reports: "))
    folder = Path(report_line.removeprefix("Reports: "))
    summary = json.loads((folder / "summary.json").read_text())
    media = json.loads((folder / "shared-key.json").read_text())["media"]
    run = summary["runs"][0]
    if not run["independent_decode_passed"] or not summary["owned_server_stopped"]:
        raise RuntimeError(name + ": independent decode or owned-process cleanup failed")
    tracks = media["inbound"]
    if name == "legacy_framing":
        if run["media_correctness_passed"] or len(tracks) != 2 or any(t["frames"] for t in tracks):
            raise RuntimeError("Legacy framing did not reproduce its known failure")
    elif name == "safe_framing":
        if not run["media_correctness_passed"] or len(tracks) != 2 or any(t["frames"] < 200 or t["frames"] != t["pixel_markers"] for t in tracks):
            raise RuntimeError("Safe framing did not preserve every decoded marker")
    else:
        if run["media_correctness_passed"] or len(tracks) != 2 or any(t["frames"] - t["pixel_markers"] != 1 for t in tracks):
            raise RuntimeError("Verification did not catch exactly one deliberately wrong ID per track")
    return {"scenario": name, "passed": True, "report": str(folder.relative_to(ROOT) / "summary.json")}


def main():
    results = [
        scenario("legacy_framing", ["--metadata-codec", "legacy", "--unsafe-timestamp"], 1),
        scenario("safe_framing", ["--metadata-codec", "safe", "--unsafe-timestamp"], 0),
        scenario("wrong_frame_id", ["--metadata-codec", "safe", "--tamper-frame-id-at", "120"], 1),
    ]
    path = ROOT / "artifacts" / (time.strftime("%Y%m%d-%H%M%S") + "-metadata-regression.json")
    path.write_text(json.dumps({"passed": True, "cases": results}, indent=2))
    print("Metadata regression passed: " + str(path), flush=True)


if __name__ == "__main__":
    main()
