/*
 * rb_dpcspipe.h - RDPCSPIPE_PHY_CNTL6, the OTHER DP-alt register. READ-ONLY.
 *
 * 300. For five boots (459, 461, 464, 468, 469) this project read
 * `RDPCSTX2_RDPCSTX_PHY_CNTL6` as `0x00063333` and called PHY2 dark, while
 * the DMUB's own `QUERY_DP_ALT` said DP alt mode was GRANTED on PHY2. The
 * contradiction was recorded as unresolved and `PHY_CNTL6` was flagged as
 * "a suspect, not a witness". The resolution was in `dmubbl dpaltq`'s own
 * printed output the whole time:
 *
 *     "RDPCSTX2_PHY_CNTL6 is the WRONG REGISTER for this part - Linux reads
 *      RDPCSPIPE_PHY_CNTL6 for UNIPHY_C on YELLOW_CARP_B0"
 *
 * This is the recurring error in its "read the evidence already in hand
 * before designing the next experiment" form, and it cost five boots of
 * treating a register as a witness after our own tool had said it was the
 * wrong one.
 *
 * ------------------------------------------------------------ what upstream says
 *
 * `docs/linux-ref/dpcs_4_2_0_offset.h:11960` is explicit, as a comment in the
 * generated header:
 *
 *     //[Note] Hack. RDPCSPIPE only has 2 instances.
 *     regRDPCSPIPE0_RDPCSPIPE_PHY_CNTL6  0x2d73   BASE_IDX 2
 *     regRDPCSPIPE1_RDPCSPIPE_PHY_CNTL6  0x2e4b   BASE_IDX 2
 *     regRDPCSPIPE2_RDPCSPIPE_PHY_CNTL6  0x2d73   <- instance 0 again
 *     regRDPCSPIPE3_RDPCSPIPE_PHY_CNTL6  0x2e4b   <- instance 1 again
 *     regRDPCSPIPE4_RDPCSPIPE_PHY_CNTL6  0x2d73   <- instance 0 again
 *
 * So there are **TWO** real registers, 0xd8 apart - the same stride as the
 * RDPCSTX block - and the five-entry table exists only so
 * `DPCS_DCN31_REG_LIST(id)` compiles for id 0..4.
 *
 * ------------------------------------------- WHAT THIS FILE REFUSES TO DECIDE
 *
 * **WHICH INSTANCE IS UNIPHY_C IS NOT ESTABLISHED HERE.** Upstream's table
 * maps index 2 onto instance 0, but that table is a COMPILE HACK by its own
 * comment, not a statement about the die, and this project has been bitten by
 * exactly this before: `RBFBAuxEn` and `RBFBHpdSink` are masks that were read
 * as indexes (2FB), and 2C1's PHY numbering could not be assumed to match
 * HPD2/AUX2 either. There are two registers and three PHYs that
 * `QUERY_DP_ALT` reports as USB-C (is_usb mask 0x1c = PHYs 2, 3, 4), so the
 * mapping cannot even be one-to-one.
 *
 * Therefore BOTH instances are read and BOTH are published raw, and the
 * identification is left to the comparison below. No accessor in this file
 * takes a UNIPHY index.
 *
 * ---------------------------------------------- what makes a reading meaningful
 *
 * `QUERY_DP_ALT` is an INDEPENDENT instrument and it is already attested:
 * boot 469 reproduced 2EB's result and its is_usb mask equalled 2E1's VBIOS
 * display-path read exactly, from a source the command does not derive from.
 * It reports, for PHY2, `alt_disable = 0`.
 *
 * So the question this register answers is a one-bit agreement test:
 *
 *   - an instance whose DPALT_DISABLE is **0** agrees with the DMUB, and that
 *     is the first corroboration that PHY2's DP-alt state is granted in a
 *     REGISTER - which is what would retire `RDPCSTX2`'s `0x00063333` as a
 *     misread rather than a measurement.
 *   - both instances **1** would mean this block does not agree with the DMUB
 *     either, and then the disagreement is not about which register but about
 *     which instrument, with the ROM match on the DMUB's side.
 *
 * -------------------------------------- AND WHAT IT CANNOT ANSWER, said plainly
 *
 * **THIS REGISTER HAS NO LANE-POWER FIELDS.** `dpcs_4_2_0_offset.h:11945-11957`
 * defines exactly three for it - DPALT_DP4, DPALT_DISABLE, DPALT_DISABLE_ACK -
 * where RDPCSTX's PHY_CNTL6 also carries `DP_REF_CLK_EN/REQ` and the four
 * per-lane `MPLL_EN`/`PSTATE` fields. So this read CANNOT say whether a lane
 * is powered, and a reading here must not be reported as one. "Is DP alt mode
 * granted" and "is the MPLL running" are two questions, and conflating them is
 * what made `0x00063333` look like a single verdict (2BD's shape: one field
 * read as the whole state).
 *
 * Read-only. No setter, and nothing in the tree may add one without first
 * establishing on hardware that the live word decodes as this register
 * (rule 47 asked of a write).
 */
#ifndef RB_DPCSPIPE_H
#define RB_DPCSPIPE_H

#include <stdint.h>

/* TWO, because upstream says two. The five-entry table is a compile hack. */
#define RB_DPCSPIPE_COUNT        2u
#define RB_DPCSPIPE_DW_INST0     0x2d73u
#define RB_DPCSPIPE_DW_INST1     0x2e4bu
#define RB_DPCSPIPE_DW_INVALID   0u

