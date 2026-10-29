#!/bin/sh
# 2xd - THE WORKING BRIDGE, and it is a crude one. Read the caveats.
#
# MEASURED, on boot 210: `screencapture -v` running gives 39.75 sets/s
# (2xa measured 71.30 in another window) against 0.31 sets/s for our own
# ScreenCaptureKit stream demanding 144 frames/s, and 0.05-0.08 idle. The
# user's screen came alive during the run. So this works and the SCK
# equivalent does not, for a reason NOT yet established - our stream sets the
# same `system screen capture status 1` and creates the same display stream
# state, and SCK still reports every frame IDLE, i.e. "the display did not
# change". See 2xc/2xd.
#
# WHAT THIS COSTS, said plainly:
#   - It writes video. About 660 KB/s, deleted at the end of every cycle, so
#     peak disk use is one cycle's file (~40 MB at the default 60 s) but the
#     BYTES WRITTEN ARE REAL - roughly 2.3 GB/hour through the SSD.
#   - There is a brief stall between cycles while one recording ends and the
#     next starts. The desktop will hitch once a cycle.
#   - macOS shows a screen-recording indicator the whole time, and anything
#     on screen is being encoded to a temporary file. It is deleted each
#     cycle and never leaves the machine, but it IS written to disk.
#
# It is a workaround, not a driver. A graphics driver that needs a screen
# recorder running forever is not finished.
#
# No sudo. Ctrl-C to stop; the file is removed on exit.

set -u
CYCLE=${CYCLE:-60}
DIR=${TMPDIR:-/tmp}
OUT="$DIR/rbgpu-keepalive.mov"

cleanup() { rm -f "$OUT"; echo; echo "keepalive: stopped, temp video removed"; exit 0; }
trap cleanup INT TERM HUP

snap() {
    ioreg -rl -c RBAMDFramebuffer 2>/dev/null | awk '
        /"RBFBAmtrSets" = / { s = $NF }
        /"RBFBVBLFires" = / { f = $NF }
        END { if (s == "" || f == "") exit 1; print s, f }'
}

echo "keepalive: holding the desktop up with ${CYCLE}s screencapture cycles"
echo "  baseline: 0.05-0.08 sets/s frozen, ~40-71 sets/s while recording (2xa/2xd)"
echo "  ctrl-C to stop"
echo

n=0
while :; do
    a=$(snap) || { echo "  RBAMDFramebuffer not publishing - armed GPU-fb boot?"; exit 1; }
    rm -f "$OUT"
    screencapture -v -V "$CYCLE" -x -D 1 "$OUT" >/dev/null 2>&1
    b=$(snap) || b="$a"
    n=$((n + 1))
    sz=$(stat -f%z "$OUT" 2>/dev/null || echo 0)
    rm -f "$OUT"
    echo "$a $b" | awk -v n="$n" -v sz="$sz" '{
        ds = $3 - $1; df = $4 - $2; sec = (df > 0) ? df / 143.83 : 0
        printf "  cycle %-4d sets %6d  %6.2f s  -> %6.2f sets/s   (%.1f MB written, deleted)\n",
               n, ds, sec, (sec > 0 ? ds / sec : 0), sz / 1048576
        if (ds < 0) print "    *** negative - counters reset, new boot"
        if (sec > 0 && ds / sec < 5) print "    *** recording ran but the rate did NOT rise - this bridge is not working"
    }'
done
