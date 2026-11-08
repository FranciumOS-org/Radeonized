/* SPDX-License-Identifier: MIT */
/* Timer-backed VBL callbacks, shared with the host regression executable. */
#ifdef RBGPU_FB_VBL_HOSTTEST
#include "fb_vbl_shim.hpp"
#else
#include "RBAMDFramebuffer.hpp"
#include <IOKit/IOLib.h>
#include <kern/clock.h>
#include <mach/mach_time.h>
#define LOG(fmt, ...) IOLog("RBAMDFramebuffer: " fmt "\n", ##__VA_ARGS__)
#endif

/* 2B3: needed on BOTH sides of the guard - the hosttest compiles this file
 * too, which is the 2B1 lesson about the shim. */
#include "rb_hpd.h"
#include "rb_aux.h"
#include "rb_dpalt.h"
#include "rb_dio.h"
#include "rb_dpcspipe.h"
#include "rb_dpcd.h"
#include "rb_dpdto.h"

/* -- the vblank ---------------------------------------------------------- */

/* Why a thread_call rather than a real interrupt: the display engine's own
 * vblank IRQ is wired to the AMD driver's interrupt controller, which we do not
 * own, and 2s4 measured that what the display pipe actually needs is a 'vbl '
 * callback at the panel's rate - not a particular source for it. The comparable
 * working driver drives its VBL handlers from a thread_call for the same reason
 * (2sj). The rate is the measured refresh, so the pacing is right even though
 * the phase is not locked to scanout. */

bool RBAMDFramebuffer::anyVBLEnabled(void)
{
    for (unsigned i = 0; i < kMaxInterrupts; i++) {
        if (interrupts[i].used && interrupts[i].enabled &&
            interrupts[i].type == kIOFBVBLInterruptType) {
            return true;
        }
    }
    return false;
}

uint64_t RBAMDFramebuffer::foreignFires(void) const
{
    uint64_t n = 0;
    for (unsigned i = 0; i < kMaxInterrupts; i++) {
        if (interrupts[i].used && interrupts[i].target != this &&
            interrupts[i].type == kIOFBVBLInterruptType) {
            n += interrupts[i].fires;
        }
    }
    return n;
}

/* 2zk: publish from a frame-path call site, at most once per pubMs.
 *
 * The throttle is a deadline comparison and two counter bumps on the fast
 * path - no clock conversion, because pubIntervalAbs is converted once at
 * setup - against ~110 setProperty calls on the slow one. pubMs == 0 keeps
 * the old unconditional behaviour so the A side of the comparison is one
 * boot-arg away. */
void RBAMDFramebuffer::publishVBLStateHot(void)
{
    pubHotCalls++;
    if (pubMs != 0) {
        const uint64_t now = mach_absolute_time();
        if (now < pubNextAbs) { return; }
        pubNextAbs = now + pubIntervalAbs;
    }
    pubHotDone++;
    publishVBLState();
}

