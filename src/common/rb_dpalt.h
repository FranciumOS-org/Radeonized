/* rb_dpalt.h - the five RDPCSTX PHY blocks: DP alt mode, lane power and the
 * reference clock. READ-ONLY, and there is deliberately no setter.
 *
 * WHY THIS BLOCK, AND WHY NOT THE DMUB ROUTE. 2BE read Linux's
 * dcn31_link_encoder_is_in_alt_mode() and found two ways to ask whether a
 * PHY is in DP alt mode: DMUB_CMD__VBIOS_TRANSMITTER_QUERY_DP_ALT (26), and
 * this register. It recorded the register as the RISKY one, because Linux
 * marks it "Legacy path, avoid if possible" and prefers the DMUB query to
 * avoid "hanging the RDCPSPIPE if DMCUB wasn't already running".
 *
 * That ordering is wrong for THIS machine, and the evidence was already in
 * hand when 2BE wrote it down:
 *
 *  - The DMUB reply path is MEASURED NOT TO WORK here. Boot 275 (2zr) pushed
 *    QUERY_FEATURE_CAPS into the inbox, the bytes landed and persisted, RPTR
 *    stepped over the slot, and the firmware did NOT clear ret_status and
 *    wrote no payload - verdict UNCHANGED. QUERY_DP_ALT returns its answer by
 *    exactly that mechanism (DM_DMUB_WAIT_TYPE_WAIT_WITH_REPLY writes the
 *    reply back into the command slot), so on this firmware it would very
 *    likely return nothing, for the three reasons 2zr could not discriminate.
 *  - The stated precondition for the hang is that DMCUB is NOT running.
 *    DMCUB 4.0.15.47 is running here and 2zs measured a GPINT round trip
 *    against a positive control, so the condition is measured false rather
 *    than assumed false.
 *  - This is BASE_IDX 2, the same segment the OTG, HUBP, HPD and AUX reads
 *    already come from, so there is no new mapping - only a larger bound.
 *
 * None of that makes the read free: it is still a register access in a PHY
 * block this project has never touched, and the failure mode Linux warns
 * about is a hang, not a refused value. That is why it is opt-in, read-only,
 * and has no setter.
 *
 * Offsets from docs/linux-ref/dpcs_4_2_0_offset.h, fields from
 * dpcs_4_2_0_sh_mask.h. RULE 46, DISCHARGED IN THE RESOURCE FILE AGAIN:
 * dcn31_resource.c:86 includes "dpcs/dpcs_4_2_0_offset.h" - DCN 3.1.2 uses
 * the DPCS 4.2.0 register set, NOT a 3.1.x one. dpcs_3_1_4_offset.h exists
 * upstream, which is exactly what makes the wrong guess plausible, so the
 * version was read out of the include line rather than inferred from the
 * DCN version. Same shape as 2B8 finding DCN 3.1 uses the DCE110 AUX engine.
 */

#ifndef RB_DPALT_H
#define RB_DPALT_H

#include <stdint.h>

/* regRDPCSTX0..4_RDPCSTX_PHY_CNTL6 = 0x2946, 0x2a1e, 0x2af6, 0x2bce, 0x2ca6,
 * all BASE_IDX 2. The stride is uniform: each successive pair differs by
 * 0xd8, checked for all four gaps rather than derived from the first two
 * (rule 18 - two numbers agreeing is not an identification). */
#define RB_DPALT_COUNT          5u
#define RB_DPALT_DW_PHY_CNTL6   0x2946u
#define RB_DPALT_DW_STRIDE      0xd8u
#define RB_DPALT_DW_INVALID     0u

/* The DP alt mode fields, dpcs_4_2_0_sh_mask.h. */
#define RB_DPALT_DP4            0x00010000u  /* RDPCS_PHY_DPALT_DP4,         bit 16 */
#define RB_DPALT_DISABLE        0x00020000u  /* RDPCS_PHY_DPALT_DISABLE,     bit 17 */
#define RB_DPALT_DISABLE_ACK    0x00040000u  /* RDPCS_PHY_DPALT_DISABLE_ACK, bit 18 */

/* The reference clock, same register. */
#define RB_DPALT_REF_CLK_EN     0x00080000u  /* bit 19 */
#define RB_DPALT_REF_CLK_REQ    0x00100000u  /* bit 20 */

