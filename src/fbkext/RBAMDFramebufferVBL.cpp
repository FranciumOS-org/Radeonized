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
    setProperty("RBFBHubpOtg1", hubpOtgCtl1, 32);
    setProperty("RBFBHubpOtg2", hubpOtgCtl2, 32);
    setProperty("RBFBHubpOtg3", hubpOtgCtl3, 32);
    setProperty("RBFBHubpOtgAnyArmed", hubpOtgAny);
    setProperty("RBFBAmtrNotReadyArmed", amtrNotReady);
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