void RBAMDFramebuffer::publishVBLState(void)
{
    static const char *const usedKeys[] = {
        "RBFBVBL0Used", "RBFBVBL1Used", "RBFBVBL2Used", "RBFBVBL3Used"};
    static const char *const enabledKeys[] = {
        "RBFBVBL0Enabled", "RBFBVBL1Enabled", "RBFBVBL2Enabled", "RBFBVBL3Enabled"};
    static const char *const selfKeys[] = {
        "RBFBVBL0Self", "RBFBVBL1Self", "RBFBVBL2Self", "RBFBVBL3Self"};
    static const char *const fireKeys[] = {
        "RBFBVBL0Fires", "RBFBVBL1Fires", "RBFBVBL2Fires", "RBFBVBL3Fires"};
    unsigned used = 0, enabled = 0;
    for (unsigned i = 0; i < kMaxInterrupts; i++) {
        const Interrupt &irq = interrupts[i];
        used += irq.used ? 1 : 0;
        enabled += (irq.used && irq.enabled) ? 1 : 0;
        setProperty(usedKeys[i], irq.used);
        setProperty(enabledKeys[i], irq.used && irq.enabled);
        setProperty(selfKeys[i], irq.used && irq.target == this);
        setProperty(fireKeys[i], irq.fires, 64);
    }
    setProperty("RBFBVBLRegistered", (UInt64)used, 32);
    setProperty("RBFBVBLEnabledCount", (UInt64)enabled, 32);
    setProperty("RBFBVBLEnabled", enabled != 0);
    setProperty("RBFBVBLEnables", (UInt64)vblEnables, 32);
    setProperty("RBFBVBLFires", vblFires, 64);
    setProperty("RBFBVBLRunning", vblRunning);
    /* 2xe: the fire count's own clock, published in the SAME pass so a reader
     * can divide without assuming a rate. Every sets/s figure in this project
     * came from `fires / 143.83`, which was wrong two ways at once: the
     * cadence was really ~134.7 Hz (fixed above), and this publish only runs
     * every few hundred fires, so the registry value is a staircase. Taking
     * elapsed time from a stamp written beside the counter removes both
     * errors - the quantisation now cancels, because both numbers are from
     * the same instant. */
    uint64_t uptime = 0;
    clock_get_uptime(&uptime);
    uint64_t uptimeNs = 0;
    absolutetime_to_nanoseconds(uptime, &uptimeNs);
    setProperty("RBFBVBLUptimeNs", uptimeNs, 64);
    setProperty("RBFBVBLResyncs", (UInt64)vblResyncs, 32);

    /* 2wc: the 'amtr' counters. They live here and not in the attribute
     * handlers because those run on the accelerator's thread with its locks
     * held and must not allocate; this is the thread_call. Rule 14 - the
     * counts are published whether or not they moved, so zero is a reading
     * and not an absence, and the refusal path has its own key (rule 19). */
    setProperty("RBFBAmtrArmed", amtrArmed);
    setProperty("RBFBAmtrGets", amtrGets, 64);
    setProperty("RBFBAmtrSets", amtrSets, 64);
    setProperty("RBFBAmtrPeak", amtrPeak, 64);
    setProperty("RBFBSelfDisarmed", selfDisarmed);
    setProperty("RBFBAmtrBucket", amtrBucket, 64);
    setProperty("RBFBAmtrNoFlip", amtrNoFlip, 64);
    setProperty("RBFBAmtrSnapped", amtrSnapped);
    setProperty("RBFBAmtrPtr", amtrPtr, 64);
    setProperty("RBFBAmtrRefused", amtrRefused, 64);
    /* 2wh: the HUBP write. RBFBHubpArmed is published UNCONDITIONALLY, so it
     * is the rule-15 identity key for this build whatever the boot-args say.
     * Refuse, Orig and the pipe state are published even when we never wrote,
     * because that is exactly the boot where they say why (rule 14). */
    setProperty("RBFBRetireArmed", retireArmed);
    setProperty("RBFBRetireEnables", retireEnables, 64);
    setProperty("RBFBForeignFires", foreignFires(), 64);
    /* Rule 15: RBFBPollArmed is published UNCONDITIONALLY, so it is this
     * build's identity key whatever the boot-args say. Rule 14: the counts
     * are published whether or not they moved. */
    /* 2zk: published unconditionally, so pubMs is this build's identity key
     * and the two counters say what the throttle actually elided (rule 14). */
    setProperty("RBFBPubMs", (uint64_t)pubMs, 32);
    setProperty("RBFBPubHotCalls", pubHotCalls, 64);
    setProperty("RBFBPubHotDone", pubHotDone, 64);
    setProperty("RBFBPubSetsOk", pubSetsOk, 64);
    setProperty("RBFBPubSetsRefused", pubSetsRefused, 64);
    setProperty("RBFBPubSetsOther", pubSetsOther, 64);
    setProperty("RBFBPubMsLast", (uint64_t)(int64_t)pubMsLast, 64);
    setProperty("RBFBPollArmed", pollArmed);
    setProperty("RBFBPollReads", pollReads, 64);
    setProperty("RBFBPollChanges", pollChanges, 64);
    setProperty("RBFBPollFirstSeen", pollFirstSeen, 64);
    setProperty("RBFBRetireKicks", retireKicks, 64);
    setProperty("RBFBRetireDelivered", retireDelivered, 64);
    setProperty("RBFBHubpArmed", hubpArmed);
    setProperty("RBFBHubpReady", hubpReady);
    setProperty("RBFBHubpRefuse", (uint64_t)hubpRefuse, 32);
    setProperty("RBFBHubpOtgCtl", (uint64_t)hubpOtgCtl, 32);
    setProperty("RBFBHubpPitch", (uint64_t)hubpPitch, 32);
    setProperty("RBFBHubpConfig", (uint64_t)hubpConfig, 32);
    setProperty("RBFBHubpAddrCfg", (uint64_t)hubpAddrCfg, 32);
    setProperty("RBFBHubpFlipCtl", (uint64_t)hubpFlipCtl, 32);
    setProperty("RBFBHubpFlipCtl2", (uint64_t)hubpFlipCtl2, 32);
    /* 2B1: the live state of the second pending slot, from the REGISTRY's
     * point of view - published periodically, because 2A3's lesson is that a
     * field a runtime dial can change must be published where it is read. */
    setProperty("RBFBHubpTriple", hubpTripleArmed);
    setProperty("RBFBHubpTripleWrites", hubpTripleWrites, 64);
    setProperty("RBFBHubpTripleReadback", (uint64_t)hubpTripleReadback, 32);
    setProperty("RBFBHubpTripleOrig", (uint64_t)hubpTripleOrig, 32);
    setProperty("RBFBHubpTripleRefused", hubpTripleRefused, 64);
    setProperty("RBFBHubpTripleRefusedVal",
                (uint64_t)hubpTripleRefusedVal, 32);
    setProperty("RBFBHubpPendChecks", hubpPendChecks, 64);
    setProperty("RBFBHubpPendHits", hubpPendHits, 64);
    /* 2za: the latch-latency histogram. Published as a string because the
     * shape IS the result - "how many vblanks did each flip take to latch" -
     * and six separate keys would be read one at a time and compared by hand.
     * Bin 1 is a flip that made its own frame; bin 2 is half rate. */
    {
        char lb[160];
        snprintf(lb, sizeof(lb), "1:%llu 2:%llu 3:%llu 4:%llu 5+:%llu",
                 (unsigned long long)hubpLatch.hist[1],
                 (unsigned long long)hubpLatch.hist[2],
                 (unsigned long long)hubpLatch.hist[3],
                 (unsigned long long)hubpLatch.hist[4],
                 (unsigned long long)hubpLatch.hist[5]);
        setProperty("RBFBHubpLatchHist", lb);
    }
    setProperty("RBFBHubpLatched", hubpLatch.latched, 64);
    setProperty("RBFBHubpLatchPreempted", hubpLatch.preempted, 64);
    setProperty("RBFBHubpLatchSamples", hubpLatch.samples, 64);
    setProperty("RBFBHubpLatchMax", (UInt64)hubpLatch.maxVbls, 32);
    /* 2zd: "the vblank is driving" must be a measurement, not a
     * configuration - Drove against Calls is what says so. */
    setProperty("RBFBVblRetArmed", vblRetArmed, 32);
    setProperty("RBFBVblRetCalls", vblRetCalls, 64);
    setProperty("RBFBVblRetDrove", vblRetDrove, 64);
    setProperty("RBFBVblRetRefused", vblRetRefused, 64);
    setProperty("RBFBVblRetLookups", vblRetLookups, 32);
    setProperty("RBFBVblRetModeSkips", vblRetModeSkips, 64);
    setProperty("RBFBHubpDeferArmed", hubpDeferPend);
    setProperty("RBFBHubpDeferrals", hubpDeferrals, 64);
    /* 2B3: resample the HPD blocks on every publish, so a cable plugged in
     * after boot is visible without a reboot. Five register reads at the
     * publish interval, which 2zz measured as a path costing 1.4 % of the
     * frame rate at ~110 setProperty calls - ten MMIO reads is not a
     * meaningful addition to that, and the dial that throttles it
     * (RBFBPubMs) throttles this with it. */
    hpdSample();
    setProperty("RBFBHpdSink", (uint64_t)hpdSinkMask, 32);
    setProperty("RBFBHpdSinkEver", (uint64_t)hpdSinkEver, 32);
    setProperty("RBFBHpdEn", (uint64_t)hpdEnMask, 32);
    setProperty("RBFBHpdSetup", (uint64_t)hpdSetupMask, 32);
    setProperty("RBFBHpdSetupEn", (uint64_t)hpdSetupEnMask, 32);
    setProperty("RBFBHpdChanges", hpdChanges, 64);
    setProperty("RBFBHpdReady", hpdReady);
    /* The raw words too: a mask is a decoded summary and 2B0 is what it costs
     * to publish only the summary (the verdict was wrong and the raw
     * histogram beside it is what refuted it in the same output). */
    for (unsigned i = 0; i < RB_HPD_COUNT; i++) {
        char key[20];
        /* 2B4: 64 bits, not 32. ioreg prints a 32-bit OSNumber whose bit 31
         * is set as a sign-extended 64-bit decimal, which is how HPD2's
         * 0xff0ff013 came back as 18446744073693818899. Publishing the width
         * the value is read at fixes it at the source, for every reader,
         * instead of only in the one tool that was patched. */
        snprintf(key, sizeof(key), "RBFBHpd%uStatus", i);
        setProperty(key, (uint64_t)hpdStatus[i], 64);
        snprintf(key, sizeof(key), "RBFBHpd%uCtl", i);
        setProperty(key, (uint64_t)hpdControl[i], 64);
    }
    /* 2C0: the RDPCSTX PHY blocks. dpaltSample() returns immediately unless
     * the boot armed -rbgpufbdpalt AND the aperture covered them, so an
     * unarmed boot pays nothing and publishes the refusal rather than
     * silence (rule 14). RBFBDpaltArmed and RBFBDpaltReady are published
     * separately because "the user did not ask" and "the aperture was too
     * small" are different facts a single flag would merge. */
    dpaltSample();
    setProperty("RBFBDpaltArmed", dpaltArmed);
    setProperty("RBFBDpaltReady", dpaltReady);
    setProperty("RBFBDpaltSweep", (uint64_t)dpaltSweep, 32);
    setProperty("RBFBDpaltActive", (uint64_t)dpaltActiveMask, 32);
    setProperty("RBFBDpaltNotDisabled", (uint64_t)dpaltNotDisabledMask, 32);
    setProperty("RBFBDpaltEverNotDisabled", (uint64_t)dpaltEverNotDisabled, 32);
    setProperty("RBFBDpaltChanges", dpaltChanges, 64);
    for (unsigned i = 0; i < RB_DPALT_COUNT; i++) {
        char key[24];
        /* 64 bits for the same 2B4 reason as the HPD words above. */
        snprintf(key, sizeof(key), "RBFBDpalt%uCntl6", i);
        setProperty(key, (uint64_t)dpaltWord[i], 64);
    }

    /* 2FC: the DIO encoder-type mux, one word per UNIPHY link. Published
     * whichever way the sweep falls, raw beside the summary, for the reason
     * that has now paid three times (2B0, 2B4, 2B9): the raw words refuted
     * this project's own verdict each time.
     *
     * RBFBDioSweep IS THE CONTROL AND IS READ FIRST. Only DIFFERS proves
     * these six words are the six registers; UNIFORM is what a reset block,
     * a wrong address and a correctly-on-DIG mux all produce, because
     * ENCODER_TYPE_DIG is zero (link_encoder.h:129). A reader who takes a
     * uniform zero as "the mux is fine" has read nothing. */
    dioSample();
    setProperty("RBFBDioReady", dioReady);
    setProperty("RBFBDioSweep", (uint64_t)dioSweep, 32);
    setProperty("RBFBDioNonzero", (uint64_t)dioNonzeroMask, 32);
    for (unsigned i = 0; i < RB_DIO_COUNT; i++) {
        char key[24];
        /* 64 bits for the same 2B4 sign-extension reason as every other raw
         * word in this function. */
        snprintf(key, sizeof(key), "RBFBDioLink%uCntl", i);
        setProperty(key, (uint64_t)dioWord[i], 64);
    }

    /* 300: RDPCSPIPE_PHY_CNTL6, both instances, raw beside the verdicts.
     *
     * RBFBDpcsPipeAgree IS THE PRODUCT and the others are how it was reached.
     * 0 means QUERY_DP_ALT did not answer for PHY2 in this boot, so there was
     * nothing to agree with and NO comparison was made - it is not a
     * disagreement (rule 14). 1 means an instance's DPALT_DISABLE matches the
     * DMUB's alt_disable, which is the first corroboration in a register that
     * PHY2's DP-alt state is what the DMUB says. 2 means neither does, which
     * is the larger finding: two instruments disagreeing, with the ROM match
     * on the DMUB's side.
     *
     * AND THE LINE A READER MUST NOT CROSS: these three bits are DPALT only.
     * This register defines no MPLL, no reference clock and no PSTATE, so
     * nothing published here says whether a lane is POWERED. That question is
     * still open and RDPCSTX2's 0x00063333 is still not an answer to it. */
    dpcspipeSample();
    setProperty("RBFBDpcsPipeReady", dpcspipeReady);
    setProperty("RBFBDpcsPipeAgree", (uint64_t)dpcspipeAgree, 32);
    setProperty("RBFBDpcsPipeSweep", (uint64_t)dpcspipeSweep, 32);
    for (unsigned i = 0; i < RB_DPCSPIPE_COUNT; i++) {
        char key[32];
        /* 64 bits for the same 2B4 sign-extension reason as every other raw
         * word in this function. */
        snprintf(key, sizeof(key), "RBFBDpcsPipe%uCntl6", i);
        setProperty(key, (uint64_t)dpcspipeWord[i], 64);
        snprintf(key, sizeof(key), "RBFBDpcsPipe%uVerdict", i);
        setProperty(key, (uint64_t)dpcspipeVerdict[i], 32);
        /* The dword each word came from, published beside it: two real
         * registers 0xd8 apart whose upstream table aliases three more
         * indexes onto them, so "which address was this" is part of the
         * reading and not a detail (rule 33). */
        snprintf(key, sizeof(key), "RBFBDpcsPipe%uDw", i);
        setProperty(key, (uint64_t)rb_dpcspipe_dw(i), 32);
    }
    /* 2EF: pipe 0's live DP pixel-clock DTO at BASE_IDX 1, with the same four
     * offsets at BASE_IDX 2 as the alias control. dtoSample() returns
     * immediately unless the boot armed -rbgpufbdto, so an unarmed boot pays
     * nothing - and publishes its refusal rather than falling silent, because
     * "not armed", "the aperture refused" and "read and found nothing" are
     * three different facts (rule 14).
     *
     * THE TWO VERDICTS ARE PUBLISHED SEPARATELY ON PURPOSE. "Is BASE_IDX 1 a
     * real address?" and "is pipe 0 actually driven by this DTO?" fail for
     * different reasons, and if the firmware brought the panel up on a PHYPLL
     * instead then PHASE[0] can be zero at a base that is perfectly correct.
     * Each of those three outcomes was written down before the boot (2EF),
     * so whichever comes back was predicted rather than explained. */
    dtoSample();
    otgSample();
    setProperty("RBFBDtoArmed", dtoArmed);
    setProperty("RBFBDtoReady", dtoReady);
    setProperty("RBFBDtoSeg1", (uint64_t)RB_DPDTO_SEG1, 32);
    setProperty("RBFBDtoBase", (uint64_t)dtoBaseVerdict, 32);
    setProperty("RBFBDtoPipe", (uint64_t)dtoPipeVerdict, 32);
    /* What PHASE[0] was compared against, and what it would have to be for
     * the comparison to mean anything. A refusal must publish the value it
     * refused on (rule 19). */
    setProperty("RBFBDtoExpectedHz", (uint64_t)dtoExpectedHz, 64);
    /* WHICH reference the comparison used - measured on a FRAME_COUNT edge,
     * or the live raster times the rate the vblank clock runs at. Two
     * readings taken against different references are two readings. */
    setProperty("RBFBDtoRefSrc", (uint64_t)dtoRefSrc, 32);
    setProperty("RBFBDtoHTotal", (uint64_t)dtoHTotal, 32);
    setProperty("RBFBDtoVTotal", (uint64_t)dtoVTotal, 32);
    setProperty("RBFBDtoRefreshMhz", (uint64_t)dtoRefreshMhz, 32);
    setProperty("RBFBDtoRefHz", (uint64_t)dtoRefHz, 64);
    for (unsigned i = 0; i < RB_DPDTO_COUNT; i++) {
        char key[28];
        /* 64 bits for the same 2B4 reason as the HPD words above - a 32-bit
         * OSNumber with bit 31 set prints sign-extended, and a PHASE of
         * 342 MHz is comfortably inside 31 bits but a garbage word at a wrong
         * base is exactly the thing that would not be. */
        snprintf(key, sizeof(key), "RBFBDto%uRate", i);
        setProperty(key, (uint64_t)dtoWord[i][RB_DPDTO_REG_RATE_CNTL], 64);
        snprintf(key, sizeof(key), "RBFBDto%uPhase", i);
        setProperty(key, (uint64_t)dtoWord[i][RB_DPDTO_REG_PHASE], 64);
        snprintf(key, sizeof(key), "RBFBDto%uModulo", i);
        setProperty(key, (uint64_t)dtoWord[i][RB_DPDTO_REG_MODULO], 64);
        snprintf(key, sizeof(key), "RBFBDto%uPhyPll", i);
        setProperty(key, (uint64_t)dtoWord[i][RB_DPDTO_REG_PHYPLL], 64);
    }
    /* The alias control's own four words, published raw: if these equal the
     * pipe-0 group above, the "DTO" being read is the DOMAIN block. They are
     * also readable against rb_pg.h's own publish of the same registers in a
     * boot that arms both, which is a second, independent way to see it. */
    for (unsigned r = 0; r < RB_DPDTO_REG_COUNT; r++) {
        char key[28];
        snprintf(key, sizeof(key), "RBFBDtoAlias%u", r);
        setProperty(key, (uint64_t)dtoAlias[r], 64);
    }
    /* 2F1: the write side, in one helper so the periodic publish and the dial's
     * own exit path cannot drift apart - 2E3 is what a dial whose state is
     * only published periodically costs (rule 31, and the fifth instance). */
    dtoPublish();
    otgPublish();
    /* 2B8: the AUX engines and the LIVE OTG words. */
    auxSample();
    setProperty("RBFBAuxEn", (uint64_t)auxEnMask, 32);
    setProperty("RBFBAuxMatch0", (uint64_t)auxMatchMask, 32);
    for (unsigned i = 0; i < RB_AUX_COUNT; i++) {
        char key[24];
        snprintf(key, sizeof(key), "RBFBAux%uCtl", i);
        setProperty(key, (uint64_t)auxControl[i], 64);
        snprintf(key, sizeof(key), "RBFBAux%uDphy", i);
        setProperty(key, (uint64_t)auxDphy[i], 64);
        snprintf(key, sizeof(key), "RBFBAux%uStat", i);
        setProperty(key, (uint64_t)auxStatus[i], 64);
        snprintf(key, sizeof(key), "RBFBAux%uArb", i);
        setProperty(key, (uint64_t)auxArb[i], 64);
    }
    for (unsigned i = 0; i < 4; i++) {
        char key[24];
        snprintf(key, sizeof(key), "RBFBOtgLive%u", i);
        setProperty(key, (uint64_t)otgLive[i], 64);
    }
    setProperty("RBFBAuxDpcdArmed", auxDpcdArmed);
    setProperty("RBFBAuxDpcdAttempts", auxDpcdAttempts, 64);
    setProperty("RBFBAuxDpcdOks", auxDpcdOks, 64);
    setProperty("RBFBAuxDpcdRefuse", (uint64_t)auxDpcdRefuse, 64);
    setProperty("RBFBAuxDpcdInst", (uint64_t)auxDpcdInst, 64);
    setProperty("RBFBAuxDpcdArbSeen", (uint64_t)auxDpcdArbSeen, 64);
    setProperty("RBFBAuxDpcdStatus", (uint64_t)auxDpcdStatus, 64);
    setProperty("RBFBAuxDpcdReply", (uint64_t)auxDpcdReplyCode, 64);
    setProperty("RBFBAuxDpcdDefers", (uint64_t)auxDpcdDefers, 32);
    setProperty("RBFBAuxDpcdDeferV", (uint64_t)auxDpcdDeferVerdict, 32);
    setProperty("RBFBAuxDpcdBytes", (uint64_t)auxDpcdBytes, 64);
    setProperty("RBFBAuxDpcdSuspect", auxDpcdSuspect);
    auxWPublish();                      /* 2F3 */
    dptxSample();                       /* 2F4, read-only */
    dptxPublish();
    /* The payload as raw bytes. Published individually rather than as one
     * packed word for 2B0's reason: the summary (revision, link rate, lanes)
     * is a reading OF this, and a wrong decode has to be refutable from the
     * same output. */
    for (unsigned i = 0; i < RB_DPCD_MAX_READ; i++) {
        char key[24];
        snprintf(key, sizeof(key), "RBFBAuxDpcd%02u", i);
        setProperty(key, (uint64_t)auxDpcdData[i], 64);
    }
    /* 2BC: the EDID. As ONE OSData blob and not 128 keys - this publish runs
     * on Apple's thread and 2zz measured ~110 setProperty calls there at
     * 1.4 % of the frame rate, so 128 more would be the instrument becoming
     * the defect (rule 16). ioreg prints it as bytes, which is what the tool
     * parses. */
    setProperty("RBFBEdidAttempts", auxEdidAttempts, 64);
    setProperty("RBFBEdidOks", auxEdidOks, 64);
    setProperty("RBFBEdidRefuse", (uint64_t)auxEdidRefuse, 64);
    setProperty("RBFBEdidInst", (uint64_t)auxEdidInst, 64);
    setProperty("RBFBEdidBytes", (uint64_t)auxEdidBytes, 64);
    setProperty("RBFBEdidDefers", (uint64_t)auxEdidDefers, 64);
    setProperty("RBFBEdidHeaderOk", auxEdidHeaderOk);
    setProperty("RBFBEdidSumOk", auxEdidSumOk);
    if (auxEdidBytes > 0) {
        setProperty("RBFBEdid", auxEdidData, (unsigned int)auxEdidBytes);
    }
    setProperty("RBFBHubpOtg1", hubpOtgCtl1, 32);
    setProperty("RBFBHubpOtg2", hubpOtgCtl2, 32);
    setProperty("RBFBHubpOtg3", hubpOtgCtl3, 32);
    setProperty("RBFBHubpOtgAnyArmed", hubpOtgAny);
    /* 2B5: this used to publish amtrNotReady - the LIVE value - under a name
     * claiming the boot set it up, so it read No after a runtime holdset 0 on
     * a boot that armed the hold fine, and nothing recorded the boot's own
     * state at all (rule 34). The snapshot is taken once, on the first
     * publish, before any setProperties can have reached us. */
    if (!amtrNotReadySetupHave) {
        amtrNotReadySetup = amtrNotReady;
        amtrNotReadySetupHave = true;
    }
    setProperty("RBFBAmtrNotReadyArmed", amtrNotReadySetup);
    setProperty("RBFBAmtrHeld", amtrHeld, 64);
    setProperty("RBFBAmtrHoldExpired", amtrHoldExpired, 64);
    /* 2xs: the backlight. Published unconditionally so `RBFBBlArmed` is the
     * cheap independent check that this binary is the one running (rule 15),
     * and so a refusal says WHICH gate refused and with what register values
     * (rule 14) - "we did not write" and "the period read zero, so 2or's
     * falsification test fired" are different findings. */
    setProperty("RBFBBlArmed", blArmed);
    setProperty("RBFBBlReady", blReady);
    setProperty("RBFBBlRefuse", (uint64_t)blRefuse, 32);
    setProperty("RBFBBlOtgCtl", (uint64_t)blOtgCtl, 32);
    /* 2zi: sample the live registers BEFORE publishing them, so the values in
     * this publish describe this moment rather than the previous one. */
    blSampleLive();
    setProperty("RBFBBlLiveCntl", (uint64_t)blLiveCntl, 32);
    setProperty("RBFBBlLiveLock", (uint64_t)blLiveLock, 32);
    setProperty("RBFBBlLiveCount", (uint64_t)blLiveCount, 32);
    setProperty("RBFBBlLiveReads", blLiveReads, 64);
    setProperty("RBFBBlLiveDiffs", blLiveDiffs, 64);
    /* 2zl: DMCUB inbox 1. RBFBDmubReads is the rule-14 denominator - without
     * it a zero move count cannot be told from a sampler that never ran. */
    setProperty("RBFBDmubInboxBase", (uint64_t)dmubInboxBase, 32);
    setProperty("RBFBDmubInboxSize", (uint64_t)dmubInboxSize, 32);
    setProperty("RBFBDmubWptr0", (uint64_t)dmubWptr0, 32);
    setProperty("RBFBDmubRptr0", (uint64_t)dmubRptr0, 32);
    setProperty("RBFBDmubWptr", (uint64_t)dmubWptr, 32);
    setProperty("RBFBDmubRptr", (uint64_t)dmubRptr, 32);
    setProperty("RBFBDmubReads", dmubReads, 64);
    setProperty("RBFBDmubWptrMoves", dmubWptrMoves, 64);
    setProperty("RBFBDmubRptrMoves", dmubRptrMoves, 64);
    /* 2zn: the locate, the scan control and every send, in one place. */
    dmubPublish();
    setProperty("RBFBBlCntl", (uint64_t)blCntl, 32);
    setProperty("RBFBBlCntl2", (uint64_t)blCntl2, 32);
    setProperty("RBFBBlPeriod", (uint64_t)blPeriod, 32);
    setProperty("RBFBBlLock", (uint64_t)blLock, 32);
    setProperty("RBFBBlLevel0", (uint64_t)blLevel0, 32);
    setProperty("RBFBBlFloor", (uint64_t)blFloor, 32);
    setProperty("RBFBBlFloorRefused", (uint64_t)blFloorRefused, 32);
    setProperty("RBFBBlLastLevel", (uint64_t)blLastLevel, 32);
    setProperty("RBFBBlLastCount", (uint64_t)blLastCount, 32);
    setProperty("RBFBBlReadback", (uint64_t)blReadback, 32);
    setProperty("RBFBBlSets", blSets, 64);
    setProperty("RBFBBlGets", blGets, 64);
    setProperty("RBFBBlPendTimeouts", blPendTimeouts, 64);
    setProperty("RBFBBlParmAdds", blParmAdds, 64);
    /* 2xy: the AppleBacklightDisplay match. RBFBBlBuiltIn says the boot armed
     * it; the two counters separate "the probe never asked" from "it asked
     * and still did not match" (rule 14). */
    setProperty("RBFBBlBuiltIn", blBuiltIn);
    setProperty("RBFBBlFlagsGets", blFlagsGets, 64);
    setProperty("RBFBBlSenseGets", blSenseGets, 64);
    /* 2y0: each of these separates a distinct outcome - see the comment on
     * the members in RBAMDFramebuffer.hpp. */
    setProperty("RBFBBlParmArmed", blParmArmed);
    setProperty("RBFBBlParmSetDisplays", blParmSetDisplays, 64);
    setProperty("RBFBBlParmUpdates", blParmUpdates, 64);
    setProperty("RBFBBlParmPublished", blParmPublished, 64);
    setProperty("RBFBBlParmPublishFails", blParmPublishFails, 64);
    setProperty("RBFBBlParmNoDict", blParmNoDict, 64);
    setProperty("RBFBBlParmSets", blParmSets, 64);
    /* 2y4: OTG0's timing as the firmware left it, plus the DRR group. Live
     * each publish for FRAME_COUNT's sake is NOT what this is - these are the
     * blSetup() snapshot, so they do not move, and the refresh they imply is
     * clk/(H_TOTAL * V_TOTAL). */
    setProperty("RBFBDrrArmed", drrArmed);
    setProperty("RBFBDrrDone", drrDone);
    setProperty("RBFBDrrReverted", drrReverted);
    setProperty("RBFBDrrRefuse", (uint64_t)drrRefuse, 32);
    setProperty("RBFBDrrLines", (uint64_t)drrLines, 32);
    setProperty("RBFBDrrSecs", (uint64_t)drrSecs, 32);
    setProperty("RBFBDrrDelay", (uint64_t)drrDelay, 32);
    setProperty("RBFBDrrRuns", drrRuns, 64);
    setProperty("RBFBDrrMhzBefore", (uint64_t)drrMhzBefore, 32);
    setProperty("RBFBDrrMhzDuring", (uint64_t)drrMhzDuring, 32);
    setProperty("RBFBDrrMhzAfter", (uint64_t)drrMhzAfter, 32);
    setProperty("RBFBDrrMinRb", (uint64_t)drrMinRb, 32);
    setProperty("RBFBDrrMaxRb", (uint64_t)drrMaxRb, 32);
    setProperty("RBFBDrrCtlRb", (uint64_t)drrCtlRb, 32);
    setProperty("RBFBDrrCtl0", (uint64_t)drrCtl0, 32);
    setProperty("RBFBDrrVTotalLive", (uint64_t)drrVTotalLive, 32);
    setProperty("RBFBDrrStep", (uint64_t)drrStep, 32);
    /* 2y6: the mode table. ModeCur against ModeLastAsked is what separates
     * "the compositor never asked" from "it asked and we refused" - the two
     * that looked identical for the whole of the brightness work (rule 14).
     * Switches counts real transitions; Applies counts every call including
     * the compositor re-asking for the mode it is already in, which it does
     * on wake and on every reconfigure. */
    setProperty("RBFBModeArmed", modeArmed);
    setProperty("RBFBModeRefuse", (uint64_t)modeRefuse, 32);
    setProperty("RBFBModeCur", (uint64_t)(uint32_t)modeCur, 32);
    setProperty("RBFBModeLastAsked", (uint64_t)modeLastAsked, 32);
    setProperty("RBFBModeCount", (uint64_t)getDisplayModeCount(), 32);
    setProperty("RBFBModeSwitches", modeSwitches, 64);
    setProperty("RBFBModeApplies", modeApplies, 64);
    /* Derived, not read: clk = measured mHz * H_TOTAL * V_TOTAL. Published so
     * a boot can check the derivation rather than take it on trust - BaseMhz
     * and HTotal0/VTotal0 are the three inputs it came from. */
    setProperty("RBFBModeClkHz", (uint64_t)modeClkHz, 32);
    setProperty("RBFBModeBaseMhz", (uint64_t)modeBaseMhz, 32);
    setProperty("RBFBModeHTotal0", (uint64_t)modeHTotal0, 32);
    setProperty("RBFBModeVTotal0", (uint64_t)modeVTotal0, 32);
    /* 2z0: the whole DERIVED list, so a boot can see what this panel was
     * offered rather than inferring it from a count. Millihertz per entry,
     * index 0 native; the line counts travel beside them because the mHz is
     * computed FROM the lines and a reading should be checkable. */
    setProperty("RBFBModeListN", (uint64_t)modeCount, 32);
    {
        char buf[160];
        unsigned o = 0;
        for (unsigned i = 0; i < modeCount && o + 24 < sizeof(buf); i++) {
            o += (unsigned)snprintf(buf + o, sizeof(buf) - o, "%s%u@%u",
                                    i ? " " : "", modeList[i].lines,
                                    modeList[i].mhz);
        }
        buf[o < sizeof(buf) ? o : sizeof(buf) - 1] = '\0';
        setProperty("RBFBModeList", buf);
    }
    {
        const int ci = modeIndexOf(modeCur);
        setProperty("RBFBModeCurMhz",
                    (uint64_t)(ci >= 0 ? modeList[ci].mhz : 0u), 32);
        setProperty("RBFBModeCurLines",
                    (uint64_t)(ci >= 0 ? modeList[ci].lines : 0u), 32);
    }
    setProperty("RBFBModeMinRb", (uint64_t)modeMinRb, 32);
    setProperty("RBFBModeMaxRb", (uint64_t)modeMaxRb, 32);
    setProperty("RBFBModeCtlRb", (uint64_t)modeCtlRb, 32);
    /* The frame clock itself, so "the mode switched" and "the compositor is
     * being paced for it" are two readable facts rather than one assumed one.
     * This is the whole gap boot 255 left open on purpose. */
    setProperty("RBFBVBLPeriodNs", vblPeriodNs, 64);
    setProperty("RBFBVBLRetunes", vblRetunes, 64);
    /* 2y7: the rate SINCE the current mode began, in millihertz, which is the
     * number boot 256 could not read from the kext at all. Published beside
     * its own inputs so a reading can be checked rather than trusted, and 0
     * before any retune rather than a division by a zero window. */
    {
        uint64_t ns = 0;
        absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
        const uint64_t dn = (ns > vblNsAtRetune) ? (ns - vblNsAtRetune) : 0;
        const uint64_t df = vblFires - vblFiresAtRetune;
        setProperty("RBFBVBLModeFires", df, 64);
        setProperty("RBFBVBLModeNs", dn, 64);
        setProperty("RBFBVBLModeEnables",
                    (uint64_t)(vblEnables - vblEnablesAtRetune), 32);
        setProperty("RBFBVBLModeMhz",
                    dn ? (df * 1000000000000ull / dn) : 0ull, 64);
    }
    publishFrameTime();
    setProperty("RBFBOtgHTotal", (uint64_t)otgHTotal, 32);
    setProperty("RBFBOtgVTotal", (uint64_t)otgVTotal, 32);
    setProperty("RBFBOtgVTotalMin", (uint64_t)otgVTotalMin, 32);
    setProperty("RBFBOtgVTotalMax", (uint64_t)otgVTotalMax, 32);
    setProperty("RBFBOtgVTotalMid", (uint64_t)otgVTotalMid, 32);
    setProperty("RBFBOtgVTotalCntl", (uint64_t)otgVTotalCntl, 32);
    setProperty("RBFBOtgVBlankSE", (uint64_t)otgVBlankSE, 32);
    setProperty("RBFBOtgFrameCount0", (uint64_t)otgFrameCount0, 32);
    setProperty("RBFBBlParmSetsPerc", blParmSetsPerc, 64);
    setProperty("RBFBBlParmSetsLin", blParmSetsLin, 64);
    setProperty("RBFBBlParmSetsUsable", blParmSetsUsable, 64);
    setProperty("RBFBBlParmSetFails", blParmSetFails, 64);
    setProperty("RBFBBlParmLastAsked", (uint64_t)blParmLastAsked, 32);
    setProperty("RBFBHubpTiling", (uint64_t)hubpTiling, 32);
    setProperty("RBFBHubpOrig", hubpOrig, 64);
    setProperty("RBFBHubpWrites", hubpWrites, 64);
    setProperty("RBFBHubpLast", hubpLast, 64);
    setProperty("RBFBHubpReadback", hubpReadback, 64);
    setProperty("RBFBHubpRejects", hubpRejects, 64);
    setProperty("RBFBHubpAll", hubpAll);
    setProperty("RBFBHubpRejectAddr", hubpRejectAddr, 64);
    setProperty("RBFBHubpRejectWhy", (uint64_t)hubpRejectWhy, 32);

    setProperty("RBFBAmtrFlipSnapped", amtrFlipSnapped);
    setProperty("RBFBAmtrFlipPtr", amtrFlipPtr, 64);
    setProperty("RBFBAmtrFlipRefused", amtrFlipRefused, 64);
    if (amtrFlipSnapped) {
        setProperty("RBFBAmtrFlipSnap", amtrFlipSnap,
                    (unsigned int)sizeof(amtrFlipSnap));
        /* 2wf: the three fields the static read PREDICTS, published on their
         * own so the boot tests that read instead of assuming it. +0x00 is the
         * u32 AMDFramebuffer hands to saveFlipTransactionPixelFormat and
         * getDisplayPipeTransactionFlip sets to 0/6/7/8/9; +0x04 is the
         * rotation byte it writes from its own argument; +0x58 is the one
         * address store seen directly (base + offset off a vt[0x158] call).
         * If +0x58 is zero while the blob holds a plausible GPU VA elsewhere,
         * the prediction is wrong and the blob says where it actually is -
         * which is the whole reason the blob is published too. */
        setProperty("RBFBAmtrFlipFmt",
                    *(const uint32_t *)(amtrFlipSnap + 0x00), 32);
        setProperty("RBFBAmtrFlipRot",
                    (uint64_t)amtrFlipSnap[0x04], 32);
        setProperty("RBFBAmtrFlipA58",
                    *(const uint64_t *)(amtrFlipSnap + 0x58), 64);
    }
    if (amtrSnapped) {
        setProperty("RBFBAmtrSnap", amtrSnap, (unsigned int)sizeof(amtrSnap));
    }
}

