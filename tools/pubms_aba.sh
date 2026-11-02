#!/bin/sh
# 2zy: the A/B/A that 2zm built pubms for, at NATIVE, inside ONE boot.
#
#   sudo tools/pubms_aba.sh [SECS] [RUNS]       (defaults 20 and 6)
#
# WHY THIS SHAPE AND NOT A COMPARISON BETWEEN BOOTS. Rule 44 was paid for
# twice. 2ze found ten runs on one boot spanning 63.98-100.77 at native, so a
# single 20 s sample cannot resolve under ~20 %. Then 2zg found that six
# INTERNALLY CONSISTENT samples on boot 267 (78.14-94.90, mean 90.4) did not
# reproduce - boot 268 gave 72.50 (n=12) in the same configuration with the
# gate provably inert. Boot-to-boot variance exceeds within-boot variance, so
# six samples inside one boot measure that boot and nothing else.
#
# Hence A/B/A rather than A/B: if A1 and A2 agree with each other and B
# differs from both, the difference is the DIAL and not the boot, and no
# second boot is needed. If A1 and A2 disagree, the boot drifted during the
# run and the B rows mean nothing - which the summary says outright instead of
# averaging them into a verdict (rule 14).
#
# WHAT IT TESTS. 2zk measured publishVBLState() running 227 times a second
# from setInterruptState - twice per transaction, because Apple arms and
# disarms the vblank around each one - at ~110 setProperty calls each, on
# Apple's own thread, INSIDE the call that gates every frame. ~25 000 registry
# insertions a second. Six models of the ~73 sets/s ceiling died without
# anyone examining it (rule 16: the instrument is a suspect). Boot 271 measured
# the throttle at 60 Hz: 49.0 -> 59.43. At NATIVE it has never been measured,
# and the arithmetic is why it is worth a run: two publishes per transaction at
# ~110 registry insertions each, if a pair costs ~6-7 ms, is one whole native
# frame period - which is what `fed` ~50 % looks like.
#
# B is pubms 0, the UNTHROTTLED pre-2zk behaviour. A is 500, the default.
set -eu

[ "$(id -u)" = 0 ] || { echo "run with sudo: sudo tools/pubms_aba.sh" >&2; exit 1; }

root="$(cd "$(dirname "$0")/.." && pwd)"
cd "$root"
SECS=${1:-20}
RUNS=${2:-6}
out=/tmp/pubms_aba.$$
: > "$out"

prop() { ioreg -rl -c RBAMDFramebuffer | awk -F'= ' -v k="\"$1\"" \
           '$0 ~ k { gsub(/[" ]/,"",$2); print $2; exit }'; }

# Rule 34/39: say what the experiment depends on and verify it BEFORE any row
# is taken, because a reading from the wrong configuration does not locate
# anything.
lines=$(prop RBFBModeCurLines)
mhz=$(prop RBFBModeCurMhz)
pub=$(prop RBFBPubMs)
vblret=$(prop RBFBVblRetArmed)
skips=$(prop RBFBVblRetModeSkips)
echo "preconditions: mode ${mhz} mHz, ${lines} lines, RBFBPubMs ${pub}, vblret ${vblret}, ModeSkips ${skips}"
[ "$lines" = "1129" ] || {
  echo "REFUSED: not at native (1129 lines). 2zf measured a sub-native mode" >&2
  echo "         retiring at half its vblank rate, so a rate from one is not" >&2
  echo "         comparable. Run: ./build/out/modeset 143.8" >&2
  exit 2; }
[ -n "$pub" ] || { echo "REFUSED: no RBFBPubMs - this kext predates 2zm." >&2; exit 2; }

phase() {                      # phase <label> <pubms>
  lbl=$1; want=$2
  ./build/out/pubms "$want" >/dev/null 2>&1 || true
  got=$(prop RBFBPubMs)
  # Rule 20/26: read the property, never the command's exit status.
  [ "$got" = "$want" ] || {
    echo "REFUSED: asked RBFBPubMs=$want, the kext reports $got." >&2
    echo "         Nothing below would be attributable, so stopping." >&2
    exit 3; }
  echo
  echo "=== $lbl : RBFBPubMs = $got  ($RUNS runs of ${SECS}s) ==="
  i=1
  while [ "$i" -le "$RUNS" ]; do
    r=$(sh tools/paired.sh "$SECS" 2>/dev/null || true)
    if printf '%s' "$r" | grep -q REFUSED; then
      printf '  run %d: REFUSED (vbl/s off the mode rate - row discarded)\n' "$i"
      printf '%s skip 0 0\n' "$lbl" >> "$out"
    else
      s=$(printf '%s' "$r" | awk '/sets\/s/ {print $2}')
      f=$(printf '%s' "$r" | awk '/sets\/s/ {for(i=1;i<=NF;i++) if($i=="fed") print $(i+1)}')
      printf '  run %d: sets/s %-8s fed %s %%\n' "$i" "$s" "$f"
      printf '%s ok %s %s\n' "$lbl" "$s" "$f" >> "$out"
    fi
    i=$((i+1))
  done
}

phase A1 500
phase B  0
phase A2 500

echo
echo "restoring the default"
./build/out/pubms 500 >/dev/null 2>&1 || true
echo "RBFBPubMs now $(prop RBFBPubMs)"

echo
echo "================ SUMMARY ================"
awk '
  $2=="ok" { n[$1]++; sum[$1]+=$3; fsum[$1]+=$4
             if (mn[$1]=="" || $3+0 < mn[$1]+0) mn[$1]=$3
             if (mx[$1]=="" || $3+0 > mx[$1]+0) mx[$1]=$3 }
  $2=="skip" { bad[$1]++ }
  END {
    split("A1 B A2", ord, " ")
    for (k=1; k<=3; k++) {
      p = ord[k]
      if (n[p] > 0)
        printf "%-3s n=%d  mean sets/s %7.2f   min %7s  max %7s   fed %5.1f %%%s\n",
               p, n[p], sum[p]/n[p], mn[p], mx[p], fsum[p]/n[p],
               (bad[p] ? sprintf("   (%d discarded)", bad[p]) : "")
      else
        printf "%-3s NO USABLE ROWS\n", p
    }
    if (n["A1"] && n["A2"] && n["B"]) {
      a1 = sum["A1"]/n["A1"]; a2 = sum["A2"]/n["A2"]; b = sum["B"]/n["B"]
      drift = 100.0*(a2-a1)/a1
      a = (a1+a2)/2.0
      printf "\nA1 -> A2 drift %+.1f %%", drift
      if (drift < -8 || drift > 8) {
        printf "  *** THE BOOT DRIFTED ***\n"
        printf "The two A phases disagree by more than the dial could be worth,\n"
        printf "so the B rows are not attributable to RBFBPubMs. This is the\n"
        printf "2zg failure caught in one boot instead of two. No verdict.\n"
      } else {
        printf "  (the A phases agree, so B is attributable to the dial)\n"
        printf "B vs mean(A)   %+.1f %%   (%.2f against %.2f)\n", 100.0*(b-a)/a, b, a
        printf "\nB is pubms 0, the UNTHROTTLED behaviour. A NEGATIVE number here\n"
        printf "means the publish cost is real and the 500 ms throttle is buying\n"
        printf "it back. A number inside +-5 %% means publishVBLState is NOT the\n"
        printf "~73 ceiling, and it joins the six dead models (2zb/2zc/2zd/2zg).\n"
      }
    }
  }' "$out"
rm -f "$out"
