/* 306: the RDPCSTX PHY status survey. READ-ONLY, no setter anywhere.
 *
 * Shares `-rbgpufbdpalt`'s arm and no new token: it reads the same block
 * `dpaltSample()` already reads, by the same aperture, and adding a token
 * that prefixes nothing would still be a token for no new risk. The arm's
 * documented hazard (Linux marks the RDPCSTX write path "avoid if possible")
 * is about WRITES; there are none here.
 */
#include "RBAMDFramebuffer.hpp"
#include "rb_dpcsphy.h"
#include <IOKit/IOLib.h>

static const uint32_t kRbDmuSeg2Phy = 0x34c0;

void RBAMDFramebuffer::phySample(void)
{
    uint32_t i, r;

    phyReady = false;
    phySweepDiffers = 0;
    for (i = 0; i < RB_DPCSPHY_COUNT; i++) {
        for (r = 0; r < RB_DPCSPHY_R_COUNT; r++) { phyInst[i].w[r] = 0; }
        phyVerdict[i] = RB_DPCSPHY_V_UNKNOWN;
        phyClkDetValid[i] = 0;
    }

    if (!dpaltArmed || !hubpReady || !hubpRegs) { return; }

    /* The aperture bound, asked of the HIGHEST dword this file can touch -
     * instance 4's PHY_CNTL0. A survey that ran off the end would read
     * whatever follows and the numbers would look like findings. The length
     * comes from the mapping itself, not from a constant: a hardcoded
     * aperture size is a constant that is only a constant on the machine it
     * was measured on (rule 41). */
    if (!hubpMap) { return; }
    {
        const uint32_t hi = kRbDmuSeg2Phy +
            rb_dpcsphy_dw(RB_DPCSPHY_COUNT - 1u, RB_DPCSPHY_R_PHY_CNTL0);
        if (((uint64_t)(hi + 1u) * 4ull) > (uint64_t)hubpMap->getLength()) {
            return;
        }
    }

    for (i = 0; i < RB_DPCSPHY_COUNT; i++) {
        for (r = 0; r < RB_DPCSPHY_R_COUNT; r++) {
            const uint32_t dw = rb_dpcsphy_dw(i, (enum rb_dpcsphy_reg)r);
            if (dw == 0u) { continue; }   /* refused, not offset 0 */
            phyInst[i].w[r] = hubpRegs[kRbDmuSeg2Phy + dw];
        }
        phyVerdict[i]     = rb_dpcsphy_verdict(&phyInst[i]);
        phyClkDetValid[i] = (uint32_t)rb_dpcsphy_clkdet_valid(&phyInst[i]);
    }

    phySweepDiffers =
        (uint32_t)rb_dpcsphy_sweep_differs(phyInst, RB_DPCSPHY_COUNT);
    phyReady = true;
}

