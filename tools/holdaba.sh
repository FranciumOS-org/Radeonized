#!/bin/sh
# 2A2: A/B/A the 2xo kIOReturnNotReady hold at native, in ONE boot.
#
#   sudo tools/holdaba.sh [SECS] [RUNS]
#
# A = hold ON (the shipping configuration), B = hold OFF. 2A1 measured the
# hold owning ~21 % of the transaction interval and 2xo recorded 27 % when it
# was introduced, so the expected B is roughly +25 %. That is a PREDICTION
# this script exists to test, not a result.
#
# *** THIS ONE HAS A VISIBLE COST AND YOU MUST WATCH FOR IT. ***
# The hold is what stopped the compositor drawing into a surface that is still
# the live scanout (2xo). With it OFF, expect the whole-frame flicker 2xo
# fixed. That flicker is DATA, not a crash: it is the other half of the trade,
# and a B phase that is fast AND clean would mean 2xo's diagnosis was wrong.
# So note whether the screen tears or flickers during B and say so - rule 45,
# the deliverable is something only a human can see.
#
# Nothing here reboots and nothing here is persistent: the dial is a property,
# the script always restores A, and `./build/out/ftbudget` plus
# `sudo ./build/out/holdset 1` would put it back by hand if this is killed.
set -u

[ "$(id -u)" = 0 ] || { echo "needs root: the dial is a driver property." >&2; exit 1; }
cd "$(dirname "$0")/.." || exit 1
[ -x build/out/holdset ] || { echo "run: make holdset" >&2; exit 1; }
[ -x build/out/animtest ] || { echo "run: make animtest" >&2; exit 1; }

SECS=${1:-20}
RUNS=${2:-6}

prop() { ioreg -rl -c RBAMDFramebuffer | awk -F'= ' -v k="\"$1\"" \
           '$0 ~ k { gsub(/[" ]/,"",$2); print $2; exit }'; }

LINES=$(prop RBFBModeCurLines)
[ -n "$LINES" ] || { echo "REFUSED: no RBAMDFramebuffer in the registry." >&2; exit 2; }
[ "$LINES" = "1129" ] || {
  echo "REFUSED: not native ($LINES lines). The hold's share was measured at" >&2
  echo "         native and the flip timing differs per mode (rule 39)." >&2
  exit 3; }

# Rule 34: say what this depends on and check it before any row. A boot that
# never armed the hold has nothing to turn off, and every row would be a B row
# labelled A.
armed=$(prop RBFBAmtrNotReady)
case "$armed" in
  Yes|yes|true|1) ;;
  *) echo "REFUSED: RBFBAmtrNotReady reads '$armed' - this boot does not have" >&2
     echo "         the 2xo hold armed, so there is no A side to compare." >&2
     echo "         It needs -rbgpufbnotready in boot-args (tools/efi.sh)." >&2
     exit 4 ;;
esac
echo "native, 2xo hold armed. A = hold ON, B = hold OFF."
echo "*** WATCH THE SCREEN DURING B: 2xo's flicker is the other half of the"
echo "*** trade, and whether it appears is part of the result."

phase() {                      # phase <label> <0|1>
  lbl=$1; want=$2
  ./build/out/holdset "$want" >/dev/null 2>&1 || true
  got=$(prop RBFBAmtrNotReady)
  case "$got" in
    Yes|yes|true|1) have=1 ;;
    *) have=0 ;;
  esac
  # Rule 20/26: read the property, never the command's exit status, and read
  # it in the representation it is published in - a bool prints as `Yes`.
  [ "$have" = "$want" ] || {
    echo "REFUSED: asked RBFBAmtrNotReady=$want, the kext reports $got." >&2
    ./build/out/holdset 1 >/dev/null 2>&1
    exit 5; }
  echo
  echo "=== $lbl : hold $([ "$want" = 1 ] && echo ON || echo OFF)  ($RUNS runs of ${SECS}s) ==="
  i=1
  while [ "$i" -le "$RUNS" ]; do
    row=$(sh tools/paired.sh "$SECS" 2>&1)
    case "$row" in
      *REFUSED*) echo "  run $i: REFUSED (discarded)" ;;
      *) echo "$row" | awk -v n="$i" -v f="$MEANDIR/$lbl" '/sets\/s/ {
             printf "  run %d: sets/s %-8s fed %s %%\n", n, $2, $8
             print $2 >> f }' ;;
    esac
    i=$((i+1))
  done
}

MEANDIR=$(mktemp -d) || exit 1
trap 'rm -rf "$MEANDIR"' EXIT INT TERM

phase A1 1
phase B  0
phase A2 1

echo
echo "restoring the shipping configuration"
./build/out/holdset 1 >/dev/null 2>&1
echo "RBFBAmtrNotReady now $(prop RBFBAmtrNotReady)"
# 2B6: COMPUTE the verdict rather than describing it. The old ending printed
# "if A1 and A2 disagree by more than ~8 % the boot drifted" and left the
# arithmetic to the reader - and boot 292 came in at +15.1 % drift against
# B's +15.9 % gain, i.e. the drift was the SAME SIZE as the effect, which is
# easy to miss by eye across eighteen rows. 2A1's lesson: a threshold stated
# in prose is not a guard, and after 2A1 a threshold must also be shown not to
# be load-bearing - so the drift and the gain are both PRINTED, and the
# refusal names which one it is refusing on.
mean() { [ -s "$1" ] && awk '{s+=$1; n++} END {if (n) printf "%.2f", s/n}' "$1"; }
a1=$(mean "$MEANDIR/A1"); b=$(mean "$MEANDIR/B"); a2=$(mean "$MEANDIR/A2")
echo
if [ -z "$a1" ] || [ -z "$b" ] || [ -z "$a2" ]; then
    echo "NO VERDICT: a phase produced no usable rows."
else
    awk -v a1="$a1" -v b="$b" -v a2="$a2" 'BEGIN {
        printf "A1 %.2f   B %.2f   A2 %.2f\n", a1, b, a2
        drift = (a2 - a1) / a1 * 100.0
        base  = (a1 + a2) / 2.0
        gain  = (b - base) / base * 100.0
        printf "A1 -> A2 drift %+.1f %%     B vs mean(A) %+.1f %%\n", drift, gain
        ad = drift < 0 ? -drift : drift
        ag = gain  < 0 ? -gain  : gain
        if (ad > 8.0 && ad >= ag * 0.5) {
            print "REFUSED: the boot drifted by as much as the effect, so B is"
            print "NOT attributable to the dial. Rule 44 - this needs another boot,"
            print "and 2B6 is what reading it anyway would have recorded."
        } else if (ad > 8.0) {
            print "CAUTION: drift above 8 %, but small against the effect."
            print "Treat the gain as a lower bound and repeat on a second boot."
        } else {
            print "A1 and A2 agree, so B IS attributable to the dial."
            print "Then the question is the FLICKER, not the frame rate (rule 45):"
            print "a fast AND clean B would mean the 2xo diagnosis was wrong."
        }
    }'
fi
echo
echo "The original reading, kept because it is what the thresholds mean:"
echo "if A1 and A2 disagree by more than ~8 % the boot drifted and B is not"
echo "attributable. If they agree and"
echo "B is ~+25 %, the hold's measured 21 % share is confirmed end to end -"
echo "and then the question is the flicker, not the frame rate."
