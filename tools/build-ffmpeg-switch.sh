#!/usr/bin/env bash
# Builds devkitPro's switch-ffmpeg 7.1 (with the NVDEC "nvtegra" hwaccel) plus the fixes in
# patches/ffmpeg-*.patch, and installs it into build/ffmpeg-switch. The Makefile links against
# that copy when it exists, instead of the stock portlibs FFmpeg.
#
# Usage: DEVKITPRO=/opt/devkitpro tools/build-ffmpeg-switch.sh
set -euo pipefail

: "${DEVKITPRO:?set DEVKITPRO to your devkitPro install}"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SRC="$ROOT/build/ffmpeg-src"
PREFIX="$ROOT/build/ffmpeg-switch"
VER=7.1
PKGBASE=https://raw.githubusercontent.com/devkitPro/pacman-packages/master/switch/ffmpeg
PORTLIBS_PREFIX="$DEVKITPRO/portlibs/switch"

export PATH="$PORTLIBS_PREFIX/bin:$DEVKITPRO/devkitA64/bin:$DEVKITPRO/tools/bin:$PATH"

mkdir -p "$SRC"
cd "$SRC"
[ -f ffmpeg-$VER.tar.xz ] || curl -sSL -o ffmpeg-$VER.tar.xz https://ffmpeg.org/releases/ffmpeg-$VER.tar.xz
[ -f ffmpeg-$VER.patch ] || curl -sSL -o ffmpeg-$VER.patch $PKGBASE/ffmpeg-$VER.patch
[ -f tls.patch ] || curl -sSL -o tls.patch $PKGBASE/tls.patch
sha256sum -c - <<EOF
40973d44970dbc83ef302b0609f2e74982be2d85916dd2ee7472d30678a7abe6  ffmpeg-$VER.tar.xz
1792380b992e3554a4abcddf0d7b395bfd8c118ac7c6e38c8f2fb0d39753a390  ffmpeg-$VER.patch
57ea7ec8ed26d13d3172d0fd589b7883d22f0c180d50b7434fcc73fb2b3ab7d7  tls.patch
EOF

rm -rf ffmpeg-$VER
tar xf ffmpeg-$VER.tar.xz
cd ffmpeg-$VER
patch -Np1 -s -i ../ffmpeg-$VER.patch
patch -Np1 -s -i ../tls.patch
for p in "$ROOT"/patches/ffmpeg-*.patch; do
    echo "applying $(basename "$p")"
    patch -Np1 -s -i "$p"
done

# devkitPro's PKGBUILD configuration plus the https/crypto/data protocols that HLS
# (AirPlay video casting) needs, installed to our own prefix
./configure --prefix="$PREFIX" --enable-gpl --disable-shared --enable-static \
    --cross-prefix=aarch64-none-elf- --enable-cross-compile \
    --arch=aarch64 --cpu=cortex-a57 --target-os=horizon --enable-pic \
    --pkg-config=aarch64-none-elf-pkg-config \
    --extra-cflags="-D__SWITCH__ -D_GNU_SOURCE -O2 -march=armv8-a -mtune=cortex-a57 -mtp=soft -fPIC -ftls-model=local-exec -I$PORTLIBS_PREFIX/include -isystem $DEVKITPRO/libnx/include" \
    --extra-cxxflags="-D__SWITCH__ -D_GNU_SOURCE -O2 -march=armv8-a -mtune=cortex-a57 -mtp=soft -fPIC -ftls-model=local-exec" \
    --extra-ldflags="-fPIE -L$PORTLIBS_PREFIX/lib -L$DEVKITPRO/libnx/lib" \
    --disable-runtime-cpudetect --disable-programs --disable-debug --disable-doc --disable-autodetect \
    --enable-asm --enable-neon \
    --disable-avdevice --disable-encoders --disable-muxers \
    --enable-swscale --enable-swresample --enable-network \
    --disable-protocols --enable-protocol=file,http,https,ftp,tcp,udp,rtmp,tls,httpproxy,crypto,data \
    --enable-zlib --enable-bzlib --enable-libass --enable-libfreetype --enable-libfribidi --enable-libdav1d \
    --enable-libnx --enable-nvtegra

make -j"$(nproc)"
make install
echo "FFmpeg installed to $PREFIX"
