/* SPDX-License-Identifier: MIT */
/* 2wh: the first write this project makes to the display engine.
 *
 * Everything that DECIDES anything lives in src/common/rb_hubp.h and is run
 * by the hosttest before this file is ever compiled into a kext. The long
 * reasoning - what the write does on the chip, why only the address changes,
 * and why the register takes an MC address - is the header comment there.
 * This file is the mechanism: map BAR5, prove we are addressing the pipe 2os
 * measured, read the original pair, and on each transaction write one dword.
 *
 * The write happens in setAttribute('amtr'), which runs on the accelerator's
 * thread with its locks held, so this path MUST NOT ALLOCATE, log, or block
 * (rule 31). The mapping is therefore done once in start(); setAttribute only
 * does two volatile stores and a read-back. Everything is published from the
 * VBL thread_call, because our IOLog is invisible (rule 31).
 *
 * WHY PER TRANSACTION AND NOT ONCE: boot 194 measured 17 transactions, and a
 * compositor swap chain hands a different surface each flip. Writing one
 * address once would show exactly one stale frame. 2wg said this before the
 * write existed.
 */

#include "RBAMDFramebuffer.hpp"
#include "rb_hubp.h"
#include "rb_hpd.h"
#include "rb_aux.h"
#include "rb_dpcd.h"
#include "rb_dpalt.h"
#include "rb_dpdto.h"
#include "rb_drr.h"
#include "rb_dcn_deny.h"
#include "rb_regs.h"
#include <IOKit/pci/IOPCIDevice.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IOLib.h>

/* DMU segment 2, measured from this machine's own discovery table and
 * re-derived here the same way rbgpu_agdc_otg.cpp does. The segment is a
 * constant in this kext, and a constant is an assumption, so it is CHECKED
 * below against 2oq's four-register OTG signature before anything is
 * believed - and then again against 2os's three HUBP0 values. */
static const uint32_t kRbDmuSeg2 = 0x34c0;

/* 2oq measured OTG0_CONTROL = 0x00011201 (OTG_MASTER_EN set) with OTG1-3 at
 * exactly 0x300. The OTG dwords come from rb_regs.h's own table. */
#define RB_OTG_MASTER_EN  0x1u
#define RB_OTG_OFF_VALUE  0x300u
static const uint32_t kRbOtgControl[4] = { 0x1b41, 0x1bc1, 0x1c41, 0x1cc1 };

/* The HUBP this project writes. 2os measured HUBP0 as the live pipe - it is
 * the one whose primary surface address reads the carve-out base - and the
 * state gate re-proves that every boot rather than trusting it. */
#define RB_HUBP_INDEX  0u

static inline uint32_t rbRead(const volatile uint32_t *regs, uint32_t dw)
{
    return regs[dw];
}

