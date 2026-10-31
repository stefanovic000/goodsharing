# Channel provenance and media repeatability

Date: 2026-10-28, Europe/Budapest

Status: **diagnostics added; intermittent failures remain**

This increment follows the current media baseline in
[checkpoint 52](52-capture-diagnostics-and-current-media-baseline.md). It adds
bounded channel-aggregate measurements and a timed synthetic stereo control to
distinguish capture, transport/decoding, scheduling and device-output failures.

No audio content is retained by the new measurements. The reports contain only
whole-run aggregate signal and channel-difference values.

## Code changes

### Capture input

`CapturePublisherInput` now reports:

```text
audio_channel_difference_ratio
```

The ratio is the accumulated absolute left/right difference divided by the
average absolute signal magnitude. Distinct equal-amplitude tones produce a
positive ratio above one; identical or mono-upmixed channels produce zero.

### Decoded playout input

`RemoteAudioPlayout` now reports:

```text
decoded_channel_difference_ratio
```

This is measured on each decoded PCM block after `TrackReader` and before
scheduling or device commit.

### WASAPI commit

`WasapiPlayoutSink` now reports:

```text
submitted_channel_difference_ratio
```

This verifies the channel layout included in the actual device write.

### Timed synthetic control

`test_local_stereo.py --case timed-stereo` exercises the same unbuffered
`AudioSource::captureFrameAt()` call used by real capture while retaining the
known synthetic left/right tones.

## Current executable

```text
build/bin/GoodSharing.exe
SHA-256 E4B846AADD74C45AFBA64E2CA6D2BE7B11ACE0FEB9D43CD09CA1143E882C3AD9
```

```text
build/bin/goodsharing_stereo_probe.exe
SHA-256 72DD5B65454E448EC829AB8D9406E576081A06A22B367952E45A44A97B0C5FDF
```

All 22 CTest tests pass after these changes.

## Channel provenance results

| Evidence | Source ratio | Decoded ratio | WASAPI ratio | Longest quiet | Result |
| --- | ---: | ---: | ---: | ---: | --- |
| `041701-remote-av` | 1.25 | 1.25 | 1.25 | 0.02 ms | Pass |
| `041732-remote-av` | 1.25 | 1.25 | 1.25 | 8.35 ms | Pass |
| `041811-remote-av` | 1.25 | 0 | 0 | 8 ms | Stereo failure |
| `042211-remote-av` | 1.25 | 1.25 | 1.25 | 1,712.96 ms | Continuity failure |

The `041811` result localizes the stereo defect to the interval between:

- application capture, where channels remain distinct; and
- decoded playout input, where both channels are already identical.

It is not caused by capture downmixing or the WASAPI commit. The separate
continuity failure can occur with stereo intact.

The synthetic timed stereo control passes:

`artifacts/20261028-042017-stereo/timed-stereo.json`

Therefore `captureFrameAt()` alone is not the cause.

## Server negotiation evidence

For a passing debug run, the SFU records:

- published audio as `stereo:true`;
- `TF_STEREO` and `TF_NO_DTX` audio features;
- an Opus subscriber codec with `stereo=1`;
- a received subscriber answer whose Opus fmtp contains `stereo=1`.

The trace does not yet explain why a different run decodes mono before playout.
The next failing stereo sample must be captured with server debug enabled so its
track metadata, DownTrack codec selection and subscriber answer can be compared
directly with a passing sample.

## Continuity evidence

The `042211` run:

- retained stereo end to end;
- recorded a 1,712.96 ms quiet interval;
- sustained two low-headroom resets;
- recorded one endpoint-gap reset;
- recorded no presentation resets;
- estimated 98.77% whole-window PCM coverage.

This is a separate scheduling/device continuity problem from the complete
channel loss. It must not be hidden by lowering the tone threshold or extending
queues.

## Next steps

1. Repeat server-debug 720p runs until a stereo failure is captured.
2. Compare the failing subscriber SDP and receiver stream selection against a
   passing same-hash run.
3. Determine whether the failure occurs before or after `AudioStream::read()`.
4. Test combined versus split audio/video subscriptions with the known stereo
   source while preserving persistent-audio quality handover.
5. Capture first-fault timing on a long-gap run and correlate low-headroom,
   endpoint and presentation events.
6. Do not enable normal UI audio until repeated same-hash tests pass.

