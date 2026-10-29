#!/bin/sh
# 2wy: put a GPU load in the window no terminal can reach.
#
# WHY THIS EXISTS. Boots 199/200/201 each logged their first channel
# "event timeout" at +19 / +20 / +25.8 s. The only reading of the fast state
# (1.582 ms, 632 buffers/s, boot 199) has NO timestamp, so whether it preceded
# that boot's first timeout is inferred, not measured. Nothing can be run by
# hand that early, and in an rbgpu-fb=0 boot the desktop composites on the CPU
# and never loads the GPU at all - boot 201's sampler read u=6 submits in its
# first 10 s. So the load must be ours and it must come from launchd.
#
# WHAT IT IS, PRECISELY. A 120x DISCRIMINATOR, not a rate measurement:
# --bench 2 separates 1.5 ms from 180 ms with vast margin, and every line
# carries n= so no later reader mistakes a median of 2 for a rate (rule 18).
# The final sample is --bench 20 for a precise slow-state figure comparable
# to the 181.157 ms measured by hand in boot 201 at +608 s.
#
# RULE 16 - THE INSTRUMENT IS A SUSPECT. This submits GPU work, and
# metal_probe --queues 4 is a known wedge trigger. So: ONE queue, via --bench,
# never --queues. The control is already in hand: three boots with no early
# load put the first timeout at +19..+25.8 s. If this boot's first timeout
# lands in that band the daemon did not cause it; markedly earlier and the
# load itself matters, which is a result either way.
#
# RULE 21 - never let DYLD_INSERT_LIBRARIES into a launchd context. A daemon
# starts clean, but unset it explicitly so a stray global setenv cannot
# poison a root process.
unset DYLD_INSERT_LIBRARIES

: ${PROBE:=/usr/local/libexec/rbgpu-metal-probe}
: ${LOG:=/var/log/rbgpu-earlybench.log}
: ${DEADLINE:=180}          # stop at +180 s; RunAtLoad only, so once per boot
: ${STRIDE:=3}              # seconds between samples

# NOT sed 's/.*sec = ...': ".*sec = " is greedy and runs through to "usec = ",
# capturing the MICROseconds field. tools/ratesweep.sh was caught by exactly
# that and printed boot+1792667083.
boot_s() {
    bt=$(sysctl -n kern.boottime | awk -F'[=,]' '{ print $2 + 0 }')
    echo $(( $(date +%s) - bt ))
}

# Rule 14: every path out of this says something. A missing binary, a Metal
# device that will not create pre-login, a non-zero exit and an unparseable
# line are four different facts and must not all look like silence.
say() { echo "$(boot_s) $*" >> "$LOG"; }

bt=$(sysctl -n kern.boottime | awk -F'[=,]' '{ print $2 + 0 }')
{
    echo "=== rbgpu earlybench  boot_epoch=$bt  $(date '+%Y-%m-%d %H:%M:%S') ==="
    # NOT a nonce. The daemon runs before the peer increments rbgpu-boot, so
    # this reads N-1 (boot 202 printed 201, boot 203 printed 202) - the same lag
    # 2wg found in RBFBCrumbBoot. boot_epoch above is the authoritative identity:
    # match it against kern.boottime. Rule 8/33.
    echo "=== rbgpu-boot-at-start=$(nvram rbgpu-boot 2>/dev/null | awk '{print $2}')  (LAGS by one, do not read as the nonce) ==="
    echo "# t_s  n  wall_median_ms  buffers_s      (t_s = seconds since boot)"
} >> "$LOG"

if [ ! -x "$PROBE" ]; then
    say "FATAL no probe at $PROBE"
    exit 1
fi

while [ "$(boot_s)" -lt "$DEADLINE" ]; do
    t=$(boot_s)
    if [ "$t" -ge $(( DEADLINE - 15 )) ]; then n=20; else n=2; fi
    out=$("$PROBE" --bench "$n" 2>&1)
    rc=$?
    line=$(printf '%s\n' "$out" | grep 'SERIAL median')
    if [ -n "$line" ]; then
        # "  SERIAL median: wall 1.582 ms  gpu 0.013 ms  -> 632.0 buffers/s"
        ms=$(printf '%s\n' "$line" | awk '{ for (i=1;i<=NF;i++) if ($i=="wall") print $(i+1) }')
        bs=$(printf '%s\n' "$line" | awk '{ print $(NF-1) }')
        say "n=$n  $ms  $bs"
    else
        # No median: say why, and keep the raw head so a Metal device that
        # cannot be created before login is distinguishable from a crash.
        say "NOMEDIAN n=$n rc=$rc :: $(printf '%s\n' "$out" | head -3 | tr '\n' '|')"
    fi
    sleep "$STRIDE"
done

say "done"
