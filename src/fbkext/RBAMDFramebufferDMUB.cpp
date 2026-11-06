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
#include "rb_dpalt.h"
#include "rb_dcn_deny.h"
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
    dmubWin = -1;
    dmubLayout = 0;
    dmubSigOk = 0;
    dmubAddrKind = RB_DMUB_ADDR_NONE;
    dmubMem = 0;
    dmubRingPA = 0;
    dmubRingOff = 0;
    dmubCap = 0;
    dmubRingMD = nullptr;
    dmubRingMap = nullptr;
    dmubRing = nullptr;
    dmubRoute = 0;
    dmubScanSlots = 0;
    dmubScanPlaus = 0;
    dmubScanAbm = 0;
    dmubScanBad = 0;
    dmubScanWin = -1;
    /* 2DC */
    dmubDomSends = 0;
    dmubDomRefuse = 0;
    dmubDomInst = 0;
    dmubDomGate = 0;
    dmubDomMoved = 0;
    dmubDomRan = 0;
    dmubDomCalls = 0;
    /* 2EA */
    dpaltqVerdict = 0;
    dpaltqUsbMask = 0;
    dpaltqFill = 0;
    dpaltqRetCleared = 0;
    dpaltqCalls = 0;
    dpaltqSends = 0;
    for (unsigned i = 0; i < RB_DPALTQ_PHY_COUNT; i++) {
        dpaltqRefuse[i] = RB_DPALTQ_NOT_ARMED;
        dpaltqReply[i].phy_id = 0;
        dpaltqReply[i].is_usb = 0;
        dpaltqReply[i].is_dp_alt_disable = 0;
        dpaltqReply[i].is_dp4 = 0;
    }
    /* 2E2 */
    digCalls = 0;
    digSends = 0;
    digRan = 0;
    digRefuse = 0;
    digPhy = 0;
    digBefore = 0;
    digAfter = 0;
    digRefWord = 0;
    digVerdict = 0;
    /* 2E5. digEnc defaults ON: 2E4 already took the no-encoder reading, so
     * the useful default is the configured one, with `subret`-style A/B
     * reachable in the same boot via RBFBDigEnc. */
    digEnc = true;
    encInst = 0;
    encRefuse = 0;
    encRan = 0;
    encCfgBefore = 0;
    encCfgAfter = 0;
    encScramBefore = 0;
    encScramAfter = 0;
    encApplied = 0;
    encChanged = 0;
    encSurveyMask = 0;
    encSurveyDone = false;
    digbeMask = 0;
    digbeEnMask = 0;
    digbeDone = false;
    digbeDiffMask = 0;
    digbeEnDiffMask = 0;
    digbePostDone = false;
    for (unsigned i = 0; i < RB_DIGBE_COUNT; i++) {
        digbeWord[i] = 0;
        digbeEnWord[i] = 0;
        digbePost[i] = 0;
        digbeEnPost[i] = 0;
    }
    for (unsigned i = 0; i < RB_ENC_COUNT; i++) {
        encSurveyCfg[i] = 0; encSurveyScram[i] = 0;
    }
    pgSamples = 0;
    for (unsigned i = 0; i < RB_PG_DOMAIN_COUNT; i++) {
        pgStatus[i] = 0; pgConfig[i] = 0; pgBefore[i] = 0; pgAfter[i] = 0;
    }
    dmubScanBytes = 0;
    dmubSendsOk = 0;
    dmubSendsRefused = 0;
    dmubSendRefuse = 0;
    dmubSendWptr = 0;
    dmubAcks = 0;
    dmubAckSpins = 0;
    dmubBlLast = -1;
    dmubBlType = RB_DMUB_BL_CONTROL_PWM;
    dmubBlAuxInst = 0u;
    dmubBlTypeSets = 0u;
    dmubPipeSends = 0;
    dmubBlSends = 0;
    dmubCapsSends = 0;
    dmubReplyVerdict = RB_DMUB_REPLY_NONE;
    dmubReplySlot = 0;
    dmubConfirms = 0;
    dmubWinSel = 0;
    dmubSelSets = 0;
    dmubInboxInR3 = 0;
    for (unsigned i = 0; i < 9u; i++) { dmubRegRegs[i] = 0; dmubRegScan[i] = 0; }
    for (unsigned i = 0; i < RB_DMUB_WINSEL_COUNT; i++) {
        dmubCand[i].md = nullptr; dmubCand[i].map = nullptr;
        dmubCand[i].mem = nullptr; dmubCand[i].pa = 0;
        dmubCand[i].memAddr = 0; dmubCand[i].span = 0;
        dmubCand[i].route = 0; dmubCand[i].kind = 0;
        dmubCand[i].refuse = RB_DMUB_LOC_NO_WINDOW;
    }
    dmubGpintSends = 0;
    dmubGpintRefuse = 0;
    dmubGpintSent = 0;
    dmubGpintBack = 0;
    dmubGpintAcked = 0;
    dmubGpintSpins = 0;
    dmubScratch0 = 0; dmubScratch7 = 0; dmubDataout = 0;
    dmubScratch0Pre = 0; dmubScratch7Pre = 0;
    for (unsigned i = 0; i < RB_DMUB_CMD_SIZE; i++) {
        dmubReplySent[i] = 0;
        dmubReplyGot[i] = 0;
    }
    dmubInbox[0] = 0; dmubInbox[1] = 0;
    dmubFb[0] = 0; dmubFb[1] = 0;
    for (unsigned i = 0; i < RB_DMUB_CW_COUNT * 4u; i++) {
        dmubWinRegs[i] = 0;
        dmubWinScan[i] = 0;
    }
}

/* Map one window's memory, read-only as far as this function is concerned.
 *
 * Route 1, preferred: a sub-range of BAR0. 2pr measured the GOP framebuffer
 * at BAR0 offset 0, so BAR0 IS the carve-out seen through the aperture and
 * the ring's offset inside the carve-out is its offset inside BAR0 - device
 * memory the PCI device owns, with the ordering and cacheability IOKit
 * already gets right for MMIO.
 *
 * Route 2, fallback: map the system-physical address directly. BAR0's length
 * is whatever the firmware programmed and the mailbox is wherever the
 * firmware put it, so "inside BAR0" is a hope and not a fact - and a window
 * we can resolve but not reach would be the worst of the three outcomes to
 * misread as a resolve failure. Which route was taken is published. */
uint32_t RBAMDFramebuffer::dmubMapWin(uint64_t pa, uint64_t fbOff,
                                      uint32_t span,
                                      IOMemoryDescriptor **mdOut,
                                      IOMemoryMap **mapOut)
{
    *mdOut = nullptr;
    *mapOut = nullptr;

    if (pci) {
        IODeviceMemory *bar0 = pci->getDeviceMemoryWithRegister(kIOPCIConfigBaseAddress0);
        if (bar0 && fbOff + span <= bar0->getLength()) {
            IODeviceMemory *sub = IODeviceMemory::withSubRange(bar0, fbOff, span);
            if (sub) {
                IOMemoryMap *m = sub->map(kIOMapInhibitCache);
                if (m) { *mdOut = sub; *mapOut = m; return 1; }
                sub->release();
            }
        }
    }

    IOMemoryDescriptor *md =
        IOMemoryDescriptor::withPhysicalAddress(pa, span, kIODirectionOutIn);
    if (md) {
        IOMemoryMap *m = md->map(kIOMapInhibitCache);
        if (m) { *mdOut = md; *mapOut = m; return 2; }
        md->release();
    }
    return 0;
}

/* Count the leading run of plausible command headers, and whether anything
 * breaks it. See rb_dmub_sig_ok() for what the pair means and why.
 *
 * `run` is how many consecutive 64-byte slots from offset 0 read as
 * dmub_cmd_headers; `bad` counts slots AFTER that run which are neither a
 * header nor empty - a ring written once and never wrapped has none, and a
 * window of unrelated data almost certainly does. */
void RBAMDFramebuffer::dmubScanRun(volatile uint8_t *mem, uint32_t span,
                                   uint32_t *run, uint32_t *bad, uint32_t *abm)
{
    uint8_t slot[RB_DMUB_CMD_SIZE];
    uint32_t off, inRun = 1;

    *run = 0; *bad = 0; *abm = 0;

    for (off = 0; off + RB_DMUB_CMD_SIZE <= span; off += RB_DMUB_CMD_SIZE) {
        for (uint32_t i = 0; i < 4u; i++) { slot[i] = mem[off + i]; }
        const uint32_t h = rb_dmub_get32(slot, 0);

        if (rb_dmub_hdr_plausible(h)) {
            if (inRun) {
                (*run)++;
                if (rb_dmub_hdr_type(h) == RB_DMUB_CMD_ABM) { (*abm)++; }
            } else {
                (*bad)++;
            }
        } else if (h == 0u) {
            inRun = 0;                 /* unwritten memory - the run ended */
        } else {
            inRun = 0;
            (*bad)++;
        }
    }
}

