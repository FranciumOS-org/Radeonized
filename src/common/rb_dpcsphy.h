/* rb_dpcsphy.h - 306: the RDPCSTX PHY's own status, read-only.
 *
 * WHY THIS FILE EXISTS
 *
 * By boot 478 the encoder side is discharged. The blocker is excluded from:
 * DP-alt disable (301, and the RDPCSPIPE index question settled below), lane
 * count and scrambler (2E5's `enc1_configure_encoder`, read in boot 478 as
 * ALREADY CORRECT on DIG2 - DP_CONFIG 1 = 2 lanes against the panel's 3 = 4
 * lanes and 0 on the three dark instances), the back-end enable (477), the
 * held steer FIFO (477), DP_VID_STREAM_DIS_DEFER (source grounds, 303), the
 * raster and the pixel clock (2FB-2FE), PIPE_DTO_SRC_SEL (468), hpdsel and
 * power gating (469), the stream/training order (305b, boot 478), and the
 * DPIA branch (`DISPLAY_ENDPOINT_USB4_DPIA` is USB4 tunneling; this is plain
 * DP alt mode, so `dcn31_link_encoder_enable_dp_output` takes the
 * not-mappable arm into the atom table we already execute).
 *
 * What has NEVER been read is whether the PHY itself is alive: does it see a
 * reference clock, is its firmware SRAM loaded, are its lane clocks running,
 * are its transmit FIFOs enabled, has it logged an error. Those are five
 * registers in `dpcs_4_2_0_offset.h` and this file reads them.
 *
 * THE POINT OF EVERY FIELD HERE IS WHO ANSWERS IT. 302 built the
 * ENABLE-vs-STATUS split for the stream encoder because every earlier reading
 * on this line either answered our own writes or came from the wrong
 * register. The same discipline applies: the fields below are split into
 * DRIVEN (what software writes, which says only that software wrote it) and
 * ANSWERED (what the hardware reports, which is the reading). A survey that
 * mixed them would be another instrument that confirms our own stores.
 *
 * ------------------------------------------------- the RDPCSPIPE index, settled
 *
 * `rb_dpcspipe.h` refused to decide which RDPCSPIPE instance is UNIPHY_C,
 * correctly, because the five-entry table in the generated header is a
 * compile hack by its own comment. It is decidable by ELIMINATION rather than
 * by trusting the table:
 *
 *   - Upstream's `DPCS_DCN31_REG_LIST(id)` is indexed by the LINK ENCODER id,
 *     not by phyid (`dcn31_resource.c:404`, `link_regs(id, phyid)` - the
 *     phyid goes to UNIPHY_DCN2_REG_LIST and the id to DPCS). DIG2 is id 2.
 *   - The header aliases index 2 onto `0x2d73`, which is instance 0.
 *   - Boot 471 read instance 0 as NOT DISABLED and instance 1 as
 *     DISABLED+acked.
 *   - `QUERY_DP_ALT` is ROM-attested on three boots (449, 470, 471) and of
 *     the three USB-C PHYs only PHY2 reads `alt_disable` 0.
 *
 * If the alias ran the other way, UNIPHY_C's register would read
 * DISABLED+acked and contradict the ROM instrument. It does not. So the
 * mapping is not believed because the table says so - the alternative is
 * CONTRADICTED by a reading, which is a different and better argument. DP alt
 * is NOT disabled on our PHY, and that answer now has two sources.
 *
 * This file still touches none of that. It has NO SETTER.
 */
#ifndef RB_DPCSPHY_H
#define RB_DPCSPHY_H

#include <stdint.h>

/* dpcs_4_2_0_offset.h, BASE_IDX 2 (DMU segment 2). The stride between
 * instances is 0xd8 dwords, which is RDPCSTX1_RDPCSTX_CNTL 0x2a08 minus
 * RDPCSTX0_RDPCSTX_CNTL 0x2930 - taken from the two offsets rather than
 * assumed, and checked for all five instances by the hosttest. */
#define RB_DPCSPHY_COUNT            5u
#define RB_DPCSPHY_STRIDE           0xd8u

/* The panel's PHY. Refused by name everywhere, and the POSITIVE CONTROL:
 * UNIPHY_A carries a working DP link to the built-in panel right now, so
 * every ANSWERED field below has a known-good value in the same sample. */
#define RB_DPCSPHY_PANEL_INST       0u
/* UNIPHY_C, the Type-C port: HPD2 = AUX2 = phyid 2 = DIG2. */
#define RB_DPCSPHY_TYPEC_INST       2u

