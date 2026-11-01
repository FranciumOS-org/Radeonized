/* RBAMDFramebufferDMUB.cpp - producing on DMCUB's inbox 1.
 *
 * WHY. Brightness has been four milestones of verified links with the last
 * link - the panel moving - never instrumented (rule 45). 2zl found the
 * reason the register work cannot work: on DCN 3.1 the CPU does not write
 * BL_PWM_CNTL to change brightness, the level goes to the DMCUB as an ABM
 * command, and 2xs had transcribed dce_panel_cntl.c's method onto a chip
 * whose offsets matched and whose OWNER is the microcontroller (rule 46).
 * Boot 271 then measured the ring free - WptrMoves 0, RptrMoves 0 across 125
 * samples, WPTR == RPTR == 2368 == 37 * 64, InboxSize 8192 - so there is no
 * second producer to race and the route is live.
 *
 * THIS IS A NEW HARDWARE-WRITE SURFACE and it is staged accordingly. Three
 * things happen in this file and they are separately armed, because they
 * carry very different risk and because all three have to be possible in ONE
 * boot:
 *
 *   1. LOCATE and SCAN, read-only, always. Derive where the ring is from the
 *      CW4 window registers, map it, and read the 37 commands the firmware
 *      itself wrote at bring-up. This is the control for the whole address
 *      derivation (rule 26): 37 plausible dmub_cmd_headers at a 64-byte
 *      stride is not what a wrong address produces. It is also the best
 *      evidence this project will get about what firmware 4.0.15.47 actually
 *      accepts, because those are its own commands - so the raw bytes are
 *      published, not just a verdict on them.
 *
 *   2. SET_BACKLIGHT, only with -rbgpufbdmubbl armed AND an explicit
 *      userspace trigger. Writing 64 bytes and one WPTR.
 *
 *   3. SET_PIPE, same arm, its own trigger. dcn21_set_backlight_level binds
 *      ABM to the pipe before setting a level, so a SET_BACKLIGHT alone may
 *      be ignored - but the firmware's bring-up may already have bound it,
 *      and SET_PIPE is the more invasive command. So it is not sent unasked:
 *      try the level alone, then bind and try again.
 *
 * Nothing here is sent from the slider path. A brightness change that
 * happens only when the user asks for it by name is a brightness change
 * whose cause is known; wiring it into the UI first would make the panel's
 * behaviour a function of two writes at once, which is how 2zj's gap
 * survived four milestones.
 *
 * Every decision - where the ring is, whether the registers agree, whether a
 * slot is free, the exact bytes - is in src/common/rb_dmub.h and asserted by
 * the hosttest (rule 38). This file maps memory and stores to it.
 */

#include "RBAMDFramebuffer.hpp"
#include "rb_dmub.h"
#include <IOKit/pci/IOPCIDevice.h>
#include <IOKit/IODeviceMemory.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IOLib.h>

/* The same DMU segment the BL_PWM block uses, and deliberately its own copy
 * for the same reason RBAMDFramebufferBacklight.cpp keeps one: the segment is
 * an assumption, and this path re-checks it through registers of its own
 * (INBOX1_BASE_ADDRESS reading exactly DMUB_CW4_BASE is a far stronger
 * segment check than a plausible value at a plausible offset). */
static const uint32_t kRbDmuSeg2Dmub = 0x34c0;

#define LOG(fmt, ...) IOLog("RBAMDFramebuffer: " fmt "\n", ## __VA_ARGS__)

void RBAMDFramebuffer::dmubInit(void)
{
    dmubLocRefuse = RB_DMUB_LOC_NO_MAP;
    dmubRingPA = 0;
    dmubRingOff = 0;
    dmubRingMD = nullptr;
    dmubRingMap = nullptr;
    dmubRing = nullptr;
    dmubRoute = 0;
    dmubScanSlots = 0;
    dmubScanPlaus = 0;
    dmubScanAbm = 0;
    dmubScanBytes = 0;
    dmubSendsOk = 0;
    dmubSendsRefused = 0;
    dmubSendRefuse = 0;
    dmubSendWptr = 0;
    dmubAcks = 0;
    dmubAckSpins = 0;
    dmubBlLast = -1;
    dmubPipeSends = 0;
    dmubBlSends = 0;
    for (unsigned i = 0; i < 8; i++) { dmubLocRegs[i] = 0; }
}

