/* SPDX-License-Identifier: MIT */
/* 2F4: the DP transmitter's link block - putting a training pattern on the
 * wire, and asking the SINK whether it arrived.
 *
 * 2F3 made the sink half work (a DPCD write lands and the receiver stores it);
 * this is the other half. rb_dptx.h carries the register derivation, the two
 * pattern encodings and every refusal. What this file adds is the order of
 * operations and the one thing that makes the result mean anything:
 *
 * THE VERDICT IS THE SINK'S OWN LANE STATUS, read back over AUX2 from DPCD
 * 0x202 - a register this project does not write at all. The transmitter's own
 * registers can only say our stores landed, which is rule 20's defect if taken
 * as the answer. Same shape as DPDTO_ENABLE_STATUS in 2F0/2F2.
 *
 * AND THE SINK'S STATUS IS READ BEFORE AS WELL AS AFTER, because "CR not done"
 * is the state it is already in - so only a CHANGE in that byte is evidence
 * the pattern reached it at all.
 */
#include "RBAMDFramebuffer.hpp"
#include "rb_dptx.h"
#include "rb_dpcd.h"
#include "rb_enc.h"
#include <IOKit/IOLib.h>

static const uint32_t kRbDmuSeg2DpTx = 0x34c0;

/* Read-only, same mapping and same arming shape as hpdSample(). All five
 * instances, so the panel's is in the SAME sample as the target's - a
 * reference measured in a different window is not a reference (2F1's lesson
 * applied before the fact). */
void RBAMDFramebuffer::dptxSample(void)
{
    if (!dptxArmed || !hubpRegs) { dptxReady = false; return; }

    for (unsigned i = 0; i < RB_DPTX_COUNT; i++) {
        for (unsigned r = 0; r < RB_DPTX_REG_COUNT; r++) {
            const uint32_t dw = rb_dptx_dw(i, (enum rb_dptx_reg)r);
            dptxWord[i][r] = dw ? hubpRegs[kRbDmuSeg2DpTx + dw] : 0u;
        }
    }
    dptxReady = true;
}