/* THE DARK USB-C CONTROL, and 308's lesson as a constant. PHY3 is a USB-C
 * PHY by ROM-attested QUERY_DP_ALT (is_usb mask 0x1c = PHYs 2, 3, 4) with DP
 * alt mode DISABLED, where PHY2 has it GRANTED - and in boot 481 the two
 * read byte-identical across all five registers then surveyed. So PHY3 is
 * the control that asks the question the panel cannot: is a difference
 * PHY2's OWN, or just the signature of a PHY nobody brought up? 308's SRAM
 * candidate died on exactly that distinction, and died only because the
 * dark instances were in the same sample. */
#define RB_DPCSPHY_DARK_USBC_INST   3u

#define RB_DPCSPHY_DW_CNTL          0x2930u  /* RDPCSTX_CNTL              */
#define RB_DPCSPHY_DW_CLOCK_CNTL    0x2931u  /* RDPCSTX_CLOCK_CNTL        */
#define RB_DPCSPHY_DW_INTERRUPT     0x2932u  /* RDPCSTX_INTERRUPT_CONTROL */
#define RB_DPCSPHY_DW_SRAM_CNTL     0x2936u  /* RDPCS_TX_SRAM_CNTL        */
#define RB_DPCSPHY_DW_PHY_CNTL0     0x2940u  /* RDPCSTX_PHY_CNTL0         */

/* 309: FOUR MORE, ADDED BECAUSE THE FIVE ABOVE CANNOT TELL PHY2 FROM PHY3.
 * Boot 481 read all five registers byte-identical on those two instances
 * while ROM-attested QUERY_DP_ALT says PHY2 has DP alt GRANTED and PHY3 has
 * it DISABLED. A survey that cannot separate the port we are driving from
 * the one beside it is not a survey of the thing we care about, whatever
 * else it reports. These four are named for that question.
 *
 * RDPCSTX_DPALT_CONTROL_REG is the one worth the boot:
 * RDPCS_ALLOW_DRIVER_ACCESS and RDPCS_DRIVER_ACCESS_BLOCKED. "Who owns this"
 * asked of the hardware rather than of a struct (rule 42's question, one
 * level down). If PHY2 reads BLOCKED while the live panel reads ALLOWED, it
 * accounts for the whole picture at once - PHY_RESET never clearing, the
 * atom ENABLE moving nothing, and DP_VID_M refusing a store - without a
 * new mechanism. If all five read alike it is not the answer, and the sweep
 * says which.
 *
 * RDPCSTX_DMCU_DPALT_DIS_BLOCK_REG is DMCU's side of the same interface,
 * and carries RDPCS_DMCU_DPALT_FORCE_TX_CLK_DIS - a forced TX clock
 * disable, which is the state TX_CLOCK_ON already reports on PHY2.
 *
 * RDPCSTX_DMCU_DPALT_PHY_CNTL6 is DMCU's shadow of the DPALT fields, so it
 * is a THIRD source on DPALT_DISABLE beside RDPCSPIPE's (what Linux reads)
 * and RDPCSTX's (301's retired misread). 301 left that an OR over two
 * registers; this makes it three, and three sources can disagree in a way
 * that names the odd one out.
 *
 * RDPCSTX_CNTL2 carries RDPCS_PHY_ENC_TYPE_SEL, the per-PHY encoder type,
 * which is 2FC's DIO_LINK*_CNTL question asked at the PHY end.
 *
 * ALL FOUR ARE READ-ONLY HERE. There is no setter in this file and the
 * ownership register in particular must not get one: a driver that writes
 * ALLOW_DRIVER_ACCESS to grant itself access is not a driver that was
 * granted it. */
#define RB_DPCSPHY_DW_CNTL2         0x2939u  /* RDPCSTX_CNTL2             */
#define RB_DPCSPHY_DW_DIS_BLOCK     0x293cu  /* ..DMCU_DPALT_DIS_BLOCK_REG*/
#define RB_DPCSPHY_DW_DMCU_CNTL6    0x2955u  /* ..DMCU_DPALT_PHY_CNTL6    */
#define RB_DPCSPHY_DW_DPALT_CTRL    0x2956u  /* RDPCSTX_DPALT_CONTROL_REG */

enum rb_dpcsphy_reg {
    RB_DPCSPHY_R_CNTL = 0,
    RB_DPCSPHY_R_CLOCK_CNTL,
    RB_DPCSPHY_R_INTERRUPT,
    RB_DPCSPHY_R_SRAM_CNTL,
    RB_DPCSPHY_R_PHY_CNTL0,
    RB_DPCSPHY_R_CNTL2,
    RB_DPCSPHY_R_DIS_BLOCK,
    RB_DPCSPHY_R_DMCU_CNTL6,
    RB_DPCSPHY_R_DPALT_CTRL,
    RB_DPCSPHY_R_COUNT
};

