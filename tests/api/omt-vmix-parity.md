# OMT and VMIX Parity Tests

Run the focused API tests from a configured FFmpeg build:

```sh
sh tests/api/run-omt-vmix-parity.sh
```

This runs the existing FATE targets `fate-api-omt` and `fate-api-vmix`. The
build must enable the OMT demuxer and muxer, TCP protocol, and VMIX encoder and
decoder. No external libomt/libvmx binary is required by these tests.

## Current Coverage

`api-omt-test.c` exercises video and audio packet handling, timeouts, receiver
fan-out and slow-receiver isolation, discovery registration and resolution,
initial redirects, OMTInfo, connection and per-frame XML metadata, and tally.

`api-vmix-test.c` checks a reference decoder packet, malformed input, 10-bit
P216 decode, and encoder profile/quality headers and roundtrips for YUV422P,
UYVY422, YUYV422, NV12, YUV420P, and BGR0.

## Remaining Parity Work

- Encoder roundtrips use the native VMIX decoder; they do not prove encoded
  packets match libvmx. Add reference-generated input/output vectors or an
  optional differential test against a pinned libvmx build.
- OMT tests use local protocol fixtures; they do not run interoperability tests
  against libomt/libomtnet peers.
- Alpha, 10-bit encoding, interlace, preview, live quality changes, and live
  redirect updates are not currently covered as supported end-to-end features.
- IPv6 DNS-SD and hosting a Discovery Server are not covered.

Reference projects:

- https://github.com/openmediatransport/libvmx
- https://github.com/openmediatransport/libomtnet
- https://github.com/openmediatransport/libomt