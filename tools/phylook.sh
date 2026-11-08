#!/bin/sh
# 306: the RDPCSTX PHY status survey. READ-ONLY - there is no setter.
#
# PHY0 IS THE POSITIVE CONTROL: UNIPHY_A carries the panel's live working DP
# link right now, so every hardware-answered field has a known-good value in
# the same sample as PHY2's. PHY1/3/4 were never brought up and are the
# negative control. Five identical words would mean the 0xd8 stride did
# nothing, which is why the sweep verdict is printed FIRST.
#
# This script computes NO verdict of its own. The kext decided; recomputing a
# verdict in the reader is 2C0's defect, two sources of truth that disagree
# only when it matters.
set -e
IOREG="env -u DYLD_INSERT_LIBRARIES ioreg -rl -c RBAMDFramebuffer"

get() { $IOREG 2>/dev/null | grep "\"$1\" = " | head -1 | sed 's/.*= //'; }

ARMED=$(get RBFBPhyArmed)
READY=$(get RBFBPhyReady)
SWEEP=$(get RBFBPhySweep)

if [ -z "$ARMED" ]; then
  echo "THE LOADED KEXT DOES NOT PUBLISH RBFBPhy* AT ALL."
  echo "That is not a reading about the PHY - it is the wrong kext (rule 15)."
  echo "  kmutil inspect -B /Library/KernelCollections/AuxiliaryKernelExtensions.kc --show-kext-uuids | grep -i rembrandt"
  exit 2
fi

echo "arm (-rbgpufbdpalt): $ARMED      sample ready: $READY"
if [ "$ARMED" != "1" ]; then
  echo
  echo "NOT ARMED. Nothing below is this boot's PHY state."
  echo "  sudo tools/efi.sh bootarg add -rbgpufbdpalt"
  exit 3
fi
if [ "$READY" != "Yes" ] && [ "$READY" != "1" ]; then
  echo
  echo "NO SAMPLE. The aperture did not cover the block, or the mapping is"
  echo "absent. A missing sample is not a PHY that reads zero (rule 14)."
  exit 3
fi

echo
if [ "$SWEEP" = "1" ]; then
  echo "SWEEP: DIFFERS - the five instances are not identical, so these are"
  echo "five register sets at five addresses and the 0xd8 stride did"
  echo "something."
else
  echo "SWEEP: UNIFORM - *** READ NOTHING BELOW AS A FINDING. ***"
  echo "PHY0 carries the panel's live link and the others do not, so five"
  echo "identical words mean the instance arithmetic did nothing, not that"
  echo "the PHYs agree."
fi

echo
echo "RDPCSTX PHY status, per instance (dpcs_4_2_0_offset.h, BASE_IDX 2,"
echo "stride 0xd8 - every offset confirmed against the header, not assumed)"
echo
for I in 0 1 2 3 4; do
  case $I in
    0) TAG="<-- THE PANEL (UNIPHY_A). The positive control: this one is live." ;;
    2) TAG="<-- THE TYPE-C PORT (UNIPHY_C = HPD2 = AUX2 = DIG2). The subject." ;;
    *) TAG="<-- never brought up: the negative control" ;;
  esac
  V=$(get "RBFBPhy${I}Verdict")
  CV=$(get "RBFBPhy${I}ClkDetValid")
  case "$V" in
    0) VS="0 = UNKNOWN (no sample)" ;;
    1) VS="1 = DEAD (no reference clock, no SRAM)" ;;
    2) VS="2 = CLOCKLESS (firmware loaded, detector says no clock)" ;;
    3) VS="3 = NO_SRAM (clock detected, firmware NOT loaded)" ;;
    4) VS="4 = ALIVE (reference clock detected AND SRAM init done)" ;;
    *) VS="$V = unknown code" ;;
  esac
  printf "  PHY%s  %s\n" "$I" "$TAG"
  printf "        verdict             %s\n" "$VS"
  printf "        RDPCSTX_CNTL        %s\n" "$(get RBFBPhy${I}Cntl)"
  printf "        RDPCSTX_CLOCK_CNTL  %s\n" "$(get RBFBPhy${I}ClockCntl)"
  printf "        RDPCSTX_INTERRUPT   %s\n" "$(get RBFBPhy${I}Intr)"
  printf "        RDPCS_TX_SRAM_CNTL  %s\n" "$(get RBFBPhy${I}SramCntl)"
  printf "        RDPCSTX_PHY_CNTL0   %s\n" "$(get RBFBPhy${I}PhyCntl0)"
  if [ "$CV" = "1" ]; then
    printf "        clkdet result is BELIEVABLE (the detector is enabled)\n"
  else
    printf "        clkdet result MEANS NOTHING - the detector is DISABLED,\n"
    printf "        so a 0 result is not 'no clock' (the recurring error in\n"
    printf "        one bit). The verdict above is weakened accordingly.\n"
  fi
  echo
done

cat <<'NOTE'
HOW TO READ THIS

Each register carries two KINDS of bit and they must never be read the same
way. rb_dpcsphy.h splits them and the hosttest proves the two mask sets are
disjoint at RUNTIME:

  ANSWERED - the hardware reports it. THESE ARE THE READINGS.
    PHY_CNTL0   0x02000000 REF_CLKDET_RESULT   the clock detector's output
                0x10000000 SRAM_INIT_DONE      PHY firmware loaded
                0x20000000 SRAM_EXT_LD_DONE
                0x00040000 RTUNE_ACK
    CLOCK_CNTL  0x00000400 TX_CLOCK_ON         the clock is RUNNING
                0x00004000 SRAMCLK_CLOCK_ON
                0x00400000 OCLACLK_CLOCK_ON
    CNTL        0x00000400 PLL_UPDATE_PENDING
                0x40000000 DPALT_BLOCK_STATUS
    INTERRUPT   0x000000f0 TX0..3_FIFO_ERROR   STICKY: history, not state
                0x00000001 REG_FIFO_OVERFLOW   STICKY
    SRAM_CNTL   0x30000000 MEM_PWR_PWR_STATE

  DRIVEN - software wrote it. A set bit says ONLY that software wrote it.
    CLOCK_CNTL  0x00000001 EXT_REFCLK_EN   0x000000f0 TX0..3_CLK_EN
                0x00000200 TX_CLK_EN       0x00002000 SRAMCLK_EN
                0x00200000 OCLACLK_EN
    PHY_CNTL0   0x01000000 REF_CLKDET_EN   0x00000001 PHY_RESET
                0x00000008 TEST_POWERDOWN  0x00003e00 REF_RANGE
    CNTL        0x0000f000 TX_FIFO_LANE0..3_EN        0x02000000 TX_FIFO_EN
                0x04000000 TX_FIFO_START   0x80000000 TX_SOFT_RESET

The verdict is a function of TWO ANSWERED bits only. The first version also
read the three CLOCK_ON bits, and the break-it sweep showed that breaking
them FIRED NOTHING - every branch they reached returned the same answer. They
are published raw instead of dressed up as part of a reading.

WHAT A PHY2 OF "ALIVE" WOULD AND WOULD NOT SAY: that the PHY sees a clock and
has its firmware. NOT that it is driving our link, and not that the sink will
see symbols. DPCD 0x202 over AUX2 is the only thing that answers that, and it
has read 0x00 in six boots (459, 461, 464, 468, 469, 478).
NOTE
