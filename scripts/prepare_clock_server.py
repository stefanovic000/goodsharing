"""Prepare an opt-in local SFU report-interval experiment from pinned source."""
import difflib
import hashlib
from pathlib import Path
import urllib.request
import zipfile

ROOT = Path(__file__).resolve().parents[1]
ARCHIVE = ROOT / '.deps/sources/livekit-server-v1.13.10.zip'
SOURCE = ROOT / '.deps/sources/livekit-server-clock/livekit-1.13.10'
URL = 'https://codeload.github.com/livekit/livekit/zip/refs/tags/v1.13.10'
ARCHIVE_SHA = 'fb1ef71d2a339c19d80a0aba16f3378a6558f4fe2a0b6c89f718f8a0922c9de6'
GO_SUM_SHA = 'bd59b83446453df21c1e81687f321b5ae86665ee507e9a0f405d4c19651e0d85'
GO_MOD_SHA = '6521db4db45f8b0a1f8b2a314b89c4d35c2db3fd72a417036a374eec6829b2b5'
PARTICIPANT = 'pkg/rtc/participant.go'
PATCH = ROOT / 'patches/livekit-server-v1.13.10-rtcp-experiment.patch'


def digest(data): return hashlib.sha256(data).hexdigest()


def patched_participant(before):
    marker = 'func (p *ParticipantImpl) subscriberRTCPWorker() {\n'
    assert before.count(marker) == 1
    after = before.replace(marker, marker + '''\t// Opt-in synthetic fixture only; the upstream default remains three seconds.
\treportInterval := 3 * time.Second
\tif os.Getenv("GOODSHARING_TEST_SFU_RTCP_MS") == "500" {
\t\treportInterval = 500 * time.Millisecond
\t\tfmt.Fprintln(os.Stderr, "GoodSharing SFU RTCP experiment: interval_ms=500")
\t}
''')
    start = after.index(marker)
    end = after.index('\nfunc ', start + len(marker))
    worker = after[start:end]
    assert worker.count('time.Sleep(3 * time.Second)') == 1
    return after[:start] + worker.replace('time.Sleep(3 * time.Second)', 'time.Sleep(reportInterval)') + after[end:]


def main():
    if not ARCHIVE.exists():
        data = urllib.request.urlopen(URL, timeout=60).read()
        if digest(data) != ARCHIVE_SHA: raise RuntimeError('Pinned SFU source checksum mismatch')
        ARCHIVE.parent.mkdir(parents=True, exist_ok=True); ARCHIVE.write_bytes(data)
    if digest(ARCHIVE.read_bytes()) != ARCHIVE_SHA: raise RuntimeError('Pinned SFU source checksum mismatch')
    with zipfile.ZipFile(ARCHIVE) as z:
        prefix = 'livekit-1.13.10/'
        before = z.read(prefix + PARTICIPANT).decode()
        after = patched_participant(before)
        # Check every existing source against this exact recipe before writing.
        pending = []
        for entry in z.infolist():
            if entry.is_dir(): continue
            relative = entry.filename.removeprefix(prefix)
            if relative == entry.filename or '..' in Path(relative).parts: raise RuntimeError('Unexpected source archive path')
            original = z.read(entry)
            expected = after.encode() if relative == PARTICIPANT else original
            path = SOURCE / relative
            if path.exists() and path.read_bytes() not in (original, expected):
                raise RuntimeError('Unexpected local SFU source edits: ' + relative)
            pending.append((path, expected))
        assert digest(z.read(prefix + 'go.sum')) == GO_SUM_SHA
        assert digest(z.read(prefix + 'go.mod')) == GO_MOD_SHA
    for path, expected in pending:
        if not path.exists() or path.read_bytes() != expected:
            path.parent.mkdir(parents=True, exist_ok=True); path.write_bytes(expected)
    patch = ''.join(difflib.unified_diff(before.splitlines(True), after.splitlines(True),
        fromfile='a/' + PARTICIPANT, tofile='b/' + PARTICIPANT)).encode()
    if not PATCH.exists() or PATCH.read_bytes() != patch: PATCH.write_bytes(patch)
    print('Prepared pinned opt-in SFU RTCP report experiment')


if __name__ == '__main__': main()
