/* 310c: SEND DMUB_CMD__VBIOS_TRANSMITTER_SET_PHY_FSM (29) on PHY2.
 *
 * Opt-in `-rbgpufbfsm`, and NOTHING is written until `RBFBFsmGo` names a
 * state. The token was prefix-scanned against all 122 `-rbgpu*` tokens in
 * the tree (rule 30, which opt-in tokens are NOT covered for by
 * rb_defaults.h's assert): no exact duplicate, nothing it prefixes, nothing
 * that prefixes it. The scan's own instrument was validated in the same run
 * by checking it still finds the known `-rbgpufbotg` / `-rbgpufbotgany`
 * trap (rule 26).
 *
 * WHAT THIS ASKS, AND WHY IT IS A DIFFERENT QUESTION FROM EVERY EARLIER BOOT.
 * 307 established PHY2 is held in reset and that the atom transmitter ENABLE
 * (command 1) does not release it, in an in-boot A/B with a four-instance
 * control; 309b extended that A/B to nine registers with the same result.
 * The ceiling on all of it was that command 1 has NO reply field:
 * transmitter_control_v1_7 returns BP_RESULT_OK unconditionally, so `ran` is
 * a mailbox ack and Linux cannot tell whether the table executed either.
 *
 * Command 29's payload is documented "Data passed in/out" and carries a
 * uint32_t status, and its state enum names the exact three things three
 * milestones measured as missing: RESET_RELEASED, SRAM_LOAD_DONE, TX_EN.
 * So this boot asks something no earlier boot could: does the firmware
 * ANSWER, and does it act?
 *
 * THE VERDICT IS TWO INDEPENDENT THINGS AND THEY ARE NEVER ONE NUMBER.
 *   - the REPLY: did DMCUB write the status field (rb_phyfsm_decode)? A
 *     firmware that does not implement sub-type 29 leaves the sentinel, and
 *     that is a real answer - the first direct execution evidence this line
 *     has ever had.
 *   - the EFFECT: did PHY2's registers move? That is the nine-register
 *     survey, before and after, in the same call, with PHY0/1/3/4 as the
 *     four-instance control.
 * A reply with no effect and an effect with no reply are different findings
 * and either is worth the boot (rule 14, and 2E6's `applied` vs `changed`).
 *
 * THE LICENCE, stated in code and logged when it fails (the recurring
 * error's standing remedy). The before/after survey means nothing unless the
 * survey itself works THIS boot, so the send is declined unless this call's
 * own re-sample shows the live panel reading as a brought-up PHY:
 * PHY_RESET clear, TX_CLOCK_ON set, TX_FIFO_EN set. 309b's own data is why
 * that check is not vacuous - it is exactly the signature that separates the
 * panel from all four dark instances. And `phyReady` is checked separately,
 * so "the survey did not run" and "the survey ran and the panel looks dead"
 * have different codes.
 *
 * PHY0 IS REFUSED BY NAME, in rb_phyfsm_build() and again here. Dark PHY3 is
 * allowed, because 309 made it the control instance and a control you cannot
 * address is not a control (rule 39).
 *
 * WHAT IS NOT ESTABLISHED, and the log line says so every time: that DMCUB
 * 4.0.15.47 implements sub-type 29 at all, that the firmware acts on it for
 * a DP-alt port, and that RESET -> RESET_RELEASED is a legal DIRECT
 * transition rather than one step of a ladder. The status word is what would
 * answer the first two; the ladder question is why `RBFBFsmGo` takes the
 * state rather than defaulting to one.
 */
#include "RBAMDFramebuffer.hpp"
#include "rb_phyfsm.h"
#include "rb_dpcsphy.h"
#include <IOKit/IOLib.h>

#define LOG(fmt, ...) IOLog("RBAMDFramebuffer: " fmt "\n", ##__VA_ARGS__)

static const uint32_t kRbDmuSeg2Fsm = 0x34c0;

/* The PHY2 word the poll watches. PHY_CNTL0 carries PHY_RESET itself, so a
 * release is visible in the one register the question is about. */
static inline uint32_t fsmWatchWord(const struct rb_dpcsphy_inst *in)
{
    return in ? in->w[RB_DPCSPHY_R_PHY_CNTL0] : 0u;
}

