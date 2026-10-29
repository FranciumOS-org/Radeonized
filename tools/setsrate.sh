#!/bin/sh
# 2x7 - measure the transaction rate our framebuffer actually sees, under a
# named GPU load, with the panel's own vblank counter as the clock.
#
# The question this answers: a connected AnyDesk session takes RBFBAmtrSets
# from 0.08/s to 83.43/s (2x5). Is the lever GPU ACTIVITY or SCREEN CAPTURE?
# metal_probe --busy is GPU work and no capture at all, so if any --busy
# profile moves the rate, capture is dead.
#
# 2x6's first attempt used --heavy 20000, which bought 18.9 buffers/s: that
# measured GPU OCCUPANCY, not SUBMISSION RATE, and AnyDesk's profile is many
# small buffers. So sweep the profile instead of guessing it.
#
# ONE command queue in every arm (metal_probe --busy makes exactly one), never
# --queues, which is 2wt's known wedge trigger.
#
# No sudo. Reads only. Run it in the armed GPU-framebuffer boot.

set -u
PROBE=build/out/metal_probe
[ -x "$PROBE" ] || { echo "no $PROBE - run: make tools"; exit 1; }

ioreg -rl -c RBAMDFramebuffer > /dev/null 2>&1 || true

key() {
    ioreg -rl -c RBAMDFramebuffer 2>/dev/null \
        | grep -oE "\"$1\" = [0-9]+" | head -1 | grep -oE '[0-9]+$'
}

# RBFBVBLFires is the panel's own clock at a measured 143.83 Hz (2wj), which
# beats wall time here: it cannot be stretched by a stalled machine.
HZ=143.83

arm() {
    label="$1"; shift
    s0=$(key RBFBAmtrSets); f0=$(key RBFBVBLFires); w0=$(key RBFBHubpWrites)
    if [ -z "${s0:-}" ] || [ -z "${f0:-}" ]; then
        echo "  $label: RBAMDFramebuffer not publishing - is this an armed GPU-fb boot?"
        return 1
    fi
    t0=$(date +%s)
    if [ $# -gt 0 ]; then
        out=$(env -u DYLD_INSERT_LIBRARIES "$PROBE" "$@" 2>&1 | grep -E 'buffers in|FAIL|device\(s\)')
    else
        sleep "$IDLE"
        out="(no load)"
    fi
    t1=$(date +%s)
    s1=$(key RBFBAmtrSets); f1=$(key RBFBVBLFires); w1=$(key RBFBHubpWrites)
    ds=$((s1 - s0)); df=$((f1 - f0)); dw=$((w1 - w0)); dt=$((t1 - t0))
    awk -v l="$label" -v ds="$ds" -v df="$df" -v dw="$dw" -v dt="$dt" -v hz="$HZ" \
        'BEGIN{ sec = (df>0) ? df/hz : dt;
                printf "  %-26s sets %6d  writes %6d  vbl %7d  %6.2f s  -> %8.2f sets/s\n",
                       l, ds, dw, df, sec, (sec>0 ? ds/sec : 0) }'
    [ "$out" = "(no load)" ] || echo "        load: $out"
    # rule 38's shape: the two halves check each other. If writes ever lag
    # sets, the driver is the suspect again and this whole sweep is void.
    if [ "$dw" -ne "$ds" ]; then
        echo "        *** writes != sets ($dw vs $ds) - 2x5 no longer holds, stop here"
    fi
    return 0
}

IDLE=${IDLE:-20}

echo "setsrate: RBFBAmtrSets per second, panel vblank as the clock"
echo "  baseline for comparison: 0.08/s frozen, 83.43/s with AnyDesk connected (2x5)"
echo
arm "idle (control)"                                      || exit 1
arm "busy heavy, low rate"   --busy 20 --heavy 20000 --depth 16
arm "busy light, high rate"  --busy 20 --depth 256
arm "busy light, deep-ish"   --busy 20 --depth 64
arm "bench, serial"          --bench 400
arm "idle (control, repeat)"
echo
echo "n=1 per row (rule 18): this locates a LEVER, it is not a table of rates."
echo "Any row near 83/s = GPU work is the lever and capture is dead."
echo "Every row near 0.1/s = GPU work is NOT the lever; capture is back, test"
echo "  it with Cmd-Shift-5 -> Record Entire Screen."