bool RBAMDFramebuffer::hubpSetup(void)
{
    hubpRefuse = 0;
    hubpWrites = 0;
    hubpRejects = 0;
    hubpIndex = RB_HUBP_INDEX;
    hubpOrig = 0;
    hubpLast = 0;
    hubpReadback = 0;
    hubpFlipCtl = 0;
    hubpPendCtl = 0;
    hubpTripleArmed = false;
    hubpTripleOrig = 0;
    hubpTripleHaveOrig = false;
    hubpTripleWrites = 0;
    hubpTripleRefused = 0;
    hubpTripleRefusedVal = 0;
    hubpTripleReadback = 0;
    hpdReady = false;
    dpaltReady = false;
    dtoReady = false;
    dtoExpectedHz = 0;
    dtoRefSrc = RB_DPDTO_REF_SRC_NONE;
    dtoHTotal = 0;
    dtoVTotal = 0;
    dtoRefreshMhz = 0;
    dtoRefHz = 0;
    dtoBaseVerdict = RB_DPDTO_BASE_NOT_SAMPLED;
    dtoPipeVerdict = RB_DPDTO_PIPE_NOT_SAMPLED;
    /* 2F1: the write side. The verdict starts at NOT_RUN so a boot that never
     * programmed anything cannot be read as one that did and found nothing
     * (rule 14), and the refusal starts at OK only because dtoProgCalls 0 is
     * what separates "never called" from "called and allowed". */
    dtoCallInst = RB_DPDTO_FIRST_FREE_PIPE;
    dtoCallPixHz = 0;
    dtoCallSrcSel = 1u;
    dtoCallRefHz = 0;
    dtoCallLive = 0;
    dtoProgInst = RB_DPDTO_FIRST_FREE_PIPE;
    dtoProgPixHz = 0;
    dtoProgRefHz = 0;
    dtoProgSrcSel = 1u;   /* Linux's value; see rb_dpdto.h */
    dtoProgRefuse = RB_DPDTO_W_OK;
    dtoProgApplied = 0;
    dtoProgChanged = 0;
    dtoProgClean = 0;
    dtoRateBefore = 0;
    dtoRateAfter = 0;
    dtoPhaseAfter = 0;
    dtoModuloAfter = 0;
    dtoProgCalls = 0;
    dtoProgRan = 0;
    dtoProgVerdict = RB_DPDTO_PROG_NOT_RUN;
    for (unsigned i = 0; i < RB_DPDTO_COUNT; i++) {
        dtoAlias[i] = 0;
        for (unsigned r = 0; r < RB_DPDTO_REG_COUNT; r++) {
            dtoWord[i][r] = 0;
        }
    }
    hpdSinkMask = 0;
    hpdSinkEver = 0;
    hpdEnMask = 0;
    hpdSetupMask = 0;
    hpdSetupEnMask = 0;
    hpdChanges = 0;
    for (unsigned i = 0; i < RB_HPD_COUNT; i++) {
        hpdStatus[i] = 0;
        hpdControl[i] = 0;
    }
    for (unsigned i = 0; i < RB_AUX_COUNT; i++) {
        auxControl[i] = 0; auxDphy[i] = 0;
        auxStatus[i] = 0;  auxArb[i] = 0;
    }
    auxEnMask = 0;
    auxMatchMask = 0;
    auxDpcdAttempts = 0;
    auxDpcdOks = 0;
    auxDpcdRefuse = 0;
    auxDpcdInst = RB_AUX_COUNT;
    auxDpcdArbSeen = 0;
    auxDpcdStatus = 0;
    auxDpcdReplyCode = 0xff;
    auxDpcdBytes = 0;
    auxDpcdSuspect = false;
    auxDpcdHpd = 2;   /* 2B4: the Type-C port */
    /* 2F4: the transmitter. dptxReady stays false until a sample succeeds, so
     * every published word is either measured or declared absent (rule 14). */
    dptxReady = false;
    for (unsigned i = 0; i < RB_DPTX_COUNT; i++) {
        for (unsigned r = 0; r < RB_DPTX_REG_COUNT; r++) {
            dptxWord[i][r] = 0;
        }
    }
    dptxCalls = 0;
    dptxRan = 0;
    dptxRefuse = RB_DPTX_W_OK;
    dptxInst = 2;      /* DIG2, the Type-C port's - never 0, the panel's */
    dptxTps = RB_DPTX_TPS1;
    dptxLanes = 2;     /* 2BB read this sink's DPCD: two lanes */
    dptxTpBefore = 0; dptxTpAfter = 0;
    dptxLcBefore = 0; dptxLcAfter = 0;
    dptxDcBefore = 0; dptxDcAfter = 0;
    dptxPcBefore = 0; dptxPcAfter = 0;
    dptxApplied = 0;
    dptxClean = 0;
    dptxSinkBefore = 0xffffffffu; dptxSinkBeforeOk = 0;
    dptxSinkAfter = 0xffffffffu; dptxSinkAfterOk = 0;
    dptxMoved = 0;
    dptxObservable = 0;
    dptxDpcdRefuse = 0;
    dptxDpcdSet = 0;
    dptxVerdict = RB_DPTX_V_NOT_RUN;
    /* 2F3: the write's state. The address defaults to NOTHING writable, so a
     * bare arm cannot write: the dial has to name an address. */
    auxDpcdWCalls = 0;
    auxDpcdWOks = 0;
    auxDpcdWRefuse = RB_DPCD_W_OK;
    auxDpcdWHpd = 2;
    auxDpcdWInst = RB_AUX_COUNT;
    auxDpcdWAddr = 0;
    auxDpcdWValue = 0;
    auxDpcdWBefore = 0xffffffffu;
    auxDpcdWBeforeOk = 0;
    auxDpcdWAfter = 0xffffffffu;
    auxDpcdWAfterOk = 0;
    auxDpcdWReplyCode = 0xff;
    auxDpcdWAcked = 0;
    auxDpcdWApplied = 0;
    auxDpcdWChanged = 0;
    auxDpcdWVerdict = RB_DPCD_W_V_NOT_RUN;
    auxEdidAttempts = 0;
    auxEdidOks = 0;
    auxEdidRefuse = 0;
    auxEdidInst = RB_AUX_COUNT;
    auxEdidBytes = 0;
    auxEdidDefers = 0;
    auxEdidHeaderOk = false;
    auxEdidSumOk = false;
    for (unsigned i = 0; i < 128; i++) { auxEdidData[i] = 0; }
    for (unsigned i = 0; i < 16; i++) { auxDpcdData[i] = 0; }
    for (unsigned i = 0; i < 4; i++) { otgLive[i] = 0; }
    hubpFlipCtl2 = 0;
    hubpPendChecks = 0;
    hubpPendHits = 0;
    rb_hubp_latch_reset(&hubpLatch);
    hubpDeferrals = 0;
    hubpFlipDeadline = 0;
    hubpOtgCtl1 = 0;
    hubpOtgCtl2 = 0;
    hubpOtgCtl3 = 0;
    hubpPitch = 0;
    hubpConfig = 0;
    hubpOtgCtl = 0;
    hubpAddrCfg = 0;
    hubpTiling = 0;
    hubpRegs = nullptr;
    hubpMap = nullptr;
    hubpReady = false;

    if (!hubpArmed) { return false; }
    if (!pci) { hubpRefuse = 2; return false; }

    /* Read-WRITE this time, which is the whole difference from 2rm's sampler.
     * Cache-inhibited because these are registers. No change to the PCI
     * command register: memory decoding is already on - the firmware lit the
     * panel through this very aperture - and the probe rules forbid us
     * touching decoding or bus mastering. */
    IOMemoryMap *map = pci->mapDeviceMemoryWithRegister(kIOPCIConfigBaseAddress5,
                                                        kIOMapInhibitCache);
    if (!map) { hubpRefuse = 3; return false; }

    const uint64_t length = map->getLength();
    uint32_t needed = kRbDmuSeg2 +
        rb_hubp_dw(hubpIndex, RB_HUBP_REG_ADDR_HI);
    /* 2B3: the HPD blocks sit ABOVE everything else this kext reads
     * (0x1f16+32 against OTG's 0x1cc1), so the bound has to grow with them or
     * the first HPD read is outside what was checked. The check stays one
     * comparison against the HIGHEST dword, rather than becoming a check per
     * block that a later addition can forget. */
    const uint32_t hpdNeeded = kRbDmuSeg2 + rb_hpd_dw_max();
    if (hpdNeeded > needed) { needed = hpdNeeded; }
    /* 2B8: and the AUX engines sit above even those (0x1fcb against HPD's
     * 0x1f36), so the same one comparison grows again rather than a new
     * check appearing beside it. */
    const uint32_t auxNeeded = kRbDmuSeg2 + rb_aux_dw_max();
    if (auxNeeded > needed) { needed = auxNeeded; }
    /* 2C0: the RDPCSTX PHY blocks sit far above all of those - 0x2ca6
     * against the AUX engines' 0x1fcb - and they are DELIBERATELY NOT folded
     * into `needed`.
     *
     * Folding them in would make every boot's panel depend on an aperture
     * large enough for a block only an opt-in read ever touches: a boot that
     * does not arm -rbgpufbdpalt would gain a stricter check for no benefit,
     * and if it ever failed, hubpSetup() would refuse and the panel would go
     * with it. So the core bound is unchanged and the PHY read gets its own,
     * checked separately and failing only itself (rule 16: the instrument
     * must not be able to break what it is measured beside). */
    const uint32_t dpaltNeeded = kRbDmuSeg2 + rb_dpalt_dw_max();
    const bool dpaltFits =
        length >= ((uint64_t)dpaltNeeded + 1) * sizeof(uint32_t);
    /* Every dword we will touch has to be inside the aperture, checked before
     * the first dereference and not after (rule 19's shape for a mapping). */
    if (length < ((uint64_t)needed + 1) * sizeof(uint32_t)) {
        hubpRefuse = 4;
        map->release();
        return false;
    }

    const volatile uint32_t *regs =
        reinterpret_cast<const volatile uint32_t *>(map->getVirtualAddress());
    if (!regs) { hubpRefuse = 3; map->release(); return false; }

    /* Armed AND the aperture covers it. Both halves are required and both are
     * published, so "no reading" can be told from "refused" (rule 14).
     *
     * 2E2: `digArmed` arms the same block, because the DIG1 transmitter
     * experiment's PRODUCT is RDPCSTX_PHY_CNTL6. It is ORed in rather than
     * left as a second boot-arg the user has to remember: an experiment that
     * needs two tokens in the right combination is one that will be run with
     * one of them missing, and its refusal would then look like a chip
     * answer (rule 34, the same reason dmubDigExperiment is a single call).
     * The aperture half is unchanged and still fails only itself. */
    /* 2EC: `feArmed` ORs in too - the DIG_BE_CNTL dwords this write
     * touches top out at 0x24b1, inside the same bound. */
    dpaltReady = (dpaltArmed || digArmed || feArmed) && dpaltFits;

    /* 2EF: the DP DTO's own bound, separate for the same reason 2C0 kept the
     * PHY blocks out of `needed`: an opt-in read must not be able to refuse
     * hubpSetup() and take the panel with it (rule 16).
     *
     * SAY WHAT THIS CHECK CAN AND CANNOT DO. BASE_IDX 1 is 0xc0, so the
     * highest dword here is 0xc0 + 0x8f = 0x14f - FAR BELOW the 0x34c0-based
     * dwords `needed` already bounds, and BAR5 is 512 KB (0x20000 dwords).
     * At today's aperture this comparison cannot fail, and a check that
     * cannot fail is not a check (rule 48). It is here anyway, and labelled,
     * because it is an assertion about the APERTURE rather than about our
     * arithmetic: `length` is read from the live BAR at runtime, and the one
     * thing that must never happen is a dereference outside it (rule 19).
     * What it is NOT is evidence the base is right - that comes from the
     * chip, below. */
    {
        const uint32_t dtoNeeded = RB_DPDTO_SEG1 + rb_dpdto_dw_max();
        const uint32_t aliasNeeded = RB_DPDTO_SEG2_CONTROL + rb_dpdto_dw_max();
        const uint32_t hi = dtoNeeded > aliasNeeded ? dtoNeeded : aliasNeeded;
        dtoReady = dtoArmed &&
                   length >= ((uint64_t)hi + 1) * sizeof(uint32_t);
    }

    /* Gate 1: are we addressing DCN at all? 2oq's signature, now decided by
     * rb_hubp_otg_gate() so the hosttest owns it and the HDMI case is
     * explicit. 2xp: all four are read UNCONDITIONALLY and published even
     * when the gate refuses - the old loop short-circuited, so a boot that
     * refused with code 5 could not say WHICH OTG was live, which is exactly
     * the answer 2or's cheap HDMI test is for (rule 14: a refusal must
     * publish what it refused on). */
    hubpOtgCtl  = rbRead(regs, kRbDmuSeg2 + kRbOtgControl[0]);
    hubpOtgCtl1 = rbRead(regs, kRbDmuSeg2 + kRbOtgControl[1]);
    hubpOtgCtl2 = rbRead(regs, kRbDmuSeg2 + kRbOtgControl[2]);
    hubpOtgCtl3 = rbRead(regs, kRbDmuSeg2 + kRbOtgControl[3]);
    if (!rb_hubp_otg_gate(hubpOtgCtl, hubpOtgCtl1, hubpOtgCtl2, hubpOtgCtl3,
                          hubpOtgAny ? 1 : 0)) {
        hubpRefuse = 5;
        map->release();
        return false;
    }

    /* Gate 2: is the pipe the one 2os measured? Pitch, format and the high
     * dword of the address, all three, decided by the pure function the
     * hosttest runs. */
    hubpPitch  = rbRead(regs, kRbDmuSeg2 + rb_hubp_dw(hubpIndex, RB_HUBP_REG_PITCH));
    hubpConfig = rbRead(regs, kRbDmuSeg2 + rb_hubp_dw(hubpIndex, RB_HUBP_REG_CONFIG));
    const uint32_t lo = rbRead(regs, kRbDmuSeg2 + rb_hubp_dw(hubpIndex, RB_HUBP_REG_ADDR));
    const uint32_t hi = rbRead(regs, kRbDmuSeg2 + rb_hubp_dw(hubpIndex, RB_HUBP_REG_ADDR_HI));
    hubpOrig = ((uint64_t)hi << 32) | lo;

    /* Read-only, and NOT a gate: nothing measured says what these should be,
     * so refusing on them would be substituting a value (rule 17). They are
     * published so that a scrambled image is distinguishable from a wrong
     * address, which is the one ambiguity this experiment cannot otherwise
     * resolve. */
    hubpAddrCfg = rbRead(regs, kRbDmuSeg2 + rb_hubp_dw(hubpIndex, RB_HUBP_REG_ADDR_CFG));
    hubpTiling  = rbRead(regs, kRbDmuSeg2 + rb_hubp_dw(hubpIndex, RB_HUBP_REG_TILING));
    /* 2xj: and the two the tearing question turns on. Same standing as the two
     * above - read, published, never written and never a gate. */
    hubpFlipCtl  = rbRead(regs, kRbDmuSeg2 + rb_hubp_dw(hubpIndex, RB_HUBP_REG_FLIP_CTL));
    hubpFlipCtl2 = rbRead(regs, kRbDmuSeg2 + rb_hubp_dw(hubpIndex, RB_HUBP_REG_FLIP_CTL2));

    const int state = rb_hubp_check_state(hubpPitch, hubpConfig, hi);
    if (state != RB_HUBP_OK) {
        hubpRefuse = (uint32_t)state;
        map->release();
        return false;
    }

    /* Gate 3: the project's own DCN write policy, which exists because a
     * base-2 dword can mean different things in dcn302 and dcn312. Both our
     * dwords are allowed, asserted by the hosttest - this re-checks it on the
     * machine so the policy cannot be bypassed by a future edit here. */
    if (rb_dcn_is_denied(rb_hubp_dw(hubpIndex, RB_HUBP_REG_ADDR)) ||
        rb_dcn_is_denied(rb_hubp_dw(hubpIndex, RB_HUBP_REG_ADDR_HI))) {
        hubpRefuse = 9;
        map->release();
        return false;
    }

    hubpMap = map;
    hubpRegs = reinterpret_cast<volatile uint32_t *>(map->getVirtualAddress());
    hubpReady = true;

    /* 2B3: the HPD read rides on this mapping and this bound check, so it is
     * armed by the same success and by nothing of its own. The setup sample is
     * kept separately from the live one because WHICH CABLE STATE it was taken
     * in is a fact about the boot (rule 33) - the usable reading is the
     * difference between two samples, not either one. */
    hpdReady = true;
    hpdSample();
    auxSample();
    dpaltSample();
    hpdSetupMask   = hpdSinkMask;
    hpdSetupEnMask = hpdEnMask;
    hpdChanges     = 0;

    return true;
}