/* 2wk: one vblank, delivered to the slots we did NOT register, as soon as
 * Apple enables one. It re-checks `enabled` here rather than trusting the
 * reason it was scheduled, so a window that has already closed costs nothing
 * and is counted as such - retireKicks minus retireDelivered is exactly how
 * often we were too late, which is the measurement this experiment owes.
 *
 * It deliberately does NOT touch vblFires: that counter is the project's
 * 143.83 Hz time reference (the crumb's rate formula divides by it), and an
 * irregular extra tick would corrupt a clock that three milestones now rely
 * on. The per-slot `fires` counter does move, because a fire really happened.
 *
 * This runs on our own thread_call, not on Apple's thread - the same context
 * the periodic path would have called `proc` from if it had ever caught the
 * window, so it is not a new hazard class, only better timing (rule 31). */
/* 2A0: file one interval into an accumulator.
 *
 * The conversion to nanoseconds happens HERE, once, at the boundary -
 * mach_absolute_time() units are not nanoseconds on every machine, and a
 * sum kept in mach units with a conversion applied at publish time would be
 * one more place for a stale scale factor to hide (rule 41).
 *
 * t1 <= t0 files nothing. That is not paranoia: deliverVBL runs on a
 * thread_call and setAttribute on Apple's thread, so the two can interleave,
 * and a subtraction that went negative would become a ~584-year sample that
 * the bound would merely DROP - silently losing the very samples the
 * instrument exists to collect, while the dropped counter looked like idle
 * desktop. Counting it as a drop keeps that visible.
 */
