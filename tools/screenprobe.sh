#!/bin/sh
# screenprobe.sh (2mx) — show a known pattern on the real screen through each
# production path in turn, capture it, and decode what the compositor did.
#
# Must be run from the user's Terminal, not from the assistant's shell: screencapture
# needs Screen Recording permission, and the assistant's shell does not have it
# ("could not create image from display"). Terminal does.
#
# No sudo, loads nothing, changes nothing. Leaves PNGs and a report in logs/.
#
# Modes, each a two-window capture - the CPU-drawn control beside one GPU path:
#   direct     a plain CAMetalLayer render           512x512
#   msaa       4x multisample, resolved              512x512
#   iosurface  render to IOSurface -> CALayer        512x512
#   big        a plain render, large                 900x880
#   bigmsaa    4x multisample, large                 900x880
set -e
cd "$(dirname "$0")/.."
OUT="logs/$(date +%Y%m%d-%H%M%S)-screenprobe"
mkdir -p "$OUT"
REPORT="$OUT/report.txt"

[ -x build/out/screenprobe ] || make screenprobe
[ -x build/out/decodescreen ] || make decodescreen

for MODE in direct msaa iosurface big bigmsaa; do
    rm -f "$OUT/ready-$MODE"
    ./build/out/screenprobe "$MODE" "$OUT/ready-$MODE" 20 > "$OUT/geom-$MODE.txt" 2>&1 &
    PROBE=$!

    i=0
    while [ ! -f "$OUT/ready-$MODE" ]; do
        i=$((i + 1))
        if [ $i -gt 400 ]; then
            echo "mode $MODE never reported the pattern on screen; its output was:" | tee -a "$REPORT"
            cat "$OUT/geom-$MODE.txt" | tee -a "$REPORT"
            kill $PROBE 2>/dev/null || true
            continue 2
        fi
        sleep 0.1
    done
    sleep 0.6

    screencapture -x "$OUT/$MODE.png" || {
        echo "screencapture failed - this shell lacks Screen Recording permission."
        echo "Grant it in System Settings > Privacy & Security >"
        echo "Screen & System Audio Recording, then run this again."
        kill $PROBE 2>/dev/null || true
        exit 1
    }
    kill $PROBE 2>/dev/null || true
    wait $PROBE 2>/dev/null || true

    # the probe prints the exact rectangles it used, so nothing is guessed here
    ARGS=$(sed -n 's/^decode with: build\/out\/decodescreen <png> //p' "$OUT/geom-$MODE.txt")
    {
        echo "================ mode: $MODE ================"
        cat "$OUT/geom-$MODE.txt"
        echo "----"
        if [ -n "$ARGS" ]; then
            ./build/out/decodescreen "$OUT/$MODE.png" $ARGS
        else
            echo "no geometry line - the probe did not get far enough to print one"
        fi
        echo ""
    } 2>&1 | tee -a "$REPORT"
done

echo ""
echo "report: $REPORT"