void RBAMDFramebuffer::hubpTeardown(void)
{
    /* Put the pipe back where the firmware had it. This runs from stop(), so
     * it is best-effort: a reboot restores it anyway, and the boot-arg is the
     * real disarm (rule 9). Restoring is still worth doing, because the one
     * thing worse than a wrong image is a wrong image that survives an
     * unload. */
    if (hubpReady && hubpRegs && hubpOrig) {
        hubpRegs[kRbDmuSeg2 + rb_hubp_dw(hubpIndex, RB_HUBP_REG_ADDR_HI)] =
            (uint32_t)(hubpOrig >> 32);
        hubpRegs[kRbDmuSeg2 + rb_hubp_dw(hubpIndex, RB_HUBP_REG_ADDR)] =
            (uint32_t)hubpOrig;
    }
    hubpReady = false;
    hubpRegs = nullptr;
    if (hubpMap) { hubpMap->release(); hubpMap = nullptr; }
}

void RBAMDFramebuffer::hubpProgram(uint64_t mc)
{
    if (!hubpReady || !hubpRegs) { return; }

    /* Rule 14: a refusal publishes what it refused and why, and never just
     * returns. hubpRejects counts them; hubpRejectAddr/hubpRejectWhy keep the
     * first one, because a last-wins key cannot express a per-entry fact and
     * the FIRST refusal is the one with a clean cause. */
    const int v = rb_hubp_check_addr(mc, (uint32_t)(hubpOrig >> 32),
                                     (uint64_t)1920 * 1080 * 4);
    if (v != RB_HUBP_OK) {
        if (!hubpRejects) {
            hubpRejectAddr = mc;
            hubpRejectWhy = (uint32_t)v;
        }
        hubpRejects++;
        return;
    }

    if (hubpMax && hubpWrites >= hubpMax) { return; }

    /* 2xk: THE INSTRUMENT, NOT THE FIX (rule 35). 2xj eliminated the flip
     * type by reading it; this counts the next candidate instead of acting on
     * it. SURFACE_FLIP_PENDING set here means DCN still has the PREVIOUS
     * address armed and unlatched, and with one pending slot (triple
     * buffering reads 0) programming over it discards that flip. A read only
     * - this function runs on the accelerator's thread with its locks held,
     * so it allocates nothing and logs nothing (rule 31), and it already
     * performs a register read on write #1. The write below is UNCHANGED:
     * nothing is skipped, deferred or reordered, so this build cannot alter
     * what the panel shows. */
    const uint32_t fc = hubpRegs[kRbDmuSeg2 +
                                 rb_hubp_dw(hubpIndex, RB_HUBP_REG_FLIP_CTL)];
    hubpPendChecks++;
    if (rb_hubp_flip_pending(fc)) { hubpPendHits++; }

    /* 2xm: THE FIX, and it only runs with -rbgpufbflippend. Boot 217 turned
     * 2xk's counter into a measurement - 593 of 33 877 flips, 1.75 %, were
     * programmed over an armed and unlatched one - so the write below is what
     * replaces a frame partway down the panel. Deferring costs that frame:
     * the PREVIOUS address latches instead, and because setAttribute('amtr')
     * already answers success instantly (2we) the compositor is not waiting
     * on us either way. So the trade is one stale frame per deferral against
     * one torn frame, at 1.26 deferrals a second.
     *
     * The decision is rb_hubp_flip_would_discard() in src/common/rb_hubp.h,
     * which the hosttest runs first, and FLIP_CONTROL2 is read live rather
     * than reused from hubpSetup() because triple buffering is the compositor
     * 's to change at any time. Separate arg from -rbgpufbhubp on purpose: if
     * deferring makes things worse, one token reverses it without giving up
     * the write that made the display work. */
    if (hubpDeferPend) {
        const uint32_t fc2 = hubpRegs[kRbDmuSeg2 +
                                      rb_hubp_dw(hubpIndex,
                                                 RB_HUBP_REG_FLIP_CTL2)];
        if (rb_hubp_flip_would_discard(fc, fc2)) {
            hubpDeferrals++;
            return;
        }
    }

    /* HIGH first, then LOW. DCN latches the pair when the LOW dword is
     * written, so this order means the hardware never sees a half-updated
     * address. On this machine HIGH does not change - every carve-out address
     * shares 0xf4 - so the first store is a no-op that exists to keep the
     * order correct if that ever stops being true. */
    hubpRegs[kRbDmuSeg2 + rb_hubp_dw(hubpIndex, RB_HUBP_REG_ADDR_HI)] =
        (uint32_t)(mc >> 32);
    hubpRegs[kRbDmuSeg2 + rb_hubp_dw(hubpIndex, RB_HUBP_REG_ADDR)] =
        (uint32_t)mc;

    /* 2xn: the window in which getAttribute('amtr') may answer NotReady.
     * Two periods, because the address latches at the NEXT VUPDATE (2xk read
     * SURFACE_FLIP_TYPE = 0) and one period of slack costs nothing while a
     * tighter bound would expire before a legitimately pending flip latched.
     * Set AFTER the stores, so a refused or deferred transaction never opens
     * a hold - and if this is ever not reached, the deadline stays at 0,
     * which rb_hubp_hold_notready() treats as already expired (asserted). */
    {
        uint64_t two = 0;
        nanoseconds_to_absolutetime((vblPeriodNs ? vblPeriodNs : kVBLPeriodNsFast) * 2, &two);
        hubpFlipDeadline = mach_absolute_time() + two;
    }

    hubpWrites++;
    /* 2za: the write is out; start its latch clock. Placed with the counter
     * that records the write actually happened, so an armed latch always
     * corresponds to a programmed flip and never to a refused one. */
    rb_hubp_latch_write(&hubpLatch);
    hubpLast = mc;

    /* Read the pair back once, on the first write only. A register that reads
     * back what we wrote is the difference between "the store executed" and
     * "the store reached the chip" - 2lq's lesson in its register form. Only
     * once, because this is the accelerator's thread. */
    if (hubpWrites == 1) {
        const uint32_t rlo = hubpRegs[kRbDmuSeg2 + rb_hubp_dw(hubpIndex, RB_HUBP_REG_ADDR)];
        const uint32_t rhi = hubpRegs[kRbDmuSeg2 + rb_hubp_dw(hubpIndex, RB_HUBP_REG_ADDR_HI)];
        hubpReadback = ((uint64_t)rhi << 32) | rlo;
    }
}