/* Read the window registers and decide. Called from blSetup()'s tail, where
 * blRegs is live and the segment has already been corroborated by OTG0 -
 * this adds eight reads to a mapping that exists. */
bool RBAMDFramebuffer::dmubSetup(void)
{
    struct rb_dmub_loc loc;

    if (!blReady || !blRegs) {
        dmubLocRefuse = RB_DMUB_LOC_NO_MAP;
        return false;
    }

    loc.inbox_base  = blRegs[kRbDmuSeg2Dmub + RB_DMUB_DW_INBOX1_BASE];
    loc.inbox_size  = blRegs[kRbDmuSeg2Dmub + RB_DMUB_DW_INBOX1_SIZE];
    loc.cw4_base    = blRegs[kRbDmuSeg2Dmub + RB_DMUB_DW_CW4_BASE_ADDRESS];
    loc.cw4_top     = blRegs[kRbDmuSeg2Dmub + RB_DMUB_DW_CW4_TOP_ADDRESS];
    loc.cw4_off_lo  = blRegs[kRbDmuSeg2Dmub + RB_DMUB_DW_CW4_OFFSET];
    loc.cw4_off_hi  = blRegs[kRbDmuSeg2Dmub + RB_DMUB_DW_CW4_OFFSET_HIGH];
    loc.fb_location = blRegs[kRbDmuSeg2Dmub + RB_DMUB_DW_FB_LOCATION_BASE];
    loc.fb_offset   = blRegs[kRbDmuSeg2Dmub + RB_DMUB_DW_FB_OFFSET];

    /* Published raw, before any judgement: a refusal code says which clause
     * fired and these say what it fired on, and the second is what a wrong
     * derivation would be diagnosed from (rule 14). The TOP_ADDRESS register
     * carries an ENABLE bit above the address, so the published value is the
     * register and not a cleaned-up address - cleaning it here would hide
     * whether CW4 is enabled at all. */
    dmubLocRegs[0] = loc.inbox_base;
    dmubLocRegs[1] = loc.inbox_size;
    dmubLocRegs[2] = loc.cw4_base;
    dmubLocRegs[3] = loc.cw4_top;
    dmubLocRegs[4] = loc.cw4_off_lo;
    dmubLocRegs[5] = loc.cw4_off_hi;
    dmubLocRegs[6] = loc.fb_location;
    dmubLocRegs[7] = loc.fb_offset;

    /* CW4_TOP_ADDRESS's low bits are the address and bit 31 the enable
     * (dcn31 writes them together with REG_SET_2), so the window check is
     * made against the address field alone while the published value keeps
     * the enable bit. 0x7fffffff is the full address field: masking wider
     * than the field would be inventing a layout, masking narrower would
     * refuse a legal window. */
    loc.cw4_top &= 0x7fffffffu;

    dmubLocRefuse = (uint32_t)rb_dmub_locate(&loc);
    if (dmubLocRefuse != RB_DMUB_LOC_OK) {
        LOG("DMUB inbox locate refused %u (inbox %#x/%#x cw4 %#x..%#x "
            "off %#x%08x fb %#x/%#x)", dmubLocRefuse, loc.inbox_base,
            loc.inbox_size, loc.cw4_base, loc.cw4_top, loc.cw4_off_hi,
            loc.cw4_off_lo, loc.fb_location, loc.fb_offset);
        return false;
    }

    dmubRingPA = rb_dmub_ring_pa(&loc);
    dmubRingOff = rb_dmub_ring_fb_off(&loc);

    /* Route 1, preferred: a sub-range of BAR0. 2pr measured the GOP
     * framebuffer at BAR0 offset 0, so BAR0 IS the carve-out seen through the
     * aperture and the ring's offset inside the carve-out is its offset
     * inside BAR0. This is device memory the PCI device owns, with the
     * ordering and cacheability IOKit already gets right for MMIO, and it
     * needs no physical-address mapping of our own.
     *
     * Route 2, fallback: map the system-physical address directly. BAR0's
     * length is whatever the firmware programmed and the mailbox is wherever
     * the firmware put it, so "inside BAR0" is a hope and not a fact - and a
     * ring we can locate but not reach would be the worst of the three
     * outcomes to misread as a locate failure. Which route was taken is
     * published. */
    if (pci) {
        IODeviceMemory *bar0 = pci->getDeviceMemoryWithRegister(kIOPCIConfigBaseAddress0);
        if (bar0) {
            const uint64_t len = bar0->getLength();
            if (dmubRingOff + RB_DMUB_RB_SIZE <= len) {
                IODeviceMemory *sub =
                    IODeviceMemory::withSubRange(bar0, dmubRingOff,
                                                 RB_DMUB_RB_SIZE);
                if (sub) {
                    IOMemoryMap *m = sub->map(kIOMapInhibitCache);
                    if (m) {
                        dmubRingMD = sub;
                        dmubRingMap = m;
                        dmubRoute = 1;
                    } else {
                        sub->release();
                    }
                }
            }
        }
    }

    if (!dmubRingMap) {
        IOMemoryDescriptor *md =
            IOMemoryDescriptor::withPhysicalAddress(dmubRingPA,
                                                    RB_DMUB_RB_SIZE,
                                                    kIODirectionOutIn);
        if (md) {
            IOMemoryMap *m = md->map(kIOMapInhibitCache);
            if (m) {
                dmubRingMD = md;
                dmubRingMap = m;
                dmubRoute = 2;
            } else {
                md->release();
            }
        }
    }

    if (!dmubRingMap) {
        dmubLocRefuse = RB_DMUB_LOC_MAP_FAILED;
        LOG("DMUB inbox at pa %#llx (fb+%#llx) located and NOT mappable",
            dmubRingPA, dmubRingOff);
        return false;
    }

    dmubRing = reinterpret_cast<volatile uint8_t *>(dmubRingMap->getVirtualAddress());
    if (!dmubRing) {
        dmubLocRefuse = RB_DMUB_LOC_MAP_FAILED;
        dmubRingMap->release(); dmubRingMap = nullptr;
        dmubRingMD->release(); dmubRingMD = nullptr;
        return false;
    }

    LOG("DMUB inbox: pa %#llx (fb+%#llx), %u bytes, route %u",
        dmubRingPA, dmubRingOff, RB_DMUB_RB_SIZE, dmubRoute);
    return true;
}