/* ===================================================================
 * ANSWERED - the hardware reports these. THESE ARE THE READINGS.
 * =================================================================== */

/* RDPCSTX_PHY_CNTL0. REF_CLKDET_RESULT is the reference-clock detector's
 * own output: the most direct statement available of whether the PHY has a
 * clock at all. SRAM_INIT_DONE says the PHY's own SRAM init finished.
 * RTUNE_ACK is the PHY acknowledging a tune request. */
#define RB_DPCSPHY_A_REF_CLKDET_RESULT  0x02000000u
#define RB_DPCSPHY_A_SRAM_INIT_DONE     0x10000000u
#define RB_DPCSPHY_A_RTUNE_ACK          0x00040000u

/* RDPCSTX_CLOCK_CNTL. The three CLOCK_ON bits are the hardware saying the
 * clock is actually running, as distinct from the EN bits below, which only
 * say somebody asked. That distinction is this file's whole reason for
 * splitting the masks. */
#define RB_DPCSPHY_A_TX_CLOCK_ON        0x00000400u
#define RB_DPCSPHY_A_SRAMCLK_CLOCK_ON   0x00004000u
#define RB_DPCSPHY_A_OCLACLK_CLOCK_ON   0x00400000u

/* RDPCSTX_CNTL. */
#define RB_DPCSPHY_A_PLL_UPDATE_PENDING 0x00000400u
#define RB_DPCSPHY_A_DPALT_BLOCK_STATUS 0x40000000u

/* RDPCSTX_INTERRUPT_CONTROL - error flags, and STICKY: they say what
 * happened, not what is true now (rule 33). Read as history. */
#define RB_DPCSPHY_A_REG_FIFO_OVERFLOW  0x00000001u
#define RB_DPCSPHY_A_TX_FIFO_ERROR      0x000000f0u  /* TX0..TX3 */
#define RB_DPCSPHY_A_DPALT_DIS_TOGGLE   0x00000002u
#define RB_DPCSPHY_A_DPALT_4LANE_TOGGLE 0x00000004u

/* RDPCS_TX_SRAM_CNTL - the memory's power state, which the hardware sets. */
#define RB_DPCSPHY_A_MEM_PWR_PWR_STATE  0x30000000u

/* RDPCSTX_CNTL2. A FIFO's own occupancy is reported BY the FIFO - there is
 * no other possible writer of "empty" and "full" - so these two are
 * ANSWERED by construction rather than by a caller we read. Said here
 * because it is the only new mask claiming a side without a source. */
#define RB_DPCSPHY_A_CR_FIFO_EMPTY      0x00000001u
#define RB_DPCSPHY_A_CR_FIFO_FULL       0x00000002u

/* ===================================================================
 * DRIVEN - software writes these. A set bit here says only that software
 * wrote it, never that the hardware did anything. Published so the pair
 * can be compared, never read as a verdict.
 * =================================================================== */
#define RB_DPCSPHY_D_EXT_REFCLK_EN      0x00000001u
#define RB_DPCSPHY_D_TX_LANE_CLK_EN     0x000000f0u  /* TX0..TX3_CLK_EN */
#define RB_DPCSPHY_D_TX_CLK_EN          0x00000200u
#define RB_DPCSPHY_D_SRAMCLK_EN         0x00002000u
#define RB_DPCSPHY_D_OCLACLK_EN         0x00200000u
#define RB_DPCSPHY_D_PHY_REF_CLKDET_EN  0x01000000u
#define RB_DPCSPHY_D_TX_FIFO_LANE_EN    0x0000f000u  /* LANE0..3 */
#define RB_DPCSPHY_D_TX_FIFO_EN         0x02000000u
#define RB_DPCSPHY_D_TX_FIFO_START      0x04000000u
#define RB_DPCSPHY_D_TX_SOFT_RESET      0x80000000u
#define RB_DPCSPHY_D_CBUS_SOFT_RESET    0x00000001u
#define RB_DPCSPHY_D_SRAM_SOFT_RESET    0x00000002u
#define RB_DPCSPHY_D_PHY_RESET          0x00000001u
#define RB_DPCSPHY_D_TEST_POWERDOWN     0x00000008u
#define RB_DPCSPHY_D_PHY_REF_RANGE      0x00003e00u