/* 2x3: FOLLOW THE SWAP CHAIN, instead of waiting to be told about it.
 *
 * WHY. Boot 206, traced live at 4 Hz: during fifteen seconds of 100 WPM
 * typing with the screen updating smoothly at the panel rate, `RBFBAmtrSets`
 * advanced exactly TWICE - 0.13/s, identical to the idle phases either side.
 * Two transactions cannot draw eight characters a second, so the screen was
 * updating WITHOUT transactions: the compositor renders into the surface our
 * HUBP write already points at, and the panel shows those renders live.
 *
 * The trace also showed exactly three addresses cycling - 0xf41002a000,
 * 0xf4110e2000, 0xf41192a000, about one 1920x1080x4 frame apart - i.e. a
 * TRIPLE-BUFFERED swap chain. We are told which buffer is current only when a
 * transaction arrives, roughly every six seconds. In between, the compositor
 * rotates and we keep scanning a stale buffer. That is the freeze, and it is
 * why "typing works, stopping freezes": typing keeps re-rendering whichever
 * buffer we happen to be on.
 *
 * So the transaction is the wrong thing to wait for. Apple fills the
 * `_AMD_PIPE_FLIP` with the current surface address (2wg measured the field;
 * 2wf measured the struct), and we already hold a validated pointer to it
 * from the first transaction. Read it every vblank and follow it.
 *
 * SAFETY. This runs on our own thread_call, never Apple's thread, and
 * allocates nothing, logs nothing and blocks on nothing (rule 31). It reads
 * sixteen bytes from a pointer that was range-checked when it was snapped
 * (rule 19) and that belongs to the pipe object, which outlives us. A torn
 * read - Apple writing the pair while we read it - cannot reach the hardware,
 * because hubpProgram() re-validates every address against the measured
 * carve-out, the 256-byte alignment and the frame size, and counts what it
 * refuses. Worst case is one stale frame until the next good read.
 *
 * It is gated on its own boot-arg AND on a transaction having arrived, so a
 * boot without the display pipe up cannot poll a pointer that was never set.
 */
