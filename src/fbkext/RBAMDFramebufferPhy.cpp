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
        "Cntl", "ClockCntl", "Intr", "SramCntl", "PhyCntl0"
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
    }
}
