#!/usr/bin/env bash
# Builds SwitchAirCast.nro inside the official devkitPro Docker image, so nothing has to be
# installed on the host (macOS or Linux). Builds the patched FFmpeg first if it is missing.
#
# Usage: tools/docker-build.sh
#        DKP_SYNC=0 tools/docker-build.sh   # skip the package upgrade (offline, repeatable)
#
# Uses the image's native architecture (arm64 on Apple Silicon). Do not force
# --platform linux/amd64: it works but runs under emulation and is several times slower.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
IMAGE="${DEVKITPRO_IMAGE:-devkitpro/devkita64}"

# The container runs as root (apt-get may need it). Its output is handed back to the host
# user on exit; Docker Desktop on macOS maps ownership anyway, Linux hosts do not.
docker run --rm -v "$ROOT":/work -w /work \
    -e DKP_SYNC="${DKP_SYNC:-1}" -e HOST_UID="$(id -u)" -e HOST_GID="$(id -g)" \
    "$IMAGE" bash -euxo pipefail -c '
    trap "chown -R \"\$HOST_UID:\$HOST_GID\" build SwitchAirCast.* 2>/dev/null || true" EXIT
    export DEVKITPRO=/opt/devkitpro

    # upgrades the toolchain, so builds can change from run to run; DKP_SYNC=0 skips it
    if [ "$DKP_SYNC" = 1 ]; then
        dkp-pacman -Syu --noconfirm
    fi
    dkp-pacman -S --noconfirm --needed \
        switch-dev switch-sdl2 switch-sdl2_ttf switch-ffmpeg switch-mbedtls switch-libsodium \
        switch-libass switch-freetype switch-libfribidi switch-dav1d switch-zlib switch-bzip2

    # the image may lack tools used by tools/build-ffmpeg-switch.sh
    for t in curl patch make xz; do
        command -v "$t" >/dev/null || { apt-get update && apt-get install -y curl patch make xz-utils coreutils pkg-config; break; }
    done

    [ -f build/ffmpeg-switch/lib/libavcodec.a ] || tools/build-ffmpeg-switch.sh
    make
    ls -la SwitchAirCast.nro
'