void RBAMDFramebuffer::ftFile(struct rb_ft_acc *acc, uint64_t t0, uint64_t t1,
                              bool bound)
{
    if (!ftOn || !acc) { return; }
    if (t1 <= t0) { acc->dropped++; return; }
    uint64_t ns = 0;
    absolutetime_to_nanoseconds(t1 - t0, &ns);
    rb_ft_add(acc, ns,
              bound ? rb_ft_gap_bound_ns(vblPeriodNs ? vblPeriodNs
                                                     : kVBLPeriodNsFast)
                    : 0ull);
}

/* 2A9: ftFile plus a histogram of the same sample.
 *
 * One conversion, one bound, two consumers - so the mean and the
 * distribution can never describe different sample sets, which is the
 * failure a second call site would quietly introduce. Always bounded: a
 * pending run spanning an idle desktop is the same nonsense here as
 * everywhere else, and a dropped sample must be dropped from BOTH or the
 * histogram's n and the accumulator's n stop agreeing - and that
 * disagreement is itself the published check. */
void RBAMDFramebuffer::ftFileHist(struct rb_ft_acc *acc,
                                  struct rb_ft_hist *hist,
                                  uint64_t t0, uint64_t t1)
{
    if (!ftOn || !acc) { return; }
    if (t1 <= t0) { acc->dropped++; return; }
    uint64_t ns = 0;
    absolutetime_to_nanoseconds(t1 - t0, &ns);
    const uint64_t period = vblPeriodNs ? vblPeriodNs : kVBLPeriodNsFast;
    if (ns > rb_ft_gap_bound_ns(period)) { acc->dropped++; return; }
    rb_ft_add(acc, ns, 0);
    rb_ft_hist_add(hist, ns, period);
}

/* 2A0: the budget, published whole.
 *
 * Every piece is published with its own count, max and drop count, not just
 * its mean - a mean over 3 samples and a mean over 1500 are different facts
 * and `ioreg` cannot tell them apart from the mean alone (rule 18). And the
 * derived quantities are published BESIDE their inputs, so the reader can
 * redo the arithmetic rather than trust it (the 2y7 lesson).
 *
 * RBFBFtRateMhz is the self-check: it is sets/s computed from the timing
 * budget, and tools/paired.sh computes sets/s from a counter delta over a
 * measured window. Two independent routes to one number. If they disagree,
 * the budget is wrong and nothing else here means anything - which is the
 * check 2zb's 194 Hz panel did not have until it was given one.
 *
 * Called from publishVBLState(), so it inherits 2zk's throttle and adds no
 * unthrottled registry traffic to the frame path. 2zz measured that cost at
 * -1.4 % and these are ~20 more properties on an already-~110 publish, but
 * the measurement whose cost is being measured must not be the one adding it.
 */
void RBAMDFramebuffer::publishFrameTime(void)
{
    setProperty("RBFBFtOn", ftOn);
    setProperty("RBFBFtResets", ftResets, 64);

    const uint64_t work = rb_ft_mean_ns(&ftWork);
    const uint64_t gap  = rb_ft_mean_ns(&ftGap);
    const uint64_t get  = rb_ft_mean_ns(&ftGet);
    const uint64_t vbl  = rb_ft_mean_ns(&ftVbl);

    setProperty("RBFBFtWorkNs", work, 64);
    setProperty("RBFBFtWorkN",  ftWork.n, 64);
    setProperty("RBFBFtWorkMax", ftWork.max_ns, 64);
    setProperty("RBFBFtWorkDrop", ftWork.dropped, 64);

    setProperty("RBFBFtGapNs", gap, 64);
    setProperty("RBFBFtGapN",  ftGap.n, 64);
    setProperty("RBFBFtGapMax", ftGap.max_ns, 64);
    setProperty("RBFBFtGapDrop", ftGap.dropped, 64);

    setProperty("RBFBFtGetNs", get, 64);
    setProperty("RBFBFtGetN",  ftGet.n, 64);
    setProperty("RBFBFtGetMax", ftGet.max_ns, 64);

    setProperty("RBFBFtVblNs", vbl, 64);
    setProperty("RBFBFtVblN",  ftVbl.n, 64);
    setProperty("RBFBFtVblMax", ftVbl.max_ns, 64);

    /* 2A1: the retry comparison. Published with both counts, because the
     * interesting failure is one bucket being nearly empty - a mean over 7
     * samples beside a mean over 6000 is not a comparison (rule 18), and
     * nothing in the means themselves would say so. */
    setProperty("RBFBFtRetryNs", rb_ft_mean_ns(&ftRetry), 64);
    setProperty("RBFBFtRetryN",  ftRetry.n, 64);
    setProperty("RBFBFtRetryMax", ftRetry.max_ns, 64);
    setProperty("RBFBFtPollNs", rb_ft_mean_ns(&ftPoll), 64);
    /* 2A7: the two halves of a hold. LatchWait is hardware time the flip
     * genuinely needs (it is vsync'd, so it cannot clear early); HoldAsks
     * says how many round trips Apple spent discovering that. Published with
     * their counts, and LatchWait.n below ftWork.n means some runs never
     * closed rather than that holding was cheap. */
    setProperty("RBFBFtLatchWaitNs", rb_ft_mean_ns(&ftLatchWait), 64);
    setProperty("RBFBFtLatchWaitN",  ftLatchWait.n, 64);
    setProperty("RBFBFtLatchWaitMax", ftLatchWait.max_ns, 64);
    setProperty("RBFBFtHoldAsksPm",
                ftHoldRunAsks.n ? (ftHoldRunAsks.sum_ns * 1000ull)
                                  / ftHoldRunAsks.n : 0ull, 64);
    setProperty("RBFBFtHoldRunsN", ftHoldRunAsks.n, 64);

    /* 2A9: the pending run, and its SHAPE. Published with the vblank period
     * beside it, because every bucket is a quarter of that period and a
     * reader who has to guess the scale cannot check the arithmetic (the 2y7
     * lesson). RBFBFtPendEarlyPm is the answer to "is there a middle
     * setting": ~0 means every flip needs the whole vblank and the 91-or-141
     * choice is real; large means a partial hold would let that fraction
     * through unheld. It is a share of a measured distribution and NOT a
     * predicted frame rate - 2A8 is what extrapolating a rate from a share
     * costs. */
    setProperty("RBFBFtPendWaitNs", rb_ft_mean_ns(&ftPendWait), 64);
    setProperty("RBFBFtPendWaitN",  ftPendWait.n, 64);
    setProperty("RBFBFtPendWaitMax", ftPendWait.max_ns, 64);
    setProperty("RBFBFtPendDropped", ftPendWait.dropped, 64);
    setProperty("RBFBFtPendAsksPm",
                ftPendRunAsks.n ? (ftPendRunAsks.sum_ns * 1000ull)
                                  / ftPendRunAsks.n : 0ull, 64);
    for (unsigned i = 0; i < RB_FT_HIST_BUCKETS; i++) {
        char key[24];
        snprintf(key, sizeof(key), "RBFBFtPendQ%u", i);
        setProperty(key, ftPendHist.b[i], 64);
    }
    setProperty("RBFBFtPendHistN", rb_ft_hist_n(&ftPendHist), 64);
    setProperty("RBFBFtPendEarlyPm", rb_ft_hist_early_pm(&ftPendHist), 64);
    setProperty("RBFBFtPollN",  ftPoll.n, 64);
    setProperty("RBFBFtPollMax", ftPoll.max_ns, 64);

    const uint64_t interval = rb_ft_interval_ns(&ftWork, &ftGap);

    /* 2A2: the hold's LIVE state, published from the PERIODIC publish and not
     * only from start().
     *
     * It was published once in start() beside the other arm keys, which is
     * right for a boot-arg and wrong for a runtime dial: `holdset 0` set the
     * field and the registry kept saying `Yes`, so tools/holdaba.sh refused
     * its own B phase after a correct write. Rule 31 with the terms swapped -
     * the usual form is "a counter that has not moved YET has not failed to
     * move", and this is the mirror: a property that is never republished
     * cannot move at all, so reading it back can only ever confirm the value
     * start() wrote. Any field a runtime dial can change has to be published
     * everywhere it is read, and the periodic publish is the one that tracks.
     */
    setProperty("RBFBAmtrNotReady", amtrNotReady);

    /* 2A2: the hold's measured share, published WITH its premise. The share
     * is only meaningful if the GET gaps account for the interval, so
     * RBFBFtGetsTilePm is published next to it and a reader that ignores it
     * can be misled - which is why ftbudget prints the tiling first and
     * refuses the share when it is off. */
    {
        const uint64_t pen = rb_ft_hold_penalty_ns(rb_ft_mean_ns(&ftRetry),
                                                   rb_ft_mean_ns(&ftPoll));
        const uint64_t hpt = rb_ft_holds_per_txn_pm(ftRetry.n, ftWork.n);
        setProperty("RBFBFtHoldPenaltyNs", pen, 64);
        setProperty("RBFBFtHoldsPerTxnPm", hpt, 64);
        setProperty("RBFBFtHoldSharePm",
                    rb_ft_hold_share_pm(pen, hpt, interval), 64);
        setProperty("RBFBFtGetsTilePm",
                    rb_ft_gets_tile_pm(rb_ft_mean_ns(&ftRetry), ftRetry.n,
                                       rb_ft_mean_ns(&ftPoll), ftPoll.n,
                                       ftWork.n, interval), 64);
    }
    setProperty("RBFBFtIntervalNs", interval, 64);
    setProperty("RBFBFtRateMhz", rb_ft_rate_mhz(interval), 64);

    /* The two 'amtr' GETs and the vblank handlers that fall inside one
     * transaction interval, rather than one of each: at fed ~50 % Apple arms
     * and disarms around every transaction (2zk measured 227 publishes a
     * second against ~73 transactions) and the panel delivers ~2 vblanks per
     * transaction. Scaled by the counts actually observed in this window
     * instead of by an assumed ratio - if the ratio changes, this follows it.
     */
    const uint64_t perTxn = ftWork.n ? ftWork.n : 1;
    const uint64_t oursNs = work
                          + (get * ftGet.n) / perTxn
                          + (vbl * ftVbl.n) / perTxn;
    setProperty("RBFBFtOursNs", oursNs, 64);
    setProperty("RBFBFtUpstreamNs",
                rb_ft_upstream_ns(interval, oursNs), 64);
    setProperty("RBFBFtUpstreamPm",
                rb_ft_share_pm(rb_ft_upstream_ns(interval, oursNs),
                               interval), 64);
    setProperty("RBFBFtOursPm", rb_ft_share_pm(oursNs, interval), 64);
}