/* ===================================================================
 * UNATTRIBUTED - RDPCSTX_PHY_CNTL0's SRAM-load handshake. These are in a
 * SET OF THEIR OWN BECAUSE NOTHING IN THIS TREE ESTABLISHES WHO WRITES
 * THEM, and 308 found SRAM_EXT_LD_DONE sitting in the ANSWERED set on
 * nothing but its name. No vendored Linux .c mentions either field: the
 * only source we have for them is `dpcs_4_2_0_sh_mask.h`, which gives the
 * bit and not the owner. Upstream's DCN20-era DPCS init writes EXT_LD_DONE
 * from software, which would make it DRIVEN, but that code is not in this
 * tree and this chip's DCN31 NULLs every dpcs_* hook, so we have no caller
 * to read (rule 46 - check the version grouping of the METHOD). Calling
 * them ANSWERED would make a 0 read as "the hardware says not loaded";
 * calling them DRIVEN would make it read as "we never wrote it". Both are
 * claims, so the file makes neither and publishes them under their own
 * name until a source says otherwise.
 *
 * WHY THEY MATTER: 308's field-by-field decode of the live panel against
 * PHY2 - the first time these words were compared per FIELD and not as
 * hex - found that EXT_LD_DONE and BYPASS are the ONLY PHY_CNTL0 bits that
 * differ besides PHY_RESET itself and REF_RANGE. The panel has both SET and
 * its PHY out of reset; PHY2 has both CLEAR and its PHY held. That is a
 * candidate mechanism and NOT a mechanism: a correlation on n=1 per side,
 * with no caller identified and no second lit instance to check it against
 * (rule 18). It is recorded as the thing to measure, not as the answer.
 *
 * AND THE TWO BITS ARE NOT INDEPENDENT EVIDENCE. In boot 479's words they
 * are SET TOGETHER on the panel (0xb0100004) and CLEAR TOGETHER on PHY2
 * (0x10100605), so the comparison is ONE observation about the SRAM
 * handshake and not two. The break-it sweep is what established that:
 * swapping the sram_bypass accessor to read EXT_LD_DONE fired nothing
 * against the real data. The hosttest pins each accessor to its own bit
 * with synthetic words; separating them on hardware needs a sample where
 * one is set without the other.
 * =================================================================== */
#define RB_DPCSPHY_U_SRAM_EXT_LD_DONE   0x20000000u
#define RB_DPCSPHY_U_SRAM_BYPASS        0x80000000u

/* 309's four registers are UNATTRIBUTED almost entirely, and for a reason
 * sharper than "no caller": their writer is DMCU, which is NEITHER of the
 * two parties the ANSWERED/DRIVEN split was built to separate. A bit DMCU
 * sets is not our store and not the hardware reporting its own state - it
 * is a third writer's. That is the clearest argument yet for the U set
 * existing at all, and it is why none of these may be read as a verdict:
 * on an interface whose point is that somebody ELSE owns the PHY, "who
 * wrote this" is the question, not a detail.
 *
 * RDPCSTX_DPALT_CONTROL_REG. */
#define RB_DPCSPHY_U_ALLOW_DRV_ACCESS   0x00000001u
#define RB_DPCSPHY_U_DRV_ACCESS_BLOCKED 0x00000010u
/* RDPCSTX_DMCU_DPALT_DIS_BLOCK_REG. */
#define RB_DPCSPHY_U_DMCU_DIS_BLOCK     0x00000001u
#define RB_DPCSPHY_U_DMCU_FORCE_TXCLKDIS 0x00000010u
/* RDPCSTX_DMCU_DPALT_PHY_CNTL6 - DMCU's shadow of the DPALT fields. Every
 * field in this register is named _RESERVED upstream, which is itself the
 * statement that it is not the driver's to write. */
#define RB_DPCSPHY_U_DMCU_DPALT_DP4     0x00010000u
#define RB_DPCSPHY_U_DMCU_DPALT_DISABLE 0x00020000u
#define RB_DPCSPHY_U_DMCU_DPALT_DIS_ACK 0x00040000u
#define RB_DPCSPHY_U_DMCU_REF_CLK_EN    0x00080000u
#define RB_DPCSPHY_U_DMCU_REF_CLK_REQ   0x00100000u
/* RDPCSTX_CNTL2 - the encoder-type select. A mux input, so plausibly
 * DRIVEN, but nothing in this tree writes it and 2FC could not name the
 * caller of the DIO-side equivalent either, so it stays here. */
#define RB_DPCSPHY_U_PHY_ENC_TYPE_SEL   0x0000000cu