/* Find the mailbox by LOOKING for it.
 *
 * Called from blSetup()'s tail, where blRegs is live and the segment has
 * already been corroborated by OTG0. Read-only throughout: it maps each
 * candidate window, counts headers, and unmaps everything but the winner.
 *
 * WHY A SEARCH. 2zn named the window (CW4, from dmub_srv.c) and boot 272
 * refuted it on the first read; the correction then found that 2zn's
 * DMUB_CW4_BASE was 0x80000000 where dmub_srv.c:84 says 0x64000000, a
 * constant taken from the register it was meant to be checked against. Two
 * conventions wrong in two milestones is enough: this names nothing and
 * believes nothing. The firmware wrote 37 commands into the real mailbox at
 * bring-up (boot 271: WPTR == RPTR == 2368 == 37 * 64), and those commands
 * are a signature no other window should carry. */
bool RBAMDFramebuffer::dmubSetup(void)
{
    struct rb_dmub_loc loc;
    struct rb_dmub_loc_out out, best;

    if (!blReady || !blRegs) {
        dmubLocRefuse = RB_DMUB_LOC_NO_MAP;
        return false;
    }

    loc.inbox_base  = blRegs[kRbDmuSeg2Dmub + RB_DMUB_DW_INBOX1_BASE];
    loc.inbox_size  = blRegs[kRbDmuSeg2Dmub + RB_DMUB_DW_INBOX1_SIZE];
    loc.fb_location = blRegs[kRbDmuSeg2Dmub + RB_DMUB_DW_FB_LOCATION_BASE];
    loc.fb_offset   = blRegs[kRbDmuSeg2Dmub + RB_DMUB_DW_FB_OFFSET];

    for (unsigned i = 0; i < RB_DMUB_CW_COUNT; i++) {
        loc.cw[i].base   = blRegs[kRbDmuSeg2Dmub + RB_DMUB_DW_CW_BASE_ADDRESS + i];
        loc.cw[i].top    = blRegs[kRbDmuSeg2Dmub + RB_DMUB_DW_CW_TOP_ADDRESS + i];
        loc.cw[i].off_lo = blRegs[kRbDmuSeg2Dmub + RB_DMUB_DW_CW_OFFSET + 2u * i];
        loc.cw[i].off_hi = blRegs[kRbDmuSeg2Dmub + RB_DMUB_DW_CW_OFFSET + 2u * i + 1u];
        dmubWinRegs[i * 4u + 0u] = loc.cw[i].base;
        dmubWinRegs[i * 4u + 1u] = loc.cw[i].top;
        dmubWinRegs[i * 4u + 2u] = loc.cw[i].off_lo;
        dmubWinRegs[i * 4u + 3u] = loc.cw[i].off_hi;
    }

    /* Published raw, before any judgement - TOP values keeping their enable
     * bit and BASE values their untruncated form, because the whole table is
     * what refuted 2zn and a search that published only its own winner would
     * have had nothing to say (rule 14). The inbox pair goes out too: it is
     * no longer used to locate anything, and 0x80000000 being neither
     * 0x64000000 nor its 29-bit truncation is an open finding, not noise. */
    dmubInbox[0] = loc.inbox_base;
    dmubInbox[1] = loc.inbox_size;
    dmubFb[0] = loc.fb_location;
    dmubFb[1] = loc.fb_offset;

    /* THE SELECTOR IS THE LAYOUT, not the contents. Boot 273 read six of the
     * seven windows exactly where dmub_srv.c:81-86 puts them (after the
     * 29-bit truncation), which identifies this firmware's layout as Linux's
     * and therefore CW4 as the mailbox. 2zo's byte signature chose CW5 -
     * DMUB_WINDOW_5_TRACEBUFF - because a trace buffer is dense records and
     * so passes any weak "looks like a header" test, and that mistake put 64
     * bytes into the trace buffer. */
    dmubLayout = rb_dmub_layout_score(&loc);
    const int want = rb_dmub_mailbox_win(&loc);

    /* Every enabled window is still resolved and READ, because the table is
     * the evidence and a chosen window published alone says nothing (rule
     * 14) - and because the per-window runs are what showed CW5 was dense
     * and CW4 was empty. Only the one the layout names is kept. */
    best.refuse = RB_DMUB_LOC_NO_WINDOW;
    best.win = -1;

    for (unsigned i = 0; i < RB_DMUB_CW_COUNT; i++) {
        const int rc = rb_dmub_win_locate(&loc, i, &out);
        uint32_t run = 0, bad = 0, abm = 0, route = 0;

        dmubWinScan[i * 4u + 0u] = (uint32_t)rc;
        dmubWinScan[i * 4u + 1u] = 0;
        dmubWinScan[i * 4u + 2u] = 0;
        dmubWinScan[i * 4u + 3u] = 0;
        if (rc != RB_DMUB_LOC_OK) { continue; }

        IOMemoryDescriptor *md = nullptr;
        IOMemoryMap *map = nullptr;
        const uint64_t fbOff = rb_dmub_ring_fb_off(&loc, &out);
        route = dmubMapWin(out.ring_pa, fbOff, out.span, &md, &map);
        if (!route || !map) {
            dmubWinScan[i * 4u + 0u] = RB_DMUB_LOC_MAP_FAILED;
            continue;
        }

        volatile uint8_t *mem =
            reinterpret_cast<volatile uint8_t *>(map->getVirtualAddress());
        if (mem) { dmubScanRun(mem, out.span, &run, &bad, &abm); }

        dmubWinScan[i * 4u + 1u] = run;
        dmubWinScan[i * 4u + 2u] = bad;
        dmubWinScan[i * 4u + 3u] = abm;

        /* 2zs: EVERY resolvable window is kept mapped, not just the one
         * the layout names. Boot 275 wrote a caps query into CW4 and the
         * firmware never cleared ret_status, and INBOX1_BASE_ADDRESS says
         * why: 0x80000000 is outside the whole region-3 range, so the
         * mailbox is behind a fixed-base aperture and CW4 was never it. The
         * caps query is a working byte-level test - it only ever needed the
         * right address - so one boot can try each candidate. */
        dmubCand[i].md = md; dmubCand[i].map = map; dmubCand[i].mem = mem;
        dmubCand[i].pa = out.ring_pa; dmubCand[i].memAddr = out.dmub_mem;
        dmubCand[i].span = out.span; dmubCand[i].route = route;
        dmubCand[i].kind = (uint32_t)out.addr_kind;
        dmubCand[i].refuse = RB_DMUB_LOC_OK;

        if (mem && (int)i == want) {
            best = out;
            best.refuse = RB_DMUB_LOC_OK;
        }
    }

    /* Regions 4, 5 and 6: offset and size only, no base register, because
     * their DMCUB-space base is fixed in hardware. dmub_dcn31_setup_windows
     * programs REGION5_OFFSET and REGION5_TOP_ADDRESS and never a base,
     * which is the shape of an aperture rather than a relocatable window -
     * and one of these three is the aperture at 0x80000000. */
    {
        static const uint32_t offLo[3] = { RB_DMUB_DW_REGION4_OFFSET,
                                           RB_DMUB_DW_REGION5_OFFSET,
                                           RB_DMUB_DW_REGION6_OFFSET };
        static const uint32_t offHi[3] = { RB_DMUB_DW_REGION4_OFFSET_HI,
                                           RB_DMUB_DW_REGION5_OFFSET_HI,
                                           RB_DMUB_DW_REGION6_OFFSET_HI };
        static const uint32_t topR[3]  = { RB_DMUB_DW_REGION4_TOP,
                                           RB_DMUB_DW_REGION5_TOP,
                                           RB_DMUB_DW_REGION6_TOP };
        for (unsigned r = 0; r < 3u; r++) {
            const unsigned slot = RB_DMUB_WINSEL_REGION4 + r;
            const uint32_t lo = blRegs[kRbDmuSeg2Dmub + offLo[r]];
            const uint32_t hi = blRegs[kRbDmuSeg2Dmub + offHi[r]];
            const uint32_t tp = blRegs[kRbDmuSeg2Dmub + topR[r]];
            struct rb_dmub_loc_out ro;
            const int rc = rb_dmub_reg_locate(&loc, lo, hi, tp, &ro);

            dmubRegRegs[r * 3u + 0u] = lo;
            dmubRegRegs[r * 3u + 1u] = hi;
            dmubRegRegs[r * 3u + 2u] = tp;
            dmubCand[slot].refuse = (uint32_t)rc;
            dmubCand[slot].memAddr = ro.dmub_mem;
            if (rc != RB_DMUB_LOC_OK) { continue; }

            IOMemoryDescriptor *rmd = nullptr;
            IOMemoryMap *rmap = nullptr;
            const uint32_t rroute =
                dmubMapWin(ro.ring_pa, ro.ring_pa - rb_dmub_fb_offset(&loc),
                           ro.span, &rmd, &rmap);
            if (!rroute || !rmap) {
                dmubCand[slot].refuse = RB_DMUB_LOC_MAP_FAILED;
                continue;
            }
            dmubCand[slot].md = rmd; dmubCand[slot].map = rmap;
            dmubCand[slot].mem = reinterpret_cast<volatile uint8_t *>(
                rmap->getVirtualAddress());
            dmubCand[slot].pa = ro.ring_pa;
            dmubCand[slot].span = ro.span;
            dmubCand[slot].route = rroute;
            dmubCand[slot].kind = (uint32_t)ro.addr_kind;

            uint32_t rr = 0, rb = 0, ra = 0;
            if (dmubCand[slot].mem) {
                dmubScanRun(dmubCand[slot].mem, ro.span, &rr, &rb, &ra);
            }
            dmubRegScan[r * 3u + 0u] = rr;
            dmubRegScan[r * 3u + 1u] = rb;
            dmubRegScan[r * 3u + 2u] = ra;
        }
    }

    /* The finding that reframes four milestones: the inbox base the firmware
     * itself wrote is OUTSIDE region 3 entirely. */
    dmubInboxInR3 = rb_dmub_inbox_in_region3(loc.inbox_base) ? 1u : 0u;

    /* The signature is now an observation, published beside the choice and
     * never consulted for it. Boot 273: CW4's 8192 bytes read ALL ZEROS with
     * the registers saying WPTR == RPTR == 2368, so "37 commands sit there"
     * was an inference from the pointer and never from memory. There is
     * nothing to recognise, which is precisely why the selector must not
     * depend on recognising it. */
    dmubSigOk = 0;
    if (best.win >= 0) {
        const uint32_t run = dmubWinScan[(unsigned)best.win * 4u + 1u];
        const uint32_t bad = dmubWinScan[(unsigned)best.win * 4u + 2u];
        const uint32_t slots = best.span / RB_DMUB_CMD_SIZE;
        dmubSigOk = rb_dmub_sig_ok(run, bad, slots) ? 1u : 0u;
    }

    dmubLocRefuse = (uint32_t)best.refuse;
    dmubWin = best.win;

    if (best.refuse != RB_DMUB_LOC_OK) {
        dmubRoute = 0;
        dmubLocRefuse = RB_DMUB_LOC_NO_WINDOW;
        LOG("DMUB: layout score %u of %u, no mailbox chosen (inbox %#x/%#x, "
            "fb %#x/%#x) - nothing mapped, nothing will be sent",
            dmubLayout, RB_DMUB_LAYOUT_CHECKED, loc.inbox_base,
            loc.inbox_size, loc.fb_location, loc.fb_offset);
        return false;
    }

    dmubRingOff = rb_dmub_ring_fb_off(&loc, &best);
    if (!dmubSelect((uint32_t)best.win)) {
        dmubLocRefuse = RB_DMUB_LOC_NO_WINDOW;
        return false;
    }
    /* 2zu: INDEX THE TABLE THE WINDOW IS ACTUALLY IN. dmubWinScan holds the
     * seven region-3 CW windows, 4 entries each - 28 in all - and selector
     * slots 7/8/9 are REGION4/5/6, whose scan lives in dmubRegScan. Indexing
     * dmubWinScan at dmubWin*4 with dmubWin == 7 reads entries 29 and 31,
     * past the end of the member array, and the two numbers that came out
     * were published as "signature 0 of 128 slots, 0 ABM -> ABSENT" beside a
     * heading that said REGION4. It was not a reading of anything, and boot
     * 279's own output showed it contradicting the aperture scan's run 37 on
     * the same memory. Rule 16: the instrument is a suspect, and rule 14: a
     * number that means nothing must not be published as if it did. */
    dmubScanSlots = dmubCap / RB_DMUB_CMD_SIZE;
    if (dmubWin >= 0 && dmubWin < 7) {
        dmubScanPlaus = dmubWinScan[(unsigned)dmubWin * 4u + 1u];
        dmubScanAbm   = dmubWinScan[(unsigned)dmubWin * 4u + 3u];
    } else if (dmubWin >= 7 && dmubWin < (int)RB_DMUB_WINSEL_COUNT) {
        const unsigned r = (unsigned)dmubWin - 7u;
        dmubScanPlaus = dmubRegScan[r * 3u + 0u];   /* run */
        dmubScanAbm   = dmubRegScan[r * 3u + 2u];   /* abm */
    } else {
        dmubScanPlaus = 0u;
        dmubScanAbm = 0u;
    }
    dmubScanBytes = dmubCap;

    LOG("DMUB mailbox: CW%d by layout %u/%u, mem %#llx (kind %u), pa %#llx "
        "(fb+%#llx), %u bytes, route %u, signature %s",
        dmubWin, dmubLayout, RB_DMUB_LAYOUT_CHECKED, dmubMem, dmubAddrKind,
        dmubRingPA, dmubRingOff, dmubCap, dmubRoute,
        dmubSigOk ? "present" : "ABSENT (the ring reads empty)");
    return true;
}