void RBAMDFramebuffer::kickVBL(void)
{
    for (unsigned i = 0; i < kMaxInterrupts; i++) {
        if (interrupts[i].used && interrupts[i].enabled &&
            interrupts[i].target != this &&
            interrupts[i].type == kIOFBVBLInterruptType && interrupts[i].proc) {
            interrupts[i].fires++;
            retireDelivered++;
            interrupts[i].proc(interrupts[i].target, interrupts[i].ref);
        }
    }
}

void RBAMDFramebuffer::vblKicked(thread_call_param_t self, thread_call_param_t)
{
    auto fb = static_cast<RBAMDFramebuffer *>(self);
    if (fb) { fb->kickVBL(); }
}

void RBAMDFramebuffer::armVBL(void)
{
    if (!vblCall || !anyVBLEnabled()) {
        vblRunning = false;
        /* Forget the phase: the next enable starts a fresh cadence rather
         * than inheriting a deadline from however long ago we stopped. */
        vblNextDeadline = 0;
        return;
    }
    vblRunning = true;

    /* 2xe: a FIXED CADENCE, not a fixed delay. The old form was
     *     clock_interval_to_deadline(vblPeriodNs, 1, &deadline)
     * which is `now + period` - and because armVBL() runs at the END of
     * deliverVBL(), `now` already includes delivering to every handler and
     * hubpPoll()'s MMIO read. That overhead was added to every period and
     * cost a measured 0.47 ms a tick (134.70 Hz instead of 143.83 Hz).
     * Advancing the PREVIOUS deadline makes the error non-cumulative: a slow
     * tick is absorbed by the next interval instead of shifting all of them.
     */
    uint64_t period = 0;
    nanoseconds_to_absolutetime(vblPeriodNs ? vblPeriodNs : kVBLPeriodNsFast, &period);
    uint64_t now = 0;
    clock_get_uptime(&now);

    if (vblNextDeadline == 0) {
        vblNextDeadline = now + period;
    } else {
        vblNextDeadline += period;
        if (vblNextDeadline <= now) {
            /* A whole period late. Arming a deadline in the past makes
             * thread_call fire immediately and again and again until it has
             * caught up - a burst at the panel's expense, delivering ticks
             * the panel never had. Resync instead, and COUNT it: a silent
             * resync would make the fire count a lie about elapsed time,
             * which is the exact defect this change exists to fix. */
            vblNextDeadline = now + period;
            vblResyncs++;
        }
    }
    thread_call_enter_delayed(vblCall, vblNextDeadline);
}

void RBAMDFramebuffer::deliverVBL(void)
{
    /* 2A0: the vblank handler's own duration. Taken around EVERYTHING this
     * function does, because all of it is inside the gap between two
     * transactions and all of it is ours - the retire offer, Apple's
     * handlers that we call synchronously, and hubpPoll. Unbounded: there is
     * no idle-desktop failure mode here, since this runs every period
     * whatever the compositor is doing. */
    const uint64_t ftT0 = ftOn ? mach_absolute_time() : 0;

    /* 2zd: BEFORE delivery, not after. The retire is what frees a surface and
     * makes DisplaySurfacePool::IsReady() true (2xe), and the handlers called
     * below are the ones that ask - so retiring first lets this frame's
     * handler see this frame's retire, where retiring afterwards would make it
     * wait for the next vblank. */
    vblRetOffer();

    unsigned delivered = 0;
    for (unsigned i = 0; i < kMaxInterrupts; i++) {
        if (interrupts[i].used && interrupts[i].enabled &&
            interrupts[i].type == kIOFBVBLInterruptType && interrupts[i].proc) {
            interrupts[i].fires++;
            interrupts[i].proc(interrupts[i].target, interrupts[i].ref);
            delivered++;
        }
    }
    /* 2x3: follow the swap chain on every vblank. Placed after delivery so
     * the handlers see a consistent tick, and before the telemetry so the
     * published counters describe this fire. */
    hubpPoll();
    vblFires++;
    if (vblFires == 1) {
        LOG("first vblank delivered to %u handler(s)", delivered);
        publishVBLState();
    }
    /* Publish rather than log: at the measured refresh a per-fire IOLog
     * would be ~144 lines a second and would itself become the suspect
     * (rule 16). ioreg reads this live. */
    /* 2xe: 1200 fires is ~8.3 s, so a 25 s measurement window carried a
     * +/-33 % quantisation error on its own time base. 288 is ~2 s. */
    if ((vblFires % 288) == 0) {
        publishVBLState();
        setProperty("RBFBVBLHandlers", (UInt64)delivered, 32);
    }
    /* 2t0: four NVRAM breadcrumbs for the whole boot, at exact equality so
     * each lands once. The registry above is for a live boot; these are the
     * only record a boot that dies leaves behind (rule 10). The first is 100,
     * not 1, so no flash commit lands inside display bring-up. */
    if (vblFires == 100) {
        crumb(10);
    } else if (vblFires == 1000) {
        crumb(11);
    } else if ((vblFires % 10000) == 0) {
        /* 2wl: from here on the crumb is PERIODIC, every 10000 fires (~70 s),
         * and s= says which sample it is - 12 at 70 s, 13 at 139 s, 29 at
         * twenty minutes. Boot 197 ran for twenty minutes and left a crumb
         * from its first seventy seconds, because the old form wrote at four
         * fixed counts and then stopped; a boot that cannot be captured must
         * leave a record of its WHOLE life, not its first minute (rule 10).
         * One flash commit every 70 s is the same order as the four this
         * already did. */
        crumb(11 + (uint32_t)(vblFires / 10000));
    }
    /* 2wp: the no-transaction deadline, on the clock that is already running.
     * 18000 fires at the panel's measured 143.83 Hz is 125 seconds. The
     * separation is measured, not guessed: boot 196 had amtrSets 4 by fire
     * 10000 (70 s) and boot 197 had transactions flowing well before that,
     * while the boot that froze had amtrSets 0 at the same point. n=2, so the
     * deadline is set nearly twice as far out as the latest first transaction
     * seen. It only arms when the framebuffer actually took the panel for
     * transactions (amtrArmed), so a plain -rbgpuownfb boot is untouched.
     *
     * Firing it in a merely slow boot costs one power cycle to re-arm; NOT
     * firing it costs the user power-cycling into a frozen screen until they
     * give up. The asymmetry is the whole design. */
    if (amtrArmed && !selfDisarmed && vblFires == 18000 && amtrSets == 0) {
        disarmFB();
        crumb(99);
    }
    /* 2wl: roll the rate bucket on the same clock. ~7 s is short enough that
     * a burst of typing fills one window and long enough that a single flip
     * does not read as a rate. */
    if ((vblFires % 1000) == 0) {
        if (amtrBucket > amtrPeak) {
            amtrPeak = amtrBucket;
        }
        amtrBucket = 0;
    }
    armVBL();
    if (ftOn) { ftFile(&ftVbl, ftT0, mach_absolute_time(), false); }
}

void RBAMDFramebuffer::vblFired(thread_call_param_t self, thread_call_param_t)
{
    static_cast<RBAMDFramebuffer *>(self)->deliverVBL();
}

IOReturn RBAMDFramebuffer::registerForInterruptType(IOSelect interruptType,
                                                    IOFBInterruptProc proc,
                                                    OSObject *target, void *ref,
                                                    void **interruptRef)
{
    if (interruptType != kIOFBVBLInterruptType) {
        /* Decline every other type explicitly. A silent success here would be
         * a registration that reports success without a patch applying, which
         * rule 20 exists to forbid. */
        return kIOReturnUnsupported;
    }
    if (!interruptRef || !proc) {
        return kIOReturnBadArgument;
    }
    *interruptRef = nullptr;
    for (unsigned i = 0; i < kMaxInterrupts; i++) {
        if (interrupts[i].used) {
            continue;
        }
        interrupts[i].type    = interruptType;
        interrupts[i].proc    = proc;
        interrupts[i].target  = target;
        interrupts[i].ref     = ref;
        /* 2sv: IOFramebuffer::open registers handleVBL at 0x14fc5ef5 and
         * never performs an initial enable. Starting disabled leaves the
         * shared vblCount at zero forever (measured in boot 171). Registration
         * starts enabled; a pipe registrant can immediately disable ITS slot,
         * as displayModeDidChange does, without stopping the framebuffer clock.
         * updateVBL's later enable is a throttle re-enable, not a bootstrap.
         */
        interrupts[i].enabled = true;
        interrupts[i].used    = true;
        interrupts[i].fires   = 0;
        *interruptRef = &interrupts[i];
        vblEnables++;
        /* 2zd: look the plugin's peer up HERE, not on the vblank. This runs
         * from IOFramebuffer::open - an ordinary IOService context where
         * taking locks and matching services is safe - and it must happen
         * before armVBL() starts delivering. */
        vblRetSetup();
        /* 2CA: and ask the chip its own name from the same safe context. The
         * plugin's register aperture comes up with the accelerator, which is
         * later than our start(), so this cannot be done where the literals
         * are published - it has to be somewhere ordinary that runs after
         * the accelerator is alive, and this is that place. */
        gpuNameSetup();
        if (!vblRunning) {
            armVBL();
        }
        publishVBLState();
        LOG("registered 'vbl ' handler %u (period %llu ns)", i,
            vblPeriodNs ? vblPeriodNs : kVBLPeriodNsFast);
        return kIOReturnSuccess;
    }
    LOG("no free interrupt slot for 'vbl ' (%u in use)", kMaxInterrupts);
    return kIOReturnNoResources;
}

IOReturn RBAMDFramebuffer::unregisterInterrupt(void *interruptRef)
{
    for (unsigned i = 0; i < kMaxInterrupts; i++) {
        if (interruptRef == &interrupts[i] && interrupts[i].used) {
            bzero(&interrupts[i], sizeof(interrupts[i]));
            if (!anyVBLEnabled() && vblCall) {
                thread_call_cancel(vblCall);
                vblRunning = false;
            }
            publishVBLState();
            return kIOReturnSuccess;
        }
    }
    return kIOReturnBadArgument;
}

IOReturn RBAMDFramebuffer::setInterruptState(void *interruptRef, UInt32 state)
{
    for (unsigned i = 0; i < kMaxInterrupts; i++) {
        if (interruptRef == &interrupts[i] && interrupts[i].used) {
            bool was = interrupts[i].enabled;
            interrupts[i].enabled = (state != 0);
            if (!was && interrupts[i].enabled) {
                vblEnables++;
                /* 2wk: answer the enable NOW, not on the next 7 ms tick.
                 * Field write plus a thread_call_enter only - no allocation,
                 * no logging, no blocking on Apple's thread (rule 31). */
                if (retireArmed && interrupts[i].target != this && vblKickCall) {
                    retireKicks++;
                    thread_call_enter(vblKickCall);
                }
            }
            if (interrupts[i].enabled && !vblRunning) {
                armVBL();
            } else if (!anyVBLEnabled() && vblCall) {
                thread_call_cancel(vblCall);
                vblRunning = false;
            }
            if (was != interrupts[i].enabled) {
                LOG("setInterruptState slot %u: %s -> %s (running %d, "
                    "enables %u, fires %llu)", i, was ? "on" : "off",
                    interrupts[i].enabled ? "on" : "off", vblRunning,
                    vblEnables, vblFires);
                /* 2zk: throttled. This is the 227/s site - Apple arms and
                 * disarms the vblank twice per transaction, so an
                 * unthrottled publish here is ~25 000 registry insertions a
                 * second inside the call that gates every frame. */
                publishVBLStateHot();
            }
            return kIOReturnSuccess;
        }
    }
    return kIOReturnBadArgument;
}

/* 2y6: retune the frame clock to a new mode's period.
 *
 * Deliberately NOT a cancel-and-rearm. The thread_call is live and delivering
 * 'vbl ' to the compositor; stopping it to change a number is a gap in the
 * frame clock, and 2xe is the standing lesson that this clock's cadence is
 * load-bearing. Instead the period is a plain member the callout re-reads each
 * time it arms, so the worst case of a switch racing a tick is one tick armed
 * at the old period - 7 ms, once. The store is an aligned 64-bit write on
 * x86_64 and cannot tear.
 *
 * vblNextDeadline is zeroed because armVBL() ACCUMULATES (`+= period`) rather
 * than re-basing from now, which is exactly what 2xe fixed and must not be
 * undone. Accumulating against a deadline computed from the old period would
 * carry the old cadence forward across the switch; zero makes the next arm
 * re-base once, at the cost of a single frame of phase.
 */
