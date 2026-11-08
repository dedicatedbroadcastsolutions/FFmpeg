# OMT and VMIX Parity Tests

Run the focused API tests from a configured FFmpeg build:

```sh
sh tests/api/run-omt-vmix-parity.sh
```

This runs the existing FATE targets `fate-api-omt` and `fate-api-vmix`. The
build must enable the OMT demuxer and muxer, TCP protocol, and VMIX encoder and
decoder. No external libomt/libvmx binary is required by these correctness tests.

## Reference Benchmark

Build pinned Linux references and run repeated side-by-side measurements:

```sh
sh tests/api/build-omt-vmix-reference.sh
sh tests/api/run-omt-vmix-benchmark.sh
```

Use `BENCH_MODE=vmix` or `BENCH_MODE=omt` to run only one domain. The VMIX-only
mode does not require Avahi; OMT and all modes require its Linux runtime libraries.

The benchmark runner first runs both FATE tests. It then builds the optional
comparison programs and reports raw CSV rows for VMIX encode/decode and OMT
loopback end-to-end and transport-only runs. The transport-only case sends the
same pre-encoded libvmx packet through each OMT path and requires byte-identical
delivery. VMIX checks profile/quality headers, decodes each
implementation's packets with both decoders, and reports source-relative PSNR
and cross-decoder error; it rejects a PSNR delta over 0.1 dB or cross error over
2 code values. OMT checks received frame counts, decoded dimensions, bytes, and
source-relative PSNR; it rejects native/reference PSNR differences over 0.1 dB.
Timing excludes process/build/startup costs and starts after loopback setup; each
repeat is printed separately, not averaged.

The VMIX comparison uses one thread for both implementations. The OMT FFmpeg
path uses one encode/decode thread; libomt controls its internal thread count,
so its end-to-end timing is a default-configuration comparison, not a
thread-normalized one.

Reference builds require `git`, `clang++`, and .NET 8 or later. Running libomt
on Linux requires `libavahi-common.so.3` and `libavahi-client.so.3`; set
`AVAHI_LIBRARY_PATH` if those libraries are outside the system linker paths.
References are checked out at fixed commits by the build script. Set
`OMT_REFERENCE_ROOT` to preserve them outside the default temporary directory.

Set `BENCH_WIDTH`, `BENCH_HEIGHT`, `BENCH_FRAMES`, and `BENCH_REPEATS` to change
the default 1280x720, 300-frame, three-repeat workload. Dimensions must be
multiples of 16.

## Current Coverage

`api-omt-test.c` exercises video and audio packet handling, timeouts, receiver
fan-out and slow-receiver isolation, discovery registration and resolution,
initial redirects, OMTInfo, connection and per-frame XML metadata, and tally.

`api-vmix-test.c` checks a reference decoder packet, malformed input, 10-bit
P216 decode, and encoder profile/quality headers and roundtrips for YUV422P,
UYVY422, YUYV422, NV12, YUV420P, and BGR0.

## Remaining Parity Work

- The libomt benchmark needs Avahi runtime libraries and was not runnable in the
  current container; it must pass in an environment with those dependencies
  before its results are considered validated.
- Alpha, 10-bit encoding, interlace, preview, live quality changes, and live
  redirect updates are not currently covered as supported end-to-end features.
- IPv6 DNS-SD and hosting a Discovery Server are not covered.

Reference projects:

- https://github.com/openmediatransport/libvmx
- https://github.com/openmediatransport/libomtnet
- https://github.com/openmediatransport/libomt