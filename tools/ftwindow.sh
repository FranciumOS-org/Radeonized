#!/bin/sh
# 2A0: take a frame-time budget over ONE bracketed window, and cross-check it.
#
#   sudo tools/ftwindow.sh [SECS]
#
# WHY BRACKETED. The accumulators are cumulative, and the desktop is idle most
# of the time. A budget read without a reset describes however many minutes of
# idle went before it - the gap bound discards the worst of that, but the
# remaining mix is not a frame-path measurement. Reset, run the load, read:
# then the window is exactly the load (rule 39 - say which configuration a
# reading came from, and make it be one).
#
# WHY THE CROSS-CHECK IS NOT OPTIONAL. The budget's interval and paired.sh's
# sets/s are two independent routes to the same number. 2zb reported a 194 Hz
# panel because one route had no second route to disagree with it. If the two
# disagree here, this script says so and refuses the verdict rather than
# printing a decomposition nobody should believe.
set -u

[ "$(id -u)" = 0 ] || { echo "needs root: ftbudget writes a driver property." >&2; exit 1; }
cd "$(dirname "$0")/.." || exit 1
[ -x build/out/ftbudget ] || { echo "run: make ftbudget" >&2; exit 1; }
[ -x build/out/animtest ] || { echo "run: make animtest" >&2; exit 1; }
[ -f tools/paired.sh ]   || { echo "tools/paired.sh is missing." >&2; exit 1; }

SECS=${1:-20}

prop() { ioreg -rl -c RBAMDFramebuffer | awk -F'= ' -v k="\"$1\"" \
           '$0 ~ k { gsub(/[" ]/,"",$2); print $2; exit }'; }

LINES=$(prop RBFBModeCurLines)
MHZ=$(prop RBFBModeCurMhz)
[ -n "$LINES" ] || { echo "REFUSED: no RBAMDFramebuffer in the registry." >&2; exit 2; }
echo "mode ${MHZ} mHz, ${LINES} lines"

# The ceiling is a NATIVE-mode fact: at 60 Hz boot 271 read fed 97-99 %, so
# the compositor feeds every vblank there and there is no missing half to
# decompose. Refusing is rule 39 - a measurement taken where the symptom is
# absent does not locate it.
if [ "$LINES" != "1129" ]; then
  echo "REFUSED: this is not native. The ~73 ceiling and fed ~50 % are" >&2
  echo "         native-mode facts; at 60 Hz fed is 97-99 % and there is" >&2
  echo "         nothing to decompose. Switch with build/out/modeset." >&2
  exit 3
fi

./build/out/ftbudget reset >/dev/null || exit 4
got=$(prop RBFBFtOn)
# Rule 20/26: read the property, not the command's exit status.
[ "$got" = "1" ] || { echo "REFUSED: RBFBFtOn reads '$got', not 1." >&2; exit 5; }

echo
echo "=== window: $SECS s of animtest, paired with the latch histogram ==="
PAIRED=$(sh tools/paired.sh "$SECS" 2>&1)
echo "$PAIRED"
case "$PAIRED" in *REFUSED*) echo; echo "paired.sh refused its own row, so its sets/s is not usable as a";
  echo "cross-check. The budget below stands alone - treat it as untested."; ;; esac

echo
echo "=== the budget for that window ==="
./build/out/ftbudget

# The two routes, side by side, with the disagreement computed rather than
# left to the reader.
SETS=$(echo "$PAIRED" | awk '/sets\/s/ { print $2; exit }')
RATE=$(prop RBFBFtRateMhz)
if [ -n "$SETS" ] && [ -n "$RATE" ] && [ "$RATE" != "0" ]; then
  echo
  echo "$SETS $RATE" | awk '{
    b = $2/1000.0; c = $1;
    d = 100.0*(b-c)/c;
    printf "CROSS-CHECK  budget %.2f /s  vs  counter %.2f /s   %+.1f %%\n", b, c, d;
    if (d < -5.0 || d > 5.0) {
      print "  THE TWO ROUTES DISAGREE. The budget is wrong - most likely the";
      print "  window was not exclusively the load, or samples were dropped";
      print "  (check the dropped columns). No decomposition above is a finding.";
    } else {
      print "  The two routes agree, so the decomposition is usable.";
    }
  }'
fi
