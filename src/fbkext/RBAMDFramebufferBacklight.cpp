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

extern "C" {
#include "rb_fbmode.h"
}
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

/* 2zl: DMCUB's inbox-1 ring, dcn_3_1_2_offset.h:1015-1022, base_idx 2 - the
 * same segment and the same read-only mapping as the BL_PWM block above, and
 * at a far lower dword than BL_PWM_GRP1_REG_LOCK, so the bound blSetup()
 * already checks covers them with nothing new to map.
 *
 * WHY THESE AND NOT MORE PWM REGISTERS. 2zk read dcn31_panel_cntl.c: on
 * DCN 3.1 the CPU does not write BL_PWM_CNTL to change brightness. The whole
 * of `dcn31_link_panel_cntl_funcs` is DMUB round trips and it has NO
 * .set_backlight_level at all; dcn21_set_backlight_level takes the
 * register-writing dce110 path only `if (dc->dc->res_pool->dmcu)`, which is
 * the old DMCU, and otherwise ends at `dmub_abm_set_backlight()`. So the
 * level lives in the DMCUB and the route to it is a command on this ring.
 * Before any of that can be considered, ONE thing has to be known: whether
 * anything else is producing on it. These four say so, read-only. */
#define RB_DMUB_DW_INBOX1_BASE  0x01d4u
#define RB_DMUB_DW_INBOX1_SIZE  0x01d5u
#define RB_DMUB_DW_INBOX1_WPTR  0x01d6u
#define RB_DMUB_DW_INBOX1_RPTR  0x01d7u

/* OTG0, for the segment check. 2xp established that OTG0's master-enable is
 * the clause that corroborates the segment and that OTG1-3 do NOT - a live
 * OTG is a plausible value at a plausible offset - so only OTG0 is used
 * here, and an HDMI-attached boot cannot refuse the backlight. */
#define RB_BL_OTG0_DW        0x1b41u
#define RB_BL_OTG_MASTER_EN  0x1u

/* 2y4: OTG0's timing and DRR group, READ-ONLY. dcn_3_1_2_offset.h:8767-8776
 * and 0x1b2a/0x1b36/0x1b4c, base_idx 2; the same values rb_regs.h carries and
 * the hosttest cross-checks. Read here because the framebuffer already has
 * this exact BAR5 map and has already proved the segment with OTG0_CONTROL,
 * so the values cost one boot that was happening anyway rather than one of
 * their own. Nothing writes them: changing a refresh rate means writing
 * V_TOTAL_MIN/MAX, and what the current values of this group ARE is the thing
 * that has to be known before a restore can be written at all. */
#define RB_OTG0_DW_H_TOTAL        0x1b2au
#define RB_OTG0_DW_V_TOTAL        0x1b2fu
#define RB_OTG0_DW_V_TOTAL_MIN    0x1b30u
#define RB_OTG0_DW_V_TOTAL_MAX    0x1b31u
#define RB_OTG0_DW_V_TOTAL_MID    0x1b32u
#define RB_OTG0_DW_V_TOTAL_CNTL   0x1b33u
#define RB_OTG0_DW_V_BLANK_SE     0x1b36u
/* 3fb: rb_regs.h:122, base_idx 2, the H axis of the pair already read below. */
#define RB_OTG0_DW_H_BLANK_SE     0x1b2bu
#define RB_OTG0_DW_FRAME_COUNT    0x1b4cu

/* Bounded wait for BL_PWM_GRP1_REG_UPDATE_PENDING to clear. Linux waits
 * 1 us x 10000; this runs on IOGraphics' gated workloop, so it is bounded
 * far tighter and a timeout is COUNTED rather than retried (rule 14: a path
 * that gives up must say so). The measured lock register has
 * BL_PWM_GRP1_UPDATE_AT_FRAME_START clear, so the update is immediate and
 * not frame-synced - a frame-synced update would need ~7 ms, which is why
 * this is worth stating rather than assuming. */
#define RB_BL_PENDING_SPINS  2000u

/* 2y4: zeroed from start(), not from blSetup(), because blSetup() does not
 * run at all on a `-rbgpunofbbacklight` boot and these are published
 * unconditionally - uninitialised memory in a published property is
 * indistinguishable from a reading (rule 14, and rule 19's "publish the value
 * you refused"). Zero is a value this group can never legitimately hold: a
 * live raster cannot have H_TOTAL 0. */
void RBAMDFramebuffer::otgTimingReset(void)
{
    otgHTotal = 0;
    otgVTotal = 0;
    otgVTotalMin = 0;
    otgVTotalMax = 0;
    otgVTotalMid = 0;
    otgVTotalCntl = 0;
    otgVBlankSE = 0;
    otgFrameCount0 = 0;
}

