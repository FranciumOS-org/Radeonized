#!/bin/sh
# 300: RDPCSPIPE_PHY_CNTL6 - the OTHER DP-alt register, and the one five boots
# did not read.
#
# WHY THIS TOOL EXISTS. Boots 459, 461, 464, 468 and 469 read
# RDPCSTX2_RDPCSTX_PHY_CNTL6 as 0x00063333, concluded PHY2's analog lane was
# dark, and logged the contradiction with QUERY_DP_ALT (which said DP alt mode
# was GRANTED on PHY2) as unresolved. `dmubbl dpaltq` had been printing the
# answer the whole time: Linux reads RDPCSPIPE_PHY_CNTL6 for UNIPHY_C on
# YELLOW_CARP_B0, not RDPCSTX's.
#
# WHAT IT CANNOT SETTLE, SAID BEFORE ANY VALUE. This register defines exactly
# three fields - DPALT_DP4, DPALT_DISABLE, DPALT_DISABLE_ACK. It has NO MPLL,
# NO reference clock and NO PSTATE. So nothing printed below says whether a
# lane is POWERED. That question is still open.
#
# The verdicts are NOT recomputed here: RBFBDpcsPipeAgree, *Sweep and
# *Verdict come from rb_dpcspipe.h in the kext, so the hosttest owns the rule
# (2C0's defect, where dpaltlook.sh reimplemented a verdict in a heredoc and
# left hosttested logic correct while what a human read was wrong).
set -u

IOREG=$(ioreg -rl -c RBAMDFramebuffer 2>/dev/null)

read_key() {
    printf '%s\n' "$IOREG" |
        tr ',' '\n' | sed -n "s/.*\"$1\" = \([0-9][0-9]*\).*/\1/p" | head -1
}

agree=$(read_key RBFBDpcsPipeAgree)

if [ -z "${agree:-}" ]; then
    echo "no RBFBDpcsPipe* keys: this framebuffer build does not publish them."
    echo "(rule 15: check the UUID in the COLLECTION, not the source tree:"
    echo " kmutil inspect -B /Library/KernelCollections/AuxiliaryKernelExtensions.kc \\"
    echo "   --show-kext-uuids | grep -i rembrandt)"
    exit 2
fi

if printf '%s\n' "$IOREG" | grep -q '"RBFBDpcsPipeReady" = No'; then
    echo "NOT READY. This read shares -rbgpufbdpalt / -rbgpufbdig / -rbgpufbfe:"
    echo "  sudo tools/efi.sh bootarg add -rbgpufbdig"
    echo "...or the BAR5 aperture did not cover dword 0x2e4b, which is ABOVE"
    echo "every other register this kext reads - the one bound here that can"
    echo "really fail. Check RBFBHubpRefuse. Nothing below is a reading."
    exit 3
fi

# THE PRODUCT FIRST, because it is the only thing here that is an answer.
case "${agree:-0}" in
  0) echo "AGREEMENT NOT EVALUATED - QUERY_DP_ALT did not answer for PHY2 in"
     echo "this boot, so there was nothing to compare against. THIS IS NOT A"
     echo "DISAGREEMENT. Run the independent instrument first:"
     echo "  sudo tools/efi.sh bootarg add -rbgpufbaltq   (then reboot)"
     echo "  build/out/dmubbl dpaltq"
     echo "and re-run this. The raw words below are still real reads." ;;
  1) echo "AGREES - an instance's DPALT_DISABLE matches the DMUB's alt_disable"
     echo "for PHY2."
     echo
     echo "THIS IS THE FINDING 300 WAS BUILT FOR. It is the first corroboration"
     echo "IN A REGISTER that PHY2's DP-alt state is what QUERY_DP_ALT says,"
     echo "from a block Linux actually reads for UNIPHY_C. It retires"
     echo "RDPCSTX2's 0x00063333 as a MISREAD of the wrong register rather"
     echo "than a measurement of a dark lane."
     echo
     echo "IT DOES NOT SAY THE LANE IS POWERED. No field here answers that."
     echo "It also does not identify WHICH instance is UNIPHY_C: the test is"
     echo "an OR over two candidates and one matching bit across two is not an"
     echo "identification (rule 18)." ;;
  2) echo "DISAGREES - neither instance's DPALT_DISABLE matches the DMUB."
     echo
     echo "This is the LARGER finding: two instruments disagree, and the ROM"
     echo "match (boot 469's is_usb mask equalled 2E1's independent VBIOS"
     echo "display-path read) is on the DMUB's side. Then the question is not"
     echo "which register but which instrument, and the next step is a third"
     echo "one - not another read of either of these two." ;;
  *) echo "AGREE: unrecognised code ${agree}. Refusing to interpret." ; exit 4 ;;
