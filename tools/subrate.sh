#!/bin/sh
# 2D2: the FRAME-RATE side of 2CF, interleaved.
#
# WHY. Boot 426 measured sub 4 at 116.62 sets/s / fed 81.2 % against sub 1 at
# 70.90 / 48.4 % - **+64 %, in the direction I named as the RISK**. 2CF said to
# watch for 2zc's two-sided curve costing frame rate; instead the rate went up
# by two thirds with the hold still ON, so there is no flicker trade in it.
#
# That lands on the frontier this card has called open since 2zd: `fed` ~48 %
# is "the compositor hands us a transaction only on every second vblank" (2A0),
# the ~73 ceiling that killed seven models. B pins at **99 % of its own latch
# ceiling** where A sits at 48 %.
#
# BUT IT WAS n=1 PER SIDE AND NOT INTERLEAVED - two commands typed in
# sequence, B then A, which is precisely the shape that made 2B6
# unattributable (A1->A2 drift +15.1 % against a +15.9 % effect). The effect
# here is 4.3x that drift against this boot's own A and 1.8x it against the
# best hold-ON reading ever recorded (~92), so it is very likely real - and
# "very likely" is not what rule 44 asks for. This script is the repeat:
# alternating, single-window granularity, with the dial verified per run.
#
# WHAT TO WATCH BESIDES THE RATE. B's latch moved - mean vblanks 1.218 with
# bin2+ 21.6 %, against A's 1.000 / 0.0 %. More occasions per frame means more
# flips in flight, so that is expected rather than alarming, and `preempt`
# went +3 -> +7 (with the hold ON, where 2B0/2B6 recorded +0/+2). 2B7 is the
# warning: `preempt` does NOT order by a dial in a non-interleaved comparison,
# so read it here, interleaved, before attaching any meaning to it.
#
# paired.sh already refuses a row whose `vbl/s` disagrees with the mode's own
# rate by over 5 %, which is the cross-check 2zb's 194 Hz panel lacked.
#
# RESTORES sub=1 as its last act, and says so - 2B6's method error was a
# restoring script invalidating the next command typed.
#
# Needs root (paired.sh does; subret does not).
set -u
SUB=build/out/subret
RUNS=${1:-3}
SIDES=${2:-"1 4"}

[ -x "$SUB" ] || { echo "no $SUB - make subret" >&2; exit 2; }
[ -x tools/paired.sh ] || { echo "no tools/paired.sh" >&2; exit 2; }

trap '"$SUB" 1 >/dev/null 2>&1; echo; echo "(interrupted - restored sub=1)"; exit 130' INT TERM PIPE

src=$(ioreg -rl -c RembrandtGPUPeer | tr ',' '\n' | sed -n 's/.*"vblret_src"=\([0-9]*\).*/\1/p' | head -1)
sub=$(ioreg -rl -c RembrandtGPUPeer | tr ',' '\n' | sed -n 's/.*"vblret_sub"=\([0-9]*\).*/\1/p' | head -1)
[ -n "${sub:-}" ] || { echo "REFUSED: vblret_sub absent - plugin predates 2CF (rule 15)." >&2; exit 1; }
if [ "${src:-0}" != "1" ]; then
  echo "REFUSED: vblret_src is ${src:-absent} - the TIMER drives, so interior" >&2
  echo "occasions are never placed and this dial is inert (rule 39)." >&2
  exit 5
fi

OUT=$(mktemp /tmp/subrate.XXXXXX)
echo "=== 2D2 interleaved frame-rate A/B: sides [$SIDES], $RUNS runs each ==="
ioreg -rl -c RBAMDFramebuffer | grep -E 'RBFBModeCurMhz|RBFBAmtrNotReady"' | sed 's/^ *| */  /'
echo

i=1
while [ "$i" -le "$RUNS" ]; do
  for s in $SIDES; do
    echo "--- run $i   sub $s ---"
    if ! "$SUB" "$s" > /tmp/.subrate.$$ 2>&1; then
      echo "  SUBRET FAILED - not measuring this run:"; sed 's/^/    /' /tmp/.subrate.$$
      rm -f /tmp/.subrate.$$; "$SUB" 1 >/dev/null 2>&1
      echo "  (restored sub=1)"; rm -f "$OUT"; exit 3
    fi
    rm -f /tmp/.subrate.$$
    r=$(sudo tools/paired.sh 20 2>&1)
    echo "$r" | grep -E 'sets/s|mean vblanks|bin2|latch ceiling|REFUS' | sed 's/^/  /'
    rate=$(echo "$r" | sed -n 's/.*sets\/s *\([0-9.]*\).*/\1/p' | head -1)
    fed=$(echo  "$r" | sed -n 's/.*fed \([0-9.]*\) %.*/\1/p' | head -1)
    mv=$(echo   "$r" | sed -n 's/.*mean vblanks *\([0-9.]*\).*/\1/p' | head -1)
    pre=$(echo  "$r" | sed -n 's/.*preempt +\([0-9]*\).*/\1/p' | head -1)
    printf '%s %s %s %s %s\n' "$s" "${rate:-NA}" "${fed:-NA}" "${mv:-NA}" "${pre:-NA}" >> "$OUT"
    echo
  done
  i=$((i + 1))
done

"$SUB" 1 >/dev/null 2>&1
echo "=== per-side summary (interleaved, so a trend cannot masquerade as the dial) ==="
awk '{n[$1]++; r[$1]+=$2; f[$1]+=$3; m[$1]+=$4; p[$1]+=$5
      if (lo[$1]=="" || $2+0<lo[$1]) lo[$1]=$2; if ($2+0>hi[$1]) hi[$1]=$2}
     END{ printf "  sub   n   sets/s mean    range          fed %%   mean vbl  preempt\n"
          for (s in n) printf "  %-3s  %2d   %7.2f   %6.2f-%-6.2f  %5.1f   %6.3f   %5.1f\n", \
               s, n[s], r[s]/n[s], lo[s], hi[s], f[s]/n[s], m[s]/n[s], p[s]/n[s] }' "$OUT" | sort
rm -f "$OUT"
echo
echo "=== restored sub=1. THE NEXT COMMAND YOU TYPE RUNS ON THE A SIDE. ==="
echo "Compare the per-side means AND the ranges: if they overlap, the boot"
echo "drifted as much as the dial moved and this is 2B6 again (rule 44)."
