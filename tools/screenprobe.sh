#!/bin/sh
# screenprobe.sh (2mx) — show the known pattern on screen and capture it.
#
# Must be run from the user's Terminal, not from Claude's shell: screencapture
# needs Screen Recording permission, and Claude's shell does not have it
# ("could not create image from display"). Terminal does.
#
# Takes no sudo, loads nothing, changes nothing. Leaves a PNG in logs/.
set -e
cd "$(dirname "$0")/.."
OUT="logs/$(date +%Y%m%d-%H%M%S)-screenprobe"
mkdir -p "$OUT"

[ -x build/out/screenprobe ] || make screenprobe

./build/out/screenprobe 8 > "$OUT/geometry.txt" 2>&1 &
PROBE=$!
sleep 3
screencapture -x "$OUT/screen.png" || {
    echo "screencapture failed - this shell lacks Screen Recording permission."
    echo "Grant it to Terminal in System Settings > Privacy & Security >"
    echo "Screen & System Audio Recording, then run this again."
    kill $PROBE 2>/dev/null || true
    exit 1
}
wait $PROBE 2>/dev/null || true

cat "$OUT/geometry.txt"
echo ""
echo "captured: $OUT/screen.png"