void RBAMDFramebuffer::hubpPoll(void)
{
    if (!pollArmed || !hubpReady || !amtrFlipSnapped || !amtrSets) {
        return;
    }
    /* 2za: sample the latch BEFORE deciding whether to program again, so a
     * flip that landed during this frame is credited to the vblank that saw
     * it and not to the next one. One volatile register read per vblank -
     * 144 a second, allocating nothing and logging nothing (rule 31), the
     * same budget 2xk's read already proved acceptable on this path. */
    if (hubpRegs) {
        const uint32_t lfc = hubpRegs[kRbDmuSeg2 +
                                      rb_hubp_dw(hubpIndex,
                                                 RB_HUBP_REG_FLIP_CTL)];
        rb_hubp_latch_vbl(&hubpLatch, rb_hubp_flip_pending(lfc));
    }

    /* volatile: Apple updates this behind our back, which is the entire
     * point - the compiler must not hoist the load out of the vblank. */
    const volatile uint64_t *flip = (const volatile uint64_t *)amtrFlipPtr;
    const uint64_t mc = flip[1];        /* +0x08 LOW, +0x0c HIGH (2wg) */

    pollReads++;
    if (!pollFirstSeen) {
        pollFirstSeen = mc;
    }
    /* hubpLast is only updated by a write that passed every guard, so
     * comparing against it means a refused address is retried next vblank
     * rather than latching us out of the chain. */
    if (mc != hubpLast) {
        pollChanges++;
        hubpProgram(mc);
    }
}

/* 2xn: the 'amtr' GET's answer, kept here because kRbDmuSeg2 is this file's
 * and 2wh's rule stands - the DMU segment is a constant, a constant is an
 * assumption, and one copy of an assumption is better than two. The policy
 * itself is rb_hubp_hold_notready() in src/common/rb_hubp.h, run by the
 * hosttest; this is only the register read. Called from getAttribute on the
 * accelerator's thread: two MMIO reads, no allocation, no log, no block. */
/* 2A9: the read, with no decision and no side effect.
 *
 * This used to be the first half of hubpHoldNotReady(), behind its
 * `!amtrNotReady` guard - so turning the 2xo hold off stopped the bit being
 * read at all, and every instrument built on it went silent in the one
 * configuration where the flicker happens. Splitting them costs nothing (the
 * caller reads the word once either way) and lets the frame-time budget
 * bracket a pending run in both regimes. */
/* 2B1: set or clear SURFACE_TRIPLE_BUFFER_ENABLE, and VERIFY it.
 *
 * Follows hubp2_enable_triplebuffer(): a plain read-modify-write of
 * DCSURF_FLIP_CONTROL2, which dcn31_hubp_funcs maps verbatim, so the method
 * is grouped right for this chip (rule 46; see docs/linux-ref/UPSTREAM.md).
 * What Linux ALSO does and we do not is pair a triple-buffered flip with
 * triplebuffer_lock/unlock on the timing generator - stated in rb_hubp.h and
 * repeated here because it is the reason this is opt-in.
 *
 * Three things make it a dial rather than a write:
 *   - the pre-write snapshot is taken ONCE and kept, so `triple 0` restores
 *     the firmware's own word rather than a reconstruction of it;
 *   - a FLIP_CONTROL2 carrying bits outside the three documented fields is
 *     REFUSED and the refused value published (rule 19), because then this is
 *     not the register this code believes it is;
 *   - the readback decides the return value, not the fact that the store ran
 *     (rule 20, and rule 13 - a store cannot report failure here at all).
 */
bool RBAMDFramebuffer::hubpTripleApply(bool enable)
{
    if (!hubpReady || !hubpRegs) {
        return false;
    }
    const unsigned idx = kRbDmuSeg2 + rb_hubp_dw(hubpIndex,
                                                 RB_HUBP_REG_FLIP_CTL2);
    const uint32_t cur = hubpRegs[idx];
    if (!rb_hubp_triple_may_write(cur)) {
        hubpTripleRefused++;
        hubpTripleRefusedVal = cur;
        return false;
    }
    if (!hubpTripleHaveOrig) {
        hubpTripleOrig = cur;
        hubpTripleHaveOrig = true;
    }
    /* Restoring puts the firmware's own word back, bit for bit. Arming only
     * ever changes bit 10 of whatever is live. */
    const uint32_t want = enable ? rb_hubp_triple_set(cur, 1)
                                 : hubpTripleOrig;
    hubpRegs[idx] = want;
    hubpTripleWrites++;
    const uint32_t back = hubpRegs[idx];
    hubpTripleReadback = back;
    hubpTripleArmed = rb_hubp_triple_enabled(back) ? true : false;
    return rb_hubp_triple_enabled(back) == (enable ? 1 : 0);
}

bool RBAMDFramebuffer::hubpFlipPending(void)
{
    if (!hubpReady || !hubpRegs) {
        return false;
    }
    const uint32_t fc = hubpRegs[kRbDmuSeg2 +
                                 rb_hubp_dw(hubpIndex, RB_HUBP_REG_FLIP_CTL)];
    hubpPendCtl = fc;
    return rb_hubp_flip_pending(fc);
}

/* The decision, on a pending flag the caller already read. Unchanged in
 * behaviour: !amtrNotReady or a clear bit still answers ready, and the
 * deadline is still the backstop that keeps the hold bounded (2xn). */
bool RBAMDFramebuffer::hubpHoldNotReady(bool pending)
{
    if (!amtrNotReady || !pending) {
        return false;
    }
    if (rb_hubp_hold_notready(hubpPendCtl, mach_absolute_time(),
                              hubpFlipDeadline)) {
        amtrHeld++;
        return true;
    }
    amtrHoldExpired++;
    return false;
}


/* 2B3: read all five HPD blocks. Read-only, no side effect on the chip, and
 * nothing here decides anything about the frame path - it exists so the
 * question "does this silicon see a display on that port" has an answer that
 * is a measurement rather than an inference.
 *
 * Called at setup and from the periodic publish, so a cable plugged in after
 * boot shows up without a reboot. hpdSinkEver is the OR of every mask seen,
 * because the publish is periodic and a cable in and out between two of them
 * would otherwise leave no trace (rule 31).
 */
void RBAMDFramebuffer::hpdSample(void)
{
    if (!hpdReady || !hubpRegs) { return; }

    for (unsigned i = 0; i < RB_HPD_COUNT; i++) {
        const uint32_t dwStat = rb_hpd_dw_int_status(i);
        const uint32_t dwCtl  = rb_hpd_dw_control(i);
        /* RB_HPD_DW_INVALID cannot happen for i < RB_HPD_COUNT, but a read of
         * dword 0 of the segment is a read of some other block entirely, so
         * it is refused here rather than trusted (rule 19). */
        if (dwStat == RB_HPD_DW_INVALID || dwCtl == RB_HPD_DW_INVALID) {
            continue;
        }
        hpdStatus[i]  = hubpRegs[kRbDmuSeg2 + dwStat];
        hpdControl[i] = hubpRegs[kRbDmuSeg2 + dwCtl];
    }

    const uint32_t sink = rb_hpd_sink_mask(hpdStatus, hpdControl,
                                           RB_HPD_COUNT);
    if (sink != hpdSinkMask) { hpdChanges++; }
    hpdSinkMask  = sink;
    hpdSinkEver |= sink;
    hpdEnMask    = rb_hpd_enabled_mask(hpdControl, RB_HPD_COUNT);
}


