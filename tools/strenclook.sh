#!/bin/sh
# 302: the DIO STREAM encoder - the block this project had never read or
# written, across all thirteen boots of the external-display line.
#
# WHAT THIS IS FOR. The sink has seen nothing while every write we make reads
# back changed. 301 retired the last explanation ("the analog PHY is off" was
# read off a register that does not describe UNIPHY_C). A DP link carrying no
# STREAM transmits nothing a sink recognises, and no stream encoder register
# had ever been touched here.
#
# WHY THE PANEL IS THE POINT. Instance 0 is the panel's LIVE, WORKING DP
# stream. Every field below therefore has a known-good value in the same
# sample as DIG2's, which is the strongest in-boot positive control this
# project has had for a write set. The three instances never brought up are
# the negative control in that same sample.
#
# THE VERDICTS ARE NOT RECOMPUTED HERE. RBFBStrencBase, *Sweep and *Verdict
# come from rb_strenc.h in the kext, so the hosttest owns the rule - 2C0's
# defect, where dpaltlook.sh reimplemented a verdict in a shell heredoc and
# left hosttested logic correct while what a human read was wrong.
set -u

IOREG=$(ioreg -rl -c RBAMDFramebuffer 2>/dev/null)

read_key() {
    printf '%s\n' "$IOREG" |
        tr ',' '\n' | sed -n "s/.*\"$1\" = \([0-9][0-9]*\).*/\1/p" | head -1
}

base=$(read_key RBFBStrencBase)

if [ -z "${base:-}" ]; then
    echo "no RBFBStrenc* keys: this framebuffer build does not publish them."
    echo "(rule 15: check the UUID in the COLLECTION, not the source tree:"
    echo " kmutil inspect -B /Library/KernelCollections/AuxiliaryKernelExtensions.kc \\"
    echo "   --show-kext-uuids | grep -i rembrandt)"
    exit 2
fi

if printf '%s\n' "$IOREG" | grep -q '"RBFBStrencReady" = No'; then
    echo "NOT READY. This read shares -rbgpufbdpalt / -rbgpufbdig / -rbgpufbfe:"
    echo "  sudo tools/efi.sh bootarg add -rbgpufbdig"
    echo "...or the BAR5 aperture did not cover dword 0x254f. Check"
    echo "RBFBHubpRefuse. Nothing below is a reading."
    exit 3
fi

htotal=$(read_key RBFBStrencHTotal)
vtotal=$(read_key RBFBStrencVTotal)
activew=$(read_key RBFBStrencActiveW)
activeh=$(read_key RBFBStrencActiveH)
agree=$(read_key RBFBStrencActiveAgree)

# THE GATE FIRST. Nothing below it is a reading unless this confirms.
case "${base:-0}" in
  0) echo "BASE NOT SAMPLED - no sample yet. Published every RBFBPubMs"
     echo "(default 500 ms); give it a second and re-run."
     exit 3 ;;
  1) echo "BASE CONFIRMED - instance 0's DP_MSA_TIMING_PARAM1 decodes as this"
     echo "boot's own live OTG0 raster: OTG H_TOTAL ${htotal:-?} / V_TOTAL ${vtotal:-?},"
     echo "MSA carrying each PLUS ONE (the OTG register holds total-1,"
     echo "dcn10_optc.c:191; MSA holds the true total,"
     echo "dcn10_stream_encoder.c:419). The offsets are right, by a check the"
     echo "chip had to satisfy rather than by reasoning." ;;
  2) echo "BASE REFUSED: H_TOTAL DISAGREES. Instance 0 does not decode as the"
     echo "live raster, and H_TOTAL does not move with DRR."
     echo
     echo "BEFORE CONCLUDING THE ADDRESSES ARE WRONG, CHECK THE OFF-BY-ONE."
     echo "MSA holds the TRUE total and the OTG register holds total-1, so MSA"
     echo "must read ${htotal:-?}+1 and ${vtotal:-?}+1. Boot 472 refused here for"
     echo "exactly that reason and the addresses were correct. If the MSA word"
     echo "below is off by any other amount, then it is an address fault."
     echo
     echo "NOTHING BELOW IS A READING until that is settled."
     exit 4 ;;
  3) echo "BASE: V_TOTAL ODD - H_TOTAL agrees but V_TOTAL is outside the DRR"
     echo "window. Weaker than an H_TOTAL mismatch and reported separately,"
     echo "because that is what a DRR transition mid-sample looks like and it"
     echo "is not an address fault. Re-run; if it persists, read 2FB before"
     echo "concluding anything (143.83 Hz was once taken as true while the"
     echo "panel sat at 61.40)." ;;
  4) echo "BASE: instance 0's word is ZERO. That is what a dead aperture, a"
     echo "gated block and a wrong segment all read, and it is NOT a"
     echo "disagreement. NOTHING BELOW IS A READING."
     exit 4 ;;
  5) echo "BASE: NO REFERENCE - this boot has no live OTG0 raster to compare"
     echo "against, so no check was made. Not a failure and not a pass. The"
     echo "raw words below are still real reads of real addresses, but the"
     echo "addresses are UNVERIFIED this boot."
     ;;
  *) echo "BASE: unrecognised verdict ${base}. Refusing to interpret." ; exit 4 ;;
