#!/usr/bin/env bash
# End-to-end test of AirPlay video casting on the desktop build:
#   receiver (offscreen) + local HTTP server for HLS segments + fake_youtube sender.
#
# Usage: tests/run_youtube_test.sh <hls-dir> [work-dir]
#   <hls-dir> holds master.m3u8 (URIs starting with mlhls://localhost/) and the media
#   playlists/segments it refers to. Build first: make -f Makefile.linux all tests
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
HLS="$(cd "$1" && pwd)"
WORK="${2:-$(mktemp -d)}"
mkdir -p "$WORK"
rm -f "$WORK"/log.txt "$WORK"/snap.bmp

RX_PORT=$((20000 + RANDOM % 10000))
HTTP_PORT=$((30000 + RANDOM % 10000))
printf 'port = %d\n' "$RX_PORT" > "$WORK/config.ini"

python3 -m http.server "$HTTP_PORT" --bind 127.0.0.1 --directory "$HLS" > "$WORK/http.txt" 2>&1 &
HTTP_PID=$!
SWITCHCAST_DATA_DIR="$WORK" SWITCHCAST_SNAPSHOT="$WORK/snap.bmp" SDL_VIDEODRIVER=offscreen SDL_AUDIODRIVER=dummy \
    "$ROOT/build/linux/switchcast" > "$WORK/rx.txt" 2>&1 &
RX_PID=$!
trap 'kill $RX_PID $HTTP_PID 2>/dev/null; wait $RX_PID $HTTP_PID 2>/dev/null' EXIT
sleep 2

"$ROOT/build/linux/fake_youtube" 127.0.0.1 "$RX_PORT" "$HLS" "http://127.0.0.1:$HTTP_PORT"
STATUS=$?
echo "receiver log: $WORK/log.txt"
exit $STATUS