/* A whole instance's five words. */
struct rb_dpcsphy_inst {
    uint32_t w[RB_DPCSPHY_R_COUNT];
};

static inline uint32_t rb_dpcsphy_reg_dw(enum rb_dpcsphy_reg r)
{
    switch (r) {
    case RB_DPCSPHY_R_CNTL:        return RB_DPCSPHY_DW_CNTL;
    case RB_DPCSPHY_R_CLOCK_CNTL:  return RB_DPCSPHY_DW_CLOCK_CNTL;
    case RB_DPCSPHY_R_INTERRUPT:   return RB_DPCSPHY_DW_INTERRUPT;
    case RB_DPCSPHY_R_SRAM_CNTL:   return RB_DPCSPHY_DW_SRAM_CNTL;
    case RB_DPCSPHY_R_PHY_CNTL0:   return RB_DPCSPHY_DW_PHY_CNTL0;
    case RB_DPCSPHY_R_CNTL2:       return RB_DPCSPHY_DW_CNTL2;
    case RB_DPCSPHY_R_DIS_BLOCK:   return RB_DPCSPHY_DW_DIS_BLOCK;
    case RB_DPCSPHY_R_DMCU_CNTL6:  return RB_DPCSPHY_DW_DMCU_CNTL6;
    case RB_DPCSPHY_R_DPALT_CTRL:  return RB_DPCSPHY_DW_DPALT_CTRL;
    default: break;
    }
    return 0u;
}

/* The dword for one instance's register, or 0 for a bad argument - 0 is not
 * a valid offset in this block, so a refusal cannot be mistaken for one. */
static inline uint32_t rb_dpcsphy_dw(uint32_t inst, enum rb_dpcsphy_reg r)
{
    const uint32_t base = rb_dpcsphy_reg_dw(r);
    if (inst >= RB_DPCSPHY_COUNT || base == 0u) { return 0u; }
    return base + inst * RB_DPCSPHY_STRIDE;
}

/* ------------------------------------------------------------ the verdict
 *
 * Built from ANSWERED bits only, and deliberately coarse: this says whether
 * the PHY is alive, not whether it is carrying our link. A finer verdict
 * would be reading a mechanism out of a bitfield, which is rule 17.
 */
enum rb_dpcsphy_verdict {
    RB_DPCSPHY_V_UNKNOWN   = 0,  /* no sample                              */
    RB_DPCSPHY_V_DEAD      = 1,  /* no ref clock, no SRAM                  */
    RB_DPCSPHY_V_CLOCKLESS = 2,  /* SRAM loaded but the detector says no clk */
    RB_DPCSPHY_V_NO_SRAM   = 3,  /* clock detected but firmware not loaded  */
    RB_DPCSPHY_V_ALIVE     = 4,  /* ref clock detected AND SRAM init done   */
    /* THE DETECTOR IS OFF, so no clock-based verdict is available. Boot 479
     * is why this value exists: with REF_CLKDET_EN clear on all five
     * instances, the first version of this function called the PANEL'S OWN
     * LIVE PHY "CLOCKLESS". It classified on an input whose licence had
     * failed - the recurring error, in the one place written to prevent it.
     * A verdict with no basis is now a verdict of its own, not a guess. */
    RB_DPCSPHY_V_NO_LICENCE = 5
};

/* THE CLOCK_ON BITS ARE NOT IN THE VERDICT, and that is deliberate.
 *
 * The first version of this function read them into an `on` term. The
 * break-it sweep then found that breaking `on` FIRED NOTHING, because every
 * branch `on` could reach returned DEAD anyway: it was a term in a
 * conjunction carrying no information (rule 48's exact shape), dressed up as
 * part of the reading. Rather than invent a verdict for a state no boot has
 * shown, the three CLOCK_ON bits are published RAW and the verdict stays a
 * two-input function of the two bits whose meaning is established.
 *
 * Both inputs are ANSWERED bits. A break that swaps either for its DRIVEN
 * neighbour is caught by a failing test. */
static inline int rb_dpcsphy_clkdet_valid(const struct rb_dpcsphy_inst *in);