esac

echo
sweep=$(read_key RBFBStrencSweep)
case "${sweep:-0}" in
  0) echo "SWEEP NOT SAMPLED." ;;
  1) echo "SWEEP: DIFFERS - the five instances are not identical, so these are"
     echo "five register sets at five addresses and the 0x100 instance stride"
     echo "did something." ;;
  2) echo "SWEEP: UNIFORM - all five DP_VID_STREAM_CNTL words are identical."
     echo
     echo "THIS IS A DEFECT, NOT A NULL. Unlike the DIO mux, uniformity here"
     echo "cannot be the truth: instance 0 is a RUNNING stream and the others"
     echo "are not. Five identical words mean the instance arithmetic did"
     echo "nothing. Treat the per-instance lines below as suspect." ;;
  3) echo "SWEEP REFUSED: bad arguments inside the kext. A bug, not a reading." ;;
  *) echo "SWEEP: unrecognised verdict ${sweep}." ;;
esac

# THE SECOND ADDRESS CHECK, which DRR cannot move. Printed next to the base
# verdict because the two fail for different reasons.
echo
case "${agree:-}" in
  1) echo "ACTIVE RASTER AGREES - MSA's HWIDTH/VHEIGHT equal OTG0's own active"
     echo "window, ${activew:-?} x ${activeh:-?}, derived from the blank registers in the"
     echo "same sample. This is the stronger of the two address checks: unlike"
     echo "either total it does NOT move with the DRR ladder, so it needs no"
     echo "window and no slack." ;;
  0) echo "ACTIVE RASTER DISAGREES - MSA's HWIDTH/VHEIGHT do not equal OTG0's"
     echo "active window of ${activew:-?} x ${activeh:-?}. Since this check has no DRR"
     echo "window to hide behind, a disagreement here is a STRONGER address"
     echo "signal than the totals. Read the MSA_PARAM4 word below." ;;
  -1) echo "ACTIVE RASTER: NO REFERENCE - OTG0's blank words did not give a sane"
      echo "window, so no comparison was made. NOT a disagreement." ;;
  *) echo "ACTIVE RASTER: key absent or unrecognised (${agree:-none})." ;;
esac

en=$(read_key RBFBStrencEnMask)
st=$(read_key RBFBStrencStatusMask)
run=$(read_key RBFBStrencRunMask)

echo
printf "  ENABLE mask  0x%02x   what a driver WRITES  (DP_VID_STREAM_ENABLE)\n" "${en:-0}"
printf "  STATUS mask  0x%02x   what the HARDWARE answers (DP_VID_STREAM_STATUS)\n" "${st:-0}"
printf "  RUNNING mask 0x%02x   both\n" "${run:-0}"
echo
echo "  These are MASKS, not indexes or counts: bit 2 is DIG2. RBFBAuxEn and"
echo "  RBFBHpdSink were masks read as indexes and that cost two misreads"
echo "  (2FB, rule 33)."
echo
echo "  The gap between ENABLE and STATUS is the whole value of this block as"
echo "  an instrument. Every other reading on this line either answered our"
echo "  own writes or came from the wrong register."

echo
echo "DP stream encoder, per instance (dcn_3_1_2_offset.h, BASE_IDX 2)"
echo

i=0
while [ "$i" -lt 5 ]; do
    verdict=$(read_key "RBFBStrenc${i}Verdict")
    words=""
    r=0
    while [ "$r" -lt 14 ]; do
        w=$(read_key "$(printf 'RBFBStrenc%uR%02u' "$i" "$r")")
        words="$words ${w:-0}"
        r=$((r + 1))
    done
    python3 -I - "$i" "${verdict:-0}" $words <<'PY'