/* Point the push at one candidate window. The default is whatever dmubSetup
 * chose; this is how one boot tries every address the registers name instead
 * of one per boot, which is what the last four milestones each cost. */
bool RBAMDFramebuffer::dmubSelect(uint32_t n)
{
    if (n >= RB_DMUB_WINSEL_COUNT) { return false; }
    if (dmubCand[n].refuse != RB_DMUB_LOC_OK || !dmubCand[n].mem) {
        return false;
    }
    dmubWinSel = n;
    dmubRing = dmubCand[n].mem;
    dmubCap = dmubCand[n].span;
    dmubRingPA = dmubCand[n].pa;
    dmubMem = dmubCand[n].memAddr;
    dmubRoute = dmubCand[n].route;
    dmubAddrKind = dmubCand[n].kind;
    dmubLocRefuse = RB_DMUB_LOC_OK;
    dmubWin = (int)n;

    /* 2DA addendum: RE-READ the window we just pointed at.
     *
     * The scan, the signature and the ring dump were computed once in the
     * locate path with dmubWin == 4 and never recomputed here, so after a
     * `win 7` the tool printed "mailbox REGION4" beside CW4's signature and
     * CW4's all-zero dump - two numbers about a window no longer being
     * pushed into, in the same screen as the heading that contradicted them.
     * Boot 279 hit the identical contradiction through a different defect
     * (RBAMDFramebufferDMUB.cpp's out-of-bounds dmubWinScan index); that one
     * was fixed and this half was not. **Rule 31 for a DERIVED reading: a
     * value computed at setup is stale for every dial that can move
     * afterwards** - 2A3's defect in a scan rather than in a scalar.
     *
     * This runs from setProperties, never the frame path, so the IOMalloc in
     * dmubScan() is allowed (rule 31 not engaged). */
    {
        uint32_t run = 0, bad = 0, abm = 0;
        const uint32_t slots = dmubCap / RB_DMUB_CMD_SIZE;

        dmubScanRun(dmubRing, dmubCap, &run, &bad, &abm);
        dmubScanSlots = slots;
        dmubScanPlaus = run;
        dmubScanBad   = bad;
        dmubScanAbm   = abm;
        dmubScanBytes = dmubCap;
        dmubSigOk = rb_dmub_sig_ok(run, bad, slots) ? 1u : 0u;
        dmubScanWin = (int)n;        /* WHICH window these four describe */
        dmubScan();                  /* republish RBFBDmubRingDump */
    }
    return true;
}

/* 2DC: the DOMAIN power-gating words, read-only.
 *
 * Four statuses and four configs out of the aperture hubpRegs already maps -
 * dword 0x80..0x87, BASE_IDX 2, far below rb_hpd_dw_max() - so there is no
 * new mapping, no aperture growth and no write. Unarmed: a boot reads its
 * baseline whether or not the sender is armed, which is 2BA's division
 * between a free survey and an opt-in transaction.
 *
 * pgSamples counts the reads, because four zero words from a block that was
 * never sampled and four zero words read off the chip are the same bytes
 * (rule 14). */
