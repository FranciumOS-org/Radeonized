/* SPDX-License-Identifier: MIT */
/* 2xs: the panel's backlight - the second write this project makes to the
 * display engine, and a much smaller one than 2wh's.
 *
 * Everything that DECIDES anything lives in src/common/rb_backlight.h and is
 * run by the hosttest before this file is ever compiled into a kext, the same
 * contract rb_hubp.h has. The long reasoning - what the write does on the
 * chip, why only the duty cycle changes, where the arithmetic comes from and
 * why there is a non-zero floor - is the header comment there. This file is
 * the mechanism: map BAR5, prove we are addressing PWRSEQ0, and on request do
 * Linux's lock / write / unlock / wait sequence on one field.
 *
 * WHY THIS IS SEPARATELY ARMED FROM EVERYTHING ELSE. Rule 38 says two halves
 * that are separately armed must check each other, and names what each one
 * depends on. The HUBP write depends on transactions arriving through
 * setAttribute('amtr'), so it needs the accelerator; this depends on nothing
 * but owning the panel, because the backlight PWM is already running and we
 * only change its duty cycle. So it takes its own boot-arg, its own BAR5 map
 * and its own gates, and `-rbgpufbhubp` is neither required nor implied. The
 * failure mode is a dim screen, not a frozen one.
 *
 * HOW THE REQUEST ARRIVES, read end to end in 2xs with no boot:
 *
 *   IODisplay::doIntegerSet (IOGraphicsFamily 0x14fb1d46)
 *       - the gIODisplayBrightnessKey branch (0x14fb1f02) is ONLY a
 *         recordGTraceToken; it falls through to the generic path
 *   -> 0x14fb1fa6  h = OSDynamicCast(IODisplayParameterHandler, display->0x90)
 *   -> h->vt[0x858]  = IOFramebufferParameterHandler::doIntegerSet (0x14fce436)
 *   -> 0x14fce511    name = key->getCStringNoCopy()
 *      0x14fce51d    fourcc = bswap32(*(uint32_t *)name)   <- the FIRST FOUR
 *      0x14fce58c    fb->vt[0xa58](0, fourcc, value)         CHARACTERS
 *
 * So the attribute is not a constant anybody chose - it is DERIVED from the
 * parameter name, and kIODisplayBrightnessKey is the string "brightness",
 * whose first four characters are 'b','r','i','g'. 'brig' is therefore the
 * only value it can ever be, and the hosttest asserts that derivation from
 * the string rather than restating the constant (2v1's rule: a field compared
 * against the macro that wrote it is not a test).
 *
 * AND THE HANDLER IS CONFIRMED PRESENT, from the live registry rather than
 * by inference: our own AppleDisplay already carries
 *     "IODisplayParameters" = {"bgsc"=..,"rgsc"=..,"ggsc"=..,"vblm"=..,"ownr"=..}
 * and those four-character names are exactly what IOFramebufferParameter-
 * Handler::displayModeChange (0x14fc8ece) builds: it asks us 'pcnt' for a
 * count, 'parm' for an array of 8-byte entries whose low dword is a FourCC,
 * byte-swaps each into a 4-character name, and then asks us for that
 * attribute with a three-word buffer { value, min, max }. They come from
 * IOFramebuffer's own base handler (0x14fcc5cb writes 'rgsc', 'ggsc', 'bgsc'
 * then 'vblm'), so the mechanism is already working on this machine for four
 * parameters - we are adding a fifth, not building a path.
 *
 * WHAT THIS DOES NOT DO. It does not make the System Settings slider or the
 * brightness keys work. Those drive kIODisplayBrightnessKey, the string, and
 * displayModeChange only ever creates FOUR-character names, so the parameter
 * we publish is called "brig" and not "brightness". The class that publishes
 * the long name is AppleBacklightDisplay, which has 0 instances here because
 * its probe() refuses - a separate milestone with its own gates (2xs names
 * them: kIOConnectionBuiltIn in our connection flags, and a getAppleSense
 * display type of 2, 14 or 20). This step deliberately proves the HARDWARE
 * half with no UI dependency, because a brightness write that does not work
 * and a UI that does not call it are otherwise indistinguishable.
 */

#include "RBAMDFramebuffer.hpp"
#include "rb_backlight.h"
#include "rb_dcn_deny.h"
#include "rb_regs.h"
#include <IOKit/pci/IOPCIDevice.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IOLib.h>
/* 2xy: kPanelTFTConnect, the csDisplayType value getAppleSense reports. */
#include <IOKit/ndrvsupport/IOMacOSVideo.h>

/* DMU segment 2. Deliberately a second copy of the constant rather than a
 * shared one: 2wh's own note says "the segment is a constant in this kext,
 * and a constant is an assumption", and the point of re-deriving it here is
 * that this path CHECKS it independently rather than inheriting a belief from
 * a gate that ran for a different register. */
