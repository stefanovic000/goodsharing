"""Retain license evidence for the pinned build; this is not a release clearance."""
import hashlib
import json
from pathlib import Path
import re
import shutil

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / 'third_party'
SOURCES = ROOT / '.deps/sources'


def retain(source, destination):
    target = OUT / destination
    target.parent.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(source, target)
    return {'path': str(target.relative_to(ROOT)).replace('\\', '/'), 'sha256': hashlib.sha256(target.read_bytes()).hexdigest()}


def notices(folder, destination):
    result = []
    for path in sorted(folder.iterdir()):
        if path.is_file() and path.name.upper().startswith(('LICENSE', 'LICENCE', 'COPYING', 'COPYRIGHT', 'NOTICE')):
            result.append(retain(path, Path(destination) / path.name))
    return result


def main():
    OUT.mkdir(exist_ok=True)
    records = []
    for name, path in [('livekit-cpp', SOURCES / 'cpp-80130a2/client-sdk-cpp-80130a2'), ('livekit-rust', SOURCES / 'rust-a6f93a16/rust-sdks-a6f93a16')]:
        records.append({'component': name, 'notices': notices(path, 'licenses/' + name)})
    records.append({'component': 'webrtc-89d790b', 'notices': [retain(ROOT / '.deps/webrtc-89d790b/win-x64-release/LICENSE.md', 'licenses/webrtc/THIRD_PARTY_LICENSES.md')]})
    records.append({'component': 'libyuv-91727608', 'notices': notices(SOURCES / 'rust-a6f93a16/rust-sdks-a6f93a16/yuv-sys/libyuv', 'licenses/libyuv')})
    retain(ROOT / '.deps/webrtc-89d790b/win-x64-release/args.gn', 'build-evidence/webrtc-args.gn')
    # GN selects Chrome branding; retain that actual config rather than one of
    # the unused older Chromium configurations included in the same archive.
    config = 'include/third_party/ffmpeg/chromium/config/Chrome/win/x64/config.h'
    retain(ROOT / '.deps/webrtc-89d790b/win-x64-release' / config, 'build-evidence/ffmpeg-Chrome-win-x64-config.h')
    soxr = SOURCES / 'rust-a6f93a16/rust-sdks-a6f93a16/soxr-sys/src'
    headers = []
    for path in sorted(soxr.iterdir()):
        if path.is_file():
            text = path.read_text(encoding='utf-8', errors='replace')
            if text.startswith('/*') and '*/' in text:
                headers.append(path.name + '\n' + text.split('*/', 1)[0] + '*/\n')
    (OUT / 'licenses/livekit-rust/SOXR_SOURCE_HEADERS.txt').write_text('\n'.join(headers), encoding='utf-8')
    for path in sorted((ROOT / '.deps/build-livekit-wrapper/_deps').glob('*-src')):
        records.append({'component': path.name, 'notices': notices(path, 'licenses/cpp/' + path.name)})
    qt_packages = []
    for path in sorted((ROOT / '.deps/qt/6.8.3/msvc2022_64/sbom').glob('*.spdx.json')):
        data = json.loads(path.read_text(encoding='utf-8'))
        retain(path, Path('qt-sbom') / path.name)
        for license in data.get('hasExtractedLicensingInfos', []):
            text = license.get('extractedText', '')
            identifier = re.sub(r'[^A-Za-z0-9_.-]', '_', license['licenseId'])
            target = OUT / 'licenses/qt' / (identifier + '.txt')
            target.parent.mkdir(parents=True, exist_ok=True)
            if target.exists() and target.read_text(encoding='utf-8') != text:
                raise RuntimeError('Conflicting Qt license identifier: ' + identifier)
            target.write_text(text, encoding='utf-8')
        for package in data.get('packages', []):
            qt_packages.append({key: package.get(key) for key in ('name', 'versionInfo', 'licenseDeclared', 'licenseConcluded', 'copyrightText')})
    registry = ROOT / '.tools/cargo/registry/src'
    cargo = {}
    for line in (ROOT / 'artifacts/rust-licenses-tree.txt').read_text(encoding='utf-8-sig').splitlines():
        left, _, license = line.partition('|')
        match = re.match(r'^(\S+) v(\S+)', left)
        if not match:
            continue
        name, version = match.groups()
        key = name + '-' + version
        if key in cargo:
            continue
        local = re.search(r'\((C:.*?)\)', left)
        paths = [Path(local[1])] if local else list(registry.glob('*/' + key))
        cargo[key] = {'name': name, 'version': version, 'license_declared': license.removesuffix(' (*)'), 'notices': notices(paths[0], 'licenses/rust/' + key) if paths else [], 'manifest_found': bool(paths)}
    result = {'status': 'development inventory; distribution review remains required', 'scope': 'Cargo normal/build edges for Windows x64; installed Qt SBOMs include tools and unused modules', 'sources': records, 'qt_packages': qt_packages, 'rust_packages': list(cargo.values())}
    (OUT / 'inventory.json').write_text(json.dumps(result, indent=2), encoding='utf-8')
    print(json.dumps({'rust_packages': len(cargo), 'qt_packages': len(qt_packages), 'missing_cargo_manifests': [k for k, v in cargo.items() if not v['manifest_found']], 'inventory': str(OUT / 'inventory.json')}))


if __name__ == '__main__':
    main()
