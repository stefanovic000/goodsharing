"""Fail closed on a stale/unpatched installed SDK before building the app."""
import hashlib
import json
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SDK = ROOT / '.deps/livekit-patched'


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def verify():
    infos = {}
    for name, patch in [('wrapper-patch-info', 'livekit-cpp-1.12.3-stereo-build.patch'), ('engine-build-info', 'livekit-rust-a6f93a16-shutdown.patch')]:
        info = json.loads((SDK / (name + '.json')).read_text(encoding='utf-8-sig'))
        infos[name] = info
        if info['patch_sha256'] != digest(ROOT / 'patches' / patch):
            raise RuntimeError('Stale SDK patch: ' + name)
        if name == 'engine-build-info' and 'patches/preencoded_chain.h' not in info.get('patch_inputs_sha256', {}):
            raise RuntimeError('Engine missing preencoded chain input provenance')
        if name == 'engine-build-info' and 'patches/strict_frame_cryptor.h' not in info.get('patch_inputs_sha256', {}):
            raise RuntimeError('Engine missing strict cryptor input provenance')
        if name == 'engine-build-info' and 'patches/closed_frame_transformer.h' not in info.get('patch_inputs_sha256', {}):
            raise RuntimeError('Engine missing early closed receiver input provenance')
        if name == 'engine-build-info' and 'patches/webrtc-89d790b-frame-crypto.patch' not in info.get('patch_inputs_sha256', {}):
            raise RuntimeError('Engine missing cryptor parser input provenance')
        if name == 'engine-build-info':
            for required in ('patches/receiver_media_clock.h', 'patches/webrtc-89d790b-receiver-clock.patch', 'scripts/receiver_timing_patch.py', 'scripts/prepare_receiver_clock.py', 'patches/source_audio_clock.h', 'scripts/audio_timing_patch.py', 'scripts/sender_audio_timing_patch.py'):
                if required not in info.get('patch_inputs_sha256', {}):
                    raise RuntimeError('Engine missing original source capture timing provenance: ' + required)
            expected_source = info.get('repaired_cryptor_source_sha256')
            if not expected_source or digest(ROOT / '.deps/sources/webrtc-89d790b/frame_crypto_transformer.fixed.cc') != expected_source:
                raise RuntimeError('Engine and native test cryptor sources differ')
            if info.get('webrtc_removed_cryptor_object') != 'obj/api/crypto/frame_crypto_transformer/frame_crypto_transformer.obj':
                raise RuntimeError('Engine lacks explicit original cryptor object replacement')
            if info.get('webrtc_removed_clock_objects') != ['obj/audio/audio/channel_receive.obj', 'obj/video/video/rtp_video_stream_receiver2.obj']:
                raise RuntimeError('Engine lacks explicit original receiver object replacement')
        for relative, expected in info.get('patch_inputs_sha256', {}).items():
            if digest(ROOT / relative) != expected:
                raise RuntimeError('Stale SDK patch input: ' + relative)
        for relative, expected in info.get('files', {}).items():
            if digest(SDK / relative) != expected:
                raise RuntimeError('Installed SDK artifact mismatch: ' + relative)
    return infos


def runtime_info():
    infos = verify()
    hashes = {}
    for info in infos.values():
        for relative, expected in info.get('files', {}).items():
            if not relative.startswith('bin/'):
                continue
            actual = digest(ROOT / 'build' / relative)
            if actual != expected:
                raise RuntimeError('Probe runtime is stale; run scripts/build.ps1: ' + relative)
            hashes[relative] = actual
    return {'installed_sdk': infos, 'tested_runtime_sha256': hashes}


def main():
    verify()
    print('Verified patched LiveKit wrapper and engine provenance')


if __name__ == '__main__':
    main()