/* 2B8: read the five DP AUX engines, and the OTG pipe words LIVE.
 *
 * Read-only, on hubpSetup()'s mapping and bound, armed by the same success as
 * hpdSample(). Nothing here decides anything and there is no writer - taking
 * the AUX arbitration lock on the engine driving the live panel is a way to
 * lose the display, and that decision is not taken in a survey (rb_aux.h).
 *
 * THE LIVE OTG WORDS ARE THE OTHER HALF. 2B5 found that BOTH existing OTG
 * readings are one-shots from setup - the plugin's rbgpu_agdc_otg.cpp sampler
 * and this kext's own hubpOtgCtl1..3 - so when the user attached a display and
 * asked whether a second pipe had lit, nothing in the registry could answer.
 * These are re-read on every publish, so OTG1..3 leaving 0x300 is visible the
 * moment it happens.
 */
void RBAMDFramebuffer::auxSample(void)
{
    if (!hpdReady || !hubpRegs) { return; }

    for (unsigned i = 0; i < RB_AUX_COUNT; i++) {
        const uint32_t dwCtl  = rb_aux_dw(i, RB_AUX_REG_CONTROL);
        const uint32_t dwDphy = rb_aux_dw(i, RB_AUX_REG_DPHY_RX_CTL1);
        const uint32_t dwStat = rb_aux_dw(i, RB_AUX_REG_SW_STATUS);
        const uint32_t dwArb  = rb_aux_dw(i, RB_AUX_REG_ARB_CONTROL);
        if (dwCtl == RB_AUX_DW_INVALID || dwDphy == RB_AUX_DW_INVALID ||
            dwStat == RB_AUX_DW_INVALID || dwArb == RB_AUX_DW_INVALID) {
            continue;
        }
        auxControl[i] = hubpRegs[kRbDmuSeg2 + dwCtl];
        auxDphy[i]    = hubpRegs[kRbDmuSeg2 + dwDphy];
        auxStatus[i]  = hubpRegs[kRbDmuSeg2 + dwStat];
        auxArb[i]     = hubpRegs[kRbDmuSeg2 + dwArb];
    }
    auxEnMask = rb_aux_enabled_mask(auxControl, RB_AUX_COUNT);

    /* Compared against instance 0 as a STATED ASSUMPTION, not a finding: the
     * AUX-instance-to-connector mapping is in no header (the same gap 2B4 hit
     * for HPD, where the answer came from a difference and not a datasheet).
     * Every raw word is published, so the comparison can be redone by hand
     * against a different reference if instance 0 turns out not to be the
     * panel's. */
    auxMatchMask = 0;
    for (unsigned i = 0; i < RB_AUX_COUNT; i++) {
        if (rb_aux_matches_ref(auxControl[i], auxDphy[i],
                               auxControl[0], auxDphy[0])) {
            auxMatchMask |= (1u << i);
        }
    }

    for (unsigned i = 0; i < 4; i++) {
        otgLive[i] = hubpRegs[kRbDmuSeg2 + kRbOtgControl[i]];
    }
}

/* 2C0: the five RDPCSTX PHY blocks - DP alt mode, lane power, reference
 * clock. Read-only, no writer, and gated on dpaltArmed as well as on the
 * aperture, because this is the one block in this kext whose documented
 * failure mode is a HANG rather than a refused value: Linux marks the
 * register path "Legacy path, avoid if possible" and prefers a DMUB query to
 * avoid "hanging the RDCPSPIPE if DMCUB wasn't already running".
 *
 * Why it is read here anyway, and why the register beats the DMUB query on
 * THIS machine, is argued in rb_dpalt.h; the short form is that boot 275
 * measured the inbox reply path returning nothing (2zr) while DMCUB 4.0.15.47
 * is demonstrably alive on the GPINT route (2zs), so the stated precondition
 * for the hang is measured false and the alternative is measured dead.
 *
 * The verdict is NOT computed here - rb_dpalt.h owns every judgement so the
 * hosttest owns it too, and the raw words are published whichever way the
 * verdict falls (publishing raw beside the summary has now paid for itself in
 * 2B0, 2B4 and 2B9). */
void RBAMDFramebuffer::dpaltSample(void)
{
    if (!dpaltReady || !hubpRegs) { return; }

    for (unsigned i = 0; i < RB_DPALT_COUNT; i++) {
        const uint32_t dw = rb_dpalt_dw(i);
        /* RB_DPALT_DW_INVALID cannot happen for i < RB_DPALT_COUNT, but a
         * read of dword 0 of the segment is a read of another block
         * entirely, so it is refused rather than trusted (rule 19). */
        if (dw == RB_DPALT_DW_INVALID) { continue; }
        dpaltWord[i] = hubpRegs[kRbDmuSeg2 + dw];
    }

    dpaltActiveMask = rb_dpalt_active_mask(dpaltWord, RB_DPALT_COUNT);

    const uint32_t nd = rb_dpalt_not_disabled_mask(dpaltWord, RB_DPALT_COUNT);
    /* Boot 297 published `transitions 1` with no cable ever attached: the
     * mask starts at 0, the first sample made it 0x01, and the counter
     * recorded that as a transition. A counter that counts its own first
     * reading cannot be read as a cable event, so the first sample is
     * excluded - the quantity wanted is CHANGES SINCE the baseline, and the
     * baseline is the first sample rather than the zero the field was born
     * with (rule 14's shape for a counter).
     *
     * The test is its own flag rather than `dpaltSweep != NOT_SAMPLED`, which
     * also works today only because the sweep verdict is assigned BELOW this
     * line - a silent ordering dependency that the next edit to this function
     * would break with no symptom. */
    if (dpaltSampled && nd != dpaltNotDisabledMask) { dpaltChanges++; }
    dpaltSampled = true;
    dpaltNotDisabledMask  = nd;
    dpaltEverNotDisabled |= nd;

    dpaltSweep = (int)rb_dpalt_sweep_verdict(dpaltWord, RB_DPALT_COUNT);
}

/* 2EF: PIPE 0'S LIVE DP PIXEL-CLOCK DTO, AND THE SEGMENT CHECK.
 *
 * Read-only, no writer, and the reason it exists at all is that the WRITE it
 * precedes would be the first this kext makes at BASE_IDX 1 - a segment whose
 * offsets 0x0080..0x0083 collide with the DOMAIN power-gating registers at
 * BASE_IDX 2, where this kext does all its work. A write at the wrong one of
 * those two bases would hit the power gating of the pipe scanning the panel.
 * So the address is established first, by arithmetic the chip itself has to
 * satisfy (rule 47 asked of a write: establish the live word decodes as the
 * register the conclusion assumes).
 *
 * Nothing here decides anything - rb_dpdto.h owns every judgement so the
 * hosttest owns it too - and every raw word is published whichever way the
 * verdict falls, which has now refuted three wrong verdicts of this
 * project's own (2B0, 2B4, 2B9).
 *
 * The expected pixel clock is `modeClkHz`: the live raster times the refresh
 * measured on an OTG0_FRAME_COUNT edge, formed by rb_drr.h. It is read here
 * rather than recomputed because one product computed twice is two products
 * (and 0 means mode setup never got a measurement, which is its own verdict
 * code and not a wrong address).
 */
