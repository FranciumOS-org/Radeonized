#!/bin/sh
# 2x9/2xa - is an active SCREEN CAPTURE the lever that makes WindowServer
# composite for RBAMDFramebuffer?
#
# 2x7 killed GPU activity: five single-queue load profiles all read BELOW the
# control. What a connected AnyDesk session also does is CAPTURE THE SCREEN,
# and WindowServer names that mechanism itself - WSDisplayStreamNeedsRecomposite,
# beside CreateDisplayStreamState and a "system screen capture status" variable
# (2x9). This varies exactly that and nothing else.
#
# FIVE instrument defects paid for in this session, each fixed here:
#
#  1. ONE ioreg per sample. 2x7 ran a separate ioreg per counter, seconds
#     apart at up to 83/s, and its own writes!=sets check then fired on
#     sampling skew. Rule 16 on the check itself.
#  2. PARSE THE VALUE, NEVER A BYTE OFFSET. The first version computed
#     substr(RSTART+18) and dropped the LEADING DIGIT of every counter, so
#     465 read as 65 and 66000 as 6000 - and once a truncated value rolled
#     (499->500 becomes 99->00) the delta went NEGATIVE. The run printed
#     "sets -98" and "vbl -6421", which is how it was caught. $NF is the
#     value; offsets are an assumption.
#  3. A SETTLE before every window. 2x7's "idle control" read 6.83 sets/s -
#     the decaying tail of the keystroke that launched it. Typing is the one
#     thing that makes this machine composite (2wl), so a control started
#     right after a keystroke measures the keystroke.
#  4. screencapture NEEDS -V. Backgrounding `screencapture -v FILE` with no
#     -V wrote 0 bytes and created no stream at all; `-V <seconds>` records
#     and exits by itself (verified: 375327 bytes, exit 0). TCC was never
#     the problem, the invocation was.
#  5. THE LOG PREDICATE'S SENDER IS "SkyLight", NOT "WindowServer". The
#     lines read `WindowServer: (SkyLight) [com.apple.SkyLight:default]`, so
#     a sender == "WindowServer" predicate returned 1 line where the OR form
#     returns 45. Rule 36's third instance in this project. The predicate
#     below is VERIFIED against a known-present case: a 6 s capture yields
#     CreateDisplayStreamState 1, status 1, status 0, Recomposite 1.
#
# No sudo. Run it in the armed GPU-framebuffer boot with AnyDesk DISCONNECTED.

set -u

HZ=143.83
SETTLE=${SETTLE:-15}
WINDOW=${WINDOW:-25}
OUT=${TMPDIR:-/tmp}/rbgpu-levertest.mov
PRED='subsystem == "com.apple.SkyLight" OR sender == "SkyLight" OR sender == "WindowServer"'

[ "$(id -u)" = "0" ] && { echo "run this WITHOUT sudo (TCC is per-user)"; exit 1; }

# One registry read, all three counters, value taken as the last field.
snap() {
    ioreg -rl -c RBAMDFramebuffer 2>/dev/null | awk '
        /"RBFBAmtrSets" = /   { s = $NF }
        /"RBFBHubpWrites" = / { w = $NF }
        /"RBFBVBLFires" = /   { f = $NF }
        END { if (s == "" || f == "") exit 1; print s, w, f }'
}

# WindowServer's own answer to "was anything capturing?", over one arm's
# wall-clock bounds. The producer's word beats checking our own child.
capstatus() {
    n=$(/usr/bin/log show --start "$1" --end "$2" --info --predicate "$PRED" 2>/dev/null | wc -l | tr -d ' ')
    ev=$(/usr/bin/log show --start "$1" --end "$2" --info --predicate "$PRED" 2>/dev/null \
        | grep -oE 'system screen capture status [0-9]+|WSDisplayStreamNeedsRecomposite|CreateDisplayStreamState' \
        | sort | uniq -c | awk '{ $1=$1; printf "%s; ", $0 }')
    echo "${ev:-none} [${n} SkyLight lines as the control]"
}

measure() {
    label="$1"
    printf '  %-30s settle %ss ' "$label" "$SETTLE"
    sleep "$SETTLE"
    t_a=$(date '+%Y-%m-%d %H:%M:%S')
    a=$(snap) || { echo "- RBAMDFramebuffer not publishing; armed GPU-fb boot?"; return 1; }
    printf 'measure %ss ' "$WINDOW"
    sleep "$WINDOW"
    b=$(snap) || { echo "- lost the framebuffer mid-window"; return 1; }
    t_b=$(date '+%Y-%m-%d %H:%M:%S')
    echo "$a $b" | awk -v l="$label" -v hz="$HZ" '{
        ds = $4 - $1; dw = $5 - $2; df = $6 - $3
        sec = (df > 0) ? df / hz : 0
        printf "\n    %-28s sets %5d  writes %5d  vbl %6d  %6.2f s  -> %7.2f sets/s\n",
               l, ds, dw, df, sec, (sec > 0 ? ds / sec : 0)
        if (ds < 0 || df < 0)
            print "    *** NEGATIVE delta - counters reset (new boot) or the parser is broken. VOID."
        if (df == 0)
            print "    *** vbl did not advance - the VBL thread_call is stopped. VOID."
    }'
    echo "      WindowServer: $(capstatus "$t_a" "$t_b")"
}

echo "levertest: is an active screen capture the lever?"
echo "  endpoints: 0.08 sets/s frozen (2x5), 71.3-83.4 sets/s with AnyDesk connected (2x5/2x9)"
echo "  each arm = ${SETTLE}s settle (discarded) + ${WINDOW}s measured"
ps -axo pid=,command= | grep -i anydesk | grep -q -- "--backend" \
    && echo "  *** AnyDesk --backend is RUNNING - disconnect it or every arm is a capture arm"
echo

rm -f "$OUT"
measure "A  idle, no capture" || exit 1

# -V covers the whole arm plus margin, so the capture outlives the window and
# exits on its own. No -C: the cursor flag was in the version that wrote 0 bytes.
CAPSECS=$((SETTLE + WINDOW + 8))
echo "  starting screencapture -v -V $CAPSECS ..."
screencapture -v -V "$CAPSECS" -x -D 1 "$OUT" >/dev/null 2>&1 &
CAP=$!
sleep 5
sz1=$(stat -f%z "$OUT" 2>/dev/null || echo 0)
sleep 4
sz2=$(stat -f%z "$OUT" 2>/dev/null || echo 0)
echo "  capture pid $CAP, file ${sz1} -> ${sz2} bytes"
[ "$sz2" -gt "$sz1" ] 2>/dev/null || echo "  *** file not growing - arm B may be VOID; check the WindowServer line"

measure "B  capture ACTIVE"

wait "$CAP" 2>/dev/null
echo "  capture done, $(stat -f%z "$OUT" 2>/dev/null || echo 0) bytes"

measure "C  idle again, no capture"
rm -f "$OUT"

echo
echo "  Read the WindowServer lines FIRST. Arm B tests capture only if it shows"
echo "    'system screen capture status 1'; if A or C show it too the arms are"
echo "    not the contrast they claim to be (rule 16). n=1 per arm (rule 18)."
echo "  B high with A and C near 0.1 = CAPTURE IS THE LEVER."
echo "  All three alike = capture is dead, measured this time, and what remains"
echo "    is whatever else a live AnyDesk session holds open."