bool RBAMDFramebuffer::blSetup(void)
{
    blRefuse = 0;
    blReady = false;
    geoOk = false; geoWhy = 0; geoRowSrc = 0;
    otgHBlankSE = 0;
    blMap = nullptr;
    blRegs = nullptr;
    blSets = 0;
    blGets = 0;
    blLastCount = 0;
    blReadback = 0;
    blLiveCntl = 0;
    blLiveLock = 0;
    blLiveCount = 0;
    blLiveReads = 0;
    blLiveDiffs = 0;
    dmubInboxBase = 0;
    dmubInboxSize = 0;
    dmubWptr0 = 0;
    dmubRptr0 = 0;
    dmubWptr = 0;
    dmubRptr = 0;
    dmubReads = 0;
    dmubWptrMoves = 0;
    dmubRptrMoves = 0;
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

    /* 2y4: sampled here, before gate 1's refusal, so they are published even
     * on a boot where the backlight declines - a refusal tells us nothing
     * about the raster, and these are the numbers a refresh-rate change has
     * to start from. */
    otgHTotal      = regs[kRbDmuSeg2Bl + RB_OTG0_DW_H_TOTAL];
    otgVTotal      = regs[kRbDmuSeg2Bl + RB_OTG0_DW_V_TOTAL];
    otgVTotalMin   = regs[kRbDmuSeg2Bl + RB_OTG0_DW_V_TOTAL_MIN];
    otgVTotalMax   = regs[kRbDmuSeg2Bl + RB_OTG0_DW_V_TOTAL_MAX];
    otgVTotalMid   = regs[kRbDmuSeg2Bl + RB_OTG0_DW_V_TOTAL_MID];
    otgVTotalCntl  = regs[kRbDmuSeg2Bl + RB_OTG0_DW_V_TOTAL_CNTL];
    otgVBlankSE    = regs[kRbDmuSeg2Bl + RB_OTG0_DW_V_BLANK_SE];
    otgHBlankSE    = regs[kRbDmuSeg2Bl + RB_OTG0_DW_H_BLANK_SE];
    otgFrameCount0 = regs[kRbDmuSeg2Bl + RB_OTG0_DW_FRAME_COUNT];

    /* 3fb (issue #4): derive the framebuffer geometry here, for the same
     * reason 2y4 sampled the raster here - before any gate below refuses,
     * because a backlight refusal says nothing about the raster, and because
     * start() needs this before it builds the aperture at line ~905 and
     * before IOFramebuffer::start(). hubpSetup() has already run (one call
     * earlier in start()), so hubpPitch is in hand if it read.
     *
     * THE STRIDE FALLS BACK, THE SIZE DOES NOT. rb_fbmode.h is strict and
     * refuses a pitch below the active width, which a failed hubpSetup would
     * produce as pitch 0. Letting that decline the panel would make taking
     * the panel newly dependent on hubpSetup succeeding - a coupling this
     * file deliberately does not have ("independent of hubpSetup's result by
     * design", start():828). So when the HUBP pitch is unusable the stride is
     * assumed to be the active width, which is what this board measures
     * anyway (1920 px, rows unpadded, checked against IOFBMemorySize), and
     * geoRowSrc says which happened. Width and height are the defect in #4
     * and they come from the OTG either way; the stride is a refinement that
     * only matters on a panel whose rows are padded. */
    {
        struct rb_fbmode_in fin;
        struct rb_fbmode_out fout;
        uint32_t wActive = rb_fbmode_active(otgHBlankSE, NULL, NULL);

        memset(&fin, 0, sizeof(fin));
        fin.h_total           = otgHTotal;
        fin.v_total           = otgVTotal;
        fin.h_blank_start_end = otgHBlankSE;
        fin.v_blank_start_end = otgVBlankSE;
        if (hubpPitch != 0u && ((hubpPitch & 0xffffu) + 1u) >= wActive) {
            fin.surface_pitch = hubpPitch;
            geoRowSrc         = 1;
        } else {
            fin.surface_pitch = (wActive > 0u) ? (wActive - 1u) : 0u;
            geoRowSrc         = 2;
        }

        memset(&fout, 0, sizeof(fout));
        geoWhy = rb_fbmode_derive(&fin, &fout);
        geoOk  = (geoWhy == RB_FBMODE_OK);
        if (geoOk) {
            geoW       = fout.width;
            geoH       = fout.height;
            geoRow     = fout.bytes_per_row;
            geoAper    = fout.aperture_bytes;
            geoPitchPx = fout.pitch_px;
            geoPadded  = fout.padded;
            /* No log line here: this file has no logging macro, and rule 31
             * says IOLog is not an instrument - what makes this readable is
             * RBFBGeoW/H/Row/Aper/PitchPx/Padded/RowSrc/Why/Ok, published
             * from the VBL path, plus the raw otgHBlankSE/otgVBlankSE and
             * hubpPitch that were already published beside them. A refused
             * derivation is readable the same way, and start() publishes
             * RBFBGeoWhy itself before declining so the code survives even a
             * boot where the periodic publish never runs. */
        }
    }

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

    /* 2zn: now that the segment is corroborated and the mapping is live,
     * locate DMCUB's inbox-1 ring and read what the firmware put in it.
     * Read-only, unconditional, and NOT gated on -rbgpufbdmubbl: the whole
     * question 2zl left open is whether the address derivation is right, and
     * the firmware's own 37 bring-up commands are the answer. A boot that
     * refuses to WRITE still gets to read them. Both calls publish their own
     * refusals and neither can fail the backlight. */
    if (dmubSetup()) { dmubScan(); }

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
    /* Before blRegs goes: the ring mapping is independent of the BL_PWM
     * mapping but its pointers are read through blRegs, so it is released
     * first. Nothing is sent on the way out - a backlight command to a
     * microcontroller that outlives this kext would be a change we could not
     * then undo. */
    dmubTeardown();

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

/* 2zi: what the register holds BETWEEN our writes.
 *
 * blReadback is taken microseconds after our own store, so it can only say
 * the store reached the chip; blCntl is a start() snapshot and never moves.
 * Neither can see a second writer, and boot 269 needs exactly that: the duty
 * cycle round-trips and the panel does not move. Called from the periodic
 * publish (every 288 vblanks, ~2 s), which already does read-only MMIO in
 * hubpPoll(), so this adds a read and no new context.
 *
 * blLiveDiffs only counts once we have written at least once, because before
 * that the live value IS the firmware's and disagreeing with a zero
 * blLastCount would be an artefact of the counter rather than a finding
 * (rule 14: say what would have to be true for the reading to mean
 * anything). */
void RBAMDFramebuffer::blSampleLive(void)
{
    if (!blReady || !blRegs) { return; }

    blLiveCntl  = blRegs[kRbDmuSeg2Bl + RB_BL_DW_CNTL];
    blLiveLock  = blRegs[kRbDmuSeg2Bl + RB_BL_DW_LOCK];
    blLiveCount = blLiveCntl & RB_BL_CNTL_ACTIVE_CNT_MASK;
    blLiveReads++;
    if (blSets > 0 && blLiveCount != blLastCount) { blLiveDiffs++; }

    /* 2zl: is anything producing on DMCUB's inbox 1?
     *
     * A ring whose WPTR never moves after bring-up has one producer at most,
     * and 2zk's read of Apple's own binary says which: nm over
     * AMDRadeonX6000Framebuffer finds `initializeDmcuB` and two DMCUB
     * VERSION reads and NO abm/backlight symbol of any kind (3314 symbols,
     * so the empty result is against a known-present control, rule 26) -
     * Apple boots the DMCUB on this part and never sends it an ABM command.
     * If that is right, dmubWptrMoves stays at zero for the whole boot and
     * the ring is free. If it moves, we would be a SECOND producer on it and
     * that route is closed rather than merely difficult - which is a fact
     * worth having before writing anything, not after (rule: find the
     * consumer before writing to a field).
     *
     * Published whether or not it moved, with the first value kept beside
     * the current one so "never moved" and "we never looked" are different
     * readings (rule 14). */
    {
        const uint32_t w = blRegs[kRbDmuSeg2Bl + RB_DMUB_DW_INBOX1_WPTR];
        const uint32_t r = blRegs[kRbDmuSeg2Bl + RB_DMUB_DW_INBOX1_RPTR];
        if (dmubReads == 0) {
            dmubInboxBase = blRegs[kRbDmuSeg2Bl + RB_DMUB_DW_INBOX1_BASE];
            dmubInboxSize = blRegs[kRbDmuSeg2Bl + RB_DMUB_DW_INBOX1_SIZE];
            dmubWptr0 = w;
            dmubRptr0 = r;
        } else {
            if (w != dmubWptr) { dmubWptrMoves++; }
            if (r != dmubRptr) { dmubRptrMoves++; }
        }
        dmubWptr = w;
        dmubRptr = r;
        dmubReads++;
    }
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