void RBAMDFramebuffer::dtoSample(void)
{
    if (!dtoReady || !hubpRegs) { return; }

    for (unsigned i = 0; i < RB_DPDTO_COUNT; i++) {
        for (unsigned r = 0; r < RB_DPDTO_REG_COUNT; r++) {
            const uint32_t dw = rb_dpdto_dw(i, r);
            /* Cannot happen for these bounds, but a dword 0 of a segment is
             * a read of another block entirely, so it is refused rather than
             * trusted (rule 19, and dpaltSample()'s own shape). */
            if (dw == RB_DPDTO_DW_INVALID) { continue; }
            dtoWord[i][r] = hubpRegs[RB_DPDTO_SEG1 + dw];
        }
    }

    /* THE ALIAS CONTROL: pipe 0's four offsets read at the segment this kext
     * already uses. There they are DOMAIN0_PG_CONFIG, DOMAIN0_PG_STATUS,
     * DOMAIN1_PG_CONFIG and DOMAIN1_PG_STATUS - rb_pg.h's own registers. If
     * the two bases return the same four words, the segment arithmetic did
     * nothing and what was read above is the power-gating block. Taken in the
     * SAME sample as the words it controls, because a control measured in a
     * different window is not a control (rule 44). */
    for (unsigned r = 0; r < RB_DPDTO_REG_COUNT; r++) {
        const uint32_t dw = rb_dpdto_dw(0, r);
        if (dw == RB_DPDTO_DW_INVALID) { continue; }
        dtoAlias[r] = hubpRegs[RB_DPDTO_SEG2_CONTROL + dw];
    }

    /* THE REFERENCE THE ARITHMETIC IS CHECKED AGAINST, and the first draft of
     * this got it wrong in a way that would have cost a boot.
     *
     * It used `modeClkHz` - the panel's pixel clock measured on an
     * OTG0_FRAME_COUNT edge, which is the best number this project has. But
     * `modeSetup()` returns at `if (!argEnabled("-rbgpufbmodes"))` BEFORE it
     * computes that field, so on an ordinary boot `modeClkHz` is 0 and this
     * whole read would have published RB_DPDTO_BASE_NO_EXPECTED and measured
     * nothing. Worse, the obvious fix - depend on `-rbgpufbmodes` - drags in a
     * second display mode and is mutually exclusive with -rbgpufbdrr. That is
     * rule 34 exactly: a dial's state at the start is part of the arm, and an
     * experiment that needs two tokens in the right combination will be run
     * with one of them missing.
     *
     * So there are two sources and the boot SAYS WHICH IT USED (rule 17 - a
     * reading taken against a measured rate and one taken against a registry
     * constant are not the same reading):
     *   MEASURED: modeClkHz, when mode setup actually ran and measured.
     *   RASTER:   OTG0's live H_TOTAL x V_TOTAL - read right here, from the
     *             chip, so this does not depend on any other setup path -
     *             times the rate the vblank clock is actually running at.
     *             That rate starts as this panel's measured 16.16 constant
     *             and is retuned per mode, so it is a measured constant
     *             rather than a live measurement, which is why it is labelled
     *             and not called derived.
     * Either way the comparison window is 0.5 %, three orders of magnitude
     * wider than the spread between this project's own three derivations of
     * the panel's clock - the reference only has to be good enough to exclude
     * a DIFFERENT REGISTER, which is the failure this read exists to catch. */
    dtoHTotal = hubpRegs[kRbDmuSeg2 + RB_DPDTO_DW_OTG0_H_TOTAL] &
                RB_DPDTO_TOTAL_MASK;
    dtoVTotal = hubpRegs[kRbDmuSeg2 + RB_DPDTO_DW_OTG0_V_TOTAL] &
                RB_DPDTO_TOTAL_MASK;
    dtoRefreshMhz = rb_dpdto_mhz_from_period_ns(
        vblPeriodNs ? vblPeriodNs : kVBLPeriodNsFast);
    if (modeClkHz) {
        dtoExpectedHz = modeClkHz;
        dtoRefSrc = RB_DPDTO_REF_SRC_MEASURED;
    } else {
        dtoExpectedHz = rb_drr_clk_from_mhz(dtoRefreshMhz, dtoHTotal,
                                            dtoVTotal);
        dtoRefSrc = dtoExpectedHz ? RB_DPDTO_REF_SRC_RASTER
                                  : RB_DPDTO_REF_SRC_NONE;
    }

    dtoBaseVerdict = rb_dpdto_base_verdict(dtoWord[0], dtoAlias, dtoExpectedHz);
    dtoPipeVerdict = rb_dpdto_pipe_verdict(dtoWord[0][RB_DPDTO_REG_RATE_CNTL]);
    dtoRefHz = rb_dpdto_ref_hz(dtoWord[0], dtoBaseVerdict);
}

/* 2F1: PROGRAM ONE DARK PIPE'S DP PIXEL-CLOCK DTO.
 *
 * `dcn31_program_pix_clk`'s DP branch, in its own order: PHASE, then MODULO,
 * then the enable in PIXEL_RATE_CNTL. 2EE established this is the path on this
 * chip for this signal (the VBIOS `set_pixel_clock` command is reached only in
 * the HDMI/DVI `else`), and 2F0 confirmed the base on hardware.
 *
 * THE GATE IS THIS BOOT'S OWN BASE VERDICT, not 2F0's number. dtoSample() is
 * re-run on every periodic publish, so the verdict this refusal consumes was
 * computed from the chip within the last publish interval - and it is computed
 * from PHASE[0] against the live raster, which no wrong address can satisfy. A
 * write at the other candidate base lands on DOMAIN2/3's power gating, so this
 * is rule 47 asked of a write with the answer measured rather than argued.
 *
 * It is re-sampled HERE rather than trusted from whenever the last publish
 * happened, because a dial must not rest on a snapshot (rule 31) - and because
 * the target pipe's live word is needed anyway, to refuse a pipe whose DTO is
 * already running.
 *
 * `blRegs` is the writable mapping of the same BAR5 window `hubpRegs` reads
 * through, as feConnect() and encConfigure() already use. No new mapping and
 * no new aperture question: these dwords top out at 0x14f, BELOW the 0x34c0
 * segment every mapping in this kext has had to clear to be usable at all -
 * an inequality the hosttest pins rather than this comment asserting it.
 *
 * NOT reproduced, and said here before any reading so a null cannot be
 * over-read: everything downstream of the clock. There is no OTG timing on
 * pipe 1, no trained link, no stream mapped to the encoder and no surface. So
 * a STARTED verdict means "the DTO makes a clock", which is the whole claim -
 * nothing here should put a picture on the monitor, and the absence of one is
 * not evidence against the write.
 */
/* 2F1: publish the write side. Called from publishVBLState() AND from the way
 * out of the dial's own setProperties handler, so a tool's read-back is that
 * call's state and not the previous snapshot's - 2E3's structural fix,
 * applied here before a tool could be written against the stale path. These
 * run from setProperties at human frequency, so rule 16's cost argument for
 * the RBFBPubMs throttle does not reach them. */