void RBAMDFramebuffer::pgSample(uint32_t *status, uint32_t *config)
{
    if (!hubpRegs || !status || !config) { return; }

    /* 2DD: kRbDmuSeg2Dmub is the whole fix. The DOMAIN offsets are
     * BASE_IDX 2, exactly like the HUBP, OTG, HPD and AUX blocks this kext
     * already reads, and every one of those indexes as `kRbDmuSeg2* + dword`.
     * 2DC omitted it here and read dwords 0x80..0x87 of the mapping - a
     * readable address inside BAR5 that is not this block, so it answered
     * eight zeros with no error and the auto-candidate guard refused on them.
     * Never index this array with a dword offset read off a vendored header
     * without the segment base. */
    for (uint32_t i = 0; i < RB_PG_DOMAIN_COUNT; i++) {
        status[i] = hubpRegs[kRbDmuSeg2Dmub + rb_pg_status_dw(i)];
        config[i] = hubpRegs[kRbDmuSeg2Dmub + rb_pg_config_dw(i)];
    }
    pgSamples++;
}

/* 2DC: one DOMAIN_CONTROL command. */
uint32_t RBAMDFramebuffer::dmubSendDomain(uint32_t inst, uint32_t gate)
{
    uint8_t cmd[RB_DMUB_CMD_SIZE];

    if (!dmubDomArmed) { return dmubDomRefuse = 20u; }
    if (!rb_dmub_domain_inst_valid(inst)) { return dmubDomRefuse = 21u; }

    rb_dmub_build_domain_control(cmd, inst, gate);
    const uint32_t rc = dmubPush(cmd);
    if (rc == 0u) { dmubDomSends++; }
    dmubDomRefuse = rc;
    return rc;
}

/* 2DC: THE WHOLE EXPERIMENT IN ONE CALL - sample, send, sample.
 *
 * Deliberately not three keys. Four sessions in a row were spent on a null
 * whose cause was state that moved between the arm and the action, and the
 * last two were the ring selector: once because the default points at the
 * window this project's own source rules out, and once because a reboot
 * reset it and nothing in the handover checked the boot. An experiment whose
 * arm and action are one call cannot be run in the wrong order, cannot be
 * split by a reboot, and cannot read a stale published value as its own
 * precondition (rule 34, made structural instead of advisory).
 *
 * The verdict is the DIFFERENCE across the send, which needs no claim about
 * the polarity of DOMAIN_PGFSM_PWR_STATUS - a field whose meaning lives in a
 * Linux file this project has not vendored. rb_pg.h says why at length.
 *
 * Refusal codes are distinct (rule 14): 20 not armed, 21 inst out of the
 * 6-bit field, 22 no register aperture, 23 the named instance is refused by
 * rb_pg_sel_refuse, 24 no gated domain exists so the test is NOT AVAILABLE
 * on this boot, and anything under 20 is dmubPush's own. */
uint32_t RBAMDFramebuffer::dmubDomainExperiment(uint32_t inst, uint32_t gate)
{
    if (!dmubDomArmed) { return dmubDomRefuse = 20u; }
    if (!hubpRegs)     { return dmubDomRefuse = 22u; }

    const uint32_t sel = rb_pg_sel_refuse(inst);
    if (sel != RB_PG_SEL_OK) {
        dmubDomRefuse = 23u;
        dmubDomInst = inst;
        return 23u;
    }

    /* 2DE: `inst` is the DOMAIN NUMBER the command carries; the status words
     * are indexed by SLOT, and the two part company past DOMAIN3. Converting
     * here, once, is what stops a DOMAIN16 command being judged by DOMAIN0's
     * word. */
    const int slotI = rb_pg_slot_for_inst(inst);
    if (slotI < 0) { dmubDomRefuse = 23u; dmubDomInst = inst; return 23u; }
    const uint32_t slot = (uint32_t)slotI;

    pgSample(pgBefore, pgConfig);

    /* 2DD: eight zero words is an unreadable block, not a chip answer. This
     * refusal goes BEFORE the reference check, because otherwise an
     * unreadable sample arrives at it as "every domain matches DOMAIN0" and
     * comes back as 24 - a true statement about the words and a false one
     * about the chip, which is what boot 436 reported. */
    if (!rb_pg_sample_readable(pgBefore, pgConfig)) {
        dmubDomRefuse = 25u;
        dmubDomInst = inst;
        return 25u;
    }

    /* A power-UP asked of a domain already in the reference state cannot
     * show anything, and its null would be unreadable (rule 39). Refuse
     * rather than measure where the effect cannot appear. */
    if (!gate && rb_pg_like_reference(pgBefore[slot],
                                      pgBefore[RB_PG_DOMAIN_REFERENCE])) {
        dmubDomRefuse = 24u;
        dmubDomInst = inst;
        return 24u;
    }

    dmubDomInst = inst;
    dmubDomGate = gate;

    const uint32_t rc = dmubSendDomain(inst, gate);
    if (rc != 0u) { return rc; }

    /* The power-gating FSM is not instantaneous; Linux waits on this field
     * with a 1 us poll and a 1000-iteration bound in the path that is not
     * vendored here, so the bound is reproduced rather than the polarity.
     * A command that executed slowly must not read as one that did nothing. */
    for (uint32_t spin = 0; spin < 1000u; spin++) {
        pgSample(pgAfter, pgConfig);
        if (rb_pg_moved_mask(pgBefore, pgAfter) != 0u) { break; }
        IODelay(1);
    }

    dmubDomMoved = rb_pg_moved_mask(pgBefore, pgAfter);
    dmubDomRan++;
    dmubDomRefuse = 0u;
    return 0u;
}

/* 2E2: THE DIG1 TRANSMITTER EXPERIMENT - sample, send, sample, in one call.
 *
 * Same single-call shape as dmubDomainExperiment above, for the reason
 * recorded there: an experiment whose arm and action are separate commands
 * is one that will be run in the wrong order, or split by a reboot that
 * resets the ring selector, and will then read a stale published value as
 * its own precondition (rule 34, made structural).
 *
 * TWO words are read, and the second one is the point. `digBefore`/`digAfter`
 * are the TARGET PHY's RDPCSTX_PHY_CNTL6; `digRefWord` is PHY0's, the live
 * panel's, which 2C1 measured at 0x00194444 and which must still read lit for
 * the comparison to mean anything. A PHY_CNTL6 block that has stopped
 * answering the way the one measured reading says it does would give a
 * difference nobody can interpret - and a reading taken from a block that is
 * not answering is exactly what cost 2zw six milestones. So the control is
 * read FIRST and a dark control refuses before anything is sent.
 *
 * What is deliberately NOT done: `enc1_configure_encoder`'s two link-encoder
 * writes (`DP_CONFIG.DP_UDI_LANES`, `DP_DPHY_SCRAM_CNTL.DPHY_SCRAMBLER_ADVANCE`).
 * They are a block this kext has never written, and the question here is
 * whether the FIRMWARE executes the command. Their absence is why a STILL
 * verdict is not on its own "this firmware discards VBIOS commands"
 * (rb_dig.h says the same at length, and rule 14 is why it is said twice).
 */
/* 2E5: enc1_configure_encoder, transcribed.
 *
 * Two writes, and they are NOT the same kind - DP_CONFIG is a REG_SET with an
 * init value of 0 (the whole word) and the scrambler is a REG_UPDATE (an RMW
 * that must preserve BS_COUNT and KCODE). rb_enc.h owns both shapes and the
 * hosttest pins that they differ.
 *
 * This is a WRITE to a block this kext has never written, so it is bounded
 * the way the send is: instance 0 - the panel's own link encoder - is refused
 * by name, the dcn302/dcn312 deny list is re-checked on the machine so a
 * future edit here cannot bypass it (the shape blSetup's gate 3 uses), and
 * the READBACK decides whether it took rather than the store (rules 13/20).
 * Every word either side is published, so a null is attributable.
 *
 * Runs from setProperties, never the frame path (rule 31). */
/* 2E6: read all five link encoders ONCE, before any send can perturb one of
 * them, and latch. Read-only, no setter, and every register is inside the
 * aperture `dpaltFits` already bounds - so this adds no write surface and no
 * new mapping.
 *
 * Latched, but only a REAL reading latches: with the aperture not yet up this
 * returns without setting `encSurveyDone`, so the periodic publish retries.
 * Caching an early transient as though it were permanent is 2CA's defect and
 * 2CE's correction. */
void RBAMDFramebuffer::encSurvey(void)
{
    if (encSurveyDone) { return; }
    if (!hubpRegs || !dpaltReady) { return; }

    for (unsigned i = 0; i < RB_ENC_COUNT; i++) {
        encSurveyCfg[i]   = hubpRegs[kRbDmuSeg2Dmub + rb_enc_dw_config(i)];
        encSurveyScram[i] = hubpRegs[kRbDmuSeg2Dmub + rb_enc_dw_scram(i)];
    }
    encSurveyMask = rb_enc_configured_mask(encSurveyCfg, encSurveyScram);
    encSurveyDone = true;
}