static const uint32_t kRbDmuSeg2Bl = 0x34c0;

/* PWRSEQ0_BL_PWM_*, dcn_3_1_2_offset.h, base_idx 2. Cross-checked against
 * rb_regs.h by the hosttest. */
#define RB_BL_DW_CNTL    0x2f19u
#define RB_BL_DW_CNTL2   0x2f1au
#define RB_BL_DW_PERIOD  0x2f1bu
#define RB_BL_DW_LOCK    0x2f1cu

/* OTG0, for the segment check. 2xp established that OTG0's master-enable is
 * the clause that corroborates the segment and that OTG1-3 do NOT - a live
 * OTG is a plausible value at a plausible offset - so only OTG0 is used
 * here, and an HDMI-attached boot cannot refuse the backlight. */
#define RB_BL_OTG0_DW        0x1b41u
#define RB_BL_OTG_MASTER_EN  0x1u

/* Bounded wait for BL_PWM_GRP1_REG_UPDATE_PENDING to clear. Linux waits
 * 1 us x 10000; this runs on IOGraphics' gated workloop, so it is bounded
 * far tighter and a timeout is COUNTED rather than retried (rule 14: a path
 * that gives up must say so). The measured lock register has
 * BL_PWM_GRP1_UPDATE_AT_FRAME_START clear, so the update is immediate and
 * not frame-synced - a frame-synced update would need ~7 ms, which is why
 * this is worth stating rather than assuming. */
#define RB_BL_PENDING_SPINS  2000u

bool RBAMDFramebuffer::blSetup(void)
{
    blRefuse = 0;
    blReady = false;
    blMap = nullptr;
    blRegs = nullptr;
    blSets = 0;
    blGets = 0;
    blLastCount = 0;
    blReadback = 0;
    blPendTimeouts = 0;
    blParmAdds = 0;
    blFlagsGets = 0;
    blSenseGets = 0;

    if (!blArmed) {
        blRefuse = 1;
        return false;
    }

    /* The provider, cast once in start() - the same field hubpSetup() uses. */
    if (!pci) {
        blRefuse = 2;
        return false;
    }

    IOMemoryMap *map = pci->mapDeviceMemoryWithRegister(kIOPCIConfigBaseAddress5,
                                                        kIOMapInhibitCache);
    if (!map) { blRefuse = 3; return false; }

    /* Every dword we will touch has to be inside the aperture, checked before
     * the first dereference and not after (rule 19's shape for a mapping). */
    const uint64_t length = map->getLength();
    const uint32_t needed = kRbDmuSeg2Bl + RB_BL_DW_LOCK;
    if (length < ((uint64_t)needed + 1) * sizeof(uint32_t)) {
        blRefuse = 4;
        map->release();
        return false;
    }

    const volatile uint32_t *regs =
        reinterpret_cast<const volatile uint32_t *>(map->getVirtualAddress());
    if (!regs) { blRefuse = 3; map->release(); return false; }

    /* Gate 1: are we addressing DCN at all? OTG0's master-enable, which the
     * firmware set when it lit this panel. Published either way. */
    blOtgCtl = regs[kRbDmuSeg2Bl + RB_BL_OTG0_DW];
    if ((blOtgCtl & RB_BL_OTG_MASTER_EN) == 0u) {
        blRefuse = 5;
        map->release();
        return false;
    }

    /* Read the four PWRSEQ0 registers. All four are published even when a
     * gate below refuses, because "we refused" and "we refused BECAUSE the
     * period was zero" are different findings (rule 14, and 2xp's own
     * correction to gate 1 in the HUBP path). */
    blCntl   = regs[kRbDmuSeg2Bl + RB_BL_DW_CNTL];
    blCntl2  = regs[kRbDmuSeg2Bl + RB_BL_DW_CNTL2];
    blPeriod = regs[kRbDmuSeg2Bl + RB_BL_DW_PERIOD];
    blLock   = regs[kRbDmuSeg2Bl + RB_BL_DW_LOCK];
    blLevel0 = rb_bl_level_from_pwm(blCntl, blPeriod);

    /* Gate 2: 2or's falsification test, stated before any of this was built -
     * "BL_PWM_PERIOD == 0 would mean the panel is driven some other way and
     * this route is wrong" - plus BL_PWM_EN, because a duty cycle on a
     * disabled PWM drives nothing. Decided by rb_backlight.h so the hosttest
     * owns it. */
    if (!rb_bl_can_control(blCntl, blPeriod)) {
        blRefuse = 6;
        map->release();
        return false;
    }

    /* Gate 3: the project's own DCN write policy. Both dwords we will write
     * must be allowed, re-checked on the machine so a future edit here cannot
     * bypass it. The hosttest asserts the same two, and asserts that the
     * list is non-empty by checking 0x288b IS denied. */
    if (rb_dcn_is_denied(RB_BL_DW_CNTL) || rb_dcn_is_denied(RB_BL_DW_LOCK)) {
        blRefuse = 9;
        map->release();
        return false;
    }

    blMap = map;
    blRegs = reinterpret_cast<volatile uint32_t *>(map->getVirtualAddress());
    blOrigCntl = blCntl;
    blReady = true;
    return true;
}

