#!/usr/bin/env bash
# Generates an HLS folder shaped like the YouTube app's streams, for run_youtube_test.sh:
# master.m3u8 (mlhls://localhost/ URIs) with 720p and 360p H.264 variants, a VP9 variant
# the receiver must skip, and separate AAC audio, all fragmented MP4 in 4 s segments.
# local.m3u8 is the same master with relative URIs, for player_test over HTTP.
#
# Usage: tests/make_hls_fixture.sh <out-dir> [fps] [seconds]   (defaults: 60 fps, 40 s)
set -euo pipefail
OUT="$1"
FPS="${2:-60}"
SECS="${3:-40}"
mkdir -p "$OUT"
OUT="$(cd "$OUT" && pwd)"
GOP=$((FPS * 2))

# <dir> <ffmpeg input and codec options...>: one fMP4 media playlist in <dir>
variant() {
    local dir="$OUT/$1"
    shift
    rm -rf "$dir" && mkdir -p "$dir"
    (cd "$dir" && ffmpeg -loglevel error -y "$@" -t "$SECS" -f hls -hls_time 4 -hls_playlist_type vod \
        -hls_segment_type fmp4 -hls_fmp4_init_filename init.mp4 -hls_segment_filename 'seg%d.m4s' index.m3u8)
}

h264() {
    variant "$1" -f lavfi -i "testsrc2=size=$2:rate=$FPS" -an -c:v libx264 -preset veryfast -profile:v "$3" \
        -pix_fmt yuv420p -g "$GOP" -keyint_min "$GOP" -sc_threshold 0
}

h264 v720 1280x720 high
h264 v360 640x360 main
variant vp9 -f lavfi -i "testsrc2=size=640x360:rate=$FPS" -an -c:v libvpx-vp9 -deadline realtime -cpu-used 8 \
    -b:v 500k -g "$GOP"
variant audio -f lavfi -i "sine=frequency=440:sample_rate=44100" -vn -c:a aac -ac 2 -b:a 128k

cat > "$OUT/master.m3u8" <<EOF
#EXTM3U
#EXT-X-VERSION:7
#EXT-X-INDEPENDENT-SEGMENTS
#EXT-X-MEDIA:TYPE=AUDIO,GROUP-ID="aud",NAME="English",LANGUAGE="en",DEFAULT=YES,AUTOSELECT=YES,URI="mlhls://localhost/audio/index.m3u8"
#EXT-X-STREAM-INF:BANDWIDTH=3000000,CODECS="avc1.64001f,mp4a.40.2",RESOLUTION=1280x720,FRAME-RATE=$FPS.000,AUDIO="aud"
mlhls://localhost/v720/index.m3u8
#EXT-X-STREAM-INF:BANDWIDTH=900000,CODECS="avc1.4d401e,mp4a.40.2",RESOLUTION=640x360,FRAME-RATE=$FPS.000,AUDIO="aud"
mlhls://localhost/v360/index.m3u8
#EXT-X-STREAM-INF:BANDWIDTH=700000,CODECS="vp09.00.30.08,mp4a.40.2",RESOLUTION=640x360,FRAME-RATE=$FPS.000,AUDIO="aud"
mlhls://localhost/vp9/index.m3u8
EOF
sed 's#mlhls://localhost/##' "$OUT/master.m3u8" > "$OUT/local.m3u8"
echo "HLS fixture in $OUT ($FPS fps, $SECS s)"
