"""One reviewed wire extension shared by both pinned SDK build preparations."""
E2EE_ORIGINAL = '''message E2eeStateChanged {
  required string participant_identity = 1; // Using sid instead of identity for ffi communication
  required EncryptionState state = 2;
}'''
E2EE_PATCHED = E2EE_ORIGINAL[:-1] + '''  // GoodSharing: bind readiness/failure to an individual published track.
  optional string track_sid = 3;
}'''


def patch_room_protocol(content):
    text = content.decode('utf-8')
    if text.count(E2EE_ORIGINAL) != 1:
        raise RuntimeError('Unexpected pinned encryption event protocol')
    return text.replace(E2EE_ORIGINAL, E2EE_PATCHED).encode('utf-8')
