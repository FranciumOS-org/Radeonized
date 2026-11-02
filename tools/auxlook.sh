#!/bin/sh
# 2B8: are the DP AUX engines configured, and has a second OTG pipe lit?
#
# Read-only. The verdict that matters is a COMPARISON: the firmware trained
# the eDP panel on this chip, so one of these engines is in a known-working
# configuration right now, and an engine that matches it is one a transaction
# could plausibly run on. An engine with AUX_EN clear or a zero DPHY word was
# never brought up.
#
# The AUX-instance-to-connector mapping is in NO header - 2B4 had to find the
# HPD one by unplugging the cable. So instance 0 is compared against as a
# stated assumption and every raw word is printed, because the assumption may
# be wrong and the words are what settle it.
set -u
cd "$(dirname "$0")/.." || exit 1

rd() { ioreg -rl -c RBAMDFramebuffer 2>/dev/null |
       tr ',' '\n' | sed -n "s/.*\"$1\" = \([0-9][0-9]*\).*/\1/p" | head -1; }
u32() { python3 -I -c 'import sys; print("0x%08x" % (int(sys.argv[1]) & 0xffffffff))' \
        "${1:-0}" 2>/dev/null || echo "0x????????"; }

en=$(rd RBFBAuxEn)
if [ -z "${en:-}" ]; then
    echo "no RBFBAux* keys: this framebuffer build does not publish them."
    echo "(rule 15: check the UUID inside the COLLECTION.)"
    exit 2
fi
match=$(rd RBFBAuxMatch0)
hpdsink=$(rd RBFBHpdSink)

echo "DP AUX engines (DCN 3.1.2, DCE110 engine, five instances, read-only):"
i=0
while [ "$i" -lt 5 ]; do
    bit=$((1 << i))
    c=$(rd "RBFBAux${i}Ctl"); d=$(rd "RBFBAux${i}Dphy")
    s=$(rd "RBFBAux${i}Stat"); a=$(rd "RBFBAux${i}Arb")
    if [ $((en & bit)) -ne 0 ]; then
        if [ "$i" = 0 ]; then note="ENABLED (the assumed reference)"
        elif [ $((match & bit)) -ne 0 ]; then
            note="ENABLED and matches instance 0  <-- a transaction could run here"
        else note="ENABLED but timed differently from instance 0"
        fi
    else
        # AUX_EN clear: this engine was never brought up, and a transaction on
        # it would time out indistinguishably from an absent sink (rule 14).
        note="AUX_EN CLEAR - never brought up, says nothing about the sink"
    fi
    printf "  AUX%d  ctl %s  dphy %s  stat %s  arb %s\n        %s\n" \
        "$i" "$(u32 "$c")" "$(u32 "$d")" "$(u32 "$s")" "$(u32 "$a")" "$note"
    i=$((i + 1))
done

printf "\n  AUX_EN mask   0x%02x      matches-instance-0 mask 0x%02x\n" "$en" "$match"
printf "  HPD sink mask 0x%02x      (2B4: HPD2 is the Type-C port)\n" "${hpdsink:-0}"

echo
echo "OTG pipes, LIVE (2B5: every earlier OTG reading was a one-shot from"
echo "setup, so nothing could say whether a pipe lit with the cable in):"
i=0
while [ "$i" -lt 4 ]; do
    v=$(rd "RBFBOtgLive${i}")
    w=$(u32 "$v")
    case "$w" in
        0x00000300) st="off (the firmware's idle value)" ;;
        *) if [ "$i" = 0 ]; then st="running - this is the panel"
           else st="NOT 0x300 - SOMETHING LIT THIS PIPE"; fi ;;
    esac
    printf "  OTG%d  %s   %s\n" "$i" "$w" "$st"
    i=$((i + 1))
done

echo
echo "How to read it, with the cable PLUGGED IN:"
echo "  * AUX2 enabled and matching instance 0 -> the engine for the Type-C"
echo "    port is already configured, and a DPCD read is the next step."
echo "  * AUX2 with AUX_EN clear -> the engine is ours to bring up first,"
echo "    which is a new write surface on a block the DMCUB also arbitrates."
echo "  * OTG1..3 all 0x300 -> nothing has lit a second pipe, so the black"
echo "    screen is not a half-configured pipe we could finish."