import sys
i = int(sys.argv[1]); v = int(sys.argv[2])
w = [int(x) & 0xffffffff for x in sys.argv[3:]]

# The ONE mapping from published index to register name. rb_strenc.h's enum is
# the source; this list follows it and the kext publishes bare indexes so the
# names live in exactly one place.
NAMES = ["DP_PIXEL_FORMAT", "DP_MSA_COLORIMETRY", "DP_MSA_MISC",
         "DP_MSA_TIMING_PARAM1", "DP_MSA_TIMING_PARAM2",
         "DP_MSA_TIMING_PARAM3", "DP_MSA_TIMING_PARAM4", "DP_SEC_FRAMING4",
         "DP_VID_STREAM_CNTL", "DP_STEER_FIFO", "DP_VID_TIMING",
         "DP_VID_N", "DP_VID_M", "DIG_FE_CNTL"]
BASE = [0x2109, 0x210a, 0x210e, 0x214c, 0x214d, 0x214e, 0x214f, 0x2130,
        0x210c, 0x210d, 0x2110, 0x2111, 0x2112, 0x208b]

tag = ""
if i == 0:
    tag = "   <-- THE PANEL. The positive control: this one must be RUNNING."
elif i == 2:
    tag = "   <-- the Type-C port (HPD2 = AUX2 = phyid 2 = DIG2)"
else:
    tag = "   <-- never brought up: the negative control"

VERD = {
    0: "0 = NOT SAMPLED - not a reading",
    1: "1 = RUNNING (enable and status both set)",
    2: "2 = ASKED, NOT RUNNING (enable set, status CLEAR) -- the state "
       "upstream's own REG_WAIT exists for",
    3: "3 = STATUS WITHOUT ENABLE - should not occur; suspect the word is "
       "not this register",
    4: "4 = OFF (both clear)",
}

print("  DIG%d / DP%d%s" % (i, i, tag))
print("        verdict  %s" % VERD.get(v, "%d = unrecognised" % v))
for n, (name, b) in enumerate(zip(NAMES, BASE)):
    print("        %-22s 0x%08x   (dword 0x%x)" % (name, w[n], b + i * 0x100))

# The two decodes a reader would otherwise do by eye and get wrong.
p1 = w[3]
print("        -> MSA H_TOTAL %d, V_TOTAL %d" % ((p1 >> 16) & 0xffff, p1 & 0xffff))
fe = w[13]
print("        -> DIG_SOURCE_SELECT %d (which OTG feeds this front end), "
      "DIG_START %d" % (fe & 0x7, 1 if fe & 0x400 else 0))
print()
PY
    i=$((i + 1))
done

# ---- 304: the MSA derivation, and the panel as its control ----
#
# THE PRODUCT IS PRINTED FIRST AND THE VERDICT IS NEVER RECOMPUTED HERE
# (2C0's defect). The kext decided it; this reads it.
echo "=== 304: THE MSA DERIVATION, CHECKED AGAINST THE PANEL"
echo

MM=$(ioreg -rl -c RBAMDFramebuffer 2>/dev/null |
     sed -n 's/.*"RBFBStrencMsaMismatch" = \([0-9-]*\).*/\1/p' | head -1)

if [ -z "$MM" ]; then
    echo "  RBFBStrencMsaMismatch ABSENT."
    echo
    echo "  This build does not publish it, or the framebuffer loaded is not"
    echo "  this one. Check the UUID INSIDE THE COLLECTION before reading"
    echo "  anything else (rule 15):"
    echo "    kmutil inspect -B /Library/KernelCollections/AuxiliaryKernelExtensions.kc \\"
    echo "      --show-kext-uuids | grep -i rembrandt"
    exit 1
fi

case "$MM" in
0)
    echo "  MISMATCH 0 - THE DERIVATION REPRODUCES THE PANEL BIT FOR BIT."
    echo
    echo "  All four MSA words derived from OTG0's eight registers equal"
    echo "  DIG0's four live words, in one sample. The arithmetic that would"
    echo "  program DIG2 is correct on hardware that is actually running a DP"
    echo "  stream - which is the only thing that licenses writing it"
    echo "  anywhere (rule 47 asked of a write)."
    ;;