void RBAMDFramebuffer::dmubTeardown(void)
{
    dmubRing = nullptr;
    if (dmubRingMap) { dmubRingMap->release(); dmubRingMap = nullptr; }
    if (dmubRingMD)  { dmubRingMD->release();  dmubRingMD = nullptr; }
}

/* The CONTROL. Read every slot the firmware wrote and say how many of them
 * look like commands.
 *
 * The verdict is the pair (slots, plausible): equal and non-zero is the
 * address derivation confirmed, and anything else is it refuted. A count
 * alone would not do - rule 18 - and neither would a yes/no, because the
 * bytes themselves are the thing worth having: they are firmware
 * 4.0.15.47's own ABM traffic, which is the only direct evidence about the
 * command layout this chip accepts. So the whole rptr..wptr region is
 * published as RBFBDmubRingDump and the length beside it.
 *
 * Run once, from setup, on the way to a desktop - never from the frame path.
 * It allocates. */
void RBAMDFramebuffer::dmubScan(void)
{
    if (!dmubRing) { return; }

    const uint32_t rptr = blRegs[kRbDmuSeg2Dmub + RB_DMUB_DW_INBOX1_RPTR];
    const uint32_t wptr = blRegs[kRbDmuSeg2Dmub + RB_DMUB_DW_INBOX1_WPTR];

    if (!rb_dmub_ptr_ok(rptr, RB_DMUB_RB_SIZE) ||
        !rb_dmub_ptr_ok(wptr, RB_DMUB_RB_SIZE)) {
        /* Not legal ring positions, so there is nothing to scan and the
         * zeroes below are not a reading. Say so by leaving slots 0 with the
         * pointers published next to it. */
        return;
    }

    /* The producer's own history is [0, wptr) when the ring has not yet
     * wrapped, which boot 271 says is the case (rptr == wptr == 2368, both
     * having started there). Scanning from 0 rather than from rptr is
     * deliberate: rptr == wptr means "empty", so a scan of the OUTSTANDING
     * region would read nothing at all, and what is wanted is the commands
     * already consumed. If the ring has wrapped, wptr is still where the
     * producer stopped and [0, wptr) is still 64-byte slots of real
     * commands, just older ones. */
    const uint32_t bytes = (wptr == 0u) ? RB_DMUB_RB_SIZE : wptr;

    uint8_t *buf = (uint8_t *)IOMalloc(bytes);
    if (!buf) { return; }

    for (uint32_t i = 0; i < bytes; i++) { buf[i] = dmubRing[i]; }

    uint32_t slots = 0, plaus = 0, abm = 0;
    for (uint32_t off = 0; off + RB_DMUB_CMD_SIZE <= bytes;
         off += RB_DMUB_CMD_SIZE) {
        const uint32_t h = rb_dmub_get32(buf, off);
        slots++;
        if (rb_dmub_hdr_plausible(h)) { plaus++; }
        if (rb_dmub_hdr_type(h) == RB_DMUB_CMD_ABM) { abm++; }
    }

    dmubScanSlots = slots;
    dmubScanPlaus = plaus;
    dmubScanAbm = abm;
    dmubScanBytes = bytes;

    OSData *d = OSData::withBytes(buf, bytes);
    if (d) {
        setProperty("RBFBDmubRingDump", d);
        d->release();
    }
    IOFree(buf, bytes);

    LOG("DMUB ring scan: %u slots, %u plausible headers, %u ABM, %u bytes",
        slots, plaus, abm, bytes);
}

