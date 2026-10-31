# Dual-peer stereo offer and current audio acceptance

Date: 2026-10-28, Europe/Budapest

Status: **normal UI audio enabled; repeated local 720p/1080p checks pass**

This checkpoint resolves the intermittent decoded mono failure that remained
after the scheduler and endpoint recovery work.

## Root cause

The local SFU debug trace showed:

- the published audio track was `stereo:true`;
- subscriber answers contained `a=fmtp:111 ...;stereo=1...`;
- the failure still decoded identical left/right channels.

Comparison of publisher offers found the missing link:

```text
Failed publisher offer:
a=fmtp:111 minptime=10;useinbandfec=1

Pinned subscriber answer:
a=fmtp:111 minptime=10;stereo=1;useinbandfec=1
```

The pinned Rust engine applied stereo SDP munging to subscriber answers and
single-peer offers, but **not to offers from the separate publisher transport**.
The app uses `single_peer_connection=false`, so the publisher could encode Opus
without a stereo channel request even though the source and track metadata were
stereo.

## SDK fix

`scripts/prepare_livekit_engine.py` now applies the existing, section-scoped
stereo fmtp munging to every offer after the single-peer-only branch. The change:

- adds `stereo=1` only to active Opus audio sections;
- does not alter `sprop-stereo`;
- preserves explicit `stereo=0`;
- is idempotent;
- retains the existing answer munging.

The patched SDK was rebuilt and independently verified.

New publisher offers now contain:

```text
a=fmtp:111 minptime=10;stereo=1;useinbandfec=1
```

Evidence:

`artifacts/20261028-154119-remote-av/720/server/server.log`

## Test controls

`test_remote_av.py --disable-audio-retry` disables the application-level stereo
resubscription. This proves the SDK SDP repair rather than relying on a retry to
mask the defect.

## Audio results

Five consecutive untraced 720p runs with retries disabled:

| Evidence | Stereo | Longest quiet | Result |
| --- | --- | ---: | --- |
| `154159` | Pass | 1.69 ms | Pass |
| `154224` | Pass | 0.02 ms | Pass |
| `154248` | Pass | 0.02 ms | Pass |
| `154313` | Pass | 1.75 ms | Pass |
| Debug `154119` | Pass | 0.02 ms | Pass |

Two consecutive untraced 1080p runs with retries disabled:

| Evidence | Stereo | Longest quiet | Conversion average |
| --- | --- | ---: | ---: |
| `154352` | Pass | 0.02 ms | 14.62 ms |
| `154417` | Pass | 0.02 ms | 15.33 ms |

Both 1080p runs passed their complete A/V checks. The better conversion times
also reduced raw replacement pressure in these samples.

## Normal UI audio

Synchronized remote audio is enabled by default in the normal application.
Automated video-only tests explicitly set:

```text
GOODSHARING_TEST_DISABLE_AV_PLAYBACK=1
```

The owned two-client launcher smoke test passes with both normal clients showing
audio/video receiving enabled and no automatic capture.

## Validation

- CTest: 22/22.
- Native stereo negotiation tests: 4/4.
- Native shutdown cancellation test: 1/1.
- Installed SDK/runtime provenance verification: pass.

Current application:

```text
build/bin/GoodSharing.exe
SHA-256 DF65BF755347B256CEFDE51D5B671DC695AA8DABAAEC591684808B8E9E39DEB2
```

Current runtime:

```text
build/bin/livekit.dll
SHA-256 FBEDCAFD56C5D4F007B1EEE660AFA2C09B4B5CD247922C7B7FD23AF3A8D036DF

build/bin/livekit_ffi.dll
SHA-256 8BBF8C07FD4D99614E2FCFB51459F8ACAA2C3EB0A12B7CCFCC34B00667B1017C
```

## Interpretation

- The split audio/video subscription was not inherently mono.
- The separate publisher transport was omitting the Opus stereo request.
- Application-level resubscription is retained as a bounded compatibility
  fallback, but current stereo tests pass with retries disabled.
- Local 720p and 1080p audio continuity now pass repeated tests.

## Remaining scope

- Separate-PC, physical speaker/display synchronization, endpoint device changes,
  long soak and Internet jitter remain unverified.
- Video cadence still has occasional source-position skips and the 1080p
  conversion budget remains close to the 60 FPS limit.

