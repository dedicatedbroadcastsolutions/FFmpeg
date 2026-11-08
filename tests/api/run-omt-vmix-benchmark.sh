#!/bin/sh

set -eu

repo_root=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
cd "$repo_root"

width=${BENCH_WIDTH:-1280}
height=${BENCH_HEIGHT:-720}
frames=${BENCH_FRAMES:-300}
repeats=${BENCH_REPEATS:-3}
benchmark_mode=${BENCH_MODE:-all}
ref_root=${OMT_REFERENCE_ROOT:-${TMPDIR:-/tmp}/omt-native-reference}
omt_library=$ref_root/libomt/bin/Release/net8.0/linux-x64/native
avahi_library_path=${AVAHI_LIBRARY_PATH:-}
workdir=$(mktemp -d)

cleanup() {
    rm -rf "$workdir"
}
trap cleanup EXIT HUP INT TERM

case "$repeats" in
    ''|*[!0-9]*|0)
        printf 'BENCH_REPEATS must be a positive integer.\n' >&2
        exit 2
        ;;
esac
case "$benchmark_mode" in
    all|vmix|omt) ;;
    *)
        printf 'BENCH_MODE must be all, vmix, or omt.\n' >&2
        exit 2
        ;;
esac

sh tests/api/build-omt-vmix-reference.sh

make fate-api-omt fate-api-vmix
make tests/api/api-vmix-bench-test tests/api/api-omt-bench-test \
    LIBVMX_ROOT="$ref_root/libvmx" LIBOMT_ROOT="$ref_root/libomt"

export LD_LIBRARY_PATH="$ref_root/libvmx/build:$omt_library${avahi_library_path:+:$avahi_library_path}${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"

printf '# ffmpeg_commit=%s\n' "$(git rev-parse --short HEAD)"
printf '# ffmpeg_configuration=%s\n' "$(./ffmpeg -version | sed -n 's/^configuration: //p')"
printf '# host_cpu=%s\n' "$(awk -F: '/model name/ { sub(/^[[:space:]]+/, "", $2); print $2; exit }' /proc/cpuinfo)"
printf '# repeats=%s width=%s height=%s frames=%s\n' "$repeats" "$width" "$height" "$frames"
printf '# benchmark_mode=%s\n' "$benchmark_mode"
printf '# libvmx_commit=%s\n' "$(git -C "$ref_root/libvmx" rev-parse --short HEAD)"
printf '# libomtnet_commit=%s\n' "$(git -C "$ref_root/libomtnet" rev-parse --short HEAD)"
printf '# libomt_commit=%s\n' "$(git -C "$ref_root/libomt" rev-parse --short HEAD)"

if [ "$benchmark_mode" != omt ]; then
    printf '%s\n' '# VMIX encode/decode; headers and both cross-decode directions are validated before results are accepted.'
    run=1
    while [ "$run" -le "$repeats" ]; do
        printf '# VMIX repeat %s\n' "$run"
        tests/api/api-vmix-bench-test "$width" "$height" "$frames"
        run=$((run + 1))
    done
fi

if [ "$benchmark_mode" != vmix ]; then
    if ! command -v avahi-daemon >/dev/null 2>&1 || ! avahi-daemon --check >/dev/null 2>&1; then
        printf '%s\n' 'libomt runtime benchmark requires a running Avahi daemon.' \
            'Start it with: sudo service avahi-daemon start' >&2
        exit 1
    fi
    for library in libavahi-common.so.3 libavahi-client.so.3; do
        if ldconfig -p 2>/dev/null | grep -q "$library"; then
            continue
        fi
        if [ -n "$avahi_library_path" ] && [ -f "$avahi_library_path/$library" ]; then
            continue
        fi
        printf 'Missing %s; install Avahi runtime libraries or set AVAHI_LIBRARY_PATH.\n' \
            "$library" >&2
        exit 1
    done

    input=$workdir/input.yuyv
    tests/api/api-omt-bench-test --generate "$input" "$width" "$height"
    printf '%s\n' '# OMT end-to-end; same YUYV source, medium profile, quality 80.' \
        '# FFmpeg uses one encode/decode thread; libomt uses its automatic thread count.'
    run=1
    while [ "$run" -le "$repeats" ]; do
        port=$((20000 + ($$ + run * 251) % 29998))
        printf '# OMT repeat %s\n' "$run"
        native_result=$(tests/api/api-omt-bench-test --native "$input" "$width" "$height" "$frames" "$port")
        reference_result=$(tests/api/api-omt-bench-test --reference "$input" "$width" "$height" "$frames" "$((port + 1))")
        native_psnr=$(printf '%s\n' "$native_result" | awk -F, '$1 == "omt" { print $10 }')
        reference_psnr=$(printf '%s\n' "$reference_result" | awk -F, '$1 == "omt" { print $10 }')
        if [ -z "$native_psnr" ] || [ -z "$reference_psnr" ]; then
            printf 'OMT repeat %s did not produce both quality measurements.\n' "$run" >&2
            exit 1
        fi
        awk -v run="$run" -v native="$native_psnr" -v reference="$reference_psnr" \
            'BEGIN { delta = native - reference; if (delta < 0) delta = -delta; printf "# OMT quality repeat %s: FFmpeg %.3f dB, libomt %.3f dB, delta %.3f dB\n", run, native, reference, delta; if (delta > 0.1) exit 1 }' || {
            printf 'OMT quality parity failed; performance results rejected.\n' >&2
            exit 1
        }
        printf '%s\n%s\n' "$native_result" "$reference_result"
        run=$((run + 1))
    done
fi