/* Push one 64-byte command and ring the only doorbell there is.
 *
 * dmub_srv.c:1037-1041 is the whole sequence: push into the ring, then
 * dmub_rb_flush_pending (which READS BACK every byte between rptr and wptr -
 * that is its entire body, dmub_cmd.h:8351-8370, a write-combine flush and
 * not a cache operation), then set_inbox1_wptr. There is no interrupt to
 * raise and no scratch register to poke.
 *
 * Returns the refusal code, 0 on success. Every refusal is counted and the
 * last one published: a send that did not happen and a send that happened
 * and did nothing must not read alike, which is the whole shape of 2zj. */
uint32_t RBAMDFramebuffer::dmubPush(const uint8_t *cmd)
{
    if (!dmubBlArmed) { dmubSendsRefused++; return dmubSendRefuse = 1; }
    if (!dmubRing || !blRegs) { dmubSendsRefused++; return dmubSendRefuse = 2; }
    if (dmubLocRefuse != RB_DMUB_LOC_OK) {
        dmubSendsRefused++; return dmubSendRefuse = 3;
    }

    const uint32_t wptr = blRegs[kRbDmuSeg2Dmub + RB_DMUB_DW_INBOX1_WPTR];
    const uint32_t rptr = blRegs[kRbDmuSeg2Dmub + RB_DMUB_DW_INBOX1_RPTR];

    if (!rb_dmub_ptr_ok(wptr, RB_DMUB_RB_SIZE) ||
        !rb_dmub_ptr_ok(rptr, RB_DMUB_RB_SIZE)) {
        dmubSendsRefused++; return dmubSendRefuse = 4;
    }
    if (rb_dmub_rb_full(wptr, rptr, RB_DMUB_RB_SIZE)) {
        dmubSendsRefused++; return dmubSendRefuse = 5;
    }

    /* 1. The command, at WPTR. */
    for (uint32_t i = 0; i < RB_DMUB_CMD_SIZE; i++) {
        dmubRing[wptr + i] = cmd[i];
    }

    /* 2. dmub_rb_flush_pending: read it back. The reads are what flush the
     * store buffer; the values are deliberately not checked against what was
     * written, because this mapping is uncached device memory and a readback
     * comparison here would be testing the mapping rather than the flush -
     * the thing that proves the command arrived is RPTR moving, below. */
    {
        volatile uint32_t sink = 0;
        for (uint32_t i = 0; i < RB_DMUB_CMD_SIZE; i += 4) {
            sink += rb_dmub_get32((const uint8_t *)(dmubRing + wptr + i), 0);
        }
        (void)sink;
    }

    /* 3. The doorbell. */
    const uint32_t next = rb_dmub_next_wptr(wptr, RB_DMUB_RB_SIZE);
    blRegs[kRbDmuSeg2Dmub + RB_DMUB_DW_INBOX1_WPTR] = next;
    dmubSendWptr = next;
    dmubSendsOk++;

    /* 4. Did the microcontroller take it? RPTR catching up to our WPTR is
     * the only acknowledgement in this protocol, and it is a far stronger
     * reading than the store having executed (rule 45's lesson applied
     * before, not after: the panel is still the deliverable, but "the
     * firmware consumed the command" is a measurable intermediate and
     * without it a dark panel has two explanations). Bounded spin, counted,
     * never retried - this can be called from a gated context. */
    {
        unsigned spins = 1000;                /* 1 ms at 1 us a turn */
        while (spins--) {
            if (blRegs[kRbDmuSeg2Dmub + RB_DMUB_DW_INBOX1_RPTR] == next) {
                dmubAcks++;
                break;
            }
            IODelay(1);
        }
        if (spins == (unsigned)-1) { dmubAckSpins++; }
    }

    dmubSendRefuse = 0;
    return 0;
}