static inline int rb_dpcsphy_verdict(const struct rb_dpcsphy_inst *in)
{
    uint32_t p;
    int clk, sram;

    if (!in) { return RB_DPCSPHY_V_UNKNOWN; }
    /* THE LICENCE COMES FIRST. Both clock-bearing verdicts rest on
     * REF_CLKDET_RESULT, and that bit means nothing while the detector is
     * disabled, so there is nothing to classify. */
    if (!rb_dpcsphy_clkdet_valid(in)) { return RB_DPCSPHY_V_NO_LICENCE; }

    p = in->w[RB_DPCSPHY_R_PHY_CNTL0];
    clk  = (p & RB_DPCSPHY_A_REF_CLKDET_RESULT) ? 1 : 0;
    sram = (p & RB_DPCSPHY_A_SRAM_INIT_DONE) ? 1 : 0;

    if (clk && sram)  { return RB_DPCSPHY_V_ALIVE; }
    if (sram)         { return RB_DPCSPHY_V_CLOCKLESS; }
    if (clk)          { return RB_DPCSPHY_V_NO_SRAM; }
    return RB_DPCSPHY_V_DEAD;
}

/* ---------------------------------------------------- the bits that MATTERED
 *
 * Boot 479 did not turn on the clock-detector verdict above. What it answered
 * came from three bits whose licence is the PANEL IN THE SAME SAMPLE rather
 * than an enable bit, and they are given accessors so the reader never has to
 * mask by hand:
 *
 *   PHY0 (the live panel)  reset 0   tx_clock_on 1   fifo_en 1
 *   PHY2 (the Type-C port) reset 1   tx_clock_on 0   fifo_en 0
 *
 * PHY_RESET is DRIVEN - software holds it - so a 1 says software has not
 * released it, which is exactly the finding. TX_CLOCK_ON is ANSWERED;
 * TX_FIFO_EN is DRIVEN.
 *
 * A LIMIT OF BOOT 479'S DATA, recorded because the break-it sweep found it:
 * in that sample TX_CLOCK_ON (0x400) and the DRIVEN TX_CLK_EN (0x200) are
 * SET TOGETHER on the panel and CLEAR TOGETHER on PHY2, and so are
 * TX_FIFO_EN and the per-lane TX_FIFO_LANE*_EN. Swapping either accessor for
 * its neighbour fired NOTHING against the real words. So those words pin the
 * FINDING - the panel's PHY is released with its clock and FIFO up and PHY2's
 * is not - but they do NOT pin which bit of each pair carries it, and
 * TX_CLOCK_ON cannot be claimed as evidence independent of the enable beside
 * it. The hosttest pins the bits with synthetic words instead. Separating
 * them needs a boot where one is set without the other.
 */
/* Shared refusals for every *_diff() in this file. NEGATIVE, because 0 is
 * the meaningful answer "they agree": a comparison that could not be made
 * must not read as one that found nothing (rule 14). */
#define RB_DPCSPHY_DIFF_E_NULL      (-1)
#define RB_DPCSPHY_DIFF_E_SAME      (-2)   /* subject IS the control */

/* 309's ownership read. Three separate accessors rather than one verdict,
 * because the CLOCK_ON lesson applies here with force: a verdict folding
 * ALLOW, BLOCKED and DMCU's DIS_BLOCK together would report the same answer
 * whichever of them moved, and the whole point is WHICH. All return -1 on
 * NULL (rule 14). */
static inline int rb_dpcsphy_allow_driver_access(
        const struct rb_dpcsphy_inst *in)
{
    if (!in) { return -1; }
    return (in->w[RB_DPCSPHY_R_DPALT_CTRL] & RB_DPCSPHY_U_ALLOW_DRV_ACCESS)
           ? 1 : 0;
}

static inline int rb_dpcsphy_driver_access_blocked(
        const struct rb_dpcsphy_inst *in)
{
    if (!in) { return -1; }
    return (in->w[RB_DPCSPHY_R_DPALT_CTRL] & RB_DPCSPHY_U_DRV_ACCESS_BLOCKED)
           ? 1 : 0;
}

static inline int rb_dpcsphy_dmcu_dis_block(const struct rb_dpcsphy_inst *in)
{
    if (!in) { return -1; }
    return (in->w[RB_DPCSPHY_R_DIS_BLOCK] & RB_DPCSPHY_U_DMCU_DIS_BLOCK)
           ? 1 : 0;
}

static inline int rb_dpcsphy_force_tx_clk_dis(const struct rb_dpcsphy_inst *in)
{
    if (!in) { return -1; }
    return (in->w[RB_DPCSPHY_R_DIS_BLOCK] & RB_DPCSPHY_U_DMCU_FORCE_TXCLKDIS)
           ? 1 : 0;
}

/* DMCU's shadow of DPALT_DISABLE - the THIRD source on a question 301 left
 * as an OR over two registers. Returned on its own so it can disagree. */
