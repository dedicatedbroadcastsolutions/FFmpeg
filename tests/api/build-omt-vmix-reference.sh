#!/bin/sh

set -eu

ref_root=${OMT_REFERENCE_ROOT:-${TMPDIR:-/tmp}/omt-native-reference}
mkdir -p "$ref_root"

checkout_reference() {
    name=$1
    url=$2
    revision=$3
    path=$ref_root/$name

    if [ ! -d "$path/.git" ]; then
        if [ -e "$path" ]; then
            printf 'Refusing to use non-git reference path: %s\n' "$path" >&2
            exit 1
        fi
        git clone --filter=blob:none "$url" "$path"
    fi
    if [ -n "$(git -C "$path" status --porcelain --untracked-files=no)" ]; then
        printf 'Reference checkout has local changes: %s\n' "$path" >&2
        exit 1
    fi
    git -C "$path" fetch --quiet origin "$revision"
    git -C "$path" checkout --quiet --detach "$revision"
}

command -v git >/dev/null
command -v clang++ >/dev/null
command -v dotnet >/dev/null

checkout_reference libvmx https://github.com/openmediatransport/libvmx.git \
    31724bb96530325a989bc157c8e07538d6f1c4db
checkout_reference libomtnet https://github.com/openmediatransport/libomtnet.git \
    7d29031ff3f355ffa1f0cd08ee24d02e258445dd
checkout_reference libomt https://github.com/openmediatransport/libomt.git \
    bb4b67be8e5964f1017592b9fc5406495e704e5d

(
    cd "$ref_root/libvmx/build"
    bash buildlinuxx64.sh
)
(
    cd "$ref_root/libomtnet"
    DOTNET_CLI_TELEMETRY_OPTOUT=1 dotnet build libomtnet.sln -c Release
)
(
    cd "$ref_root/libomt"
    DOTNET_CLI_TELEMETRY_OPTOUT=1 dotnet publish libomt.sln -r linux-x64 -c Release
)

printf 'OMT_REFERENCE_ROOT=%s\n' "$ref_root"