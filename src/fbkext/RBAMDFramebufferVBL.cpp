/* SPDX-License-Identifier: MIT */
/* Timer-backed VBL callbacks, shared with the host regression executable. */
#ifdef RBGPU_FB_VBL_HOSTTEST
#include "fb_vbl_shim.hpp"
#else
#include "RBAMDFramebuffer.hpp"
#include <IOKit/IOLib.h>
#include <kern/clock.h>
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
        return;
    }
    vblRunning = true;
    uint64_t deadline = 0;
    clock_interval_to_deadline((uint32_t)kVBLPeriodNs, 1, &deadline);
    thread_call_enter_delayed(vblCall, deadline);
}

void RBAMDFramebuffer::deliverVBL(void)
{
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
    if ((vblFires % 1200) == 0) {
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
        if (!vblRunning) {
            armVBL();
        }
        publishVBLState();
        LOG("registered 'vbl ' handler %u (period %llu ns)", i, kVBLPeriodNs);
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
                publishVBLState();
            }
            return kIOReturnSuccess;
        }
    }
    return kIOReturnBadArgument;
}
