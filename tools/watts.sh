#!/bin/sh
# watts.sh [secs] - whole-system power draw from the battery gauge.
#
# No kext, no sudo, no SMU. AppleSmartBattery publishes InstantAmperage (mA,
# signed) and Voltage (mV), and their product is what the machine is actually
# pulling. It is the only wattmeter this project has that costs nothing.
#
# IT ONLY READS ON BATTERY. With the adapter connected and the pack full,
# amperage is 0 because nothing flows through the gauge - that is not a
# measurement of 0 W (rule 14: absence is not a result), so this refuses
# rather than printing a comforting zero. Unplug first.
#
# Amperage is a 64-bit unsigned field carrying a signed value, so a discharge
# reads as a huge positive number; it is folded back below.
secs=${1:-20}
ext=$(ioreg -rl -c AppleSmartBattery 2>/dev/null | sed -n 's/.*"ExternalConnected" = \(.*\)/\1/p' | head -1)
if [ "$ext" = "Yes" ]; then
    echo "watts.sh: adapter connected - the gauge reads 0 and that is not 0 W." >&2
    echo "          Unplug, then re-run. (Charging rate is a different question" >&2
    echo "          and needs a non-full battery.)" >&2
    exit 1
fi
echo "sampling ${secs}s on battery..."
i=0
sum=0
n=0
while [ $i -lt "$secs" ]; do
    set -- $(ioreg -rl -c AppleSmartBattery 2>/dev/null | awk '
        /"InstantAmperage"/ { gsub(/[^0-9-]/,"",$3); a=$3 }
        /"Voltage" =/       { gsub(/[^0-9-]/,"",$3); v=$3 }
        END { print a, v }')
    a=$1; v=$2
    if [ -n "$a" ] && [ -n "$v" ] && [ "$v" -gt 0 ] 2>/dev/null; then
        # fold the unsigned 64-bit field back to signed
        w=$(awk -v a="$a" -v v="$v" 'BEGIN{
            if (a > 9000000000000000000) { a = a - 18446744073709551616 }
            printf "%.2f", (-a) * v / 1000000 }')
        echo "  t+${i}s  ${w} W   (${a} mA @ ${v} mV)"
        sum=$(awk -v s="$sum" -v w="$w" 'BEGIN{printf "%.4f", s+w}')
        n=$((n+1))
    fi
    i=$((i+2))
    sleep 2
done
[ "$n" -gt 0 ] && awk -v s="$sum" -v n="$n" 'BEGIN{printf "\nmean %.2f W over %d samples\n", s/n, n}'
