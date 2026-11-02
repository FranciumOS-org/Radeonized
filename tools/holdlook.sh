#!/bin/sh
# 2A4: turn the 2xo hold off for a few seconds so a HUMAN can look at the
# screen, then put it back. No measurement, no rows - the deliverable here is
# an observation only a person can make (rule 45), and 2A3's A/B produced the
# numbers without anyone watching.
#
#   sudo tools/holdlook.sh [SECS]        default 10
#
# The restore is in a trap, so ctrl-C, a hangup or an error all still put the
# hold back. That matters because the thing being looked FOR is a flickering
# screen, which is exactly the state in which a person wants to abort - and
# rule 37 says a feature must stay disarmable from the machine it runs on.
set -u

[ "$(id -u)" = 0 ] || { echo "needs root: the dial is a driver property." >&2; exit 1; }
cd "$(dirname "$0")/.." || exit 1
[ -x build/out/holdset ] || { echo "run: make holdset" >&2; exit 1; }
[ -x build/out/animtest ] || { echo "run: make animtest" >&2; exit 1; }

SECS=${1:-10}
restore() { echo; echo "putting the hold back..."; ./build/out/holdset 1 >/dev/null 2>&1; \
            echo "RBFBAmtrNotReady: $(ioreg -rl -c RBAMDFramebuffer | awk -F'= ' \
              '/"RBFBAmtrNotReady"/ { gsub(/[" ]/,"",$2); print $2; exit }')"; }
trap 'restore; exit 130' INT TERM HUP
trap restore EXIT

echo "The hold goes OFF for ${SECS}s with an animation running."
echo
echo "WATCH FOR: whole-frame flicker or brightness pulsing (2xo's symptom),"
echo "tearing lines, or momentary garbage. Clean is also a result."
echo
sleep 2

./build/out/holdset 0 || exit 2
env -u DYLD_INSERT_LIBRARIES ./build/out/animtest --secs "$SECS" >/dev/null 2>&1