/* 2E7: DIG_BE_CNTL on all five instances, once, latched. Read-only, no
 * setter, and 0x24b0 is below the bound `dpaltFits` already checks - so no
 * new mapping and no new write surface, exactly as encSurvey().
 *
 * The reading is the whole point: DIG_FE_SOURCE_SELECT 0 on instance 2 means
 * 2E1's "MISSING STEP" really is undone and the write is still to make;
 * non-zero means the firmware did it - as 2E7 found it had configured the
 * link encoder - and that candidate collapses with no write at all. */
void RBAMDFramebuffer::digbeSurvey(void)
{
    if (digbeDone) { return; }
    if (!hubpRegs || !dpaltReady) { return; }

    for (unsigned i = 0; i < RB_DIGBE_COUNT; i++) {
        digbeWord[i]   = hubpRegs[kRbDmuSeg2Dmub + rb_digbe_dw(i)];
        digbeEnWord[i] = hubpRegs[kRbDmuSeg2Dmub + rb_digbe_en_dw(i)];
    }
    digbeMask   = rb_digbe_connected_mask(digbeWord);
    digbeEnMask = rb_digbe_enabled_mask(digbeEnWord);
    digbeDone = true;
}

/* Re-read both blocks after a send. Called from dmubDigExperiment() on the
 * path that actually produced on the ring, never from a refusal, so a
 * published diff mask of 0 means "the command ran and the back end did not
 * move" and not "nothing was sent" (rule 14 - and `digbePostDone` is what
 * separates them, because 0 is also the initial value).
 *
 * NOT latched, unlike the pristine survey: each send gets its own after-state,
 * since the pristine side is the one that must stay unspoiled. */
void RBAMDFramebuffer::digbeResample(void)
{
    if (!hubpRegs || !dpaltReady) { return; }

    for (unsigned i = 0; i < RB_DIGBE_COUNT; i++) {
        digbePost[i]   = hubpRegs[kRbDmuSeg2Dmub + rb_digbe_dw(i)];
        digbeEnPost[i] = hubpRegs[kRbDmuSeg2Dmub + rb_digbe_en_dw(i)];
    }
    digbeDiffMask   = rb_digbe_diff_mask(digbeWord, digbePost);
    digbeEnDiffMask = rb_digbe_diff_mask(digbeEnWord, digbeEnPost);
    digbePostDone = true;
}

uint32_t RBAMDFramebuffer::encConfigure(uint32_t inst, uint32_t lanes)
{
    encInst = inst;

    /* blRegs is the WRITABLE mapping of the same BAR5 window hubpRegs reads
     * through; dmubPush already writes INBOX1_WPTR through it. The encoder
     * dwords top out at DP4's 0x251e, below rb_dpalt_dw_max()'s 0x2ca6, so
     * the aperture 2C0 checks separately covers them - asserted in the
     * hosttest rather than trusted here. */
    const int haveRegs = (blRegs != nullptr) && (hubpRegs != nullptr)
                         && dpaltReady;

    /* Clamped only so the deny-list lookup cannot index out of range before
     * rb_enc_refuse() gets to reject the instance itself. */
    const uint32_t safe  = (inst < RB_ENC_COUNT) ? inst : 0u;
    const uint32_t cfgDw = rb_enc_dw_config(safe);
    const uint32_t scrDw = rb_enc_dw_scram(safe);
    const int denied = rb_dcn_is_denied(cfgDw) || rb_dcn_is_denied(scrDw);

    const uint32_t why = rb_enc_refuse(inst, lanes, digArmed ? 1 : 0,
                                       haveRegs, denied);
    if (why != RB_ENC_OK) { return encRefuse = why; }

    encCfgBefore   = hubpRegs[kRbDmuSeg2Dmub + cfgDw];
    encScramBefore = hubpRegs[kRbDmuSeg2Dmub + scrDw];

    blRegs[kRbDmuSeg2Dmub + cfgDw] = rb_enc_dp_config_word(lanes);
    blRegs[kRbDmuSeg2Dmub + scrDw] = rb_enc_scram_word(encScramBefore);

    encCfgAfter   = hubpRegs[kRbDmuSeg2Dmub + cfgDw];
    encScramAfter = hubpRegs[kRbDmuSeg2Dmub + scrDw];
    encApplied    = (uint32_t)rb_enc_applied(encCfgAfter, encScramAfter,
                                             lanes);
    encChanged    = (uint32_t)rb_enc_changed(encCfgBefore, encCfgAfter,
                                             encScramBefore, encScramAfter);
    encRan++;
    return encRefuse = RB_ENC_OK;
}

uint32_t RBAMDFramebuffer::dmubDigExperiment(uint32_t phyid)
{
    struct rb_dig_params p;
    uint8_t cmd[RB_DMUB_CMD_SIZE];

    digPhy = phyid;

    /* The target's parameters are the derived Type-C set with the phyid
     * overridden, so a caller can aim elsewhere but cannot assemble a
     * half-derived payload - rb_dig.h has the only producer. */
    rb_dig_typec_enable(&p);
    p.phyid = phyid;

    /* The control, read before the refusal that depends on it. hubpRegs is
     * the same BAR5 window the OTG, HPD, AUX and PG reads come from; the
     * RDPCSTX block sits inside it when dpaltFits (RBAMDFramebufferHUBP.cpp
     * grows the aperture bound by rb_dpalt_dw_max()). */
    const int haveRegs = (hubpRegs != nullptr) && dpaltReady;
    if (haveRegs) {
        digRefWord = hubpRegs[kRbDmuSeg2Dmub + rb_dpalt_dw(RB_DIG_PHY_PANEL)];
    } else {
        digRefWord = 0u;
    }

    const uint32_t why = rb_dig_refuse(&p, digArmed ? 1 : 0, haveRegs,
                                       rb_dig_phy_lit(digRefWord));
    if (why != RB_DIG_OK) { return digRefuse = why; }

    digBefore = hubpRegs[kRbDmuSeg2Dmub + rb_dpalt_dw(phyid)];
    digAfter = digBefore;
    digVerdict = RB_DIG_V_STILL;

    /* 2E5: configure the encoder FIRST, inside the same call, because
     * dcn10_link_encoder_enable_dp_output does it before the command and
     * because a precondition that is a previous command is one that gets
     * skipped (rule 34 - 2DA lost two sessions to exactly that).
     *
     * The link encoder instance IS the phyid: link_regs(2, C) hands the same
     * index to LE_DCN31_REG_LIST, UNIPHY_DCN2_REG_LIST and
     * DPCS_DCN31_REG_LIST, so DP2_*, RDPCSTX2 and UNIPHY_C are one port.
     *
     * A refusal here does NOT stop the send: `digEnc` off reproduces 2E4's
     * arm exactly, and a configure that refused while the send went anyway
     * is still a reading - as long as the boot can say which it was, which
     * is what encRefuse and encApplied are for (rule 14). */
    if (digEnc) {
        encConfigure(phyid, p.lanenum);
    } else {
        encRefuse = RB_ENC_REFUSE_DISARMED;
        encApplied = 0u;
        encChanged = 0u;
    }

    rb_dig_build_transmitter(cmd, &p);

    const uint32_t rc = dmubPush(cmd);
    if (rc != 0u) { return digRefuse = rc; }
    digSends++;

    /* Bringing a PHY up is not instantaneous - it is PLL lock. Linux waits
     * on the DMUB command completing and then on its own register polls
     * elsewhere; neither bound is vendored here, so this reproduces the
     * SHAPE used for the DOMAIN FSM rather than inventing a number: poll
     * until the word moves, with a bound, so a command that executed slowly
     * does not read as one that did nothing (rule 14). 2000 x 10 us = 20 ms,
     * which is two decimal orders above a PLL lock and still far short of
     * anything a user would notice; this runs from setProperties, never the
     * frame path (rule 31). */
    for (uint32_t spin = 0; spin < 2000u; spin++) {
        digAfter = hubpRegs[kRbDmuSeg2Dmub + rb_dpalt_dw(phyid)];
        if (digAfter != digBefore) { break; }
        IODelay(10);
    }

    digVerdict = rb_dig_verdict(digBefore, digAfter);
    /* 2E8: the PHY word is one product of this command; the DIG back end is
     * another, and Linux's own dcn10_is_dig_enabled() reads it. Sampled after
     * the PHY poll so a late-landing effect is still caught. */
    digbeResample();
    digRan++;
    return digRefuse = RB_DIG_OK;
}

/* 2EA: the sweep. phy_id 0..4, one command each, decoded from the slot
 * dmubPush already reads back for every command - so the reply costs nothing
 * new and the only write is the inbox push 2E9 measured safe.
 *
 * Every phy gets its own refusal code, kept per instance rather than
 * last-wins: "phy 2 answered and phys 0,1 did not" is the shape of a reply
 * path that works, and a single scalar cannot express it (rule 14, and 2E9's
 * instance-level diff is why that mattered there too).
 *
 * The panel's PHY is NOT refused here, unlike every write path in this kext:
 * this command changes nothing, and phy 0 is the control the verdict needs -
 * the ROM says it is not USB-C, so is_usb 0 there against 1 on phy 2 is the
 * agreement that makes a reply believable. */