static inline int rb_dpcsphy_dmcu_dpalt_disable(
        const struct rb_dpcsphy_inst *in)
{
    if (!in) { return -1; }
    return (in->w[RB_DPCSPHY_R_DMCU_CNTL6] & RB_DPCSPHY_U_DMCU_DPALT_DISABLE)
           ? 1 : 0;
}

/* A multi-bit field, so a VALUE (0..3), never a flag. */
static inline int rb_dpcsphy_enc_type_sel(const struct rb_dpcsphy_inst *in)
{
    if (!in) { return -1; }
    return (int)((in->w[RB_DPCSPHY_R_CNTL2] & RB_DPCSPHY_U_PHY_ENC_TYPE_SEL)
                 >> 2);
}

/* Which ownership fields differ from a CONTROL instance. Same shape and the
 * same reasons as rb_dpcsphy_sram_diff(): the panel's PHY is the only one
 * known to work, so a value on PHY2 means nothing without the live one's in
 * the same sample, and a self-comparison refuses rather than reporting
 * agreement.
 *
 * AND THE LESSON 308 PAID FOR, APPLIED IN ADVANCE: a difference here is
 * only about the Type-C port if it does NOT also hold for the instances
 * nobody brought up. 308's SRAM candidate died exactly there - it separated
 * live from dark and said nothing about PHY2. So the consumer must diff
 * PHY2 against the panel AND against PHY1/3/4, and this function is built
 * to be called both ways round. */
#define RB_DPCSPHY_ADIFF_ALLOW       0x1
#define RB_DPCSPHY_ADIFF_BLOCKED     0x2
#define RB_DPCSPHY_ADIFF_DIS_BLOCK   0x4
#define RB_DPCSPHY_ADIFF_FORCE_TXCLK 0x8
#define RB_DPCSPHY_ADIFF_DMCU_DPALT  0x10
#define RB_DPCSPHY_ADIFF_ENC_TYPE    0x20

static inline int rb_dpcsphy_access_diff(const struct rb_dpcsphy_inst *subject,
                                         const struct rb_dpcsphy_inst *control)
{
    int m = 0;
    if (!subject || !control) { return RB_DPCSPHY_DIFF_E_NULL; }
    if (subject == control)   { return RB_DPCSPHY_DIFF_E_SAME; }
    if (rb_dpcsphy_allow_driver_access(subject)
        != rb_dpcsphy_allow_driver_access(control)) {
        m |= RB_DPCSPHY_ADIFF_ALLOW;
    }
    if (rb_dpcsphy_driver_access_blocked(subject)
        != rb_dpcsphy_driver_access_blocked(control)) {
        m |= RB_DPCSPHY_ADIFF_BLOCKED;
    }
    if (rb_dpcsphy_dmcu_dis_block(subject)
        != rb_dpcsphy_dmcu_dis_block(control)) {
        m |= RB_DPCSPHY_ADIFF_DIS_BLOCK;
    }
    if (rb_dpcsphy_force_tx_clk_dis(subject)
        != rb_dpcsphy_force_tx_clk_dis(control)) {
        m |= RB_DPCSPHY_ADIFF_FORCE_TXCLK;
    }
    if (rb_dpcsphy_dmcu_dpalt_disable(subject)
        != rb_dpcsphy_dmcu_dpalt_disable(control)) {
        m |= RB_DPCSPHY_ADIFF_DMCU_DPALT;
    }
    if (rb_dpcsphy_enc_type_sel(subject) != rb_dpcsphy_enc_type_sel(control)) {
        m |= RB_DPCSPHY_ADIFF_ENC_TYPE;
    }
    return m;
}

/* The three PHY_CNTL0 fields 308 found differing between the live panel and
 * PHY2, each with its own accessor so a caller cannot read one and narrate
 * another. All return -1 on NULL rather than 0 (rule 14): a refusal and a
 * cleared bit must not look alike. */
static inline int rb_dpcsphy_sram_ext_ld_done(const struct rb_dpcsphy_inst *in)
{
    if (!in) { return -1; }
    return (in->w[RB_DPCSPHY_R_PHY_CNTL0] & RB_DPCSPHY_U_SRAM_EXT_LD_DONE)
           ? 1 : 0;
}

static inline int rb_dpcsphy_sram_bypass(const struct rb_dpcsphy_inst *in)
{
    if (!in) { return -1; }
    return (in->w[RB_DPCSPHY_R_PHY_CNTL0] & RB_DPCSPHY_U_SRAM_BYPASS) ? 1 : 0;
}

