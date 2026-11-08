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

#define RB_DPCSPHY_DW_CNTL          0x2930u  /* RDPCSTX_CNTL              */
#define RB_DPCSPHY_DW_CLOCK_CNTL    0x2931u  /* RDPCSTX_CLOCK_CNTL        */
#define RB_DPCSPHY_DW_INTERRUPT     0x2932u  /* RDPCSTX_INTERRUPT_CONTROL */
#define RB_DPCSPHY_DW_SRAM_CNTL     0x2936u  /* RDPCS_TX_SRAM_CNTL        */
#define RB_DPCSPHY_DW_PHY_CNTL0     0x2940u  /* RDPCSTX_PHY_CNTL0         */

enum rb_dpcsphy_reg {
    RB_DPCSPHY_R_CNTL = 0,
    RB_DPCSPHY_R_CLOCK_CNTL,
    RB_DPCSPHY_R_INTERRUPT,
    RB_DPCSPHY_R_SRAM_CNTL,
    RB_DPCSPHY_R_PHY_CNTL0,
    RB_DPCSPHY_R_COUNT
};

/* ===================================================================
 * ANSWERED - the hardware reports these. THESE ARE THE READINGS.
 * =================================================================== */

/* RDPCSTX_PHY_CNTL0. REF_CLKDET_RESULT is the reference-clock detector's
 * own output: the most direct statement available of whether the PHY has a
 * clock at all. SRAM_INIT_DONE and EXT_LD_DONE say whether its firmware
 * image is in place. RTUNE_ACK is the PHY acknowledging a tune request. */
#define RB_DPCSPHY_A_REF_CLKDET_RESULT  0x02000000u
#define RB_DPCSPHY_A_SRAM_INIT_DONE     0x10000000u
#define RB_DPCSPHY_A_SRAM_EXT_LD_DONE   0x20000000u
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
    RB_DPCSPHY_V_DEAD      = 1,  /* no ref clock, no SRAM, no clocks on    */
    RB_DPCSPHY_V_CLOCKLESS = 2,  /* SRAM loaded but the detector says no clk */
    RB_DPCSPHY_V_NO_SRAM   = 3,  /* clock detected but firmware not loaded  */
    RB_DPCSPHY_V_ALIVE     = 4   /* ref clock detected AND SRAM init done   */
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
static inline int rb_dpcsphy_verdict(const struct rb_dpcsphy_inst *in)
{
    uint32_t p;
    int clk, sram;

    if (!in) { return RB_DPCSPHY_V_UNKNOWN; }
    p = in->w[RB_DPCSPHY_R_PHY_CNTL0];
    clk  = (p & RB_DPCSPHY_A_REF_CLKDET_RESULT) ? 1 : 0;
    sram = (p & RB_DPCSPHY_A_SRAM_INIT_DONE) ? 1 : 0;

    if (clk && sram)  { return RB_DPCSPHY_V_ALIVE; }
    if (sram)         { return RB_DPCSPHY_V_CLOCKLESS; }
    if (clk)          { return RB_DPCSPHY_V_NO_SRAM; }
    return RB_DPCSPHY_V_DEAD;
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
