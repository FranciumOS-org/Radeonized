#!/bin/sh
# screenprobe.sh (2my) — a known pattern in real windows, captured and decoded.
#
# Run from the user's Terminal, not the assistant's shell: screencapture needs Screen
# Recording permission and the assistant's shell does not have it. No sudo, loads
# nothing, changes nothing. Leaves PNGs and a report in logs/.
#
# Modes, each a two-window capture:
#   direct     CPU control | plain CAMetalLayer render       512x512
#   msaa       CPU control | 4x multisample, resolved        512x512
#   iosurface  CPU control | render to IOSurface -> CALayer  512x512
#   big        CPU control | plain render                    900x880
#   bigmsaa    CPU control | 4x multisample                  900x880
#   bigcpu     CPU control | CPU - no Metal in the process   900x880
#
# Each mode runs REPEATS times and is captured twice per run, because the one
# corruption seen so far appeared in a single frame and a single frame is not a
# result (rule 18: do not build a table from n=1 rows).
#
# The probe is allowed to exit on its own rather than being killed. An abruptly
# killed process's window surface being recycled while still on screen would
# look exactly like the corruption being hunted, and that has to be excluded
# before the chip is blamed (rule 16: the instrument is a suspect).
set -e
cd "$(dirname "$0")/.."
# The 2my swap experiment. The corruption so far has always landed on the CPU
# window, which is also the only one drawn once and left alone; the GPU window
# redraws every 0.5s and has never been hit. These modes move that asymmetry
# around to find out which of the two properties matters:
#
#   static   neither window redraws     -> if it is "drawn once", BOTH corrupt
#   swap     CPU redraws, GPU drawn once-> if it is "drawn once", the GPU one
#                                          corrupts and the CPU one stops
#   refresh  both redraw                -> neither should corrupt
#   direct   the original asymmetry, as a control
#
# It is intermittent, so each mode runs REPEATS times (rule 18).
MODES="${MODES:-refresh refresh refresh refresh}"
REPEATS="${REPEATS:-4}"
OUT="logs/$(date +%Y%m%d-%H%M%S)-screenprobe"
mkdir -p "$OUT"
REPORT="$OUT/report.txt"

[ -x build/out/screenprobe ] || make screenprobe
[ -x build/out/decodescreen ] || make decodescreen

for MODE in $MODES; do
  R=1
  while [ $R -le $REPEATS ]; do
    TAG="$MODE-$R"
    rm -f "$OUT/ready-$TAG"
    # 5 seconds up, then it terminates itself in an orderly way
    ./build/out/screenprobe "$MODE" "$OUT/ready-$TAG" 5 > "$OUT/geom-$TAG.txt" 2>&1 &
    PROBE=$!

    i=0
    while [ ! -f "$OUT/ready-$TAG" ]; do
        i=$((i + 1))
        if [ $i -gt 400 ]; then
            echo "$TAG never reported the pattern on screen; output was:" | tee -a "$REPORT"
            cat "$OUT/geom-$TAG.txt" | tee -a "$REPORT"
            kill $PROBE 2>/dev/null || true
            R=$((R + 1))
            continue 2
        fi
        sleep 0.1
    done
    sleep 0.6

    for n in a b; do
        screencapture -x "$OUT/$TAG$n.png" || {
            echo "screencapture failed - this shell lacks Screen Recording permission."
            echo "Grant it in System Settings > Privacy & Security >"
            echo "Screen & System Audio Recording, then run this again."
            exit 1
        }
        sleep 0.8
    done

    wait $PROBE 2>/dev/null || true   # orderly exit, not a kill
    sleep 1                            # let the WindowServer settle

    ARGS=$(sed -n 's/^decode with: build\/out\/decodescreen <png> //p' "$OUT/geom-$TAG.txt")
    {
        echo "================ $TAG ================"
        if [ -n "$ARGS" ]; then
            for n in a b; do
                printf '  capture %s: ' "$n"
                ./build/out/decodescreen "$OUT/$TAG$n.png" $ARGS \
                  | sed -n 's/^window state: //p' \
                  | tr '\n' ' '
                echo ""
            done
        else
            echo "  no geometry line; the probe did not get that far"
            cat "$OUT/geom-$TAG.txt"
        fi
    } 2>&1 | tee -a "$REPORT"
    R=$((R + 1))
  done
done

echo "" | tee -a "$REPORT"
echo "report: $REPORT"
