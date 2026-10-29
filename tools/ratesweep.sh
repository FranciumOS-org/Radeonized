#!/bin/sh
# 2wv: map the fast->slow transition against time since boot.
#
# Boot 199 measured 1.582 ms serial (632 buffers/s) within its first two
# minutes and 175.7 ms (5.7/s) a few minutes later, same boot, same binary,
# with rbgpu-fb=0 so the framebuffer was not involved. The 175 ms tick is
# therefore a STATE the machine falls into, not a property of it - and every
# reading in 2ws/2wt/2wu was taken inside that state. Nothing is known about
# when or why it flips.
#
# So: sample the rate on a clock, from as early as possible, and print each
# sample against seconds-since-boot. The transition time is the one number
# that makes the next experiment designable.
#
# Reads only. No kext, no arming, no NVRAM writes. Run it as the FIRST thing
# in the boot - it costs nothing and the early samples are the whole point.
#
#   sh tools/ratesweep.sh [seconds_total] [interval]
#
# Defaults sweep 6 minutes at 10 s. Output also goes to logs/ratesweep-*.txt.

TOTAL=${1:-360}
IVAL=${2:-10}
PROBE=./build/out/metal_probe
OUT="logs/ratesweep-$(date +%Y%m%d-%H%M%S).txt"

if [ ! -x "$PROBE" ]; then
    echo "no $PROBE - run: make metal_probe" >&2
    exit 1
fi
mkdir -p logs

# Seconds since boot, from kern.boottime, so samples are comparable across
# boots and against the plugin's own +12/+100/+160 s probe phases.
boot_s() {
    # NOT sed 's/.*sec = .../': ".*sec = " is greedy and matches through to
    # "usec = ", capturing the MICROseconds field. First smoke test caught it.
    bt=$(sysctl -n kern.boottime | awk -F'[=,]' '{ print $2 + 0 }')
    now=$(date +%s)
    echo $((now - bt))
}

{
    echo "ratesweep: total ${TOTAL}s interval ${IVAL}s"
    echo "boot-args rbgpu tokens: $(nvram boot-args 2>/dev/null | tr ' ' '\n' | grep -c -- '-rbgpu')"
    echo "rbgpu-boot: $(nvram rbgpu-boot 2>/dev/null | awk '{print $2}')"
    echo "rbgpu-fb:   $(nvram rbgpu-fb 2>/dev/null | awk '{print $2}')"
    echo
    printf "%8s  %12s  %10s  %s\n" "boot+s" "median(ms)" "buffers/s" "regime"
} | tee "$OUT"

START=$(boot_s)
while :; do
    T=$(boot_s)
    [ $((T - START)) -ge "$TOTAL" ] && break
    # 6 buffers was too many: boot 200 ran at 0.1 buffers/s, so a sample cost
    # a minute and the 10 s interval stretched to 34-87 s. 4 buffers is the
    # floor that still excludes the warm-up - the median of 4 takes index 2,
    # and iteration 0 is always the fastest of a serial run because it alone
    # has random phase.
    LINE=$(env -u DYLD_INSERT_LIBRARIES "$PROBE" --bench 4 2>/dev/null \
           | grep "SERIAL median")
    MS=$(echo "$LINE" | sed -n 's/.*wall \([0-9.]*\) ms.*/\1/p')
    BPS=$(echo "$LINE" | sed -n 's/.*-> \([0-9.]*\) buffers\/s.*/\1/p')
    if [ -z "$MS" ]; then
        printf "%8s  %12s  %10s  %s\n" "$T" "-" "-" "NO READING" | tee -a "$OUT"
    else
        # 20 ms is far from both observed states (1.6 ms and 176 ms), so the
        # label cannot straddle them.
        REG=$(awk -v m="$MS" 'BEGIN { print (m < 20) ? "FAST" : "slow" }')
        printf "%8s  %12s  %10s  %s\n" "$T" "$MS" "$BPS" "$REG" | tee -a "$OUT"
    fi
    sleep "$IVAL"
done
echo "saved to $OUT" | tee -a "$OUT"