void RBAMDFramebuffer::dtoPublish(void)
{
    /* 2F1: the write side. `RBFBDtoProgCalls` is the monotonic counter a tool
     * polls past its pre-set value, because RBGPUDiag-style verification by
     * sleeping on the dialled value is the mistake rule 31 has now recorded
     * five times. Every field is published whichever way the refusal fell. */
    setProperty("RBFBDtoProgCalls", (uint64_t)dtoProgCalls, 32);
    setProperty("RBFBDtoProgRan", (uint64_t)dtoProgRan, 32);
    setProperty("RBFBDtoProgRefuse", (uint64_t)dtoProgRefuse, 32);
    /* 2F2: what the MOST RECENT call asked for and saw, which on a
     * refusal is the only thing that call established. Kept apart from
     * the run record below so a refused second invocation cannot
     * overwrite the reading of the write that did run. */
    setProperty("RBFBDtoCallInst", (uint64_t)dtoCallInst, 32);
    setProperty("RBFBDtoCallPixHz", (uint64_t)dtoCallPixHz, 64);
    setProperty("RBFBDtoCallSrcSel", (uint64_t)dtoCallSrcSel, 32);
    setProperty("RBFBDtoCallRefHz", (uint64_t)dtoCallRefHz, 64);
    setProperty("RBFBDtoCallLive", (uint64_t)dtoCallLive, 64);
    setProperty("RBFBDtoProgInst", (uint64_t)dtoProgInst, 32);
    setProperty("RBFBDtoProgPixHz", (uint64_t)dtoProgPixHz, 64);
    setProperty("RBFBDtoProgRefHz", (uint64_t)dtoProgRefHz, 64);
    setProperty("RBFBDtoProgSrcSel", (uint64_t)dtoProgSrcSel, 32);
    setProperty("RBFBDtoProgApplied", (uint64_t)dtoProgApplied, 32);
    setProperty("RBFBDtoProgChanged", (uint64_t)dtoProgChanged, 32);
    setProperty("RBFBDtoProgClean", (uint64_t)dtoProgClean, 32);
    setProperty("RBFBDtoRateBefore", (uint64_t)dtoRateBefore, 64);
    setProperty("RBFBDtoRateAfter", (uint64_t)dtoRateAfter, 64);
    /* Raw, and NOT the verdict: 2F0 measured PHASE as the clock the firmware
     * ASKED for rather than a readback of the achieved one, so finding our own
     * number here is rule 20's defect and is known in advance to happen. */
    setProperty("RBFBDtoPhaseAfter", (uint64_t)dtoPhaseAfter, 64);
    setProperty("RBFBDtoModuloAfter", (uint64_t)dtoModuloAfter, 64);
    /* The verdict reads DPDTO_ENABLE_STATUS - a bit we did not write, whose
     * two states 2F0 attested on this chip before anything was programmed. */
    setProperty("RBFBDtoProgVerdict", (uint64_t)dtoProgVerdict, 32);
}

uint32_t RBAMDFramebuffer::dtoProgram(uint32_t inst, uint32_t pixHz, int srcSel)
{
    dtoProgCalls++;

    /* 2F2: THIS CALL's own fields. The run record (dtoProg*, dtoRate*,
     * dtoPhaseAfter, dtoModuloAfter) is deliberately NOT touched here - 2F1
     * reset it at entry and a later refusal then republished a successful
     * write as "NOT RUN", destroying the only reading of it. */
    dtoCallInst = inst;
    dtoCallPixHz = pixHz;
    dtoCallSrcSel = srcSel ? 1u : 0u;
    dtoCallRefHz = 0;
    dtoCallLive = 0;

    const int haveRegs = (blRegs != nullptr) && (hubpRegs != nullptr)
                         && dtoReady;

    /* Re-read the whole block, so the base verdict and the target pipe's live
     * word come from the SAME sample as each other (rule 44's shape for a
     * control: one measured in a different window is not one). */
    if (haveRegs) { dtoSample(); }

    /* Clamped only so the live read below cannot index out of range before
     * rb_dpdto_write_refuse() gets to reject the instance itself - the same
     * shape feConnect() uses, and the refusal still names the pipe asked
     * for because dtoProgInst was stored above. */
    const uint32_t safe = (inst < RB_DPDTO_COUNT) ? inst : 0u;

    const uint32_t live = haveRegs ? dtoWord[safe][RB_DPDTO_REG_RATE_CNTL]
                                   : 0u;
    dtoCallLive = live;

    /* The reference is pipe 0's own MODULO, read off the chip rather than
     * written down - 2F0 found 600 000 000 there and Linux uses one
     * `dp_dto_ref_khz` for every pipe, so copying it makes pipe 1's ratio as
     * accurate as the panel's without the true reference ever being known. */
    const uint32_t refHz = haveRegs
                           ? dtoWord[RB_DPDTO_PANEL_PIPE][RB_DPDTO_REG_MODULO]
                           : 0u;
    dtoCallRefHz = refHz;

    const uint32_t why = rb_dpdto_write_refuse(inst, pixHz, refHz, live,
                                               dtoArmed ? 1 : 0, haveRegs,
                                               dtoBaseVerdict);
    /* The ONLY state a refusal leaves behind, besides the dtoCall* set and
     * the monotonic counter: what it refused, by code (rule 14). */
    if (why != RB_DPDTO_W_OK) { return dtoProgRefuse = why; }

    /* Past the gate, so this call is a write and owns the run record. */
    dtoProgInst = inst;
    dtoProgPixHz = pixHz;
    dtoProgSrcSel = srcSel ? 1u : 0u;
    dtoProgRefHz = refHz;
    dtoRateBefore = live;

    const uint32_t phaseDw = RB_DPDTO_SEG1 + rb_dpdto_dw(inst,
                                                 RB_DPDTO_REG_PHASE);
    const uint32_t moduloDw = RB_DPDTO_SEG1 + rb_dpdto_dw(inst,
                                                 RB_DPDTO_REG_MODULO);
    const uint32_t rateDw = RB_DPDTO_SEG1 + rb_dpdto_dw(inst,
                                                 RB_DPDTO_REG_RATE_CNTL);

    /* Linux's order, unrearranged: the divider's two terms are in place
     * before the enable that starts it. */
    blRegs[phaseDw] = pixHz;
    blRegs[moduloDw] = dtoProgRefHz;
    blRegs[rateDw] = rb_dpdto_rate_word(dtoRateBefore, srcSel);

    dtoRateAfter = hubpRegs[rateDw];
    dtoPhaseAfter = hubpRegs[phaseDw];
    dtoModuloAfter = hubpRegs[moduloDw];

    dtoProgApplied = (uint32_t)rb_dpdto_rate_applied(dtoRateAfter, srcSel);
    dtoProgChanged = (uint32_t)rb_dpdto_rate_changed(dtoRateBefore,
                                                     dtoRateAfter);
    dtoProgClean = (uint32_t)rb_dpdto_rate_clean(dtoRateBefore, dtoRateAfter);
    dtoProgVerdict = rb_dpdto_prog_verdict(dtoRateBefore, dtoRateAfter);
    dtoProgRan++;
    return dtoProgRefuse = RB_DPDTO_W_OK;
}