uint32_t RBAMDFramebuffer::phyFsmExperiment(uint32_t phyid, uint32_t state)
{
    uint8_t cmd[RB_DMUB_CMD_SIZE];
    struct rb_phyfsm_req q;
    struct rb_phyfsm_reply rep;
    uint32_t i, r, rc;

    /* fsmCalls/Sends/Replies are cumulative and deliberately NOT reset here -
     * rule 31 reads them as monotonic. Everything below is per-call. */
    fsmStatus = 0u;
    fsmDecode = RB_PHYFSM_E_NO_REPLY;
    fsmChanged = 0u;
    fsmCtlChanged = 0u;
    fsmPanelLicence = 0u;
    fsmVerdict = RB_PHYFSM_V_NO_SEND;

    if (!fsmArmed) { return fsmRefuse = RB_PHYFSM_E_NOT_ARMED; }

    /* The survey is the measurement, so its absence is its own code rather
     * than folded into a generic refusal (rule 14). */
    phySample();
    if (!phyReady) { return fsmRefuse = RB_PHYFSM_E_NO_SURVEY; }

    /* The licence: the positive control must read as a brought-up PHY in
     * THIS call's sample. 309b showed these three bits separate the live
     * panel from all four dark instances, so the check carries information. */
    {
        const struct rb_dpcsphy_inst *p = &phyInst[RB_DPCSPHY_PANEL_INST];
        const int rst  = rb_dpcsphy_in_reset(p);
        const int clk  = rb_dpcsphy_tx_clock_on(p);
        const int fifo = rb_dpcsphy_tx_fifo_en(p);
        fsmPanelLicence = (uint32_t)((rst == 0 ? 1 : 0) |
                                     (clk  == 1 ? 2 : 0) |
                                     (fifo == 1 ? 4 : 0));
        if (fsmPanelLicence != 7u) {
            LOG("RBFBFsmGo declined: the panel does not read as a brought-up "
                "PHY this sample (reset %d clk %d fifo %d, licence %#x) - "
                "a before/after on PHY%u would mean nothing",
                rst, clk, fifo, fsmPanelLicence, phyid);
            return fsmRefuse = RB_PHYFSM_E_NO_LICENCE;
        }
    }

    if (phyid == RB_PHYFSM_PANEL_INST) { return fsmRefuse = RB_PHYFSM_E_PANEL; }

    /* Snapshot all five instances, all nine registers, BEFORE. */
    for (i = 0; i < RB_DPCSPHY_COUNT; i++) {
        for (r = 0; r < RB_DPCSPHY_R_COUNT; r++) {
            fsmBefore[i].w[r] = phyInst[i].w[r];
        }
    }

    q.phy_id       = phyid;
    q.mode         = 0u;        /* ATOM_ENCODER_MODE_DP - named, never
                                 * defaulted inside the builder, because 0 is
                                 * also what an unwritten byte looks like */
    q.lane_num     = RB_PHYFSM_TYPEC_LANES;
    q.symclk_100hz = rb_phyfsm_symclk_from_10khz(RB_PHYFSM_TYPEC_SYMCLK_10KHZ);
    q.state        = state;
    q.status_fill  = RB_PHYFSM_STATUS_SENTINEL;

    rc = (uint32_t)rb_phyfsm_build(cmd, &q);
    if (rc != RB_PHYFSM_OK) { return fsmRefuse = rc; }

    fsmCalls++;
    rc = dmubPush(cmd);
    if (rc != 0u) { return fsmRefuse = rc; }
    fsmSends++;

    /* Poll for PHY2's PHY_CNTL0 to move, the same bound and reasoning
     * dmubDigExperiment uses: 2000 x 10 us = 20 ms, two decimal orders above
     * a PLL lock, and this runs from setProperties and never the frame path
     * (rule 31). A command that executed slowly must not read as one that
     * did nothing - and a command that did nothing must still be reported,
     * which is what the loop running out is. */
    {
        const uint32_t watchDw = rb_dpcsphy_dw(phyid, RB_DPCSPHY_R_PHY_CNTL0);
        const uint32_t was = fsmWatchWord(&fsmBefore[phyid]);
        if (watchDw != 0u) {
            for (uint32_t spin = 0; spin < 2000u; spin++) {
                if (hubpRegs[kRbDmuSeg2Fsm + watchDw] != was) { break; }
                IODelay(10);
            }
        }
    }

    /* AFTER, the whole survey again, so the control instances are from the
     * same post-send sample as the subject (309b's structure). */
    phySample();
    for (i = 0; i < RB_DPCSPHY_COUNT; i++) {
        for (r = 0; r < RB_DPCSPHY_R_COUNT; r++) {
            fsmAfter[i].w[r] = phyInst[i].w[r];
        }
    }

    /* Two masks, never one number: which of the subject's nine registers
     * moved, and which of the CONTROL instances moved at all. A control that
     * moved says the whole sample is suspect; a subject that moved while the
     * controls held still is the finding. */
    for (r = 0; r < RB_DPCSPHY_R_COUNT; r++) {
        if (fsmAfter[phyid].w[r] != fsmBefore[phyid].w[r]) {
            fsmChanged |= (1u << r);
        }
    }
    for (i = 0; i < RB_DPCSPHY_COUNT; i++) {
        if (i == phyid) { continue; }
        for (r = 0; r < RB_DPCSPHY_R_COUNT; r++) {
            if (fsmAfter[i].w[r] != fsmBefore[i].w[r]) {
                fsmCtlChanged |= (1u << i);
            }
        }
    }

    /* The verdict, from rb_phyfsm_verdict(), which checks the CONTROLS
     * FIRST: a subject that moved while a control moved too is CONTAMINATED,
     * not a success (rule 17). The two masks above stay published beside it
     * so the verdict can be checked against its own inputs rather than
     * trusted - a verdict is a derived number and rule 48 says a statistic
     * must be derivable from what the consumer does with it. */
    {
        uint32_t flatB[RB_DPCSPHY_COUNT * RB_DPCSPHY_R_COUNT];
        uint32_t flatA[RB_DPCSPHY_COUNT * RB_DPCSPHY_R_COUNT];
        for (i = 0; i < RB_DPCSPHY_COUNT; i++) {
            for (r = 0; r < RB_DPCSPHY_R_COUNT; r++) {
                flatB[i * RB_DPCSPHY_R_COUNT + r] = fsmBefore[i].w[r];
                flatA[i * RB_DPCSPHY_R_COUNT + r] = fsmAfter[i].w[r];
            }
        }
        fsmVerdict = (uint32_t)rb_phyfsm_verdict(flatB, flatA, phyid);
    }

    /* The reply, decoded out of the slot dmubPush already read back. This is
     * independent of whether anything moved. */
    fsmDecode = (uint32_t)rb_phyfsm_decode(dmubReplyGot, &q, &rep);
    fsmStatus = rep.status;
    fsmStateEcho = rep.state;
    fsmPhyEcho = rep.phy_id;
    if (fsmDecode == RB_PHYFSM_OK) { fsmReplies++; }

    return fsmRefuse = RB_PHYFSM_OK;
}

