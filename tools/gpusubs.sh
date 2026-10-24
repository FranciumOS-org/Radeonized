#!/bin/bash
# gpusubs.sh - does the DESKTOP COMPOSITE run on the GPU?  (2q8)
#
# No root.  `sample WindowServer` is the direct instrument but needs
# privileges; this answers the same question from a counter the kernel
# publishes to every process.
#
# The counter is `AGCInfo.fSubmissionsSinceLastCheck` on the accelerator,
# with `fLastSubmissionPID` naming who submitted last.  If SkyLight
# composited the display with CompositorMetal it would submit at display
# rate - ~134/s on this panel.  CompositorSW rasterises on the CPU and
# submits nothing.
#
# RULE 26: a counter is an instrument too.  `HWChannel GFX | Commands
# Submitted` reads 0 on this driver even while a command buffer runs to
# completion - it is DEAD and proves nothing.  So this script runs a
# known-present control FIRST (`metal_probe --dispatch`, one compute
# dispatch that verifies its own result) and REFUSES to interpret the
# desktop numbers unless the control moved the counter.
#
# Usage: tools/gpusubs.sh [seconds]      (default 10)
set -u
cd "$(dirname "$0")/.."
secs=${1:-10}
out="logs/$(date +%Y%m%d-%H%M%S)-gpusubs"
mkdir -p "$out"
exec > >(tee "$out/gpusubs.txt") 2>&1

acc() { ioreg -r -c IOAccelerator -l -w0 2>/dev/null; }
subs() { acc | grep -o '"fSubmissionsSinceLastCheck"=[0-9]*' | head -1 | cut -d= -f2; }
lastpid() { acc | grep -o '"fLastSubmissionPID"=[0-9]*' | head -1 | cut -d= -f2; }
cpu() { ps -o time= -p "$1" 2>/dev/null | tr -d ' '; }
tosec() { awk -v t="$1" 'BEGIN{n=split(t,a,":");s=0;for(i=1;i<=n;i++)s=s*60+a[i];printf "%.2f",s}'; }

echo "== preconditions (rule 14: every path says something)"
n=$(ioreg -c IOAccelerator -d1 2>/dev/null | grep -c '^+-o')
echo "IOAccelerator services        : $n"
if [ "$n" -eq 0 ]; then
    echo "INCONCLUSIVE: no accelerator in this boot - nothing to measure."
    exit 3
fi
ws=$(pgrep -x WindowServer)
echo "WindowServer pid              : ${ws:-ABSENT}"
[ -z "${ws:-}" ] && { echo "INCONCLUSIVE: no WindowServer."; exit 3; }
echo "nvram rbgpu-boot              : $(nvram rbgpu-boot 2>/dev/null | awk '{print $2}')"
echo "isAcceleratorBacked claimed   : $(ioreg -l -w0 2>/dev/null \
    | grep -o '"RBGPUDiag" = {.*}' | head -1 | tr ',' '\n' \
    | grep -E '"fbaccel_(on|set)"' | tr '\n' ' ')"

echo
echo "== CONTROL: is the counter alive?  (rule 26)"
b=$(subs)
if [ ! -x build/out/metal_probe ]; then
    echo "metal_probe not built (make tools) - control SKIPPED, so the"
    echo "desktop numbers below are UNINTERPRETABLE."
    ctl=skip
else
    env -u DYLD_INSERT_LIBRARIES ./build/out/metal_probe --dispatch >"$out/control.txt" 2>&1
    grep -qE "^  PASS" "$out/control.txt" && k=PASS || k=FAIL
    a=$(subs)
    echo "one compute dispatch, kernel result: $k"
    echo "fSubmissionsSinceLastCheck  $b -> $a   (delta $((a-b)))"
    echo "fLastSubmissionPID now      $(lastpid)  (metal_probe's own pid)"
    if [ "$k" = PASS ] && [ "$((a-b))" -ge 1 ]; then
        echo "=> counter is LIVE and attributes submissions to a pid."
        ctl=ok
    else
        echo "=> counter did NOT move for work that demonstrably ran."
        echo "   It is dead here, exactly like 'HWChannel GFX | Commands"
        echo "   Submitted'.  The desktop numbers below mean NOTHING."
        ctl=dead
    fi
fi

echo
echo "== the desktop, for ${secs}s.  Use the machine normally while this runs."
printf "%-5s %-10s %-9s %-8s %s\n" "t" "wsCPU(s)" "dCPU/s" "dSubs" "lastPID"
p=$(tosec "$(cpu "$ws")"); s0=$(subs); ps0=$s0; c0=$p
for i in $(seq 1 "$secs"); do
    sleep 1
    c=$(tosec "$(cpu "$ws")"); s=$(subs)
    printf "%-5s %-10s %-9s %-8s %s\n" "${i}s" "$c" \
        "$(awk -v a="$c" -v b="$p" 'BEGIN{printf "%.2f",a-b}')" \
        "$((s-ps0))" "$(lastpid)"
    p=$c; ps0=$s
done
s1=$(subs); c1=$(tosec "$(cpu "$ws")")
dc=$(awk -v a="$c1" -v b="$c0" 'BEGIN{printf "%.2f",a-b}')
echo
echo "== verdict"
echo "WindowServer CPU burned   : ${dc}s over ${secs}s of wall clock"
echo "GPU submissions, all pids : $((s1-s0))"
hz=$(printf '%s' "$(ioreg -l -w0 2>/dev/null | grep -o '"vbl_mhz"=[0-9]*' \
    | head -1 | cut -d= -f2)")
[ -n "$hz" ] && exp=$(( (hz/1000) * secs )) || exp=$(( 134 * secs ))
echo "a Metal composite would be : ~${exp} (one per frame at $(( exp / secs )) Hz)"
if [ "$ctl" != ok ]; then
    echo "INCONCLUSIVE: the control above did not pass."
    exit 3
fi
# Rule 14: a static screen composites NOTHING, by either compositor, so a
# quiet window is an absence and not a result.  Refuse to call it.
idle=$(awk -v d="$dc" 'BEGIN{print (d < 0.50) ? 1 : 0}')
if [ "$idle" -eq 1 ] && [ "$((s1-s0))" -eq 0 ]; then
    echo
    echo "INCONCLUSIVE: the screen was STATIC for the whole window."
    echo "WindowServer spent ${dc}s of CPU and nothing was submitted, which is"
    echo "what BOTH compositors do when no pixel changes - CompositorSW costs"
    echo "CPU per frame, not per second.  Re-run while the screen is actually"
    echo "changing: scroll a window, drag one, or open Mission Control."
    exit 3
fi
echo
echo "Submissions far below one-per-frame while WindowServer burns CPU means"
echo "the composite is on the CPU.  Note the granularity caveat: one"
echo "submission could batch several composites - so read the CPU column"
echo "with it.  A GPU composite does not cost whole CPU-seconds."
echo "saved: $out/gpusubs.txt"