/* REF_RANGE is a multi-bit field, so it is returned as its VALUE, not a
 * flag. The panel reads 0 and PHY2 reads 3; which is correct for a USB-C
 * PHY's reference clock is not established here. */
static inline int rb_dpcsphy_ref_range(const struct rb_dpcsphy_inst *in)
{
    if (!in) { return -1; }
    return (int)((in->w[RB_DPCSPHY_R_PHY_CNTL0] & RB_DPCSPHY_D_PHY_REF_RANGE)
                 >> 9);
}

/* Which of the SRAM-handshake / range fields differ between a subject
 * instance and a CONTROL instance, as a bit mask. The control is the whole
 * point: on this chip the panel's PHY is the only one known to be working,
 * so "PHY2 has EXT_LD_DONE clear" means nothing without "and the live one
 * has it set" in the same sample (rule 39).
 *
 * Returns a negative refusal, never 0, when it cannot compare - because 0
 * is the meaningful answer "they agree". */
#define RB_DPCSPHY_DIFF_EXT_LD_DONE  0x1
#define RB_DPCSPHY_DIFF_SRAM_BYPASS  0x2
#define RB_DPCSPHY_DIFF_REF_RANGE    0x4
static inline int rb_dpcsphy_sram_diff(const struct rb_dpcsphy_inst *subject,
                                       const struct rb_dpcsphy_inst *control)
{
    int m = 0;
    if (!subject || !control) { return RB_DPCSPHY_DIFF_E_NULL; }
    if (subject == control)   { return RB_DPCSPHY_DIFF_E_SAME; }
    if (rb_dpcsphy_sram_ext_ld_done(subject)
        != rb_dpcsphy_sram_ext_ld_done(control)) {
        m |= RB_DPCSPHY_DIFF_EXT_LD_DONE;
    }
    if (rb_dpcsphy_sram_bypass(subject) != rb_dpcsphy_sram_bypass(control)) {
        m |= RB_DPCSPHY_DIFF_SRAM_BYPASS;
    }
    if (rb_dpcsphy_ref_range(subject) != rb_dpcsphy_ref_range(control)) {
        m |= RB_DPCSPHY_DIFF_REF_RANGE;
    }
    return m;
}

static inline int rb_dpcsphy_in_reset(const struct rb_dpcsphy_inst *in)
{
    if (!in) { return -1; }
    return (in->w[RB_DPCSPHY_R_PHY_CNTL0] & RB_DPCSPHY_D_PHY_RESET) ? 1 : 0;
}

static inline int rb_dpcsphy_tx_clock_on(const struct rb_dpcsphy_inst *in)
{
    if (!in) { return -1; }
    return (in->w[RB_DPCSPHY_R_CLOCK_CNTL] & RB_DPCSPHY_A_TX_CLOCK_ON) ? 1 : 0;
}

static inline int rb_dpcsphy_tx_fifo_en(const struct rb_dpcsphy_inst *in)
{
    if (!in) { return -1; }
    return (in->w[RB_DPCSPHY_R_CNTL] & RB_DPCSPHY_D_TX_FIFO_EN) ? 1 : 0;
}

/* THE REF_CLKDET RESULT IS ONLY MEANINGFUL IF THE DETECTOR IS ENABLED.
 * A 0 result with the detector off says nothing at all, and reading it as
 * "no clock" is the recurring error exactly: a value read without
 * establishing what makes it mean anything. Returns 1 when the result can
 * be believed. */
static inline int rb_dpcsphy_clkdet_valid(const struct rb_dpcsphy_inst *in)
{
    if (!in) { return 0; }
    return (in->w[RB_DPCSPHY_R_PHY_CNTL0] & RB_DPCSPHY_D_PHY_REF_CLKDET_EN)
               ? 1 : 0;
}

/* Does the sweep show five distinct register sets, or one address read five
 * times? Without this a byte-identical survey could mean either, and 2FC
 * paid for the version of this question that was not asked. Returns 1 when
 * at least two instances differ. */
static inline int rb_dpcsphy_sweep_differs(const struct rb_dpcsphy_inst *a,
                                           uint32_t n)
{
    uint32_t i, r;
    /* n < 2 needs no test of its own: the loop below starts at 1 and so does
     * nothing for n == 1. Writing it as a guard would be a term the break-it
     * sweep showed carries no information. */
    if (!a) { return 0; }
    for (i = 1; i < n; i++) {
        for (r = 0; r < RB_DPCSPHY_R_COUNT; r++) {
            if (a[i].w[r] != a[0].w[r]) { return 1; }
        }
    }
    return 0;
}

#endif /* RB_DPCSPHY_H */
