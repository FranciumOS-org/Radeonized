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

# THE KEXT PUBLISHES THESE AS NUMBERS, SO IOREG PRINTS THEM IN DECIMAL
# (rule 26's NVRAM clause, one level over: the reader is an instrument too).
# A 32-bit register word read as decimal is unreadable and invites a misread
# of exactly the kind 301 spent five boots on, so the conversion happens here
# rather than in the eye. An empty or non-numeric value prints as ABSENT, not
# as 0 - a missing key read as a number is rule 14's -1/0xffffffff trap.
hexof() {
  case "$1" in
    ''|*[!0-9]*) printf 'ABSENT (key not published - NOT a zero word)' ; return ;;
  esac
  printf '0x%08x' "$1"
}

# Decode a word against a mask list, printing only the bits that are SET.
# Takes: word, then name=mask pairs. Prints nothing when no listed bit is set,
# which is itself the reading on a dark instance.
dec() {
  W=$1; shift
  OUT=''
  for P in "$@"; do
    N=${P%%=*}; M=${P#*=}
    if [ $(( W & M )) -ne 0 ]; then OUT="$OUT $N"; fi
  done
  if [ -n "$OUT" ]; then printf '          set:%s\n' "$OUT"; fi
}

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
    5) VS="5 = NO_LICENCE (the clock detector is OFF - there is nothing to
                            classify, and this is the reading on every
                            instance in boot 479, the panel included)" ;;
    *) VS="$V = unknown code" ;;
  esac
  printf "  PHY%s  %s\n" "$I" "$TAG"
  printf "        verdict             %s\n" "$VS"
  if [ "$I" = "0" ] && [ "$V" != "5" ] && [ "$CV" != "1" ]; then
    printf "        *** THE PANEL IS LIVE AND THIS VERDICT IS NOT 5. The\n"
    printf "        loaded kext PREDATES the licence gate, so it classified\n"
    printf "        on an input whose licence had failed - the defect boot\n"
    printf "        479 found, not a reading about the panel. Read the raw\n"
    printf "        words below instead. ***\n"
  fi
  WC=$(get RBFBPhy${I}Cntl);      WK=$(get RBFBPhy${I}ClockCntl)
  WI=$(get RBFBPhy${I}Intr);      WS=$(get RBFBPhy${I}SramCntl)
  WP=$(get RBFBPhy${I}PhyCntl0)

  printf "        RDPCSTX_CNTL        %s\n" "$(hexof "$WC")"
  case "$WC" in ''|*[!0-9]*) ;; *)
    dec "$WC" A:PLL_UPDATE_PENDING=1024 A:DPALT_BLOCK_STATUS=1073741824 \
             D:TX_FIFO_LANE_EN=61440 D:TX_FIFO_EN=33554432 \
             D:TX_FIFO_START=67108864 D:TX_SOFT_RESET=2147483648 ;;
  esac
  printf "        RDPCSTX_CLOCK_CNTL  %s\n" "$(hexof "$WK")"
  case "$WK" in ''|*[!0-9]*) ;; *)
    dec "$WK" A:TX_CLOCK_ON=1024 A:SRAMCLK_CLOCK_ON=16384 \
             A:OCLACLK_CLOCK_ON=4194304 D:EXT_REFCLK_EN=1 \
             D:TX0_3_CLK_EN=240 D:TX_CLK_EN=512 D:SRAMCLK_EN=8192 \
             D:OCLACLK_EN=2097152 ;;
  esac
  printf "        RDPCSTX_INTERRUPT   %s\n" "$(hexof "$WI")"
  case "$WI" in ''|*[!0-9]*) ;; *)
    dec "$WI" A:TX_FIFO_ERROR_STICKY=240 A:REG_FIFO_OVERFLOW_STICKY=1 ;;
  esac
  printf "        RDPCS_TX_SRAM_CNTL  %s\n" "$(hexof "$WS")"
  printf "        RDPCSTX_PHY_CNTL0   %s\n" "$(hexof "$WP")"
  case "$WP" in ''|*[!0-9]*) ;; *)
    dec "$WP" A:REF_CLKDET_RESULT=33554432 A:SRAM_INIT_DONE=268435456 \
             A:SRAM_EXT_LD_DONE=536870912 A:RTUNE_ACK=262144 \
             D:REF_CLKDET_EN=16777216 D:PHY_RESET=1 D:TEST_POWERDOWN=8 ;;
  esac
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

A:  is an ANSWERED bit - the hardware put it there.
D:  is a DRIVEN bit - it says ONLY that software wrote it.
A bit absent from a "set:" line is clear. A register whose whole line is
missing had no listed bit set, which on a dark instance is the reading.

BOOT 479 RECORDED A LIMIT OF ITS OWN DATA, found by the break-it sweep: in
that sample A:TX_CLOCK_ON and D:TX_CLK_EN are set together on the panel and
clear together on PHY2, and so are D:TX_FIFO_EN and D:TX_FIFO_LANE_EN. So
those words pin the FINDING - the panel's PHY is out of reset with its clock
and FIFO up and PHY2's is not - but they do NOT pin which bit of each pair
carries it. Separating them needs a boot where one is set without the other.
NOTE