void RBAMDFramebuffer::phyFsmPublish(void)
{
    static const char *const names[RB_DPCSPHY_R_COUNT] = {
        "Cntl", "ClockCntl", "Intr", "SramCntl", "PhyCntl0",
        "Cntl2", "DisBlock", "DmcuCntl6", "DpAltCtrl"
    };
    char key[64];
    uint32_t i, r;

    setProperty("RBFBFsmArmed", fsmArmed);
    setProperty("RBFBFsmCalls", fsmCalls, 64);
    setProperty("RBFBFsmSends", fsmSends, 64);
    setProperty("RBFBFsmReplies", fsmReplies, 64);
    setProperty("RBFBFsmRefuse", (uint64_t)fsmRefuse, 32);
    setProperty("RBFBFsmDecode", (uint64_t)fsmDecode, 32);
    setProperty("RBFBFsmStatus", (uint64_t)fsmStatus, 32);
    setProperty("RBFBFsmStateEcho", (uint64_t)fsmStateEcho, 32);
    setProperty("RBFBFsmPhyEcho", (uint64_t)fsmPhyEcho, 32);
    setProperty("RBFBFsmChanged", (uint64_t)fsmChanged, 32);
    setProperty("RBFBFsmCtlChanged", (uint64_t)fsmCtlChanged, 32);
    setProperty("RBFBFsmPanelLicence", (uint64_t)fsmPanelLicence, 32);
    setProperty("RBFBFsmVerdict", (uint64_t)fsmVerdict, 32);
    setProperty("RBFBFsmStateWant", (uint64_t)fsmStateWant, 32);
    setProperty("RBFBFsmPhyWant", (uint64_t)fsmPhyWant, 32);

    /* Per instance, per register, both sides. 309b's lesson: a diff is only
     * a finding against BOTH the panel and the dark USB-C control, so every
     * instance's words are published rather than only the subject's. */
    for (i = 0; i < RB_DPCSPHY_COUNT; i++) {
        for (r = 0; r < RB_DPCSPHY_R_COUNT; r++) {
            snprintf(key, sizeof(key), "RBFBFsm%uWas%s", (unsigned)i, names[r]);
            setProperty(key, (uint64_t)fsmBefore[i].w[r], 32);
            snprintf(key, sizeof(key), "RBFBFsm%uGot%s", (unsigned)i, names[r]);
            setProperty(key, (uint64_t)fsmAfter[i].w[r], 32);
        }
    }
}
