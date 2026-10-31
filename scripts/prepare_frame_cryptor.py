"""Minimal parser repair of the exact pinned upstream cryptor translation unit."""
import difflib
import hashlib
from pathlib import Path
import re
import urllib.request

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / '.deps/sources/webrtc-89d790b/frame_crypto_transformer.cc'
SOURCE_SHA256 = 'a0ad0713525437fd53dd23127e10a093442a4e7f827f22bd0b54907626c582da'
SOURCE_URL = 'https://raw.githubusercontent.com/webrtc-sdk/webrtc/89d790b/api/crypto/frame_crypto_transformer.cc'
HEADER_SHA256 = 'c9215ff1d839adce5147f75e31f6a229cc36d2cfaef6bbfe44aeab19f74f84f9'

def patched_source():
    header = ROOT / '.deps/webrtc-89d790b/win-x64-release/include/api/crypto/frame_crypto_transformer.h'
    if hashlib.sha256(header.read_text(encoding='utf-8').encode()).hexdigest() != HEADER_SHA256:
        raise RuntimeError('Pinned cryptor header ABI/source mismatch')
    if not SOURCE.exists():
        data = urllib.request.urlopen(SOURCE_URL, timeout=30).read()
        if hashlib.sha256(data).hexdigest() != SOURCE_SHA256:
            raise RuntimeError('Pinned cryptor download checksum mismatch')
        SOURCE.parent.mkdir(parents=True, exist_ok=True)
        SOURCE.write_bytes(data)
    if hashlib.sha256(SOURCE.read_bytes()).hexdigest() != SOURCE_SHA256:
        raise RuntimeError('Pinned cryptor source checksum mismatch')
    before = SOURCE.read_text(encoding='utf-8')
    after = before
    def replace(old, new, count=1):
        nonlocal after
        if after.count(old) != count: raise RuntimeError('Unexpected pinned cryptor context: ' + old[:70])
        after = after.replace(old, new)

    replace('uint8_t get_unencrypted_bytes(', 'size_t get_unencrypted_bytes(')
    replace('  uint8_t unencrypted_bytes = 0;', '  size_t unencrypted_bytes = 0;')
    replace('  uint8_t unencrypted_bytes = get_unencrypted_bytes(frame.get(), type_);',
            '  const size_t unencrypted_bytes = get_unencrypted_bytes(frame.get(), type_);', 2)
    replace('        for (const auto& index : nalu_indices) {\n          std::span<const uint8_t> slice =',
            '        for (const auto& index : nalu_indices) {\n          if (index.payload_size == 0) continue;\n          std::span<const uint8_t> slice =', 2)
    # Sender arithmetic must also reject an incomplete codec header.
    marker='  const size_t unencrypted_bytes = get_unencrypted_bytes(frame.get(), type_);'
    first = after.index(marker)
    after = after[:first] + after[first:].replace(marker, marker + '''
  if (unencrypted_bytes > data_in.size()) {
    if (last_enc_error_ != FrameCryptionState::kEncryptionFailed) {
      last_enc_error_ = FrameCryptionState::kEncryptionFailed;
      onFrameCryptionStateChanged(last_enc_error_);
    }
    return;
  }''', 1)
    normalize = '''  // H.264/H.265 escaping covers ciphertext, IV and trailer. Normalize the
  // complete encrypted tail BEFORE reading any trailer/IV offsets.
  auto malformed = [this]() {
    if (last_dec_error_ != FrameCryptionState::kDecryptionFailed) {
      last_dec_error_ = FrameCryptionState::kDecryptionFailed;
      onFrameCryptionStateChanged(last_dec_error_);
    }
  };
  if (unencrypted_bytes > data_in.size()) { malformed(); return; }
  webrtc::Buffer encrypted_buffer(data_in.data() + unencrypted_bytes,
                                  data_in.size() - unencrypted_bytes);
  if (FrameIsH264(frame.get(), type_) && NeedsRbspUnescaping(encrypted_buffer)) {
    encrypted_buffer.SetData(H264::ParseRbsp(encrypted_buffer));
#ifdef RTC_ENABLE_H265
  } else if (FrameIsH265(frame.get(), type_) && NeedsRbspUnescaping(encrypted_buffer)) {
    encrypted_buffer.SetData(H265::ParseRbsp(encrypted_buffer));
#endif
  }
  // AES-GCM requires a 16-byte tag, its IV and the two-byte trailer.
  if (encrypted_buffer.size() < 16 + getIvSize() + 2) { malformed(); return; }
'''
    second=after.index(marker, after.index('void FrameCryptorTransformer::decryptFrame('))
    after=after[:second]+after[second:].replace(marker, marker+'\n'+normalize,1)
    replace('  frame_trailer[0] = data_in[data_in.size() - 2];\n  frame_trailer[1] = data_in[data_in.size() - 1];',
            '  frame_trailer[0] = encrypted_buffer[encrypted_buffer.size() - 2];\n  frame_trailer[1] = encrypted_buffer[encrypted_buffer.size() - 1];')
    begin=after.index('  webrtc::Buffer iv = webrtc::Buffer::CreateUninitializedWithSize(ivLength);',after.index('void FrameCryptorTransformer::decryptFrame('))
    end=after.index('  webrtc::Buffer encrypted_payload =', begin)
    after=after[:begin]+'''  webrtc::Buffer iv = webrtc::Buffer::CreateUninitializedWithSize(ivLength);
  for (size_t i = 0; i < ivLength; i++) {
    iv[i] = encrypted_buffer[encrypted_buffer.size() - 2 - ivLength + i];
  }

'''+after[end:]
    # Upstream INFO diagnostics include key bytes. Never retain those logs.
    statements=list(re.finditer(r'RTC_LOG\([^;]+;',after,re.DOTALL))
    sensitive=[m for m in statements if 'to_uint8_list(' in m.group() or 'to_hex(' in m.group()]
    if len(sensitive)!=3: raise RuntimeError('Unexpected sensitive upstream diagnostics')
    for m in reversed(sensitive):
        after=after[:m.start()]+'RTC_LOG(LS_VERBOSE) << "Cryptor diagnostic values redacted";'+after[m.end():]
    after += '\n// Link anchor: require this repaired object instead of the archive copy.\nnamespace goodsharing { void requirePatchedFrameCryptor() {} }\n'
    patch=ROOT/'patches/webrtc-89d790b-frame-crypto.patch'
    data=''.join(difflib.unified_diff(before.splitlines(True),after.splitlines(True),fromfile='a/api/crypto/frame_crypto_transformer.cc',tofile='b/api/crypto/frame_crypto_transformer.cc')).encode()
    if not patch.exists() or patch.read_bytes()!=data: patch.write_bytes(data)
    return after

def main():
    output=SOURCE.with_name('frame_crypto_transformer.fixed.cc')
    data=patched_source().encode()
    if not output.exists() or output.read_bytes()!=data: output.write_bytes(data)
    print('Pinned cryptor parser source prepared: ' + hashlib.sha256(data).hexdigest())

if __name__=='__main__': main()
