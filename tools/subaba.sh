#!/bin/sh
# 2CF: the interleaved A/B for sub-vblank retire occasions.
#
# WHY A SCRIPT. Rule 44 as 2B5-2B7 corrected it: an A/B inside one boot is
# valid only INTERLEAVED, at single-window granularity. Boot 292 drifted ~15 %
# inside itself in a FIXED configuration, which is the same size as every
# effect this project has tried to measure, and `holdaba.sh`'s A1x6/Bx6/A2x6
# shape cannot tell a dial from a trend. So the sides alternate run by run,
# and nothing here is typed by hand in a block.
#
# WHAT IT MEASURES. `metal_probe --queues 8` and the FULL per-queue
# commit->GPUstart column - not just `spread`. 2C7 named that capture as the
# cheap decisive one and every run so far grepped the spread and threw the
# column away: boot 409 showed the gaps are NOT uniform (a FIRST gap of ~3
# ticks then ~1 each), so whether the first gap scales with the subdivision is
# the difference between "first paint gets faster" and "there is a fixed floor
# no dial can buy".
#
# WHAT IT DOES NOT MEASURE: the frame rate, which is the RISK side (extra
# checkTimestamps() calls in the regime 2zg measured as the quiet one, and
# 2zc's two-sided curve is the shape to watch for). Running animtest inside
# these windows would change the thing under test. Do that as its own
# interleaved pass:
#
#     sudo build/out/subret 1 && sudo tools/paired.sh 20
#     sudo build/out/subret 4 && sudo tools/paired.sh 20
#     ... alternating, >=3 per side
#
# RESTORES sub=1 AS ITS LAST ACT - which means the next command you type runs
# on the A side. That is 2B6's method error exactly: a script that restores a
# default is what invalidated the tripleset pair typed after holdaba.sh. The
# banner says so at the end rather than leaving it to be rediscovered.
#
# Does NOT need root, measured rather than assumed: on boot 424
# `IORegistryEntrySetCFProperties` on the peer took the write as the console
# user and `vblret_subsets` advanced, and metal_probe is an ordinary Metal
# client. `tickset`/`holdset` say "needs root" in their own banners; this one
# does not get to inherit that claim without checking it.
set -u
SUB=build/out/subret
PROBE=build/out/metal_probe
RUNS=${1:-3}

[ -x "$SUB" ]   || { echo "no $SUB - make subret" >&2; exit 2; }
# And on a signal: a SIGPIPE from piping this into `head` is what first left
# the dial off its default, so the restore is a trap, not a final line.
trap '"$SUB" 1 >/dev/null 2>&1; echo; echo "(interrupted - restored sub=1)"; exit 130' INT TERM PIPE
[ -x "$PROBE" ] || { echo "no $PROBE - make metal_probe" >&2; exit 2; }

# Rule 39/34, enforced: the dial is inert unless the VBLANK drives, and a null
# measured there is indistinguishable from "the subdivision does nothing".
src=$(ioreg -rl -c RembrandtGPUPeer | tr ',' '\n' | sed -n 's/.*"vblret_src"=\([0-9]*\).*/\1/p' | head -1)
sub=$(ioreg -rl -c RembrandtGPUPeer | tr ',' '\n' | sed -n 's/.*"vblret_sub"=\([0-9]*\).*/\1/p' | head -1)
if [ -z "${sub:-}" ]; then
  echo "REFUSED: vblret_sub absent - the running plugin predates 2CF." >&2
  echo "Check the loaded UUID against the build (rule 15)." >&2
  exit 1
fi
if [ "${src:-0}" != "1" ]; then
  echo "REFUSED: vblret_src is ${src:-absent}, so the TIMER drives retirement" >&2
  echo "and interior occasions are never placed - this dial is inert here." >&2
  echo "That is rule 39: a null measured in a configuration that cannot" >&2
  echo "exhibit the effect locates nothing. Use build/out/tickset instead," >&2
  echo "or switch to the native mode (build/out/modeset 100)." >&2
  exit 5
fi

echo "=== 2CF interleaved A/B, $RUNS runs per side ==="
echo "A = sub 1 (today)    B = sub 4 (three interior occasions per frame)"
ioreg -rl -c RBAMDFramebuffer | grep -E 'RBFBModeCurMhz|RBFBVBLPeriodNs|RBFBAmtrNotReady"' | sed 's/^ *| */  /'
echo

i=1
while [ "$i" -le "$RUNS" ]; do
  for s in 1 4; do
    [ "$s" = 1 ] && side=A || side=B
    echo "--- run $i  side $side  (sub $s) ---"
    # The dial is verified per run by subret itself: it polls the monotonic
    # set-counter for a FRESH snapshot (2C5) and exits 6 if sub>1 and the
    # interior retires are NOT climbing - the 2A9 shape, feature armed and
    # mechanism inert. A failure here must abort rather than measure.
    if ! "$SUB" "$s" > /tmp/.subaba.$$ 2>&1; then
      echo "  SUBRET FAILED - not measuring this run:"; sed 's/^/    /' /tmp/.subaba.$$
      rm -f /tmp/.subaba.$$
      # 2D0: RESTORE ON THIS PATH TOO. The first version exited here without
      # restoring, so boot 424 was left sitting on the B side after an abort -
      # and the next thing typed would have measured B while believing it was
      # A. Rule 34: a dial's state at the start of an experiment is part of
      # its arm, and that applies to the state a FAILURE leaves behind.
      "$SUB" 1 >/dev/null 2>&1
      echo "  (restored sub=1)"
      exit 3
    fi
    grep -E 'interior retires|vblret_sub now' /tmp/.subaba.$$ | sed 's/^/  /'
    rm -f /tmp/.subaba.$$
    # The whole column, which is the point (2C7).
    "$PROBE" --queues 8 2>&1 | sed -n '/queue   commit->GPUstart/,/spread across/p' | sed 's/^/  /'
    echo
  done
  i=$((i + 1))
done

"$SUB" 1 >/dev/null 2>&1
echo "=== restored sub=1 ==="
echo "THE NEXT COMMAND YOU TYPE RUNS ON THE A SIDE (sub 1). 2B6 lost a"
echo "milestone to exactly this: a script that restores a default is what"
echo "invalidates the pair typed after it. Set the side explicitly."
echo
echo "Read the FIRST gap as well as the spread: if queue 0's own"
echo "commit->GPUstart scales with the subdivision, first paint gets faster;"
echo "if only the later gaps scale, there is a fixed startup floor and the"
echo "dial cannot buy it (2C7's open question, never captured)."
