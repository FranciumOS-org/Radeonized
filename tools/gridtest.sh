#!/bin/sh
# gridtest.sh (2nh) — eight AppKit windows at once, each judged on its own.
#
# 2ng showed a surface is right or wrong from creation and stays so, about half
# the time. One window per run is one sample of one allocation and cannot show
# how the losers are distributed. Eight simultaneous allocations in a single
# capture can: independent ~50% means a per-surface coin flip (an alignment bit,
# or which of two pools it landed in); strict alternation means two pools used
# round-robin; runs or clusters mean a pool that changes state.
#
# Needs an ARMED boot - on an unarmed one there is no Metal device and the
# probe says so rather than drawing nothing (rule 14).
set -e
cd "$(dirname "$0")/.."
caffeinate -d -w $$ & CAF=$!
trap 'kill $CAF 2>/dev/null' EXIT
OUT="logs/$(date +%Y%m%d-%H%M%S)-gridtest"
mkdir -p "$OUT"
[ -x build/out/screenprobe ] || make screenprobe
[ -x build/out/decodescreen ] || make decodescreen

ROUNDS="${ROUNDS:-6}"
r=1
while [ $r -le $ROUNDS ]; do
    rm -f "$OUT/ready-$r"
    ./build/out/screenprobe grid "$OUT/ready-$r" 8 > "$OUT/geom-$r.txt" 2>&1 &
    P=$!
    i=0
    while [ ! -f "$OUT/ready-$r" ]; do
        i=$((i + 1))
        if [ $i -gt 400 ]; then
            echo "round $r: probe never came up; its output was:"; cat "$OUT/geom-$r.txt"
            kill $P 2>/dev/null || true; break
        fi
        sleep 0.1
    done
    sleep 0.8
    if screencapture -x "$OUT/r$r.png" 2>/dev/null && [ -s "$OUT/r$r.png" ]; then
        printf 'round %d: ' "$r"
        sed -n 's/^grid pair [0-9]*,[0-9]*: //p' "$OUT/geom-$r.txt" | while read -r ARGS; do
            ./build/out/decodescreen "$OUT/r$r.png" $ARGS 2>/dev/null \
              | sed -n 's/^window state: //p' \
              | sed 's/ (row jumps [0-9]*, col jumps [0-9]*)//g;s/cpu //;s/gpu //;s/,//'
        done | tr '\n' ' '
        echo ""
    else
        echo "round $r: capture failed"
    fi
    kill $P 2>/dev/null || true
    wait $P 2>/dev/null || true
    sleep 1
    r=$((r + 1))
done
echo ""
echo "each round lists its eight windows in creation order"
