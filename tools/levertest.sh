#!/bin/sh
# 2x8 - is an active SCREEN CAPTURE the lever that makes WindowServer
# composite for RBAMDFramebuffer?
#
# 2x7 killed GPU activity: five load profiles all read BELOW the control
# (0.09-0.24 sets/s), so a connected AnyDesk session's encoding is not what
# does it. What a connected session also does is CAPTURE THE SCREEN. This
# tests that and nothing else.
#
# Three fixes over 2x7, each one a defect that run exposed:
#
#  1. ONE ioreg per sample. 2x7 ran a separate ioreg per counter, seconds
#     apart at up to 83/s, and its own "writes != sets" check then fired on
#     sampling skew. Rule 16 applied to the check.
#  2. A SETTLE window before every measurement. 2x7's "idle control" read
#     6.83 sets/s - the decaying tail of the keystroke that launched it, not
#     the 0.08/s frozen state. Typing is the one thing that makes this
#     machine composite (2wl), so starting a control right after a keystroke
#     measures the keystroke.
#  3. The script owns the clock AND the capture, so no phase can be entered
#     at the wrong time (2x3's lesson, which cost a whole trace).
#
# And it VERIFIES THE INSTRUMENT before trusting the arm: a screen recording
# that TCC denied, or that produced no bytes, is not a capture, and 2x5 lost
# three exchanges to exactly that failure wearing the words "didn't work".
#
# No sudo. Run it in the armed GPU-framebuffer boot with AnyDesk DISCONNECTED.

set -u

HZ=143.83
SETTLE=${SETTLE:-15}
WINDOW=${WINDOW:-25}
OUT=${TMPDIR:-/tmp}/rbgpu-levertest.mov

[ "$(id -u)" = "0" ] && { echo "run this WITHOUT sudo (TCC is per-user)"; exit 1; }

# One registry read, all three counters, so they cannot skew against each other.
snap() {
    ioreg -rl -c RBAMDFramebuffer 2>/dev/null | awk '
        match($0, /"RBFBAmtrSets" = [0-9]+/)      { s = substr($0, RSTART+18, RLENGTH-18) }
        match($0, /"RBFBHubpWrites" = [0-9]+/)    { w = substr($0, RSTART+20, RLENGTH-20) }
        match($0, /"RBFBVBLFires" = [0-9]+/)      { f = substr($0, RSTART+18, RLENGTH-18) }
        END { if (s == "" || f == "") exit 1; print s, w, f }'
}

# WindowServer's OWN state variable for "something is capturing the screen".
# 2x9 found it in the SkyLight log beside WSDisplayStreamNeedsRecomposite, the
# function that forces a recomposite because a display stream needs a frame.
# This beats checking our capture process is alive: it is the producer's word,
# and it catches AnyDesk or anything else capturing behind our back.
capstatus() {
    /usr/bin/log show --start "$1" --end "$2" --info \
        --predicate 'sender == "WindowServer"' 2>/dev/null \
        | grep -oE 'system screen capture status [0-9]+|WSDisplayStreamNeedsRecomposite|CreateDisplayStreamState' \
        | sort | uniq -c | awk '{ $1=$1; printf "%s; ", $0 }'
}

measure() {
    label="$1"
    printf '  %-34s settling %ss ... ' "$label" "$SETTLE"
    sleep "$SETTLE"
    t_a=$(date '+%Y-%m-%d %H:%M:%S')
    a=$(snap) || { echo "RBAMDFramebuffer not publishing - armed GPU-fb boot?"; return 1; }
    printf 'measuring %ss ... ' "$WINDOW"
    sleep "$WINDOW"
    b=$(snap) || { echo "lost the framebuffer mid-window"; return 1; }
    t_b=$(date '+%Y-%m-%d %H:%M:%S')
    echo "$a $b" | awk -v l="$label" -v hz="$HZ" '{
        ds = $4 - $1; dw = $5 - $2; df = $6 - $3
        sec = (df > 0) ? df / hz : 0
        printf "\n    %-32s sets %5d  writes %5d  vbl %6d  %6.2f s  -> %7.2f sets/s\n",
               l, ds, dw, df, sec, (sec > 0 ? ds / sec : 0)
        if (df == 0) print "    *** vbl did not advance - the VBL thread_call is stopped, stop here"
    }'
    ev=$(capstatus "$t_a" "$t_b")
    echo "      WindowServer said: ${ev:-(nothing - no capture activity logged)}"
}

echo "levertest: is an active screen capture the lever?"
echo "  endpoints from 2x5: 0.08 sets/s frozen, 83.43 sets/s with AnyDesk connected"
echo "  each arm = ${SETTLE}s settle (discarded) + ${WINDOW}s measured"
echo

rm -f "$OUT"

measure "A  idle, no capture" || exit 1

# -v video, -x no sound cue, -D1 main display, -C capture the cursor so the
# stream has something changing even on a still desktop.
echo "  starting screencapture -v ..."
screencapture -v -x -D 1 -C "$OUT" >/dev/null 2>&1 &
CAP=$!
sleep 4

# Rule 16: prove the capture is REAL before believing the arm. A TCC refusal
# and a dead process look identical from the sets counter.
if ! kill -0 "$CAP" 2>/dev/null; then
    echo "  *** screencapture exited immediately - almost certainly TCC."
    echo "      Grant Screen Recording to this terminal, or use the GUI:"
    echo "      Cmd-Shift-5 -> Record Entire Screen -> Record, then re-run"
    echo "      with CAPTURE=manual sh tools/levertest.sh"
    exit 1
fi
sz1=$(stat -f%z "$OUT" 2>/dev/null || echo 0)
sleep 5
sz2=$(stat -f%z "$OUT" 2>/dev/null || echo 0)
echo "  capture alive (pid $CAP), file $sz1 -> $sz2 bytes"
if [ "$sz2" -le "$sz1" ] 2>/dev/null; then
    echo "  *** the file is not growing - this is NOT a working capture."
    echo "      Treat arm B as void (rule 14: absence is not a result)."
fi

measure "B  capture ACTIVE"

kill -INT "$CAP" 2>/dev/null
wait "$CAP" 2>/dev/null
echo "  capture stopped, $(stat -f%z "$OUT" 2>/dev/null || echo 0) bytes written"

measure "C  idle again, no capture"

echo
echo "  B near 83/s with A and C near 0.1/s = CAPTURE IS THE LEVER. The bridge"
echo "    is a minimal always-on stream, and the real fix is whatever makes"
echo "    WindowServer treat the panel itself as a consumer."
echo "  A, B and C all alike = capture is dead too, and what remains is"
echo "    whatever else a live AnyDesk session holds open: lsof on its pid."
echo "  n=1 per arm (rule 18). A and C are two controls, not a trend."
echo
echo "  READ THE 'WindowServer said' LINES FIRST. Arm B is only a test of"
echo "    capture if it shows 'system screen capture status 1'. If arm A or C"
echo "    shows it too, something else is capturing - AnyDesk still connected,"
echo "    or a stream outliving its process - and the arms are not the"
echo "    contrast they claim to be (rule 16)."