/* Four transmitters, each a 2-bit PSTATE and a 1-bit MPLL_EN, at a 4-bit
 * stride: TX0 PSTATE 0x3 / MPLL_EN 0x4, TX1 0x30 / 0x40, and so on. */
#define RB_DPALT_TX_COUNT       4u
#define RB_DPALT_TX_FIELD_SHIFT 4u
#define RB_DPALT_TX_PSTATE_MASK 0x3u
#define RB_DPALT_TX_MPLL_EN     0x4u

static inline uint32_t rb_dpalt_dw(unsigned i)
{
    if (i >= RB_DPALT_COUNT) { return RB_DPALT_DW_INVALID; }
    return RB_DPALT_DW_PHY_CNTL6 + (uint32_t)i * RB_DPALT_DW_STRIDE;
}

/* The highest dword this header can ask for: RDPCSTX4 at 0x2ca6. The caller
 * grows its aperture bound to cover this before any read. */
static inline uint32_t rb_dpalt_dw_max(void)
{
    return RB_DPALT_DW_PHY_CNTL6 + (RB_DPALT_COUNT - 1u) * RB_DPALT_DW_STRIDE;
}

static inline int rb_dpalt_dp4(uint32_t w)          { return (w & RB_DPALT_DP4) != 0u; }
static inline int rb_dpalt_disabled(uint32_t w)     { return (w & RB_DPALT_DISABLE) != 0u; }
static inline int rb_dpalt_disable_acked(uint32_t w){ return (w & RB_DPALT_DISABLE_ACK) != 0u; }
static inline int rb_dpalt_ref_clk_en(uint32_t w)   { return (w & RB_DPALT_REF_CLK_EN) != 0u; }
static inline int rb_dpalt_ref_clk_req(uint32_t w)  { return (w & RB_DPALT_REF_CLK_REQ) != 0u; }

static inline uint32_t rb_dpalt_tx_pstate(uint32_t w, unsigned tx)
{
    if (tx >= RB_DPALT_TX_COUNT) { return 0u; }
    return (w >> (tx * RB_DPALT_TX_FIELD_SHIFT)) & RB_DPALT_TX_PSTATE_MASK;
}

static inline int rb_dpalt_tx_mpll_en(uint32_t w, unsigned tx)
{
    if (tx >= RB_DPALT_TX_COUNT) { return 0; }
    return (w & (RB_DPALT_TX_MPLL_EN << (tx * RB_DPALT_TX_FIELD_SHIFT))) != 0u;
}

/* How many of the four transmitters have their MPLL enabled.
 *
 * This is the lane count from the CHIP's side, and it is worth having because
 * 2BB read "2 lanes" out of the sink's own DPCD at the other end of the
 * link. Two independent ends agreeing is corroboration; disagreeing says
 * which end is being believed. It is also a second reading of RB_DPALT_DP4,
 * which claims the same thing in one bit. */
static inline unsigned rb_dpalt_lanes_on(uint32_t w)
{
    unsigned n = 0, tx;

    for (tx = 0; tx < RB_DPALT_TX_COUNT; tx++) {
        if (rb_dpalt_tx_mpll_en(w, tx)) { n++; }
    }
    return n;
}

/* Is this PHY doing anything at all? The reference clock plus any enabled
 * MPLL. Used as the positive control below, not as a verdict of its own. */
static inline int rb_dpalt_phy_active(uint32_t w)
{
    if (rb_dpalt_ref_clk_en(w)) { return 1; }
    return rb_dpalt_lanes_on(w) != 0u;
}

/* WHAT ONE SAMPLE CAN AND CANNOT SAY ABOUT ONE PHY.
 *
 * RB_DPALT_DISABLE set is definite: alt mode is disabled on this PHY.
 * RB_DPALT_DISABLE clear is NOT "alt mode is active", because this register
 * has no is_usb bit - a PHY that is not wired to a Type-C connector at all
 * has nothing to disable, and its bit reads 0 exactly like an active alt-mode
 * port. That is the one thing the DMUB query has and this register has not,
 * and it must be said in the verdict rather than papered over, because
 * "DISABLE clear therefore this is the Type-C port" is precisely the
 * inference 2zw spent six milestones on in another form.
 *
 * So the honest enumeration has an explicitly ambiguous member, and the
 * identification comes from a DIFFERENCE across a cable event - 2B4's method,
 * which is what identified HPD2 in the first place. */
