#!/bin/sh
# 2FC: what the six DIO_LINK{A..F}_CNTL registers say right now - which KIND
# of encoder is muxed onto each UNIPHY link.
#
# WHAT THIS CAN AND CANNOT SETTLE, SAID BEFORE ANY VALUE. ENCODER_TYPE_DIG is
# ZERO (link_encoder.h:129), so a zero word is indistinguishable from an
# unprogrammed register, a gated aperture and a wrong address. Our link is
# HBR2 8b/10b - neither HDMI FRL nor DP 128b/132b - so zero is also what
# Linux would program for it. A uniform zero sweep therefore CLOSES the
# "DIG2's symbols are muxed away from PHY2" line and is NOT a confirmation
# that the mux is right. A non-zero ENC_TYPE_SEL on our link is the one
# finding available here.
#
# The verdict is NOT recomputed here. RBFBDioSweep and RBFBDioNonzero come
# from rb_dio.h in the kext, so the rule lives in one place and the hosttest
# owns it - which is exactly what dpaltlook.sh got wrong in 2C0 by
# reimplementing rb_dpalt_verdict() in a shell heredoc, leaving hosttested
# logic correct while what a human read was wrong.
set -u

IOREG=$(ioreg -rl -c RBAMDFramebuffer 2>/dev/null)

read_key() {
    printf '%s\n' "$IOREG" |
        tr ',' '\n' | sed -n "s/.*\"$1\" = \([0-9][0-9]*\).*/\1/p" | head -1
}

sweep=$(read_key RBFBDioSweep)

if [ -z "${sweep:-}" ]; then
    echo "no RBFBDio* keys: this framebuffer build does not publish them."
    echo "(rule 15: check the UUID in the COLLECTION, not the source tree:"
    echo " kmutil inspect -B /Library/KernelCollections/AuxiliaryKernelExtensions.kc \\"
    echo "   --show-kext-uuids | grep -i rembrandt)"
    exit 2
fi

if printf '%s\n' "$IOREG" | grep -q '"RBFBDioReady" = No'; then
    echo "NOT READY. This read shares -rbgpufbdpalt / -rbgpufbdig / -rbgpufbfe:"
    echo "  sudo tools/efi.sh bootarg add -rbgpufbdig"
    echo "...or the BAR5 aperture did not cover dword 0x1f09. Check"
    echo "RBFBHubpRefuse. Nothing below is a reading."
    exit 3
fi

# The sweep verdict IS the control, so it is printed before any value.
case "${sweep:-0}" in
  0) echo "SWEEP NOT SAMPLED - no sample yet. Give it a second (published"
     echo "every RBFBPubMs, default 500 ms) and re-run."
     exit 3 ;;
  1) echo "SWEEP: DIFFERS - at least two links carry different mux fields."
     echo "This is the only verdict here that is a CONTROL: it proves these"
     echo "six words are the six registers and not six reads of a dead"
     echo "aperture. Read the per-link lines below as real values." ;;
  2) echo "SWEEP: UNIFORM - every link reads the same mux fields."
     echo
     echo "THIS IS NOT A CONFIRMATION AND IT IS NOT AN ERROR. A reset block,"
     echo "a wrong address, a gated aperture and a mux correctly left on the"
     echo "ordinary DIG path all read identically, because ENCODER_TYPE_DIG"
     echo "is zero. If the words below are all zero, the only sound"
     echo "conclusion is NEGATIVE: the mux is not carrying a value that would"
     echo "steer DIG2's symbols away from PHY2, and the line is closed. Do"
     echo "not write it down as 'the mux is confirmed on DIG'." ;;
  3) echo "SWEEP REFUSED: bad arguments inside the kext. A bug, not a"
     echo "reading - report it." ; exit 4 ;;
  *) echo "SWEEP: unrecognised verdict ${sweep}. Refusing to interpret." ; exit 4 ;;
esac

nonzero=$(read_key RBFBDioNonzero)
: "${nonzero:=0}"

echo
echo "DIO_LINK{A..F}_CNTL (dcn_3_1_2_offset.h:11737-11747, BASE_IDX 2)"
echo

i=0
while [ "$i" -lt 6 ]; do
    raw=$(read_key "RBFBDioLink${i}Cntl")
    python3 -I - "$i" "${raw:-0}" <<'PY'
import sys
i = int(sys.argv[1])
w = int(sys.argv[2]) & 0xffffffff
name = "ABCDEF"[i]

enc  = w & 0x3
hdmi = (w & 0x70) >> 4
dp   = (w & 0x700) >> 8

# Upstream's enum, by value. 0 is reported with both readings it has, never
# with only the flattering one.
if enc == 0:
    sel = "0 = ENCODER_TYPE_DIG  -- OR the register was never programmed"
elif enc == 1:
    sel = "1 = ENCODER_TYPE_HDMI_FRL"
elif enc == 2:
    sel = "2 = ENCODER_TYPE_DP_128B132B"
else:
    sel = "3 = NOT IN UPSTREAM'S ENUM - suspect this word is not this register"

# UNIPHY_C is the port, by four independent instruments (HPD2, AUX2, phyid 2,
# DIG2). Marked so the one line that matters is not hunted for by eye.
tag = ""
if name == "C":
    tag = "   <-- the Type-C port (HPD2 = AUX2 = phyid 2 = DIG2)"
elif name == "A":
    tag = "   <-- the panel's, the in-boot control"

print("  LINK%s  0x%08x%s" % (name, w, tag))
print("        ENC_TYPE_SEL      %s" % sel)
print("        HPO_HDMI_ENC_SEL  %d" % hdmi)
print("        HPO_DP_ENC_SEL    %d" % dp)
PY
    i=$((i + 1))
done

printf "\n  nonzero mask  0x%02x   (links with ANY mux field set: the CONTROL)\n" "$nonzero"

echo
echo "HOW TO READ THIS"
echo
echo "LINKC is ours. If its ENC_TYPE_SEL is 1 or 2, the mux is steering that"
echo "link to an encoder we are not programming, and that is the finding -"
echo "boots 459, 461 and 464 would then have been writing DIG2 correctly into"
echo "a path with no exit, which is exactly what 'every readback agreed and"
echo "the sink saw nothing' looks like."
echo
echo "If it is 0, this line is CLOSED and the next step is elsewhere. Note"
echo "that LINKA, the panel's, is the weak control here and not a strong one:"
echo "the panel works, so whatever its value is must be workable, but if it"
echo "too reads 0 then 0 is simply the working value and nothing is"
echo "distinguished. That is the limit of one sample on this register."