void RBAMDFramebuffer::phyPublish(void)
{
    static const char *rn[RB_DPCSPHY_R_COUNT] = {
        "Cntl", "ClockCntl", "Intr", "SramCntl", "PhyCntl0",
        "Cntl2", "DisBlock", "DmcuCntl6", "DpAltCtrl"
    };
    char key[48];
    uint32_t i, r;

    phySample();
    setProperty("RBFBPhyReady",  phyReady);
    setProperty("RBFBPhyArmed",  (uint64_t)(dpaltArmed ? 1 : 0), 32);
    /* UNIFORM IS A POSITIVE DEFECT HERE, as in the stream encoder: PHY0
     * carries the panel's live link and the others do not, so five identical
     * words mean the 0xd8 stride did nothing. */
    setProperty("RBFBPhySweep",  (uint64_t)phySweepDiffers, 32);

    for (i = 0; i < RB_DPCSPHY_COUNT; i++) {
        for (r = 0; r < RB_DPCSPHY_R_COUNT; r++) {
            snprintf(key, sizeof(key), "RBFBPhy%u%s", i, rn[r]);
            setProperty(key, (uint64_t)phyInst[i].w[r], 64);
        }
        snprintf(key, sizeof(key), "RBFBPhy%uVerdict", i);
        setProperty(key, (uint64_t)phyVerdict[i], 32);
        /* The licence on the clock-detector reading, published BESIDE it: a
         * 0 result with the detector disabled says nothing, and reading it as
         * "no clock" is the recurring error in one bit. */
        snprintf(key, sizeof(key), "RBFBPhy%uClkDetValid", i);
        setProperty(key, (uint64_t)phyClkDetValid[i], 32);

        /* 308: the SRAM-load handshake, decoded per FIELD rather than left
         * in a hex word. These three are the only PHY_CNTL0 fields besides
         * PHY_RESET itself that differ between the live panel and PHY2, and
         * the hex words hid that for a whole milestone. Published as values,
         * not flags, for REF_RANGE - 3 and 1 must not read alike. */
        snprintf(key, sizeof(key), "RBFBPhy%uExtLdDone", i);
        setProperty(key, (uint64_t)(int64_t)
                    rb_dpcsphy_sram_ext_ld_done(&phyInst[i]), 32);
        snprintf(key, sizeof(key), "RBFBPhy%uSramBypass", i);
        setProperty(key, (uint64_t)(int64_t)
                    rb_dpcsphy_sram_bypass(&phyInst[i]), 32);
        snprintf(key, sizeof(key), "RBFBPhy%uRefRange", i);
        setProperty(key, (uint64_t)(int64_t)
                    rb_dpcsphy_ref_range(&phyInst[i]), 32);

        /* AND THE DIFF AGAINST THE ONE INSTANCE KNOWN TO WORK. A field's
         * value on PHY2 means nothing without the live panel's in the same
         * sample (rule 39), so the comparison is published rather than left
         * for a reader to do by eye off two hex words - which is exactly how
         * these two bits went unnoticed. PHY0 against itself refuses
         * (E_SAME = -2) rather than reporting agreement, so the panel's own
         * row cannot be mistaken for a control for itself. */
        snprintf(key, sizeof(key), "RBFBPhy%uSramDiff", i);
        setProperty(key, (uint64_t)(int64_t)
                    rb_dpcsphy_sram_diff(&phyInst[i],
                                         &phyInst[RB_DPCSPHY_PANEL_INST]), 32);

        /* 309: the ownership read. Each field on its own key - a single
         * "blocked?" verdict would answer the same whichever of them moved,
         * and WHICH is the whole question. */
        snprintf(key, sizeof(key), "RBFBPhy%uAllowDrvAccess", i);
        setProperty(key, (uint64_t)(int64_t)
                    rb_dpcsphy_allow_driver_access(&phyInst[i]), 32);
        snprintf(key, sizeof(key), "RBFBPhy%uDrvAccessBlocked", i);
        setProperty(key, (uint64_t)(int64_t)
                    rb_dpcsphy_driver_access_blocked(&phyInst[i]), 32);
        snprintf(key, sizeof(key), "RBFBPhy%uDmcuDisBlock", i);
        setProperty(key, (uint64_t)(int64_t)
                    rb_dpcsphy_dmcu_dis_block(&phyInst[i]), 32);
        snprintf(key, sizeof(key), "RBFBPhy%uForceTxClkDis", i);
        setProperty(key, (uint64_t)(int64_t)
                    rb_dpcsphy_force_tx_clk_dis(&phyInst[i]), 32);
        snprintf(key, sizeof(key), "RBFBPhy%uDmcuDpAltDisable", i);
        setProperty(key, (uint64_t)(int64_t)
                    rb_dpcsphy_dmcu_dpalt_disable(&phyInst[i]), 32);
        snprintf(key, sizeof(key), "RBFBPhy%uEncTypeSel", i);
        setProperty(key, (uint64_t)(int64_t)
                    rb_dpcsphy_enc_type_sel(&phyInst[i]), 32);

        /* TWO DIFFS, AND THE SECOND IS 308'S LESSON MADE STRUCTURAL.
         * Against the live panel, a difference may mean nothing more than
         * "this PHY was never brought up" - that is exactly how 308's SRAM
         * candidate looked like a finding for a day. Against PHY3, a USB-C
         * PHY with DP alt DISABLED that read byte-identical to PHY2 across
         * the whole earlier survey, a difference is PHY2's own. Publishing
         * only the first would leave the reader unable to tell the two
         * apart, so both go out and neither is called a verdict. */
        snprintf(key, sizeof(key), "RBFBPhy%uAccessDiffPanel", i);
        setProperty(key, (uint64_t)(int64_t)
                    rb_dpcsphy_access_diff(&phyInst[i],
                                           &phyInst[RB_DPCSPHY_PANEL_INST]),
                    32);
        snprintf(key, sizeof(key), "RBFBPhy%uAccessDiffDark", i);
        setProperty(key, (uint64_t)(int64_t)
                    rb_dpcsphy_access_diff(&phyInst[i],
                                        &phyInst[RB_DPCSPHY_DARK_USBC_INST]),
                    32);
        snprintf(key, sizeof(key), "RBFBPhy%uSramDiffDark", i);
        setProperty(key, (uint64_t)(int64_t)
                    rb_dpcsphy_sram_diff(&phyInst[i],
                                        &phyInst[RB_DPCSPHY_DARK_USBC_INST]),
                    32);
    }
}
