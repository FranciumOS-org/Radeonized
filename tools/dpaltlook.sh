#!/bin/sh
# 2C0: what the chip's five RDPCSTX PHY blocks say right now - DP alt mode,
# lane power and the reference clock.
#
# It does no DECIDING of its own: RBFBDpaltSweep, RBFBDpaltActive and
# RBFBDpaltNotDisabled are computed by rb_dpalt.h in the kext, so the rule
# that keeps "alt mode active" apart from "not a Type-C PHY at all" lives in
# one place and cannot drift between the kernel and this script. What it does
# decode is per-lane, from the raw word, and it says so.
#
# WHAT ONE SAMPLE CANNOT DO. This register has no is_usb bit, so a PHY with
# DPALT_DISABLE clear is either in active alt mode or wired to nothing - the
# two read identically. The identification is the DIFFERENCE across a cable
# event, which is exactly how 2B4 identified HPD2. So: run this with the
# cable OUT, then with it IN, and the instance whose DPALT bits moved is the
# port. The no-cable run is not a wasted run; it is the baseline.
set -u

IOREG=$(ioreg -rl -c RBAMDFramebuffer 2>/dev/null)

read_key() {
    printf '%s\n' "$IOREG" |
        tr ',' '\n' | sed -n "s/.*\"$1\" = \([0-9][0-9]*\).*/\1/p" | head -1
}

# Same 32-bit sign-extension trap as hpdlook.sh: ioreg prints a 32-bit
# OSNumber with bit 31 set as a sign-extended 64-bit decimal, and shell
# arithmetic gets those wrong in two different ways. Rule 26.
as_u32() {
    python3 -I -c 'import sys; print("0x%08x" % (int(sys.argv[1]) & 0xffffffff))' \
        "${1:-0}" 2>/dev/null || echo "0x????????"
}

sweep=$(read_key RBFBDpaltSweep)

if [ -z "${sweep:-}" ]; then
    echo "no RBFBDpalt* keys: this framebuffer build does not publish them."
    echo "(rule 15: check the UUID in the COLLECTION, not the source tree:"
    echo " kmutil inspect -B /Library/KernelCollections/AuxiliaryKernelExtensions.kc \\"
    echo "   --show-kext-uuids | grep -i rembrandt)"
    exit 2
fi

# Armed and ready are separate facts and are reported separately, because
# "the user did not pass the boot-arg" and "the aperture was too small" point
# at different fixes (rule 14).
if printf '%s\n' "$IOREG" | grep -q '"RBFBDpaltArmed" = No'; then
    echo "NOT ARMED. This read is opt-in: add -rbgpufbdpalt and reboot."
    echo "  sudo tools/efi.sh bootarg add -rbgpufbdpalt"
    echo "Nothing below is a reading."
    exit 3
fi
if printf '%s\n' "$IOREG" | grep -q '"RBFBDpaltReady" = No'; then
    echo "ARMED BUT NOT READY: the BAR5 aperture did not cover 0x2ca6."
    echo "Nothing below is a reading. Check RBFBHubpRefuse."
    exit 3
fi

# The sweep verdict IS the control, so it is printed before any value.
case "${sweep:-0}" in
  0) echo "SWEEP NOT SAMPLED - no sample has been taken yet. Give it a second"
     echo "(published every RBFBPubMs, default 500 ms) and re-run."
     exit 3 ;;
  2) echo "SWEEP REFUSED: not one of the five PHYs has a reference clock or an"
     echo "enabled MPLL. The panel is scanning right now, so its PHY must be"
     echo "up - therefore these five words are NOT the five PHY blocks."
     echo "A wrong base, a wrong stride or a gated aperture all read this way,"
     echo "and boot 273 read a correctly located DMUB ring as all zeros, so"
     echo "this failure mode is measured and not hypothetical. The raw words"
     echo "follow so they can be looked at, but none of them is a reading:" ;;
  3) echo "SWEEP REFUSED: bad arguments inside the kext. This is a bug, not a"
     echo "reading - report it." ; exit 4 ;;
  1) : ;;
  *) echo "SWEEP: unrecognised verdict ${sweep}. Refusing to interpret." ; exit 4 ;;
esac