void RBAMDFramebuffer::vblRetune(uint64_t periodNs)
{
    if (!periodNs) { return; }
    vblPeriodNs = periodNs;
    vblNextDeadline = 0;
    vblRetunes++;
    /* Latch the baselines BEFORE the new period takes effect, so the next
     * mode's rate is measurable from the kext rather than from a tool's
     * window (2y7). */
    vblFiresAtRetune = vblFires;
    vblEnablesAtRetune = vblEnables;
    {
        uint64_t ns = 0;
        absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
        vblNsAtRetune = ns;
    }
    setProperty("RBFBVBLPeriodNs", vblPeriodNs, 64);
}

#ifndef RBGPU_FB_VBL_HOSTTEST
/* 2zm: the live setter is kext-only. The host build of this file compiles
 * against fb_vbl_shim.hpp, which has no OSDictionary, no OSNumber and no
 * IOReturn codes - and the DECISION it makes is rb_pub_ms_ok(), which lives
 * in src/common and is asserted by defaults_test.c on the host, so nothing
 * testable is being excluded here (the throttle itself, publishVBLStateHot,
 * stays in the host build and has its own case). */
#include "rb_defaults.h"   /* rb_pub_ms_ok() */
#include "rb_dmub.h"      /* rb_dmub_level_ok() */

/* 2zm: the live tunable, reached from userspace by
 * IORegistryEntrySetCFProperties.
 *
 * WHY THIS EXISTS. Rule 44 says a configuration claim needs two boots per
 * side OR an A/B inside one boot, and until now this project has only ever
 * had the first option, because every dial it owns is a boot-arg: 2xn, 2zb
 * and 2zc spent three boots on three points of one dose-response curve, and
 * 2zg then spent two more discovering that boot-to-boot variance exceeds
 * within-boot variance - which is precisely the thing a single-boot A/B is
 * immune to. One tunable that can be moved while the frame clock keeps
 * running turns that into six runs, a write, and six more.
 *
 * WHAT IT WILL AND WILL NOT DO. `RBFBPubMs` only, bounded by the same
 * rb_pub_ms_ok() the boot-arg path uses so the two cannot drift (rule 38).
 * No register is written, no hardware state moves and nothing about the panel
 * changes - it sets how often an INSTRUMENT publishes. Any other key is
 * counted in pubSetsOther and declined, so a misspelled key is a reading and
 * not silence (rule 14), and a refused value is published in pubMsLast
 * (rule 19).
 *
 * Note what is NOT done here: the deadline is reset to zero rather than
 * recomputed from `now`, so the first publish after a change happens at once.
 * A new interval that first had to expire would put a window of the OLD
 * behaviour at the start of the new arm - which is exactly the kind of seam
 * that makes an A/B measure the transition instead of the two states. */