enum rb_dpalt_verdict {
    RB_DPALT_ALT_DISABLED  = 0,  /* DISABLE set: definite */
    RB_DPALT_ALT_AMBIGUOUS = 1,  /* DISABLE clear: active alt mode OR not a USB-C PHY */
    RB_DPALT_ALT_SETTLING  = 2   /* DISABLE set but not acked: mid-transition */
};

static inline enum rb_dpalt_verdict rb_dpalt_verdict(uint32_t w)
{
    if (!rb_dpalt_disabled(w)) { return RB_DPALT_ALT_AMBIGUOUS; }
    if (!rb_dpalt_disable_acked(w)) { return RB_DPALT_ALT_SETTLING; }
    return RB_DPALT_ALT_DISABLED;
}

/* THE SWEEP'S CONTROL, and it is inside the sample rather than alongside it.
 *
 * The eDP panel is scanning while this runs - our own HUBP0 writes have been
 * putting the desktop on it since boot ~230 - so SOME PHY on this die must
 * have its reference clock on or an MPLL enabled. If none of the five does,
 * the five words are not the five PHY blocks: a wrong base, a wrong stride,
 * or a power-gated aperture all produce that reading, and boot 273 already
 * read 8192 bytes of a correctly located DMUB ring as zeros, so "the address
 * is right and the contents are empty" is a MEASURED failure mode here and
 * not a hypothetical one (2zr).
 *
 * Deliberately NOT a refusal: all five words identical. With no cable
 * attached that is a plausible and even expected reading, and refusing it
 * would make the instrument blind in the regime it is meant to measure -
 * 2A9's defect, where turning the hold off turned the measurement off. It is
 * reported as a fact instead, and the caller publishes every raw word. */
enum rb_dpalt_sweep {
    /* 0 is NOT "OK". These words are zero before the first sample, and a
     * verdict field whose default value reads as a successful result is
     * exactly 2A3's defect seen from the other side - there, a property that
     * was never republished could not move; here, one that is never written
     * would claim a passing control. So the unwritten value has its own
     * meaning and the caller publishes it. */
    RB_DPALT_SWEEP_NOT_SAMPLED = 0,
    RB_DPALT_SWEEP_OK          = 1,
    RB_DPALT_SWEEP_NO_ACTIVE   = 2,  /* no PHY has a clock or a lane: above */
    RB_DPALT_SWEEP_BAD_ARGS    = 3
};

static inline enum rb_dpalt_sweep rb_dpalt_sweep_verdict(const uint32_t *w,
                                                         unsigned count)
{
    unsigned i;

    if (!w || count == 0u || count > RB_DPALT_COUNT) {
        return RB_DPALT_SWEEP_BAD_ARGS;
    }
    for (i = 0; i < count; i++) {
        if (rb_dpalt_phy_active(w[i])) { return RB_DPALT_SWEEP_OK; }
    }
    return RB_DPALT_SWEEP_NO_ACTIVE;
}

/* Which instances are active, as a bitmask. The panel's PHY is in here, and
 * by elimination that narrows which instance could be the Type-C one - the
 * same use 2B9 made of the panel's own AUX0 as the reference for AUX2. */
static inline uint32_t rb_dpalt_active_mask(const uint32_t *w, unsigned count)
{
    uint32_t m = 0;
    unsigned i;

    if (!w) { return 0u; }
    if (count > RB_DPALT_COUNT) { count = RB_DPALT_COUNT; }
    for (i = 0; i < count; i++) {
        if (rb_dpalt_phy_active(w[i])) { m |= (1u << i); }
    }
    return m;
}

/* Which instances report alt mode NOT disabled, as a bitmask. Ambiguous per
 * instance by itself (see rb_dpalt_verdict); it is the quantity whose CHANGE
 * across a cable event identifies the port. */
static inline uint32_t rb_dpalt_not_disabled_mask(const uint32_t *w,
                                                  unsigned count)
{
    uint32_t m = 0;
    unsigned i;

    if (!w) { return 0u; }
    if (count > RB_DPALT_COUNT) { count = RB_DPALT_COUNT; }
    for (i = 0; i < count; i++) {
        if (!rb_dpalt_disabled(w[i])) { m |= (1u << i); }
    }
    return m;
}

#endif /* RB_DPALT_H */