uint32_t RBAMDFramebuffer::dmubSendBacklight(uint32_t level)
{
    uint8_t cmd[RB_DMUB_CMD_SIZE];

    /* panel_mask bit 0 = panel control instance 0. This machine has one eDP
     * and PWRSEQ0 is the block every previous milestone read and wrote, so
     * instance 0 is measured rather than assumed. frame_ramp 0 is what
     * Linux passes for an immediate change; a ramp would make the panel's
     * response a function of a number nothing here can observe. */
    rb_dmub_build_set_backlight(cmd, level, 0u, 0x01u);
    dmubBlLast = (long)level;
    dmubBlSends++;
    return dmubPush(cmd);
}

uint32_t RBAMDFramebuffer::dmubSendPipe(void)
{
    uint8_t cmd[RB_DMUB_CMD_SIZE];

    /* otg_inst 0: OTG0 is the one running pipe, which gate 1 of the
     * backlight path has checked on every boot since 2xp. panel_inst 0 and
     * pwrseq_inst 0 for the same reason panel_mask is 1. */
    rb_dmub_build_set_pipe(cmd, 0u, 0u, 0u, RB_DMUB_SET_ABM_PIPE_NORMAL);
    dmubPipeSends++;
    return dmubPush(cmd);
}

void RBAMDFramebuffer::dmubPublish(void)
{
    setProperty("RBFBDmubBlArmed", dmubBlArmed);
    setProperty("RBFBDmubLocRefuse", (uint64_t)dmubLocRefuse, 32);
    setProperty("RBFBDmubRoute", (uint64_t)dmubRoute, 32);
    setProperty("RBFBDmubRingPA", dmubRingPA, 64);
    setProperty("RBFBDmubRingFbOff", dmubRingOff, 64);
    setProperty("RBFBDmubCw4Base", (uint64_t)dmubLocRegs[2], 32);
    setProperty("RBFBDmubCw4Top", (uint64_t)dmubLocRegs[3], 32);
    setProperty("RBFBDmubCw4OffLo", (uint64_t)dmubLocRegs[4], 32);
    setProperty("RBFBDmubCw4OffHi", (uint64_t)dmubLocRegs[5], 32);
    setProperty("RBFBDmubFbLocation", (uint64_t)dmubLocRegs[6], 32);
    setProperty("RBFBDmubFbOffset", (uint64_t)dmubLocRegs[7], 32);
    /* The control, and its denominator beside it. */
    setProperty("RBFBDmubScanSlots", (uint64_t)dmubScanSlots, 32);
    setProperty("RBFBDmubScanPlaus", (uint64_t)dmubScanPlaus, 32);
    setProperty("RBFBDmubScanAbm", (uint64_t)dmubScanAbm, 32);
    setProperty("RBFBDmubScanBytes", (uint64_t)dmubScanBytes, 32);
    /* The writes. */
    setProperty("RBFBDmubSendsOk", dmubSendsOk, 64);
    setProperty("RBFBDmubSendsRefused", dmubSendsRefused, 64);
    setProperty("RBFBDmubSendRefuse", (uint64_t)dmubSendRefuse, 32);
    setProperty("RBFBDmubSendWptr", (uint64_t)dmubSendWptr, 32);
    setProperty("RBFBDmubAcks", dmubAcks, 64);
    setProperty("RBFBDmubAckTimeouts", dmubAckSpins, 64);
    setProperty("RBFBDmubBlSends", dmubBlSends, 64);
    setProperty("RBFBDmubPipeSends", dmubPipeSends, 64);
    setProperty("RBFBDmubBlLast", (uint64_t)(uint32_t)dmubBlLast, 32);
}