16)
    echo "  MISMATCH 16 - THE DERIVATION WAS REFUSED, not disagreed with."
    echo "  Read RBFBStrencP0DeriveRc for which refusal: 1 no args, 2 total"
    echo "  zero, 3 blank zero, 4 blank END >= START, 5 active zero,"
    echo "  6 active >= total, 7 sync width zero, 8 sync START non-zero,"
    echo "  9 field overflow. 'Could not derive' is NOT 'derived and agreed'."
    ;;
*)
    echo "  MISMATCH $MM - THE DERIVATION DISAGREES WITH THE PANEL."
    echo
    echo "  The mask names the word: 1 PARAM1, 2 PARAM2, 4 PARAM3, 8 PARAM4,"
    echo "  summed. Compare RBFBStrencP0Msa1..4 against RBFBStrenc0R03..R06."
    echo
    echo "  BEFORE CONCLUDING THE ADDRESSES ARE WRONG, CHECK THE CONVENTIONS."
    echo "  Boot 472 refused this way for the off-by-one alone: the OTG"
    echo "  register holds 'total - 1' and MSA holds the true total. If only"
    echo "  PARAM3 (bit 2) disagrees, suspect the sync fields specifically -"
    echo "  the hosttest could never check those against hardware, because"
    echo "  boot 473 never read the sync registers and their test inputs were"
    echo "  back-derived from the expected output. THIS BOOT is their first"
    echo "  real check, so a PARAM3-only mismatch is a genuine finding and"
    echo "  not a regression."
    ;;
esac
echo

for pi in 0 1; do
    [ "$pi" = 0 ] && echo "  --- pipe 0 (the panel's, the control)" \
                  || echo "  --- pipe 1 (Type-C; derived but NOT checkable)"
    RC=$(ioreg -rl -c RBAMDFramebuffer 2>/dev/null |
         sed -n "s/.*\"RBFBStrencP${pi}DeriveRc\" = \([0-9-]*\).*/\1/p" | head -1)
    echo "      DeriveRc ${RC:-ABSENT}  (0 = OK)"
    for k in 0 1 2 3 4 5 6 7; do
        V=$(ioreg -rl -c RBAMDFramebuffer 2>/dev/null |
            sed -n "s/.*\"RBFBStrencP${pi}Otg${k}\" = \([0-9-]*\).*/\1/p" | head -1)
        [ -z "$V" ] && continue
        NAME=$(echo "H_TOTAL H_BLANK_START_END H_SYNC_A H_SYNC_A_CNTL V_TOTAL V_BLANK_START_END V_SYNC_A V_SYNC_A_CNTL" | cut -d' ' -f$((k + 1)))
        printf "      OTG %-20s 0x%08x\n" "$NAME" "$V"
    done
    for k in 1 2 3 4; do
        V=$(ioreg -rl -c RBAMDFramebuffer 2>/dev/null |
            sed -n "s/.*\"RBFBStrencP${pi}Msa${k}\" = \([0-9-]*\).*/\1/p" | head -1)
        [ -z "$V" ] && continue
        printf "      derived MSA_TIMING_PARAM%d  0x%08x\n" "$k" "$V"
    done
    echo
done

echo "  Pipe 1's derivation is printed because it is what a write would use."
echo "  It has NO control: there is no running stream on DIG2 to compare it"
echo "  against. An unchecked intermediate is published rather than hidden"
echo "  so it cannot later be narrated as a hardware result (rule 17)."
echo

echo "HOW TO READ THIS"
echo
echo "Instance 0 MUST come back RUNNING. It is the panel; it is lit; if it"
echo "does not, the addresses are wrong whatever the base check said, and that"
echo "is the first thing to fix."
echo
echo "For DIG2 the expected reading BEFORE any write of ours is OFF with its"
echo "MSA registers blank, because nothing has ever programmed them. That is"
echo "not a null result - it is the positive identification of what is"
echo "missing from the chain, and the fields instance 0 has populated are the"
echo "list of what a write would have to set."
echo
echo "If DIG2 instead comes back with MSA timing already programmed, the"
echo "firmware did more at boot than this project has assumed, and the"
echo "comparison against instance 0 says exactly which fields differ."
echo
echo "NOTHING HERE WRITES. There is no setter in rb_strenc.h and nothing in"
echo "the tree may add one before a boot has confirmed the base."