void RBAMDFramebuffer::blTeardown(void)
{
    /* Put the duty cycle back where the firmware had it. Best-effort, from
     * stop(): a reboot restores it anyway and the boot-arg is the real disarm
     * (rule 9), but leaving a machine at 8 % because a kext unloaded would be
     * a poor failure mode when restoring it costs one store. */
    if (blReady && blRegs && blOrigCntl) {
        blRegs[kRbDmuSeg2Bl + RB_BL_DW_LOCK] =
            (blLock & ~RB_BL_LOCK_UPDATE_PENDING) | RB_BL_LOCK_REG_LOCK;
        blRegs[kRbDmuSeg2Bl + RB_BL_DW_CNTL] = blOrigCntl;
        blRegs[kRbDmuSeg2Bl + RB_BL_DW_LOCK] =
            blLock & ~(RB_BL_LOCK_REG_LOCK | RB_BL_LOCK_UPDATE_PENDING);
    }
    blReady = false;
    blRegs = nullptr;
    if (blMap) { blMap->release(); blMap = nullptr; }
}

/* The current level, re-read from the chip rather than cached, so a value
 * somebody else changed (DMCUB's ABM, a firmware event) is reported and not
 * papered over with what we last wrote. */
uint32_t RBAMDFramebuffer::blCurrentLevel(void)
{
    if (!blReady || !blRegs) { return 0; }
    const uint32_t cntl   = blRegs[kRbDmuSeg2Bl + RB_BL_DW_CNTL];
    const uint32_t period = blRegs[kRbDmuSeg2Bl + RB_BL_DW_PERIOD];
    blGets++;
    return rb_bl_level_from_pwm(cntl, period);
}

/* dce_driver_set_backlight() step 2, docs/linux-ref/dce_panel_cntl.c:233.
 * Runs on IOGraphics' gated workloop, so it allocates nothing, logs nothing
 * and blocks on nothing (rule 31); the bounded IODelay spin is the only wait
 * and a timeout is counted, not retried. */
bool RBAMDFramebuffer::blSetLevel(uint32_t level)
{
    if (!blReady || !blRegs) { return false; }

    const uint32_t period = blRegs[kRbDmuSeg2Bl + RB_BL_DW_PERIOD];
    const uint32_t cntl   = blRegs[kRbDmuSeg2Bl + RB_BL_DW_CNTL];

    /* Re-check the gate against what the chip says NOW, not against what it
     * said in start(). If the PWM has been disabled or the period zeroed
     * since, a duty cycle computed from it would be meaningless. */
    if (!rb_bl_can_control(cntl, period)) {
        blRefuse = 6;
        return false;
    }

    const uint32_t want = rb_bl_clamp(level, blFloor);
    const uint32_t count = rb_bl_count_from_level(want, period);

    /* 2.1 Lock group 1, ignoring the master lock, exactly as Linux does -
     * REG_UPDATE_2(BL_PWM_GRP1_IGNORE_MASTER_LOCK_EN, 1, BL_PWM_GRP1_REG_LOCK, 1).
     * Read-modify-write so the frame-start and readback bits keep whatever
     * the firmware chose. */
    const uint32_t lock0 = blRegs[kRbDmuSeg2Bl + RB_BL_DW_LOCK];
    blRegs[kRbDmuSeg2Bl + RB_BL_DW_LOCK] =
        (lock0 & ~RB_BL_LOCK_UPDATE_PENDING) |
        RB_BL_LOCK_IGNORE_MASTER | RB_BL_LOCK_REG_LOCK;

    /* 2.2 Write the new active duty cycle - one field, every other bit of
     * BL_PWM_CNTL left as it was. */
    blRegs[kRbDmuSeg2Bl + RB_BL_DW_CNTL] = rb_bl_cntl_with_count(cntl, count);

    /* 2.3 Unlock. */
    blRegs[kRbDmuSeg2Bl + RB_BL_DW_LOCK] =
        (lock0 & ~(RB_BL_LOCK_REG_LOCK | RB_BL_LOCK_UPDATE_PENDING)) |
        RB_BL_LOCK_IGNORE_MASTER;

    /* 3. Wait for the pending bit to clear, bounded. */
    {
        unsigned spins = RB_BL_PENDING_SPINS;
        while (spins--) {
            if ((blRegs[kRbDmuSeg2Bl + RB_BL_DW_LOCK] &
                 RB_BL_LOCK_UPDATE_PENDING) == 0u) {
                break;
            }
            IODelay(1);
        }
        if (spins == (unsigned)-1) { blPendTimeouts++; }
    }

    /* Read back the field we wrote. A register reading back what we wrote is
     * the difference between "the store executed" and "the store reached the
     * chip" - 2lq in its register form, the same check 2wh makes. */
    blReadback = blRegs[kRbDmuSeg2Bl + RB_BL_DW_CNTL] &
                 RB_BL_CNTL_ACTIVE_CNT_MASK;
    blLastCount = count;
    blLastLevel = want;
    blSets++;
    return true;
}