IOReturn RBAMDFramebuffer::setProperties(OSObject *props)
{
    OSDictionary *dict = OSDynamicCast(OSDictionary, props);
    if (!dict) { return kIOReturnBadArgument; }

    /* 2zn: the DMCUB triggers, checked before RBFBPubMs because they are the
     * reason this setter now takes more than one key. Each one sends ONE
     * command and returns what the push decided, so a userspace trigger
     * reports its own outcome instead of leaving the caller to infer it from
     * counters read later (rule 31: a published counter is stale by design).
     *
     * Two keys and not one: dcn21_set_backlight_level binds ABM to the pipe
     * with SET_PIPE before it sets a level, and whether this chip needs that
     * is unknown - the firmware's 37 bring-up commands may already have done
     * it. Sending both unasked would make a moving panel ambiguous between
     * them, which is exactly the mistake brightness has already made four
     * times (rule 45). So: RBFBDmubBl alone first; if the panel does not
     * move, RBFBDmubPipe and then RBFBDmubBl again. Both inside one boot,
     * which is the point. */
    /* 2zt: RBFBDmubBlType sets the control type for every SUBSEQUENT send,
     * and sends nothing itself - so the three values cost one boot instead
     * of three, and a type change is never confounded with a level change
     * (rule 45: one dial per observation). aux_inst rides with it because
     * for an AUX type it is load-bearing and for PWM it must stay 0. */
    {
        OSNumber *t = OSDynamicCast(OSNumber,
                                    dict->getObject("RBFBDmubBlType"));
        if (t) {
            const long n = (long)t->unsigned32BitValue();
            if (!rb_dmub_bl_type_ok(n)) { return kIOReturnBadArgument; }
            dmubBlType = (uint32_t)n;
            dmubBlAuxInst = 0u;
            {
                OSNumber *a = OSDynamicCast(OSNumber,
                                        dict->getObject("RBFBDmubBlAuxInst"));
                if (a) { dmubBlAuxInst = a->unsigned32BitValue() & 0xffu; }
            }
            dmubBlTypeSets++;
            LOG("RBFBDmubBlType -> %u, aux_inst %u (no command sent)",
                dmubBlType, dmubBlAuxInst);
            dmubPublish();                       /* 2E3 */
            return kIOReturnSuccess;
        }
    }
    {
        OSNumber *b = OSDynamicCast(OSNumber, dict->getObject("RBFBDmubBl"));
        if (b) {
            const long want = (long)b->unsigned32BitValue();
            if (!rb_dmub_level_ok(want)) {
                dmubSendsRefused++;
                dmubSendRefuse = 6;
                return kIOReturnBadArgument;
            }
            const uint32_t rc = dmubSendBacklight((uint32_t)want);
            LOG("RBFBDmubBl %ld -> refuse %u, wptr %#x, acks %llu",
                want, rc, dmubSendWptr, dmubAcks);
            dmubPublish();                       /* 2E3 */
            return rc == 0u ? kIOReturnSuccess : kIOReturnNotPermitted;
        }
    }
    {
        OSNumber *w = OSDynamicCast(OSNumber, dict->getObject("RBFBDmubSel"));
        if (w) {
            const long n = (long)w->unsigned32BitValue();
            if (!rb_dmub_winsel_ok(n)) { return kIOReturnBadArgument; }
            const bool ok = dmubSelect((uint32_t)n);
            dmubSelSets++;
            LOG("RBFBDmubSel %ld -> %s", n, ok ? "selected" : "unavailable");
            /* 2E3: publish BEFORE returning, so the caller's read-back is
             * this call's state and not the last periodic snapshot's.
             *
             * Every DMUB key is published from publishVBLState(), which the
             * RBFBPubMs throttle holds to one in 500 ms - so `dmubbl dig`
             * asked for window 7, got SUCCESS, read the selector back
             * immediately, saw the stale 4 and refused its own send. Rule 31,
             * and the THIRD time this shape has been paid for: 2C5's four
             * false MISMATCHes, 2D1's aborted subret run, now this - written
             * into 2E2's journal entry as the thing to avoid and then done
             * fifteen lines away.
             *
             * The general fix rather than a per-dial one: a DMUB dial
             * publishes the DMUB state on its way out. These handlers run
             * from setProperties on a userspace thread at human frequency,
             * never the frame path, so rule 16's cost argument for the
             * throttle does not reach them. */
            dmubPublish();
            return ok ? kIOReturnSuccess : kIOReturnNotPermitted;
        }
    }
    if (dict->getObject("RBFBDmubGpint")) {
        const uint32_t rc = dmubGpint(RB_DMUB_GPINT_GET_FW_VERSION);
        LOG("RBFBDmubGpint -> refuse %u, sent %#x, back %#x, acked %u, "
            "scratch7 %#x -> %#x", rc, dmubGpintSent, dmubGpintBack,
            dmubGpintAcked, dmubScratch7Pre, dmubScratch7);
        dmubPublish();                           /* 2E3 */
        return rc == 0u ? kIOReturnSuccess : kIOReturnNotPermitted;
    }
    if (dict->getObject("RBFBDmubCaps")) {
        const uint32_t rc = dmubSendCaps();
        LOG("RBFBDmubCaps -> refuse %u, wptr %#x, verdict %u",
            rc, dmubSendWptr, dmubReplyVerdict);
        dmubPublish();                           /* 2E3 */
        return rc == 0u ? kIOReturnSuccess : kIOReturnNotPermitted;
    }
    if (dict->getObject("RBFBDmubPipe")) {
        const uint32_t rc = dmubSendPipe();
        LOG("RBFBDmubPipe -> refuse %u, wptr %#x, acks %llu",
            rc, dmubSendWptr, dmubAcks);
        dmubPublish();                           /* 2E3 */
        return rc == 0u ? kIOReturnSuccess : kIOReturnNotPermitted;
    }
    /* 2DC: DOMAIN_CONTROL, as ONE call that samples, sends and samples.
     *
     * Not three keys, and that is the point. Four sessions in a row were
     * spent reading a null whose cause was state that moved between the arm
     * and the action - the wrong physical port, the wrong firmware MUX mode,
     * the wrong ring window, and that window again after a reboot silently
     * reset it. Every one of those was a handover that needed two things to
     * be true at once and only checked one. An experiment that is a single
     * call cannot be run out of order, cannot be split by a reboot, and
     * cannot mistake a stale published value for its own precondition
     * (rule 34 made structural rather than advisory).
     *
     * The value is the DOMAIN instance; a negative one means "pick the
     * candidate yourself", which is what rb_pg_candidate() is for, and it
     * refuses rather than guessing when no domain is in a state a power-up
     * could change. */
    {
        OSNumber *w = OSDynamicCast(OSNumber, dict->getObject("RBFBDmubDomain"));
        if (w) {
            dmubDomCalls++;   /* 2DD: monotonic, before any refusal path */
            const int32_t want = (int32_t)w->unsigned32BitValue();
            OSNumber *g = OSDynamicCast(OSNumber,
                                        dict->getObject("RBFBDmubDomainGate"));
            const uint32_t gate = g ? (g->unsigned32BitValue() ? 1u : 0u) : 0u;
            uint32_t inst;

            if (want < 0) {
                uint32_t st[RB_PG_DOMAIN_COUNT], cf[RB_PG_DOMAIN_COUNT];
                pgSample(st, cf);
                /* 2DD: an unreadable block makes rb_pg_candidate() answer -1
                 * for the wrong reason, so it is refused before it is asked.
                 * Distinct code, because "the instrument is reading the wrong
                 * dwords" and "no domain is gated" are different answers and
                 * boot 436 could not tell them apart (rule 14). */
                if (!rb_pg_sample_readable(st, cf)) {
                    dmubDomRefuse = 25u;
                    LOG("RBFBDmubDomain auto -> refuse 25, the DOMAIN block "
                        "read as eight zero words - unreadable sample, not a "
                        "chip answer");
                    dmubPublish();               /* 2E3 */
                    return kIOReturnNotPermitted;
                }
                const int cand = rb_pg_candidate(st);   /* a SLOT */
                if (cand < 0) {
                    dmubDomRefuse = 24u;
                    LOG("RBFBDmubDomain auto -> refuse 24, no domain differs "
                        "from DOMAIN0 so a power-up cannot show anything");
                    dmubPublish();               /* 2E3 */
                    return kIOReturnNotPermitted;
                }
                /* 2DE: slot -> DOMAIN NUMBER. The experiment and the
                 * command both take the number, never the index. */
                inst = rb_pg_slot_inst((uint32_t)cand);
            } else {
                inst = (uint32_t)want;
            }

            const uint32_t rc = dmubDomainExperiment(inst, gate);
            LOG("RBFBDmubDomain %u gate %u -> refuse %u, moved %#x, wptr %#x, "
                "acks %llu", inst, gate, rc, dmubDomMoved, dmubSendWptr,
                dmubAcks);
            dmubPublish();                       /* 2E3 */
            return rc == 0u ? kIOReturnSuccess : kIOReturnNotPermitted;
        }
    }

    /* 2E2: the DIG1 transmitter ENABLE for the Type-C port.
     *
     * Same single-call shape and the same reason as the DOMAIN handler
     * above. The value is the phyid; a negative one means "the port 2E1
     * derived", which is PHY2 - so the ordinary invocation names no magic
     * number and cannot name the wrong one. PHY0 is the live panel and
     * rb_dig_refuse() declines it by name whatever is asked.
     *
     * `digCalls` is bumped FIRST, before every refusal path, so the tool can
     * poll something monotonic instead of sleeping and comparing two reads of
     * the value it wants to interpret - which is how 2C5's four false
     * MISMATCHes and 2D1's aborted run both happened. */
    /* 2E5: the encoder dial, ahead of RBFBDigPhy so one setProperties call
     * carrying both lands the dial before the send it governs. A named value
     * pins it for the boot in the only sense that matters here - nothing else
     * writes it - and it is published on the way out (2E3) so the tool reads
     * the state it just set rather than the previous snapshot. */
    /* 2EC: the FE dial, ahead of RBFBDigPhy for the same reason RBFBDigEnc
     * is - one setProperties call carrying both lands the dial before the
     * send it governs. The VALUE is the front end's ONE-HOT bit, not an
     * index: 4 is DIGC, and 0 (the default) is the documented INVALID, which
     * rb_digbe_fe_refuse() declines - so an armed boot writes nothing until
     * a front end is named. A negative value means "the front end 2EC
     * derived for the Type-C back end", DIGC, so the ordinary invocation
     * names no magic number and cannot name the wrong one. */
    {
        OSNumber *f = OSDynamicCast(OSNumber, dict->getObject("RBFBDigFe"));
        if (f) {
            const int32_t want = (int32_t)f->unsigned32BitValue();
            digFe = want < 0 ? RB_DIGBE_TYPEC_FE : (uint32_t)want;
            LOG("RBFBDigFe -> 0x%x", digFe);
            dmubPublish();
            if (!dict->getObject("RBFBDigPhy")) { return kIOReturnSuccess; }
        }
    }

    {
        OSNumber *e = OSDynamicCast(OSNumber, dict->getObject("RBFBDigEnc"));
        if (e) {
            digEnc = e->unsigned32BitValue() != 0u;
            LOG("RBFBDigEnc -> %d", digEnc ? 1 : 0);
            dmubPublish();
            if (!dict->getObject("RBFBDigPhy")) { return kIOReturnSuccess; }
        }
    }

    {
        /* 2EA: the QUERY_DP_ALT sweep. The VALUE is the fill byte, so the
         * Linux-exact control (0) and the sentinel run (0xFF) are the same
         * code path with one byte different - a deviation that can be A/B'd
         * is a deviation that can be wrong. 0xFF is the default for a bare
         * ask because its null is the interpretable one. */
        OSNumber *q = OSDynamicCast(OSNumber, dict->getObject("RBFBDpAltQ"));
        if (q) {
            const uint32_t v = q->unsigned32BitValue();
            const uint32_t fill = (v == 0u) ? RB_DPALTQ_FILL_LINUX
                                            : RB_DPALTQ_FILL;
            const uint32_t rc = dmubDpAltQuery(fill);
            LOG("RBFBDpAltQ fill=0x%02x -> rc %u verdict %u usb 0x%02x",
                fill, rc, dpaltqVerdict, dpaltqUsbMask);
            dmubPublish();              /* 2E3: readable on return */
            return rc == 0u ? kIOReturnSuccess : kIOReturnNotPermitted;
        }
    }
    {
        /* 2FF: read BEFORE the send, in the same setProperties dictionary,
         * so one call carries both and no second round trip can land between
         * them (rule 34: a dial's state at the start is part of the arm). */
        OSNumber *hs = OSDynamicCast(OSNumber,
                                     dict->getObject("RBFBDigHpdSel"));
        if (hs) { digHpdSelWant = hs->unsigned32BitValue(); }

        OSNumber *w = OSDynamicCast(OSNumber, dict->getObject("RBFBDigPhy"));
        if (w) {
            digCalls++;
            const int32_t want = (int32_t)w->unsigned32BitValue();
            const uint32_t phyid = want < 0 ? RB_DIG_TYPEC_PHYID
                                            : (uint32_t)want;
            const uint32_t rc = dmubDigExperiment(phyid);
            LOG("RBFBDigPhy %u -> refuse %u, before %#x, after %#x, "
                "ref %#x, verdict %u, wptr %#x, acks %llu; enc %d "
                "refuse %u applied %u cfg %#x->%#x scram %#x->%#x; "
                "fe 0x%x inst %u refuse %u applied %u changed %u clean %u "
                "%#x->%#x",
                phyid, rc, digBefore, digAfter, digRefWord, digVerdict,
                dmubSendWptr, dmubAcks, digEnc ? 1 : 0, encRefuse,
                encApplied, encCfgBefore, encCfgAfter, encScramBefore,
                encScramAfter, digFe, feInst, feRefuse, feApplied,
                feChanged, feClean, feBefore, feAfter);
            dmubPublish();                       /* 2E3 */
            return rc == 0u ? kIOReturnSuccess : kIOReturnNotPermitted;
        }
    }

    /* 2FA: PROGRAM A SECOND RASTER AND START IT - optc1_program_timing plus
     * optc31_enable_crtc on the pipe whose DP DTO is already running.
     *
     * ALL THE KEYS IN ONE CALL, and `RBFBOtgRGo` is what triggers it, so an
     * armed boot nobody has run the tool on has written nothing - 2zn's
     * shape. The mode is REQUIRED in full rather than defaulted: a raster is
     * eight numbers and a default for any of them would be a timing nobody
     * chose, on a pipe whose clock is live. The gate then refuses unless the
     * mode multiplies out to that pipe's DP DTO PHASE.
     *
     * Rule 34: every field is read BEFORE the trigger, so the whole mode is
     * in place before the write it governs. A partial mode is refused by the
     * builder rather than completed from somewhere. */
    {
        struct rb_otgprog_mode mo;
        bzero(&mo, sizeof(mo));
        uint32_t refreshHz = 0;
        bool haveAll = true;
        struct { const char *k; uint32_t *v; } fields[] = {
            { "RBFBOtgRHActive",   &mo.h_addressable },
            { "RBFBOtgRHFront",    &mo.h_front_porch },
            { "RBFBOtgRHSync",     &mo.h_sync_width },
            { "RBFBOtgRHTotal",    &mo.h_total },
            { "RBFBOtgRVActive",   &mo.v_addressable },
            { "RBFBOtgRVFront",    &mo.v_front_porch },
            { "RBFBOtgRVSync",     &mo.v_sync_width },
            { "RBFBOtgRVTotal",    &mo.v_total },
            { "RBFBOtgRRefreshHz", &refreshHz },
        };
        for (unsigned f = 0; f < sizeof(fields) / sizeof(fields[0]); f++) {
            OSNumber *num = OSDynamicCast(OSNumber,
                                          dict->getObject(fields[f].k));
            if (num) { *fields[f].v = num->unsigned32BitValue(); }
            else { haveAll = false; }
        }
        /* The sync polarities default to POSITIVE, which is what 2BD read in
         * this sink's own EDID detailed timing and what 1080p60 is in every
         * standard. A default here is safe in a way a raster default is not:
         * it programs one bit, the sink reports a polarity mismatch as no
         * lock rather than as a wrong picture, and the value is published so
         * a reader sees what was used. */
        mo.h_sync_positive = 1u;
        mo.v_sync_positive = 1u;
        OSNumber *hp = OSDynamicCast(OSNumber,
                                     dict->getObject("RBFBOtgRHSyncPos"));
        if (hp) { mo.h_sync_positive = hp->unsigned32BitValue() ? 1u : 0u; }
        OSNumber *vp = OSDynamicCast(OSNumber,
                                     dict->getObject("RBFBOtgRVSyncPos"));
        if (vp) { mo.v_sync_positive = vp->unsigned32BitValue() ? 1u : 0u; }
        OSNumber *pipe = OSDynamicCast(OSNumber,
                                       dict->getObject("RBFBOtgRPipe"));
        if (pipe) { otgCallInst = pipe->unsigned32BitValue(); }

        OSNumber *go = OSDynamicCast(OSNumber, dict->getObject("RBFBOtgRGo"));
        if (go) {
            if (!haveAll) {
                /* A trigger with an incomplete mode is a REFUSAL with its own
                 * code, not a silent return and not a completed mode (rule
                 * 14). RB_OTGPROG_W_MODE is the same code the builder would
                 * give, which is the honest one: the mode is what is wrong. */
                otgProgCalls++;
                otgCallRefuse = RB_OTGPROG_W_MODE;
                LOG("RBFBOtgRGo: incomplete mode, refused %u",
                    otgCallRefuse);
                otgPublish();
                return kIOReturnNotPermitted;
            }
            const uint32_t rc = otgProgram(otgCallInst, refreshHz, &mo);
            LOG("RBFBOtgRGo pipe %u %ux%u total %ux%u refresh %u mode %u Hz "
                "dto %u live %#x -> refuse %u; last run: ctl %#x->%#x, "
                "frames %u->%u, writes %u, verdict %d, ran %u",
                otgCallInst, mo.h_addressable, mo.v_addressable,
                mo.h_total, mo.v_total, refreshHz, otgCallModeHz,
                otgCallDtoHz, otgCallLive, rc,
                otgBefore, otgAfter, otgFramesBefore, otgFramesAfter,
                otgProgWrites, otgProgVerdict, otgProgRan);
            otgPublish();
            return rc == RB_OTGPROG_W_OK ? kIOReturnSuccess
                                         : kIOReturnNotPermitted;
        }
        /* A mode or pipe named with no trigger is the dial being set up, and
         * the pipe is remembered for the call that follows. */
    }

    /* 2F1: PROGRAM A DARK PIPE'S DP PIXEL-CLOCK DTO - the three writes of
     * `dcn31_program_pix_clk`'s DP branch, which 2EE established is the path
     * on this chip for this signal and 2F0 confirmed the base for on hardware.
     *
     * ALL THREE KEYS IN ONE CALL, and the pixel clock is REQUIRED rather than
     * defaulted: `RBFBDtoPixHz` is what triggers the write, so an armed boot
     * that nobody has run the tool on has written nothing - 2zn's shape, and
     * the same reason `dmubbl` sends no backlight command until a level is
     * named. The pipe and SRC_SEL are read FIRST so both dials are in place
     * before the write they govern (rule 34; a two-call handover is one that
     * gets run with the first call quietly reverted).
     *
     * SRC_SEL defaults to 1 - Linux's value - and is a dial so the firmware's
     * 0 can be tried interleaved in the same boot (rule 44). 2F0 recorded the
     * divergence; rb_dpdto.h records why the driver path's value is the one
     * reproduced. */
    {
        OSNumber *pi = OSDynamicCast(OSNumber, dict->getObject("RBFBDtoPipe1"));
        if (pi) { dtoCallInst = pi->unsigned32BitValue(); }
        OSNumber *ss = OSDynamicCast(OSNumber,
                                     dict->getObject("RBFBDtoSrcSel"));
        if (ss) { dtoCallSrcSel = ss->unsigned32BitValue() ? 1u : 0u; }
        OSNumber *hz = OSDynamicCast(OSNumber, dict->getObject("RBFBDtoPixHz"));
        if (hz) {
            /* 2F2: the selector lives in the dtoCall* set, which dtoProgram
             * rewrites at entry either way - the dtoProg* set it used to
             * share is now the run record and must survive a refusal. */
            const uint32_t rc = dtoProgram(dtoCallInst,
                                           hz->unsigned32BitValue(),
                                           (int)dtoCallSrcSel);
            LOG("RBFBDtoPixHz pipe %u hz %u ref %u srcsel %u live %#x -> "
                "refuse %u; last run: pipe %u rate %#x->%#x, phase %u, "
                "modulo %u, applied %u changed %u clean %u, verdict %d, "
                "ran %u",
                dtoCallInst, dtoCallPixHz, dtoCallRefHz, dtoCallSrcSel,
                dtoCallLive, rc,
                dtoProgInst, dtoRateBefore, dtoRateAfter, dtoPhaseAfter,
                dtoModuloAfter, dtoProgApplied, dtoProgChanged, dtoProgClean,
                dtoProgVerdict, dtoProgRan);
            dtoPublish();
            return rc == RB_DPDTO_W_OK ? kIOReturnSuccess
                                       : kIOReturnNotPermitted;
        }
        /* A pipe or SRC_SEL named with no clock is the dial being set up for
         * the call that follows, so it publishes and succeeds rather than
         * falling through to an unrelated handler. */
        if (pi || ss) { dtoPublish(); return kIOReturnSuccess; }
    }

    /* 2A0: the frame-time instrument's own dial, ahead of RBFBPubMs because
     * it is the newer one and the two are independent. Setting it RESETS the
     * accumulators either way, including when the value is unchanged, so
     * `ftreset` before a window and `ftreset` after it bracket exactly that
     * window - a cumulative sum across an idle desktop is not a frame-path
     * measurement (rule 39's shape: say which window a reading comes from).
     * Turning it OFF is the control for rule 16: two mach_absolute_time()
     * reads per transaction is small, and "small" is a claim to be measured
     * like any other. */
    /* 2A2: the 2xo hold, switchable live.
     *
     * 2A1 measured it owning ~21 % of the transaction interval, which matches
     * the 27 % 2xo recorded when it was introduced - so this is a TRADE, not
     * a bug: the hold is what stopped the compositor drawing into the live
     * scanout, and 2xo's flicker is the cost of turning it off. It is a dial
     * and not a removal for exactly that reason.
     *
     * Runtime rather than the existing -rbgpufbnotready boot-arg, because an
     * A/B of this costs one reboot per side as an arg and nothing as a
     * property - the 2zm lesson that 2zz and 2A1 both cashed in. It also
     * means the recovery from the flicker is a command, not a power cycle.
     *
     * It can only ARM what the boot already set up: hubpArmed/hubpReady gate
     * hubpHoldNotReady() regardless, so a 1 here on an unarmed boot changes
     * nothing and the reader sees RBFBAmtrNotReady stay 0 (rule 38 - the two
     * halves check each other instead of assuming).
     *
     * Switching it also zeroes the frame-time window, because a budget
     * spanning both sides of this dial is a blend of two configurations and
     * would read as a third (rule 39). */
    {
        OSNumber *h = OSDynamicCast(OSNumber,
                                    dict->getObject("RBFBNotReady"));
        if (h) {
            const long want = (long)h->unsigned32BitValue();
            if (want != 0 && want != 1) { return kIOReturnBadArgument; }
            amtrNotReady = (want != 0);
            rb_ft_reset(&ftWork); rb_ft_reset(&ftGap);
            rb_ft_reset(&ftGet);  rb_ft_reset(&ftVbl);
            rb_ft_reset(&ftRetry); rb_ft_reset(&ftPoll);
            rb_ft_reset(&ftLatchWait); rb_ft_reset(&ftHoldRunAsks);
            rb_ft_reset(&ftPendWait); rb_ft_reset(&ftPendRunAsks);
            rb_ft_hist_reset(&ftPendHist);
            ftPendStart = 0; ftPendAsks = 0;
            ftPrevExit = 0; ftPrevGetExit = 0; ftPrevGetHeld = false;
            ftHoldStart = 0; ftHoldAsks = 0;
            ftResets++;
            publishVBLState();
            LOG("RBFBNotReady -> %d (2xo hold %s), frame-time window zeroed",
                amtrNotReady ? 1 : 0, amtrNotReady ? "ON" : "OFF");
            return kIOReturnSuccess;
        }
    }

    /* 2B1: the second pending flip slot, as a RUNTIME dial.
     *
     * A dial and not a boot-arg for 2zm's reason, which 2zz and 2A4 both
     * cashed in: this has to be A/B'd against the 2xo hold inside ONE boot,
     * because the thing it is supposed to buy is "hold off AND no flicker"
     * and the only instrument for the flicker is the user's eyes (rule 45).
     * Four combinations, no reboots.
     *
     * The write is verified from the hardware readback, and a failure to move
     * the bit returns an error rather than success - rule 20, and the reason
     * holdset learned to re-read the registry in 2A3.
     *
     * Zeroes the frame-time window for the same reason the hold does: a
     * budget spanning both sides of this is a blend of two configurations.
     */
    /* 2BA: the DPCD read trigger. Two keys, because WHICH connector and
     * WHICH address are separate decisions and encoding them in one number
     * would make a typo in either look like the other. RBFBAuxHpd selects the
     * HPD block (default 2, the Type-C port per 2B4) and only records the
     * choice; RBFBAuxRead carries the address and is what actually runs a
     * transaction - so an armed boot where nobody writes RBFBAuxRead has
     * performed none, which is the property the arming promises. */
    {
        OSNumber *h = OSDynamicCast(OSNumber, dict->getObject("RBFBAuxHpd"));
        if (h) {
            const uint32_t want = h->unsigned32BitValue();
            if (want >= RB_AUX_COUNT) { return kIOReturnBadArgument; }
            auxDpcdHpd = want;
            publishVBLState();
            return kIOReturnSuccess;
        }
    }
    /* 2F3: the DPCD WRITE trigger - the first write this project makes to the
     * SINK rather than to this chip.
     *
     * THREE keys, and the one that runs the transaction is the VALUE, so an
     * armed boot where nobody writes RBFBAuxWValue has written nothing to any
     * sink - the same property 2BA's arming promises for reads and 2F1's for
     * the DTO. The address is read first and has no writable default, so a
     * bare RBFBAuxWValue cannot land anywhere: it is refused ADDR_DENIED.
     *
     * The panel is refused by HPD *and* by engine inside the kext, and the
     * tool refuses HPD 0 again on its own - the outer of three checks. */
    {
        OSNumber *wh = OSDynamicCast(OSNumber, dict->getObject("RBFBAuxWHpd"));
        if (wh) { auxDpcdWHpd = wh->unsigned32BitValue(); }
        OSNumber *wa = OSDynamicCast(OSNumber,
                                     dict->getObject("RBFBAuxWAddr"));
        if (wa) { auxDpcdWAddr = wa->unsigned32BitValue(); }
        OSNumber *wv = OSDynamicCast(OSNumber,
                                     dict->getObject("RBFBAuxWValue"));
        if (wv) {
            const uint32_t rc = auxDpcdWrite(auxDpcdWHpd, auxDpcdWAddr,
                                             wv->unsigned32BitValue());
            LOG("RBFBAuxWValue hpd %u inst %u addr %#x val %#x -> refuse %u, "
                "reply %u acked %u, before %#x(ok %u) after %#x(ok %u), "
                "applied %u changed %u, verdict %d",
                auxDpcdWHpd, auxDpcdWInst, auxDpcdWAddr, auxDpcdWValue, rc,
                auxDpcdWReplyCode, auxDpcdWAcked,
                auxDpcdWBefore, auxDpcdWBeforeOk,
                auxDpcdWAfter, auxDpcdWAfterOk,
                auxDpcdWApplied, auxDpcdWChanged, auxDpcdWVerdict);
            auxWPublish();
            return rc == RB_DPCD_W_OK ? kIOReturnSuccess
                                      : kIOReturnNotPermitted;
        }
        if (wh || wa) { auxWPublish(); return kIOReturnSuccess; }
    }

    /* 2F4: the training-pattern trigger - the PHY side.
     *
     * FOUR keys, and the one that runs it is RBFBDpTxGo, so an armed boot
     * where nobody writes RBFBDpTxGo has put no pattern on any wire. The
     * instance defaults to 2 (DIG2, the Type-C port) and the panel's 0 is
     * refused by name in the kext and again in the tool.
     *
     * The lane count is a dial rather than a constant because it is the SINK's
     * property - 2BB read two lanes out of this one's DPCD - and a different
     * sink would have a different answer. It feeds rb_dptx_cr_done(), so
     * getting it wrong would make a 1-lane CR read as a 2-lane failure. */
    {
        OSNumber *ti = OSDynamicCast(OSNumber,
                                     dict->getObject("RBFBDpTxInst"));
        if (ti) { dptxInst = ti->unsigned32BitValue(); }
        OSNumber *tp = OSDynamicCast(OSNumber, dict->getObject("RBFBDpTxTps"));
        if (tp) { dptxTps = tp->unsigned32BitValue(); }
        OSNumber *tl = OSDynamicCast(OSNumber,
                                     dict->getObject("RBFBDpTxLanes"));
        if (tl) { dptxLanes = tl->unsigned32BitValue(); }
        OSNumber *tg = OSDynamicCast(OSNumber, dict->getObject("RBFBDpTxGo"));
        if (tg) {
            const uint32_t hpd = tg->unsigned32BitValue();
            const uint32_t rc = dptxTrain(dptxInst, dptxTps, hpd, dptxLanes);
            LOG("RBFBDpTxGo inst %u tps %u hpd %u lanes %u -> refuse %u; "
                "tp %#x->%#x lc %#x->%#x dc %#x->%#x pc %#x->%#x "
                "applied %u clean %u; sink %#x(ok %u) -> %#x(ok %u) "
                "dpcdset %u dpcdrefuse %u, verdict %d",
                dptxInst, dptxTps, hpd, dptxLanes, rc,
                dptxTpBefore, dptxTpAfter, dptxLcBefore, dptxLcAfter,
                dptxDcBefore, dptxDcAfter, dptxPcBefore, dptxPcAfter,
                dptxApplied, dptxClean,
                dptxSinkBefore, dptxSinkBeforeOk,
                dptxSinkAfter, dptxSinkAfterOk,
                dptxDpcdSet, dptxDpcdRefuse, dptxVerdict);
            dptxPublish();
            return rc == RB_DPTX_W_OK ? kIOReturnSuccess
                                      : kIOReturnNotPermitted;
        }
        if (ti || tp || tl) { dptxPublish(); return kIOReturnSuccess; }
    }

    /* 2BC: the EDID trigger. Its own key, because an EDID read is a different
     * transaction type (I2C-over-AUX with MOT) and a shared key would make
     * "which protocol ran" unreadable afterwards. */
    {
        OSNumber *e = OSDynamicCast(OSNumber, dict->getObject("RBFBEdidRead"));
        if (e) {
            if (!auxDpcdArmed) { return kIOReturnNotReady; }
            const uint32_t blk = e->unsigned32BitValue();
            if (blk > 1) { return kIOReturnBadArgument; }
            const bool ok = auxEdidRead(auxDpcdHpd, blk);
            publishVBLState();
            return ok ? kIOReturnSuccess : kIOReturnIOError;
        }
    }
    {
        OSNumber *a = OSDynamicCast(OSNumber, dict->getObject("RBFBAuxRead"));
        if (a) {
            if (!auxDpcdArmed) { return kIOReturnNotReady; }
            const uint32_t addr = a->unsigned32BitValue();
            OSNumber *l = OSDynamicCast(OSNumber,
                                        dict->getObject("RBFBAuxLen"));
            uint32_t len = l ? l->unsigned32BitValue() : RB_DPCD_MAX_READ;
            if (len == 0 || len > RB_DPCD_MAX_READ) {
                return kIOReturnBadArgument;
            }
            const bool ok = auxDpcdRead(auxDpcdHpd, addr, len);
            publishVBLState();
            /* Rule 13/20: the return code says the setter ran. The verdict is
             * the published reply, which the tool reads back. An IOError here
             * means the transaction did not ACK with data - the refusal code
             * says why. */
            return ok ? kIOReturnSuccess : kIOReturnIOError;
        }
    }
    {
        OSNumber *t = OSDynamicCast(OSNumber,
                                    dict->getObject("RBFBTriple"));
        if (t) {
            const long want = (long)t->unsigned32BitValue();
            if (want != 0 && want != 1) { return kIOReturnBadArgument; }
            if (!hubpReady || !hubpRegs) {
                LOG("RBFBTriple refused: HUBP not armed this boot");
                return kIOReturnNotReady;
            }
            const bool ok = hubpTripleApply(want != 0);
            rb_ft_reset(&ftWork); rb_ft_reset(&ftGap);
            rb_ft_reset(&ftGet);  rb_ft_reset(&ftVbl);
            rb_ft_reset(&ftRetry); rb_ft_reset(&ftPoll);
            rb_ft_reset(&ftLatchWait); rb_ft_reset(&ftHoldRunAsks);
            rb_ft_reset(&ftPendWait); rb_ft_reset(&ftPendRunAsks);
            rb_ft_hist_reset(&ftPendHist);
            ftPendStart = 0; ftPendAsks = 0;
            ftPrevExit = 0; ftPrevGetExit = 0; ftPrevGetHeld = false;
            ftHoldStart = 0; ftHoldAsks = 0;
            ftResets++;
            publishVBLState();
            LOG("RBFBTriple -> %ld (readback 0x%x, %s), window zeroed",
                want, (unsigned)hubpTripleReadback,
                ok ? "took" : "DID NOT TAKE");
            return ok ? kIOReturnSuccess : kIOReturnIOError;
        }
    }
    {
        OSNumber *f = OSDynamicCast(OSNumber, dict->getObject("RBFBFtOn"));
        if (f) {
            const long want = (long)f->unsigned32BitValue();
            if (want != 0 && want != 1) { return kIOReturnBadArgument; }
            ftOn = (want != 0);
            rb_ft_reset(&ftWork);
            rb_ft_reset(&ftGap);
            rb_ft_reset(&ftGet);
            rb_ft_reset(&ftVbl);
            rb_ft_reset(&ftRetry);
            rb_ft_reset(&ftPoll);
            rb_ft_reset(&ftLatchWait);
            rb_ft_reset(&ftHoldRunAsks);
            rb_ft_reset(&ftPendWait);
            rb_ft_reset(&ftPendRunAsks);
            rb_ft_hist_reset(&ftPendHist);
            ftPendStart = 0;
            ftPendAsks = 0;
            ftPrevExit = 0;
            ftPrevGetExit = 0;
            ftPrevGetHeld = false;
            ftHoldStart = 0;
            ftHoldAsks = 0;
            ftResets++;
            publishFrameTime();
            LOG("RBFBFtOn -> %d, accumulators reset (%llu)",
                ftOn ? 1 : 0, (unsigned long long)ftResets);
            return kIOReturnSuccess;
        }
    }

    OSNumber *n = OSDynamicCast(OSNumber, dict->getObject("RBFBPubMs"));
    if (!n) {
        pubSetsOther++;
        return kIOReturnUnsupported;
    }

    /* Signed on purpose: a value that arrived negative must be REFUSED, not
     * wrapped into an interval of half an eternity. rb_pub_ms_ok() takes a
     * long and the hosttest asserts the -1 case. */
    const long want = (long)n->unsigned32BitValue();
    pubMsLast = want;
    if (!rb_pub_ms_ok(want)) {
        pubSetsRefused++;
        return kIOReturnBadArgument;
    }

    pubMs = (uint32_t)want;
    pubIntervalAbs = 0;
    if (pubMs != 0) {
        nanoseconds_to_absolutetime((uint64_t)pubMs * 1000000ull,
                                    &pubIntervalAbs);
    }
    pubNextAbs = 0;                  /* publish immediately, see above */
    pubSetsOk++;
    LOG("RBFBPubMs set live to %u ms", pubMs);
    return kIOReturnSuccess;
}
#endif /* RBGPU_FB_VBL_HOSTTEST */