uint32_t RBAMDFramebuffer::dmubDpAltQuery(uint32_t fill)
{
    uint8_t cmd[RB_DMUB_CMD_SIZE];

    dpaltqCalls++;
    dpaltqFill = fill;
    dpaltqRetCleared = 0u;

    if (!dpaltqArmed || !dmubBlArmed) {
        for (unsigned i = 0; i < RB_DPALTQ_PHY_COUNT; i++) {
            dpaltqRefuse[i] = RB_DPALTQ_NOT_ARMED;
        }
        dpaltqVerdict = RB_DPALTQ_V_NONE;
        dpaltqUsbMask = 0u;
        return RB_DPALTQ_NOT_ARMED;
    }

    for (unsigned i = 0; i < RB_DPALTQ_PHY_COUNT; i++) {
        rb_dpaltq_build(cmd, i, (uint8_t)fill);

        const uint32_t rc = dmubPush(cmd);
        if (rc != 0u) {
            dpaltqRefuse[i] = RB_DPALTQ_PUSH_FAILED;
            dpaltqReply[i].phy_id = i;
            dpaltqReply[i].is_usb = 0u;
            dpaltqReply[i].is_dp_alt_disable = 0u;
            dpaltqReply[i].is_dp4 = 0u;
            continue;
        }
        dpaltqSends++;

        /* dmubPush's step 5 has already copied the slot into dmubReplyGot,
         * after its own bounded wait on RPTR. Nothing is re-read from the
         * ring here: a second read would be a different sample from the one
         * the ack bounded (2zp - do not compare a reading with itself, and
         * do not silently take a later one either). */
        if (rb_dpaltq_ret_cleared(dmubReplyGot)) {
            dpaltqRetCleared |= (1u << i);
        }
        dpaltqRefuse[i] = (uint32_t)rb_dpaltq_decode(dmubReplyGot, i,
                                                     (uint8_t)fill,
                                                     &dpaltqReply[i]);
    }

    dpaltqVerdict = (uint32_t)rb_dpaltq_verdict(dpaltqReply, dpaltqRefuse);
    dpaltqUsbMask = rb_dpaltq_usb_mask(dpaltqReply, dpaltqRefuse);
    return RB_DPALTQ_OK;
}

void RBAMDFramebuffer::dmubTeardown(void)
{
    dmubRing = nullptr;
    dmubCap = 0;
    /* Every candidate is mapped now (2zs), so every one is released. */
    for (unsigned i = 0; i < RB_DMUB_WINSEL_COUNT; i++) {
        if (dmubCand[i].map) { dmubCand[i].map->release(); dmubCand[i].map = nullptr; }
        if (dmubCand[i].md)  { dmubCand[i].md->release();  dmubCand[i].md = nullptr; }
        dmubCand[i].mem = nullptr;
    }
    dmubRingMap = nullptr;
    dmubRingMD = nullptr;
}

/* Publish the winner's bytes. The run was already counted during the search;
 * this is the evidence itself - firmware 4.0.15.47's own ABM traffic, which
 * is the only direct statement about the command layout this chip accepts.
 * Runs once, from setup, on the way to a desktop; it allocates. */
