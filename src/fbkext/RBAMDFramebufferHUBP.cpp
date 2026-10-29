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
    const uint32_t needed = kRbDmuSeg2 +
        rb_hubp_dw(hubpIndex, RB_HUBP_REG_ADDR_HI);
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

    /* Gate 1: are we addressing DCN at all? 2oq's signature. If the segment
     * constant is wrong, these four reads are garbage and will not match. */
    hubpOtgCtl = rbRead(regs, kRbDmuSeg2 + kRbOtgControl[0]);
    bool otgOk = (hubpOtgCtl & RB_OTG_MASTER_EN) != 0;
    for (unsigned i = 1; i < 4 && otgOk; i++) {
        if (rbRead(regs, kRbDmuSeg2 + kRbOtgControl[i]) != RB_OTG_OFF_VALUE) {
            otgOk = false;
        }
    }
    if (!otgOk) { hubpRefuse = 5; map->release(); return false; }

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

    /* HIGH first, then LOW. DCN latches the pair when the LOW dword is
     * written, so this order means the hardware never sees a half-updated
     * address. On this machine HIGH does not change - every carve-out address
     * shares 0xf4 - so the first store is a no-op that exists to keep the
     * order correct if that ever stops being true. */
    hubpRegs[kRbDmuSeg2 + rb_hubp_dw(hubpIndex, RB_HUBP_REG_ADDR_HI)] =
        (uint32_t)(mc >> 32);
    hubpRegs[kRbDmuSeg2 + rb_hubp_dw(hubpIndex, RB_HUBP_REG_ADDR)] =
        (uint32_t)mc;

    hubpWrites++;
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