/* dpcs_4_2_0_offset.h:11945-11957. Identical shifts on both instances, and
 * identical to RDPCSTX's three DPALT fields - which is why the RDPCSTX word
 * could be misread as this one for five boots. */
#define RB_DPCSPIPE_DP4           0x00010000u  /* bit 16 */
#define RB_DPCSPIPE_DISABLE       0x00020000u  /* bit 17 */
#define RB_DPCSPIPE_DISABLE_ACK   0x00040000u  /* bit 18 */

/* The offsets are a TABLE, not base + i * stride. They are 0xd8 apart, which
 * is the RDPCSTX stride, and deriving one from the other would be deriving a
 * real address from a coincidence - the header's own instances 2..4 prove the
 * sequence does not continue (2FA found three different strides inside one
 * function for the same reason). */
static inline uint32_t rb_dpcspipe_dw(unsigned inst)
{
    switch (inst) {
    case 0u: return RB_DPCSPIPE_DW_INST0;
    case 1u: return RB_DPCSPIPE_DW_INST1;
    default: return RB_DPCSPIPE_DW_INVALID;
    }
}

static inline uint32_t rb_dpcspipe_dw_max(void)   /* 0x2e4b */
{
    return RB_DPCSPIPE_DW_INST1;
}

static inline int rb_dpcspipe_dp4(uint32_t w)
{
    return (w & RB_DPCSPIPE_DP4) ? 1 : 0;
}

static inline int rb_dpcspipe_disabled(uint32_t w)
{
    return (w & RB_DPCSPIPE_DISABLE) ? 1 : 0;
}

static inline int rb_dpcspipe_disable_acked(uint32_t w)
{
    return (w & RB_DPCSPIPE_DISABLE_ACK) ? 1 : 0;
}

/* Per-instance DP-alt verdict. Same three states rb_dpalt.h uses, and the
 * ambiguity is named for the same reason: DISABLE clear is "alt mode active"
 * OR "this is not a Type-C PHY at all", and no bit here separates them.
 * 0 is NOT a passing value. */
enum rb_dpcspipe_verdict {
    RB_DPCSPIPE_NOT_SAMPLED = 0,
    RB_DPCSPIPE_DISABLED    = 1,  /* DISABLE set and acked: definite        */
    RB_DPCSPIPE_SETTLING    = 2,  /* DISABLE set, not acked                 */
    RB_DPCSPIPE_NOT_DISABLED = 3  /* DISABLE clear: active alt, OR not USB-C */
};

static inline enum rb_dpcspipe_verdict rb_dpcspipe_verdict(uint32_t w)
{
    if (!rb_dpcspipe_disabled(w))      { return RB_DPCSPIPE_NOT_DISABLED; }
    if (!rb_dpcspipe_disable_acked(w)) { return RB_DPCSPIPE_SETTLING; }
    return RB_DPCSPIPE_DISABLED;
}

/* THE AGREEMENT TEST, which is the whole point of the read.
 *
 * `altDisableFromDmub` is QUERY_DP_ALT's `alt_disable` for the PHY in
 * question - an independent instrument whose answers ROM-matched on all five
 * paths in boots 449 and 469. This returns whether ANY instance of this
 * register agrees with it, because which instance is UNIPHY_C is not
 * established (see the header) and an agreement found on either one is the
 * finding; a disagreement on both is a different and larger finding.
 *
 * It is deliberately NOT a claim that the agreeing instance IS UNIPHY_C.
 * One matching bit across two candidates is not an identification (rule 18). */
static inline int rb_dpcspipe_agrees_with_dmub(const uint32_t *w,
                                               unsigned count,
                                               int altDisableFromDmub)
{
    unsigned i;
    const int want = altDisableFromDmub ? 1 : 0;

    if (!w || count == 0u || count > RB_DPCSPIPE_COUNT) { return 0; }
    for (i = 0; i < count; i++) {
        if (rb_dpcspipe_disabled(w[i]) == want) { return 1; }
    }
    return 0;
}

enum rb_dpcspipe_sweep {
    RB_DPCSPIPE_SWEEP_NOT_SAMPLED = 0,
    /* The two instances differ. The ONLY verdict here that is a control: it
     * proves these are two registers and not one address read twice, which
     * matters more than usual because upstream's own table aliases them. */
    RB_DPCSPIPE_SWEEP_DIFFERS     = 1,
    /* Identical. Says nothing on its own - two real registers in the same
     * state and one address read twice are indistinguishable. */
    RB_DPCSPIPE_SWEEP_UNIFORM     = 2,
    RB_DPCSPIPE_SWEEP_BAD_ARGS    = 3
};

static inline enum rb_dpcspipe_sweep rb_dpcspipe_sweep_verdict(
    const uint32_t *w, unsigned count)
{
    const uint32_t fields = RB_DPCSPIPE_DP4 | RB_DPCSPIPE_DISABLE |
                            RB_DPCSPIPE_DISABLE_ACK;
    unsigned i;
    uint32_t first;

    if (!w || count == 0u || count > RB_DPCSPIPE_COUNT) {
        return RB_DPCSPIPE_SWEEP_BAD_ARGS;
    }
    first = w[0] & fields;
    for (i = 1; i < count; i++) {
        if ((w[i] & fields) != first) { return RB_DPCSPIPE_SWEEP_DIFFERS; }
    }
    return RB_DPCSPIPE_SWEEP_UNIFORM;
}

#endif /* RB_DPCSPIPE_H */