void RBAMDFramebuffer::dmubScan(void)
{
    if (!dmubRing || dmubCap == 0u) { return; }

    const uint32_t bytes = dmubCap;
    uint8_t *buf = (uint8_t *)IOMalloc(bytes);
    if (!buf) { return; }

    for (uint32_t i = 0; i < bytes; i++) { buf[i] = dmubRing[i]; }

    OSData *d = OSData::withBytes(buf, bytes);
    if (d) {
        setProperty("RBFBDmubRingDump", d);
        d->release();
    }
    IOFree(buf, bytes);
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
    if (!dmubRing || !blRegs || dmubCap == 0u) {
        dmubSendsRefused++; return dmubSendRefuse = 2;
    }
    if (dmubLocRefuse != RB_DMUB_LOC_OK) {
        dmubSendsRefused++; return dmubSendRefuse = 3;
    }

    const uint32_t wptr = blRegs[kRbDmuSeg2Dmub + RB_DMUB_DW_INBOX1_WPTR];
    const uint32_t rptr = blRegs[kRbDmuSeg2Dmub + RB_DMUB_DW_INBOX1_RPTR];

    if (!rb_dmub_ptr_ok(wptr, dmubCap) ||
        !rb_dmub_ptr_ok(rptr, dmubCap)) {
        dmubSendsRefused++; return dmubSendRefuse = 4;
    }
    if (rb_dmub_rb_full(wptr, rptr, dmubCap)) {
        dmubSendsRefused++; return dmubSendRefuse = 5;
    }

    /* 1. The command, at WPTR. Keep the slot and the exact bytes: step 5
     * compares the memory against them, and a rebuilt copy would be
     * comparing a reading with itself (2zp). */
    dmubReplySlot = wptr;
    for (uint32_t i = 0; i < RB_DMUB_CMD_SIZE; i++) {
        dmubReplySent[i] = cmd[i];
        dmubRing[wptr + i] = cmd[i];
    }
    dmubReplyVerdict = RB_DMUB_REPLY_NONE;

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
    const uint32_t next = rb_dmub_next_wptr(wptr, dmubCap);
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

    /* 5. THE IDENTIFICATION. dmub_srv_cmd_get_response ends at
     * dmub_rb_get_return_data, which reads `base_address + rptr -
     * DMUB_RB_CMD_SIZE` - the slot just consumed, which is the slot we
     * wrote. So the firmware's reply, if there is one, is in these 64 bytes
     * and nowhere else. Read them back unconditionally, for every command:
     * it costs 16 dword loads from a mapping we already hold, and an ABM
     * command coming back UNCHANGED is itself a reading worth having (the
     * write landed and stuck) where a ZERO would retire this address. */
    for (uint32_t i = 0; i < RB_DMUB_CMD_SIZE; i++) {
        dmubReplyGot[i] = dmubRing[dmubReplySlot + i];
    }
    dmubReplyVerdict = (uint32_t)rb_dmub_reply_verdict(dmubReplySent,
                                                       dmubReplyGot);
    if (dmubReplyVerdict == RB_DMUB_REPLY_CONFIRMED) { dmubConfirms++; }

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
    rb_dmub_build_set_backlight(cmd, level, 0u, 0x01u,
                                dmubBlType, (uint8_t)dmubBlAuxInst);
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

/* The caps query. Read-only on the chip - it asks the firmware what it
 * supports and changes nothing - and it is the only command on this surface
 * whose reply the driver is documented to read back out of the ring, which
 * is what makes it the instrument rather than another backlight attempt. */
uint32_t RBAMDFramebuffer::dmubSendCaps(void)
{
    uint8_t cmd[RB_DMUB_CMD_SIZE];

    rb_dmub_build_query_caps(cmd);
    dmubCapsSends++;
    return dmubPush(cmd);
}

/* One GPINT, and only the version query.
 *
 * This is the whole of dmub_dcn31.c's set_gpint / is_gpint_acked /
 * get_gpint_response for a query: write DMCUB_GPINT_DATAIN1, read it back
 * until the status field clears, then read DMCUB_SCRATCH7. There is no
 * memory, no window, no mc2pa and no mapping anywhere in it, which is the
 * entire point after boot 275.
 *
 * TWO THINGS LINUX DOES THAT THIS DELIBERATELY DOES NOT.
 * dmub_dcn31_get_gpint_dataout clears DMCUB_GPINT_DATAOUT and pulses
 * DMCUB_GPINT_IH_INT_ACK with DMCUB_GPINT_IH_INT_EN off around it. That is
 * interrupt plumbing belonging to a driver that owns the DMCUB; here the
 * DMCUB is what lit the panel and is servicing whatever Apple's stack asks
 * of it, so DATAOUT is READ and nothing in DMCUB_INTERRUPT_ENABLE or
 * _ACK is touched. And the word is not cleared afterwards: once the status
 * field reads 0 the command is spent, so a write to put a zero there would
 * be a second write for no reading.
 *
 * The control is read on BOTH sides. DMCUB_SCRATCH0 is where a level-2 probe
 * read "4.0.15.47 running, 4.0.21.71 on ROM" long before this file existed,
 * and SCRATCH7 is where the answer lands - 0x01e3 against 0x01ea. Both are
 * sampled before the write and after it, so a SCRATCH7 that already held the
 * answer cannot be mistaken for a reply (rule 26's control, and 2zp's error
 * was exactly a reading compared with itself). */
uint32_t RBAMDFramebuffer::dmubGpint(uint32_t code)
{
    if (!dmubBlArmed) { return dmubGpintRefuse = 1; }
    if (!blRegs) { return dmubGpintRefuse = 2; }
    /* The gate, not a comment: DMUB_GPINT__STOP_FW halts the firmware that
     * is driving this panel, so exactly one code is buildable. */
    if (!rb_dmub_gpint_allowed(code)) { return dmubGpintRefuse = 3; }

    dmubScratch0Pre = blRegs[kRbDmuSeg2Dmub + RB_DMUB_DW_SCRATCH0];
    dmubScratch7Pre = blRegs[kRbDmuSeg2Dmub + RB_DMUB_DW_SCRATCH7];

    dmubGpintSent = rb_dmub_gpint_word(code, 0u);
    dmubGpintAcked = 0;
    dmubGpintSends++;
    blRegs[kRbDmuSeg2Dmub + RB_DMUB_DW_GPINT_DATAIN1] = dmubGpintSent;

    {
        unsigned spins = 1000;                /* 1 ms at 1 us a turn */
        while (spins--) {
            dmubGpintBack =
                blRegs[kRbDmuSeg2Dmub + RB_DMUB_DW_GPINT_DATAIN1];
            if (rb_dmub_gpint_acked(dmubGpintSent, dmubGpintBack)) {
                dmubGpintAcked = 1;
                break;
            }
            IODelay(1);
        }
        if (spins == (unsigned)-1) { dmubGpintSpins++; }
    }

    dmubScratch0 = blRegs[kRbDmuSeg2Dmub + RB_DMUB_DW_SCRATCH0];
    dmubScratch7 = blRegs[kRbDmuSeg2Dmub + RB_DMUB_DW_SCRATCH7];
    dmubDataout  = blRegs[kRbDmuSeg2Dmub + RB_DMUB_DW_GPINT_DATAOUT];

    return dmubGpintRefuse = 0;
}

void RBAMDFramebuffer::dmubPublish(void)
{
    setProperty("RBFBDmubBlArmed", dmubBlArmed);
    setProperty("RBFBDmubLocRefuse", (uint64_t)dmubLocRefuse, 32);
    setProperty("RBFBDmubInbox1Base", (uint64_t)dmubInbox[0], 32);
    setProperty("RBFBDmubInbox1Size", (uint64_t)dmubInbox[1], 32);
    setProperty("RBFBDmubFbLocation", (uint64_t)dmubFb[0], 32);
    setProperty("RBFBDmubFbOffset", (uint64_t)dmubFb[1], 32);
    /* The covering window and what its offset turned out to be. Boot 272 is
     * the reason the KIND is published: 2zn predicted a system-physical
     * address and the firmware had written an MC one. */
    setProperty("RBFBDmubWin", (uint64_t)(int64_t)dmubWin, 32);
    /* The selector, and the observation that is NOT the selector. */
    setProperty("RBFBDmubLayout", (uint64_t)dmubLayout, 32);
    setProperty("RBFBDmubLayoutOf", (uint64_t)RB_DMUB_LAYOUT_CHECKED, 32);
    setProperty("RBFBDmubSigOk", (uint64_t)dmubSigOk, 32);
    setProperty("RBFBDmubAddrKind", (uint64_t)dmubAddrKind, 32);
    setProperty("RBFBDmubMem", dmubMem, 64);
    setProperty("RBFBDmubRoute", (uint64_t)dmubRoute, 32);
    setProperty("RBFBDmubRingPA", dmubRingPA, 64);
    setProperty("RBFBDmubRingFbOff", dmubRingOff, 64);
    setProperty("RBFBDmubCap", (uint64_t)dmubCap, 32);
    /* All seven windows as read, raw, enable bits intact - the table is what
     * refuted 2zn, and a locate that published only its own chosen window
     * would have had nothing to say. */
    {
        OSData *d = OSData::withBytes(dmubWinRegs, sizeof(dmubWinRegs));
        if (d) { setProperty("RBFBDmubWindows", d); d->release(); }
        d = OSData::withBytes(dmubWinScan, sizeof(dmubWinScan));
        if (d) { setProperty("RBFBDmubWinScan", d); d->release(); }
    }
    /* The control, and its denominator beside it. */
    setProperty("RBFBDmubScanSlots", (uint64_t)dmubScanSlots, 32);
    setProperty("RBFBDmubScanPlaus", (uint64_t)dmubScanPlaus, 32);
    setProperty("RBFBDmubScanAbm", (uint64_t)dmubScanAbm, 32);
    setProperty("RBFBDmubScanBad", (uint64_t)dmubScanBad, 32);
    /* WHICH window the four scan numbers above describe. Published because
     * 2DA's screen showed CW4's signature under a REGION4 heading and the
     * only way to read that output correctly was to know the scan had not
     * been recomputed (rule 39: an instrument must say which configuration
     * its reading comes from). -1 means the locate path's own window. */
    setProperty("RBFBDmubScanWin", (uint64_t)(int64_t)dmubScanWin, 32);

    /* 2DC: DOMAIN_CONTROL and its readback. */
    setProperty("RBFBDmubDomArmed", dmubDomArmed);
    setProperty("RBFBDmubDomSends", (uint64_t)dmubDomSends, 32);
    setProperty("RBFBDmubDomRan", (uint64_t)dmubDomRan, 32);
    setProperty("RBFBDmubDomCalls", (uint64_t)dmubDomCalls, 32);
    setProperty("RBFBDmubDomRefuse", (uint64_t)dmubDomRefuse, 32);
    setProperty("RBFBDmubDomInst", (uint64_t)dmubDomInst, 32);
    setProperty("RBFBDmubDomGate", (uint64_t)dmubDomGate, 32);
    setProperty("RBFBDmubDomMoved", (uint64_t)dmubDomMoved, 32);
    setProperty("RBFBPgSamples", (uint64_t)pgSamples, 32);

    /* 2E2: the DIG1 transmitter experiment. `digCalls` is monotonic over
     * every handler entry INCLUDING refusals, so a reader waits on it rather
     * than on the value it is about to interpret - 2C5's lesson, and the
     * reason boot 437's dmubbl could report a verdict at all. The two PHY
     * words are published RAW beside the verdict (2B0/2B4/2B9/2DA: four
     * times now that has been what let a wrong summary be re-read), at 64
     * bits because a 32-bit OSNumber with bit 31 set renders as a
     * sign-extended decimal (2B4). */
    setProperty("RBFBDigArmed", digArmed);
    setProperty("RBFBDigCalls", (uint64_t)digCalls, 32);
    setProperty("RBFBDigSends", (uint64_t)digSends, 32);
    setProperty("RBFBDigRan", (uint64_t)digRan, 32);
    setProperty("RBFBDigRefuse", (uint64_t)digRefuse, 32);
    setProperty("RBFBDigPhy", (uint64_t)digPhy, 32);
    setProperty("RBFBDigVerdict", (uint64_t)digVerdict, 32);
    setProperty("RBFBDigBefore", (uint64_t)digBefore, 64);
    setProperty("RBFBDigAfter", (uint64_t)digAfter, 64);
    setProperty("RBFBDigRefWord", (uint64_t)digRefWord, 64);
    /* 2E5: the encoder configure, every word either side. `encApplied` is
     * the READBACK's verdict and not the store's, so a boot can tell "the
     * configure did not take" from "the configure took and the PHY still did
     * not move" - two findings a single flag would merge (rule 14). */
    setProperty("RBFBDigEnc", digEnc);
    setProperty("RBFBEncInst", (uint64_t)encInst, 32);
    setProperty("RBFBEncRefuse", (uint64_t)encRefuse, 32);
    setProperty("RBFBEncRan", (uint64_t)encRan, 32);
    setProperty("RBFBEncApplied", (uint64_t)encApplied, 32);
    setProperty("RBFBEncCfgBefore", (uint64_t)encCfgBefore, 64);
    setProperty("RBFBEncCfgAfter", (uint64_t)encCfgAfter, 64);
    setProperty("RBFBEncScramBefore", (uint64_t)encScramBefore, 64);
    setProperty("RBFBEncScramAfter", (uint64_t)encScramAfter, 64);
    setProperty("RBFBEncChanged", (uint64_t)encChanged, 32);
    /* 2E6's survey. Sampled here so the first periodic publish latches the
     * PRISTINE words - a send only ever happens from setProperties, later. */
    encSurvey();
    setProperty("RBFBEncSurveyDone", encSurveyDone);
    setProperty("RBFBEncSurveyMask", (uint64_t)encSurveyMask, 32);
    digbeSurvey();
    setProperty("RBFBDigBeDone", digbeDone);
    setProperty("RBFBDigBeMask", (uint64_t)digbeMask, 32);
    setProperty("RBFBDigBeEnMask", (uint64_t)digbeEnMask, 32);
    setProperty("RBFBDigBePostDone", digbePostDone);
    setProperty("RBFBDigBeDiff", (uint64_t)digbeDiffMask, 32);
    setProperty("RBFBDigBeEnDiff", (uint64_t)digbeEnDiffMask, 32);
    for (unsigned i = 0; i < RB_DIGBE_COUNT; i++) {
        char k[32];
        snprintf(k, sizeof(k), "RBFBDigBe%u", i);
        setProperty(k, (uint64_t)digbeWord[i], 64);
        snprintf(k, sizeof(k), "RBFBDigBeEn%u", i);
        setProperty(k, (uint64_t)digbeEnWord[i], 64);
        snprintf(k, sizeof(k), "RBFBDigBePost%u", i);
        setProperty(k, (uint64_t)digbePost[i], 64);
        snprintf(k, sizeof(k), "RBFBDigBeEnPost%u", i);
        setProperty(k, (uint64_t)digbeEnPost[i], 64);
    }
    /* 2EA. dpaltqCalls is monotonic and bumped BEFORE any refusal, so a
     * tool can poll it past its own pre-set value instead of sleeping on a
     * snapshot - 2C5/2D1/2E3's defect, three times paid for. */
    setProperty("RBFBDpAltQArmed", dpaltqArmed);
    setProperty("RBFBDpAltQCalls", dpaltqCalls, 64);
    setProperty("RBFBDpAltQSends", (uint64_t)dpaltqSends, 32);
    setProperty("RBFBDpAltQFill", (uint64_t)dpaltqFill, 32);
    setProperty("RBFBDpAltQVerdict", (uint64_t)dpaltqVerdict, 32);
    setProperty("RBFBDpAltQUsbMask", (uint64_t)dpaltqUsbMask, 32);
    setProperty("RBFBDpAltQRomMask", (uint64_t)RB_DPALTQ_ROM_USB_MASK, 32);
    setProperty("RBFBDpAltQRetCleared", (uint64_t)dpaltqRetCleared, 32);
    for (unsigned i = 0; i < RB_DPALTQ_PHY_COUNT; i++) {
        char k[40];
        snprintf(k, sizeof(k), "RBFBDpAltQRefuse%u", i);
        setProperty(k, (uint64_t)dpaltqRefuse[i], 32);
        snprintf(k, sizeof(k), "RBFBDpAltQUsb%u", i);
        setProperty(k, (uint64_t)dpaltqReply[i].is_usb, 32);
        snprintf(k, sizeof(k), "RBFBDpAltQDis%u", i);
        setProperty(k, (uint64_t)dpaltqReply[i].is_dp_alt_disable, 32);
        snprintf(k, sizeof(k), "RBFBDpAltQDp4%u", i);
        setProperty(k, (uint64_t)dpaltqReply[i].is_dp4, 32);
        snprintf(k, sizeof(k), "RBFBDpAltQEcho%u", i);
        setProperty(k, (uint64_t)dpaltqReply[i].phy_id, 32);
    }
    for (unsigned i = 0; i < RB_ENC_COUNT; i++) {
        char k[32];
        snprintf(k, sizeof(k), "RBFBEncCfg%u", i);
        setProperty(k, (uint64_t)encSurveyCfg[i], 64);
        snprintf(k, sizeof(k), "RBFBEncScram%u", i);
        setProperty(k, (uint64_t)encSurveyScram[i], 64);
    }
    {
        /* Raw beside any summary, which has paid for itself four times now
         * (2B0, 2B4, 2B9, 2DA): every verdict here rests on a comparison
         * against DOMAIN0, and if that reasoning is ever wrong the words
         * themselves must still be in the registry to re-read. Published at
         * 64 bits because a 32-bit OSNumber with bit 31 set renders as a
         * sign-extended decimal (2B4), and PGFSM_PWR_STATUS is bits 31:30 -
         * so every one of these words can have bit 31 set. */
        pgSample(pgStatus, pgConfig);
        OSData *st = OSData::withBytes(pgStatus, sizeof(pgStatus));
        if (st) { setProperty("RBFBPgStatus", st); st->release(); }
        OSData *cf = OSData::withBytes(pgConfig, sizeof(pgConfig));
        if (cf) { setProperty("RBFBPgConfig", cf); cf->release(); }
        OSData *bf = OSData::withBytes(pgBefore, sizeof(pgBefore));
        if (bf) { setProperty("RBFBPgBefore", bf); bf->release(); }
        OSData *af = OSData::withBytes(pgAfter, sizeof(pgAfter));
        if (af) { setProperty("RBFBPgAfter", af); af->release(); }
        /* 2DE: published as the DOMAIN NUMBER, which is what `dmubbl dom
         * <n>` takes and what the command carries - a slot index here would
         * read 4 and name DOMAIN16, and a reader would type 4. -1 is "no
         * domain differs", still the honest third outcome. */
        {
            const int cand = rb_pg_candidate(pgStatus);
            setProperty("RBFBPgCandidate",
                        (uint64_t)(int64_t)(cand < 0 ? -1 :
                            (int)rb_pg_slot_inst((uint32_t)cand)), 32);
            setProperty("RBFBPgSlots", (uint64_t)RB_PG_DOMAIN_COUNT, 32);
        }
    }
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
    /* -1 until one is sent, which reads as 4294967295 - distinguishable from
     * a real level, since the trigger refuses anything above 65536. */
    setProperty("RBFBDmubBlLast", (uint64_t)(uint32_t)dmubBlLast, 32);
    setProperty("RBFBDmubBlType", dmubBlType, 32);
    setProperty("RBFBDmubBlAuxInst", dmubBlAuxInst, 32);
    setProperty("RBFBDmubBlTypeSets", dmubBlTypeSets, 32);
    /* The reply, and the bytes it was read from. Rule 26: the verdict is a
     * derived number, so both buffers go out beside it and the tool decodes
     * them itself rather than trusting the verdict alone. */
    setProperty("RBFBDmubCapsSends", (uint64_t)dmubCapsSends, 32);
    setProperty("RBFBDmubReplyVerdict", (uint64_t)dmubReplyVerdict, 32);
    setProperty("RBFBDmubReplySlot", (uint64_t)dmubReplySlot, 32);
    setProperty("RBFBDmubConfirms", (uint64_t)dmubConfirms, 32);
    setProperty("RBFBDmubGpintSends", (uint64_t)dmubGpintSends, 32);
    setProperty("RBFBDmubGpintRefuse", (uint64_t)dmubGpintRefuse, 32);
    setProperty("RBFBDmubGpintSent", (uint64_t)dmubGpintSent, 32);
    setProperty("RBFBDmubGpintBack", (uint64_t)dmubGpintBack, 32);
    setProperty("RBFBDmubGpintAcked", (uint64_t)dmubGpintAcked, 32);
    setProperty("RBFBDmubGpintSpins", (uint64_t)dmubGpintSpins, 32);
    setProperty("RBFBDmubScratch0Pre", (uint64_t)dmubScratch0Pre, 32);
    setProperty("RBFBDmubScratch7Pre", (uint64_t)dmubScratch7Pre, 32);
    setProperty("RBFBDmubScratch0", (uint64_t)dmubScratch0, 32);
    setProperty("RBFBDmubScratch7", (uint64_t)dmubScratch7, 32);
    setProperty("RBFBDmubDataout", (uint64_t)dmubDataout, 32);
    setProperty("RBFBDmubWinSel", (uint64_t)dmubWinSel, 32);
    setProperty("RBFBDmubSelSets", (uint64_t)dmubSelSets, 32);
    setProperty("RBFBDmubInboxInR3", (uint64_t)dmubInboxInR3, 32);
    {
        OSData *d = OSData::withBytes(dmubRegRegs, sizeof(dmubRegRegs));
        if (d) { setProperty("RBFBDmubRegions", d); d->release(); }
        d = OSData::withBytes(dmubRegScan, sizeof(dmubRegScan));
        if (d) { setProperty("RBFBDmubRegScan", d); d->release(); }
        uint32_t cand[RB_DMUB_WINSEL_COUNT];
        for (unsigned i = 0; i < RB_DMUB_WINSEL_COUNT; i++) {
            cand[i] = dmubCand[i].refuse;
        }
        d = OSData::withBytes(cand, sizeof(cand));
        if (d) { setProperty("RBFBDmubCandOk", d); d->release(); }
    }
    {
        OSData *d = OSData::withBytes(dmubReplySent, RB_DMUB_CMD_SIZE);
        if (d) { setProperty("RBFBDmubReplySent", d); d->release(); }
        d = OSData::withBytes(dmubReplyGot, RB_DMUB_CMD_SIZE);
        if (d) { setProperty("RBFBDmubReplyGot", d); d->release(); }
    }
}
