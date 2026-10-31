"""Verify the separate opt-in SFU experiment; never replace the release server."""
import argparse
import hashlib
import json
from pathlib import Path
import zipfile
from prepare_clock_server import ROOT, SOURCE, ARCHIVE, ARCHIVE_SHA, PATCH, PARTICIPANT, GO_MOD_SHA, GO_SUM_SHA, patched_participant

OUTPUT = ROOT / '.deps/livekit-server-clock-experiment'
INPUTS = ('scripts/prepare_clock_server.py', 'scripts/build-clock-server.ps1', 'scripts/verify_clock_server.py')


def sha(path): return hashlib.sha256(path.read_bytes()).hexdigest()


def provenance():
    if sha(ARCHIVE) != ARCHIVE_SHA: raise RuntimeError('SFU source archive checksum mismatch')
    # Verify the complete extracted build source, including untouched files.
    with zipfile.ZipFile(ARCHIVE) as z:
        for entry in z.infolist():
            if entry.is_dir(): continue
            relative = entry.filename.removeprefix('livekit-1.13.10/')
            original = z.read(entry)
            expected = patched_participant(original.decode()).encode() if relative == PARTICIPANT else original
            if (SOURCE / relative).read_bytes() != expected: raise RuntimeError('SFU source mismatch: ' + relative)
    assert sha(SOURCE / 'go.mod') == GO_MOD_SHA and sha(SOURCE / 'go.sum') == GO_SUM_SHA
    return dict(experimental_only=True, upstream_version='1.13.10', go_version='go1.26.1 windows/amd64',
        source_archive_sha256=ARCHIVE_SHA, go_mod_sha256=GO_MOD_SHA, go_sum_sha256=GO_SUM_SHA,
        patch_sha256=sha(PATCH), participant_source_sha256=sha(SOURCE / PARTICIPANT),
        input_sha256={p: sha(ROOT / p) for p in INPUTS},
        binary_sha256=sha(OUTPUT / 'livekit-server.exe'),
        build_flags=['-mod=readonly', '-p', '2', '-trimpath', '-buildvcs=false'],
        go_test_selection='go test ./pkg/sfu/rtpstats -count=1 -v', go_top_level_tests=3)


def verify():
    info = json.loads((OUTPUT / 'build-info.json').read_text())
    if info != provenance(): raise RuntimeError('SFU experiment provenance is stale')
    return info


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--record-build', action='store_true'); args = parser.parse_args()
    if args.record_build: (OUTPUT / 'build-info.json').write_text(json.dumps(provenance(), indent=2) + '\n')
    verify()
    print('Verified isolated SFU clock experiment provenance')
