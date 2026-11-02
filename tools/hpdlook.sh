#!/bin/sh
# 2B3: what the chip's five HPD blocks say right now.
#
# It does NO decoding of its own: RBFBHpdSink and RBFBHpdEn are masks the kext
# computed with rb_hpd_verdict(), so the rule that keeps "no sink" apart from
# "the block is disabled" lives in one place and cannot drift between the
# kernel and this script.
#
# ONE SAMPLE IDENTIFIES NOTHING. DCN numbers the HPD blocks and the board
# wires them, and that mapping is in no header - so run this with the cable
# OUT, then with it IN, and the instance whose bit moved is the port.
set -u

read_key() {
    ioreg -rl -c RBAMDFramebuffer 2>/dev/null |
        tr ',' '\n' | sed -n "s/.*\"$1\" = \([0-9][0-9]*\).*/\1/p" | head -1
}

ready=$(read_key RBFBHpdReady)
sink=$(read_key RBFBHpdSink)
ever=$(read_key RBFBHpdSinkEver)
en=$(read_key RBFBHpdEn)
setup=$(read_key RBFBHpdSetup)
changes=$(read_key RBFBHpdChanges)

if [ -z "${sink:-}" ]; then
    echo "no RBFBHpd* keys: this framebuffer build does not publish them."
    echo "(rule 15: check the UUID in the collection, not the source tree.)"
    exit 2
fi

# RBFBHpdReady is published as a boolean, so it reads as Yes/No not 0/1.
if ioreg -rl -c RBAMDFramebuffer 2>/dev/null | grep -q '"RBFBHpdReady" = No'; then
    echo "HPD NOT ARMED - the BAR5 map or its bound check refused."
    echo "Nothing below is a reading. Check RBFBHubpRefuse."
    exit 3
fi
: "${ready:=}"

echo "HPD blocks (DCN 3.1.2, five instances, read-only):"
i=0
while [ "$i" -lt 5 ]; do
    bit=$((1 << i))
    st=$(read_key "RBFBHpd${i}Status")
    ct=$(read_key "RBFBHpd${i}Ctl")
    if [ $((en & bit)) -ne 0 ]; then
        if [ $((sink & bit)) -ne 0 ]; then
            verdict="SINK PRESENT  <-- something is plugged into this one"
        else
            verdict="no sink"
        fi
    else
        # The case this whole header exists for: a disabled block's sense bit
        # says nothing at all, which is not the same fact as "no sink".
        verdict="BLOCK DISABLED - says nothing either way"
    fi
    printf "  HPD%d  status 0x%08x  control 0x%08x   %s\n" \
        "$i" "${st:-0}" "${ct:-0}" "$verdict"
    i=$((i + 1))
done

printf "\n  sink mask now   0x%02x\n" "$sink"
printf "  sink mask ever  0x%02x   (OR of every sample, so a cable in and\n" "$ever"
echo   "                         out between two publishes still leaves a trace)"
printf "  enabled mask    0x%02x\n" "$en"
printf "  at setup        0x%02x   (whatever cable state THAT boot had)\n" "$setup"
printf "  transitions     %s\n" "${changes:-0}"

echo
echo "The reading is the DIFFERENCE. Run this with the cable out and again"
echo "with it in; the instance whose bit moved is the Type-C port. If no bit"
echo "moves and the blocks are ENABLED, the port does not reach this chip's"
echo "display engine in this firmware mode - which settles the line."
echo "Published every RBFBPubMs (default 500 ms), so give it a second."