void RBAMDFramebuffer::dptxPublish(void)
{
    for (unsigned i = 0; i < RB_DPTX_COUNT; i++) {
        char key[28];
        snprintf(key, sizeof(key), "RBFBDpTx%uLink", i);
        setProperty(key, (uint64_t)dptxWord[i][RB_DPTX_REG_LINK_CNTL], 64);
        snprintf(key, sizeof(key), "RBFBDpTx%uVid", i);
        setProperty(key, (uint64_t)dptxWord[i][RB_DPTX_REG_VID_STREAM], 64);
        snprintf(key, sizeof(key), "RBFBDpTx%uDphy", i);
        setProperty(key, (uint64_t)dptxWord[i][RB_DPTX_REG_DPHY_CNTL], 64);
        snprintf(key, sizeof(key), "RBFBDpTx%uTpSel", i);
        setProperty(key, (uint64_t)dptxWord[i][RB_DPTX_REG_TP_SEL], 64);
        snprintf(key, sizeof(key), "RBFBDpTx%uPrbs", i);
        setProperty(key, (uint64_t)dptxWord[i][RB_DPTX_REG_PRBS_CNTL], 64);
    }
    setProperty("RBFBDpTxArmed", dptxArmed);
    setProperty("RBFBDpTxReady", dptxReady);
    /* The reference verdict, published as its own fact: without it the block
     * is unidentified and the write is refused (rule 47 asked of a write). */
    setProperty("RBFBDpTxRefTrained",
                (uint64_t)(dptxReady &&
                           rb_dptx_ref_trained(
                               dptxWord[RB_DPTX_PANEL_INST]
                                       [RB_DPTX_REG_LINK_CNTL])
                           ? 1u : 0u), 32);

    setProperty("RBFBDpTxCalls", dptxCalls, 64);
    setProperty("RBFBDpTxRan", dptxRan, 64);
    setProperty("RBFBDpTxRefuse", (uint64_t)dptxRefuse, 32);
    setProperty("RBFBDpTxInst", (uint64_t)dptxInst, 32);
    setProperty("RBFBDpTxTps", (uint64_t)dptxTps, 32);
    setProperty("RBFBDpTxLanes", (uint64_t)dptxLanes, 32);
    /* Our own four registers, before and after, with applied and clean
     * separated - 2EC's lesson was a disturbed neighbour, not a lost store. */
    setProperty("RBFBDpTxTpBefore", (uint64_t)dptxTpBefore, 64);
    setProperty("RBFBDpTxTpAfter", (uint64_t)dptxTpAfter, 64);
    setProperty("RBFBDpTxLcBefore", (uint64_t)dptxLcBefore, 64);
    setProperty("RBFBDpTxLcAfter", (uint64_t)dptxLcAfter, 64);
    setProperty("RBFBDpTxDcBefore", (uint64_t)dptxDcBefore, 64);
    setProperty("RBFBDpTxDcAfter", (uint64_t)dptxDcAfter, 64);
    setProperty("RBFBDpTxPcBefore", (uint64_t)dptxPcBefore, 64);
    setProperty("RBFBDpTxPcAfter", (uint64_t)dptxPcAfter, 64);
    setProperty("RBFBDpTxApplied", (uint64_t)dptxApplied, 32);
    setProperty("RBFBDpTxClean", (uint64_t)dptxClean, 32);
    /* THE SINK'S OWN BYTES - the part of this we do not write. */
    setProperty("RBFBDpTxSinkBefore", (uint64_t)dptxSinkBefore, 32);
    setProperty("RBFBDpTxSinkAfter", (uint64_t)dptxSinkAfter, 32);
    setProperty("RBFBDpTxSinkBeforeOk", (uint64_t)dptxSinkBeforeOk, 32);
    setProperty("RBFBDpTxSinkAfterOk", (uint64_t)dptxSinkAfterOk, 32);
    setProperty("RBFBDpTxDpcdRefuse", (uint64_t)dptxDpcdRefuse, 32);
    setProperty("RBFBDpTxVerdict", (uint64_t)dptxVerdict, 32);
}

/* ONE training pattern onto one transmitter, then ask the sink.
 *
 * The order is dcn10_link_encoder_set_dp_phy_pattern_training_pattern()'s,
 * unrearranged, and the first write is a PLAIN write where the other three are
 * read-modify-writes - Linux's own distinction, reproduced rather than tidied.
 */