esac

sweep=$(read_key RBFBDpcsPipeSweep)
echo
case "${sweep:-0}" in
  0) echo "SWEEP NOT SAMPLED - no sample yet. Published every RBFBPubMs"
     echo "(default 500 ms); give it a second and re-run." ;;
  1) echo "SWEEP: DIFFERS - the two instances carry different DPALT fields."
     echo "This is the only verdict here that is a CONTROL: it proves these"
     echo "are TWO registers and not one address read twice, which matters"
     echo "more than usual because upstream's own table aliases indexes 2, 3"
     echo "and 4 onto these two." ;;
  2) echo "SWEEP: UNIFORM - both instances read the same DPALT fields."
     echo "NOT an error and NOT a confirmation: two real registers in the"
     echo "same state and one address read twice are indistinguishable." ;;
  3) echo "SWEEP REFUSED: bad arguments inside the kext. A bug, not a"
     echo "reading - report it." ;;
  *) echo "SWEEP: unrecognised verdict ${sweep}. Refusing to interpret." ;;
esac

echo
echo "RDPCSPIPE{0,1}_RDPCSPIPE_PHY_CNTL6 (dpcs_4_2_0_offset.h:11961-11971)"
echo

i=0
while [ "$i" -lt 2 ]; do
    raw=$(read_key "RBFBDpcsPipe${i}Cntl6")
    dw=$(read_key "RBFBDpcsPipe${i}Dw")
    verdict=$(read_key "RBFBDpcsPipe${i}Verdict")
    python3 -I - "$i" "${raw:-0}" "${dw:-0}" "${verdict:-0}" <<'PY'
import sys
i, w, dw, v = (int(x) for x in sys.argv[1:5])
w &= 0xffffffff

print("  RDPCSPIPE%d  0x%08x   at dword 0x%x, BASE_IDX 2" % (i, w, dw))
print("        DPALT_DP4          %d" % (1 if w & 0x10000 else 0))
print("        DPALT_DISABLE      %d" % (1 if w & 0x20000 else 0))
print("        DPALT_DISABLE_ACK  %d" % (1 if w & 0x40000 else 0))

names = {
    0: "0 = NOT SAMPLED - not a reading",
    1: "1 = DISABLED (set and acked): DP alt mode is OFF on this instance",
    2: "2 = SETTLING (set, not acked): mid-transition, re-read",
    3: "3 = NOT DISABLED: alt mode active -- OR this is not a Type-C PHY "
       "at all. No bit here separates those two.",
}
print("        verdict            %s" % names.get(v, "%d = unrecognised" % v))

# The low bits are where RDPCSTX keeps its MPLL and PSTATE nibbles and this
# register keeps NOTHING. Printed so a reader who has 0x00063333 in their head
# can see the difference rather than assume it.
if w & 0x0000ffff:
    print("        low 16 bits        0x%04x -- NOT a field of this register."
          % (w & 0xffff))
    print("                           RDPCSTX's PHY_CNTL6 keeps MPLL_EN and")
    print("                           PSTATE down here; this one defines")
    print("                           nothing below bit 16.")
PY
    i=$((i + 1))
done

echo
echo "HOW TO READ THIS"
echo
echo "The two instances are 0xd8 apart - the RDPCSTX stride - and upstream's"
echo "table has FIVE entries with a comment saying only two are real:"
echo "  //[Note] Hack. RDPCSPIPE only has 2 instances."
echo "So 'which instance is UNIPHY_C' is NOT established, here or anywhere in"
echo "this tree. QUERY_DP_ALT reports THREE PHYs as USB-C (is_usb mask 0x1c ="
echo "PHYs 2, 3, 4) against two registers, so the mapping cannot even be"
echo "one-to-one. The agreement above is an OR over both, deliberately."
echo
echo "AND THE ONE SENTENCE THAT MATTERS MOST: whatever this says, the lane"
echo "power question is untouched. This register has three DPALT bits and no"
echo "others. 'Is DP alt granted' and 'is the MPLL running' are two"
echo "questions, and treating them as one is what made RDPCSTX2's 0x00063333"
echo "look like a single verdict for five boots."