/* 2xy: THE SYSTEM SETTINGS SLIDER, and why it is two answers and not a write.
 *
 * 2xs made the panel's duty cycle writable and published 'brig' in the
 * connection's parameter list, which is everything a client needs to SET the
 * brightness. It did not make anything ASK. The live registry says why: the
 * IODisplay that matched our connection is plain `AppleDisplay`
 * (IODisplayConnectFlags = <00000000>), and System Settings' Display pane
 * draws its brightness slider for the IODisplay subclass that owns a
 * backlight, `IOBacklightDisplay` / `AppleBacklightDisplay` - both defined in
 * IOGraphicsFamily, confirmed present in this OS's binary by name.
 *
 * `IOBacklightDisplay::probe` walks its framebuffer's connections and takes
 * the first one that passes BOTH of these, in this order:
 *
 *     getAttributeForConnection(idx, kConnectionFlags, &flags)
 *         must succeed AND have kIOConnectionBuiltIn (0x800) set
 *     getAppleSense(idx, NULL, NULL, NULL, &displayType)
 *         must succeed AND report a panel - kPanelTFTConnect (2)
 *
 * We answered 0 to the first and declined the second, so the probe returned
 * nothing and the generic AppleDisplay won the category on its own score.
 * Both answers are therefore true statements about this machine that we were
 * simply not making: the panel IS built in, and it IS a TFT panel wired to
 * the APU's eDP - 2xq/2xr established that the removable port is the dGPU's.
 *
 * WHY ITS OWN BOOT-ARG, separate from -rbgpufbbacklight. These two answers
 * change which driver matches a connection, and IOGraphics reads
 * kIOConnectionBuiltIn for more than the slider: it marks the display
 * internal for the display-prefs key, for mirroring and for the wrangler's
 * idle dimming. That is a different and larger blast radius than writing a
 * PWM duty cycle, and rule 38's question - what does each half depend on -
 * has different answers for the two. So a boot can arm the write without the
 * match, or the match without the write. Arming the match alone is a
 * deliberate and useful state: it answers "does AppleBacklightDisplay match
 * at all on this machine" without any register write in the boot.
 *
 * Rule 30, checked against every token in use: the arg is `-rbgpufbbuiltin`,
 * which shares `-rbgpufbb` with `-rbgpufbbacklight` and diverges at the next
 * character ('a' against 'u'), so neither prefixes the other; no other
 * `-rbgpufb*` token comes within `-rbgpufb` of it, and the mechanical
 * negative `-rbgpunofbbuiltin` is likewise distinct from
 * `-rbgpunofbbacklight`. The hosttest asserts the whole table for this.
 *
 * Rule 14: both answers are COUNTED, because a probe that never asked and a
 * probe that asked and rejected us look identical from the registry
 * otherwise. RBFBBlFlagsGets and RBFBBlSenseGets separate them, and
 * RBFBBlBuiltIn says which boot this was.
 */
IOReturn RBAMDFramebuffer::getAppleSense(IOIndex connectIndex,
                                         UInt32 *senseType,
                                         UInt32 *primary,
                                         UInt32 *extended,
                                         UInt32 *displayType)
{
    if (connectIndex != 0) {
        return kIOReturnBadArgument;
    }
    if (!blBuiltIn) {
        /* 2xs' answer: we have no DDC or 3-pin sense path to this panel, so
         * declining is the honest one when the match is not armed. */
        return kIOReturnUnsupported;
    }
    /* There is no sense hardware here and we are not pretending there is.
     * What is being reported is what the firmware's own modeset already
     * proves: one fixed-in-place TFT panel on connection 0. senseType 0 is
     * "not the Apple CRT sense codes", and primary/extended are the sense
     * lines, which do not exist - zero is the only truthful value. */
    if (senseType)   { *senseType = 0; }
    if (primary)     { *primary = 0; }
    if (extended)    { *extended = 0; }
    if (displayType) { *displayType = kPanelTFTConnect; }
    blSenseGets++;
    return kIOReturnSuccess;
}