uint32_t RBAMDFramebuffer::dptxTrain(uint32_t inst, uint32_t tps,
                                     uint32_t hpd, uint32_t lanes)
{
    dptxCalls++;
    dptxInst = inst;
    dptxTps = tps;
    dptxLanes = lanes;

    const int haveRegs = (dptxArmed && hubpRegs != nullptr) ? 1 : 0;
    if (haveRegs) { dptxSample(); }   /* target and reference, one sample */

    const uint32_t safe = (inst < RB_DPTX_COUNT) ? inst : 0u;
    const uint32_t panelLc = haveRegs
        ? dptxWord[RB_DPTX_PANEL_INST][RB_DPTX_REG_LINK_CNTL] : 0u;
    const uint32_t liveVid = haveRegs
        ? dptxWord[safe][RB_DPTX_REG_VID_STREAM] : 0u;

    const uint32_t why = rb_dptx_write_refuse(inst, tps, panelLc, liveVid,
                                              dptxArmed ? 1 : 0, haveRegs);
    if (why != RB_DPTX_W_OK) { return dptxRefuse = why; }

    /* The sink's lane status BEFORE. "CR not done" is the state it is already
     * in, so without this the after-read says nothing (rule 20's shape for a
     * status byte). A failed read is recorded, not fatal. */
    dptxSinkBeforeOk = auxDpcdRead(hpd, RB_DPCD_LANE0_1_STATUS, 1) ? 1u : 0u;
    dptxSinkBefore = dptxSinkBeforeOk ? auxDpcdData[0] : 0xffffffffu;
    dptxDpcdRefuse = auxDpcdRefuse;

    const uint32_t tpDw = kRbDmuSeg2DpTx + rb_dptx_dw(inst,
                                                  RB_DPTX_REG_TP_SEL);
    const uint32_t lcDw = kRbDmuSeg2DpTx + rb_dptx_dw(inst,
                                                  RB_DPTX_REG_LINK_CNTL);
    const uint32_t dcDw = kRbDmuSeg2DpTx + rb_dptx_dw(inst,
                                                  RB_DPTX_REG_DPHY_CNTL);
    const uint32_t pcDw = kRbDmuSeg2DpTx + rb_dptx_dw(inst,
                                                  RB_DPTX_REG_PRBS_CNTL);

    dptxTpBefore = dptxWord[safe][RB_DPTX_REG_TP_SEL];
    dptxLcBefore = dptxWord[safe][RB_DPTX_REG_LINK_CNTL];
    dptxDcBefore = dptxWord[safe][RB_DPTX_REG_DPHY_CNTL];
    dptxPcBefore = dptxWord[safe][RB_DPTX_REG_PRBS_CNTL];

    /* 1. the pattern - a WHOLE-register write, which is REG_WRITE's meaning.
     *    rb_dptx_hw_pattern converts TPSn to the register's 0-based encoding;
     *    DPCD 0x102 takes a DIFFERENT number for the same pattern and the two
     *    must never be swapped. */
    blRegs[tpDw] = rb_dptx_hw_pattern(tps);
    /* 2. training complete -> 0, keeping LINK_STATUS, which is the hardware's
     *    bit and the only one here worth reading as a verdict. */
    blRegs[lcDw] = rb_dptx_link_cntl_word(dptxLcBefore);
    /* 3. PHY bypass off, so the pattern actually reaches the lanes. */
    blRegs[dcDw] = rb_dptx_dphy_cntl_word(dptxDcBefore);
    /* 4. PRBS off, keeping PRBS_SEL. */
    blRegs[pcDw] = rb_dptx_prbs_cntl_word(dptxPcBefore);

    dptxTpAfter = hubpRegs[tpDw];
    dptxLcAfter = hubpRegs[lcDw];
    dptxDcAfter = hubpRegs[dcDw];
    dptxPcAfter = hubpRegs[pcDw];

    dptxApplied = (rb_dptx_pattern(dptxTpAfter) == rb_dptx_hw_pattern(tps) &&
                   !rb_dptx_trained(dptxLcAfter) &&
                   !rb_dptx_bypass(dptxDcAfter) &&
                   !rb_dptx_prbs_on(dptxPcAfter)) ? 1u : 0u;
    dptxClean = (rb_dptx_rmw_clean(dptxLcBefore, dptxLcAfter,
                                   RB_DPTX_TRAINING_COMPLETE) &&
                 rb_dptx_rmw_clean(dptxDcBefore, dptxDcAfter,
                                   RB_DPTX_DPHY_BYPASS) &&
                 rb_dptx_rmw_clean(dptxPcBefore, dptxPcAfter,
                                   RB_DPTX_PRBS_EN)) ? 1u : 0u;

    /* The sink is given the DPCD side of the same pattern, then a moment, then
     * read. Linux's own wait is TRAINING_AUX_RD_INTERVAL; 100 us is the
     * standard minimum for TPS1 and this is one shot, not a CR loop. */
    const uint32_t dpcdPat = rb_dptx_dpcd_pattern(tps);
    dptxDpcdSet = auxDpcdWrite(hpd, RB_DPCD_TRAINING_PATTERN_SET, dpcdPat);
    IODelay(100);

    dptxSinkAfterOk = auxDpcdRead(hpd, RB_DPCD_LANE0_1_STATUS, 1) ? 1u : 0u;
    dptxSinkAfter = dptxSinkAfterOk ? auxDpcdData[0] : 0xffffffffu;

    dptxVerdict = rb_dptx_verdict((int)dptxApplied, (int)dptxClean,
                                  dptxSinkAfterOk && dptxSinkBeforeOk,
                                  dptxSinkBefore, dptxSinkAfter, lanes);
    dptxRan++;
    return dptxRefuse = RB_DPTX_W_OK;
}