active=$(read_key RBFBDpaltActive)
notdis=$(read_key RBFBDpaltNotDisabled)
ever=$(read_key RBFBDpaltEverNotDisabled)
changes=$(read_key RBFBDpaltChanges)
: "${active:=0}" "${notdis:=0}" "${ever:=0}" "${changes:=0}"

echo
echo "RDPCSTX PHY blocks (DPCS 4.2.0 on DCN 3.1.2 - dcn31_resource.c:86)"
echo
i=0
while [ "$i" -lt 5 ]; do
    raw=$(read_key "RBFBDpalt${i}Cntl6")
    hex=$(as_u32 "${raw:-0}")
    # Per-lane decode from the raw word. PSTATE is 0x3 << (tx*4), MPLL_EN is
    # 0x4 << (tx*4) - dpcs_4_2_0_sh_mask.h. Done in python with the same
    # masking as as_u32 so the sign-extended value cannot leak into it.
    python3 -I - "$i" "${raw:-0}" "$active" "$notdis" <<'PY'
import sys
i    = int(sys.argv[1])
w    = int(sys.argv[2]) & 0xffffffff
act  = int(sys.argv[3])
nd   = int(sys.argv[4])
bit  = 1 << i

lanes = [tx for tx in range(4) if w & (0x4 << (tx * 4))]
pst   = [(w >> (tx * 4)) & 0x3 for tx in range(4)]
dp4   = bool(w & 0x00010000)
dis   = bool(w & 0x00020000)
ack   = bool(w & 0x00040000)
rclk  = bool(w & 0x00080000)
rreq  = bool(w & 0x00100000)

# POWER and ALT MODE ARE INDEPENDENT AXES and must be printed as two.
# The first version of this tool let the power axis win - a dark PHY printed
# "dark (no ref clock, no MPLL)" and never reached the alt-mode bits - which
# on boot 297 hid the only difference among the four dark PHYs: PHY1 had
# DPALT_DISABLE asserted and NOT acked where PHY2-4 had both. One axis
# reported as the state, which is the same error as reading one EDID
# descriptor and calling it the mode list (2BD).
if act & bit:
    power = "ACTIVE - ref clock and/or MPLL on"
else:
    power = "dark - no ref clock, no MPLL (pstate %s)" % \
            (" ".join(str(p) for p in pst))

if dis and ack:
    alt = "DPALT_DISABLE asserted AND acked - alt mode is off on this PHY"
elif dis:
    alt = "DPALT_DISABLE asserted, NOT acked - the PHY has not released it"
else:
    # The honest one. Without an is_usb bit these two cannot be told apart.
    alt = "DPALT_DISABLE clear - active alt mode, OR not a Type-C PHY at all"

print("  PHY%d  0x%08x" % (i, w))
print("        power: %s" % power)
print("        alt:   %s" % alt)
print("        lanes with MPLL on: %s   (%d)" %
      (",".join("TX%d" % t for t in lanes) if lanes else "none", len(lanes)))
print("        DPALT_DP4 %d  ref_clk en %d req %d" % (dp4, rclk, rreq))
PY
    i=$((i + 1))
done

printf "\n  active mask        0x%02x   (a clock or an MPLL: the CONTROL)\n" "$active"
printf "  not-disabled now   0x%02x\n" "$notdis"
printf "  not-disabled ever  0x%02x   (OR of every sample, so a cable in and\n" "$ever"
echo   "                            out between two publishes leaves a trace)"
printf "  transitions        %s\n" "$changes"

echo
echo "HOW TO READ THIS"
echo
echo "The active mask locates the PHY driving the panel - that one is up"
echo "because the desktop is on it, which is simultaneously the proof that"
echo "the base and stride are right. By elimination it narrows which instance"
echo "could be the Type-C port, the same way 2B9 used the panel's own AUX0 as"
echo "the reference for AUX2."
echo
echo "The identification itself needs the cable. Run this with the monitor"
echo "unplugged and again with it plugged in: the PHY whose DPALT bits move"
echo "is the Type-C one. Until then 'not disabled' is ambiguous on every"
echo "instance and this tool will keep saying so."
echo
echo "And the lane count is worth comparing by hand: 2BB read TWO lanes out of"
echo "the sink's own DPCD at the far end of the link. A PHY showing TX0+TX1"
echo "agrees with it from the chip's side; four lanes or none disagrees, and"
echo "which end to believe is then the question."
