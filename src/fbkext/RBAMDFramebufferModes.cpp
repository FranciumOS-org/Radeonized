/* 2y6: a second display mode, so 60 Hz is something macOS can be asked for
 * rather than an experiment that reverts itself.
 *
 * WHAT BOOT 255 SETTLED AND WHAT IT DID NOT. 2y5 wrote V_TOTAL_MIN/MAX on the
 * live raster and measured 60025 mHz against a predicted 59989, with the
 * revert putting 143821 back - so the panel accepts a stretched vertical blank
 * and our arithmetic picks the right line count. What it deliberately did NOT
 * do is tell anybody: macOS advertised one mode at 143.836 Hz and our VBL
 * thread_call kept ticking at that rate, so for eight seconds the compositor
 * was paced against a rate the raster was not running at. The whole content of
 * this milestone is making those three things move together.
 *
 * THREE NUMBERS PER MODE, FROM ONE SOURCE. A mode needs the line count to
 * write, the 16.16 refreshRate IOFramebuffer advertises, and the nanosecond
 * period our VBL ticks at. rb_drr.h derives the second from the first and the
 * third from the second, in that order, so there is one rate per mode rather
 * than three expressions of it that can drift. The 2xe frame-rate work was all
 * measured against the 143.83 Hz period, and the hosttest asserts the table
 * reproduces that constant bit for bit - if the mode table silently retimes
 * the compositor, the build fails rather than the next boot being confusing.
 *
 * THE PIXEL CLOCK IS DERIVED, NOT READ. It comes from the DP PLL and this
 * project has never located it. It does not need to: clk = mHz * H_TOTAL *
 * V_TOTAL, and all three are measured on the live raster at setup. The reason
 * that is trustworthy rather than circular is the round-trip the hosttest
 * makes it do - the derived clock must return the V_TOTAL it was derived from.
 * A clock that was wrong by a factor would still yield a plausible-looking
 * 60 Hz line count, so the 60 Hz answer alone would prove nothing.
 *
 * WHY OPT-IN, when 2y5 proved the write works. The write is not the new risk
 * here; ADVERTISING is. `getDisplayModeCount()` has returned 1 since 2sl and
 * every boot since has been read against that. A second mode changes what
 * WindowServer sees before any user has asked for anything, at the moment the
 * framebuffer comes up - and 2xx is the standing lesson about what a
 * WindowServer that dislikes its framebuffer costs to diagnose. So
 * `-rbgpufbmodes` is opt-in, and on any refusal the table collapses back to
 * the single mode the last six milestones were measured on, rather than
 * offering a mode it cannot deliver.
 *
 * MUTUALLY EXCLUSIVE WITH -rbgpufbdrr, and checked rather than documented. The
 * experiment asserts ownership of V_TOTAL_MIN/MAX for eight seconds; if the
 * user has meanwhile selected 60 Hz, `rb_drr_check` would see our own
 * V_TOTAL_CONTROL and refuse with RB_DRR_FW_DRR - a refusal that reads
 * "firmware owns DRR" when in fact we do. A published value that can mean two
 * things is rule 14's complaint, so drrSetup() declines outright when the mode
 * table is armed.
 */
#include "RBAMDFramebuffer.hpp"

#include <IOKit/IOLib.h>

extern "C" {
#include "rb_drr.h"
}

/* The same BAR5 map and segment base the backlight proved with OTG0_CONTROL
 * and 2y5 wrote through. Not a second window onto the chip. */
static const uint32_t kModeSeg2 = 0x34c0;

#define MODE_DW_H_TOTAL      0x1b2au
#define MODE_DW_V_TOTAL      0x1b2fu
#define MODE_DW_V_TOTAL_MIN  0x1b30u
#define MODE_DW_V_TOTAL_MAX  0x1b31u
#define MODE_DW_V_TOTAL_CNTL 0x1b33u

/* Refusals. Numbered above rb_drr.h's enum AND above the DRR file's 10..14, so
 * one published number can never mean two things across the two features. */
#define MODE_REFUSE_NOT_ARMED 20u
#define MODE_REFUSE_NO_MAP    21u
#define MODE_REFUSE_MEASURE   22u
#define MODE_REFUSE_NO_CLK    23u
#define MODE_REFUSE_NO_LINES  24u
#define MODE_REFUSE_WRITE_LOST 25u
#define MODE_REFUSE_BAD_MODE  26u
#define MODE_REFUSE_DRR_BUSY  27u

void RBAMDFramebuffer::modeReset(void)
{
    modeArmed = false;
    modeRefuse = MODE_REFUSE_NOT_ARMED;
    modeCur = kModeIDFast;
    modeClkHz = 0;
    modeBaseMhz = 0;
    modeCount = 0;
    for (unsigned i = 0; i < RB_DRR_MAX_MODES; i++) {
        modeList[i].lines = 0;
        modeList[i].mhz = 0;
        modeList[i].rate1616 = 0;
        modeList[i].period_ns = 0;
        modeList[i].native = 0;
    }
    modeSwitches = 0;
    modeApplies = 0;
    modeLastAsked = 0;
    modeMinRb = 0;
    modeMaxRb = 0;
    modeCtlRb = 0;
    modeCtl0 = 0;
    modeMin0 = 0;
    modeMax0 = 0;
    modeHTotal0 = 0;
    modeVTotal0 = 0;
}

bool RBAMDFramebuffer::modeSetup(void)
{
    if (!argEnabled("-rbgpufbmodes")) { return false; }

    /* Rule 38: this half needs the backlight's BAR5 map, and says so here
     * rather than discovering it in setDisplayMode with the compositor
     * waiting on an answer. */
    if (!blReady || !blRegs) { modeRefuse = MODE_REFUSE_NO_MAP; return false; }

    volatile uint32_t *regs = blRegs;
    const uint32_t htotal = regs[kModeSeg2 + MODE_DW_H_TOTAL] & RB_DRR_TOTAL_MASK;
    const uint32_t vtotal = regs[kModeSeg2 + MODE_DW_V_TOTAL] & RB_DRR_TOTAL_MASK;

    modeHTotal0 = htotal;
    modeVTotal0 = vtotal;
    modeCtl0 = regs[kModeSeg2 + MODE_DW_V_TOTAL_CNTL];
    modeMin0 = regs[kModeSeg2 + MODE_DW_V_TOTAL_MIN];
    modeMax0 = regs[kModeSeg2 + MODE_DW_V_TOTAL_MAX];

    /* ONE measurement, at setup, cached. Not per setDisplayMode: that is a
     * ~300 ms bounded busy-wait and setDisplayMode is answered with the
     * compositor blocked on it. The same sampler 2y5 used and 2rn established
     * - latch both endpoints on a FRAME_COUNT edge. */
    modeBaseMhz = measureMhz(regs);
    if (modeBaseMhz == 0u) { modeRefuse = MODE_REFUSE_MEASURE; return false; }

    modeClkHz = rb_drr_clk_from_mhz(modeBaseMhz, htotal, vtotal);
    if (modeClkHz == 0u) { modeRefuse = MODE_REFUSE_NO_CLK; return false; }

    /* 2z0: the whole list, derived from this raster. The native rate is the
     * registry's own constant rather than a derived one - the measured refresh
     * moves ~0.05 % between boots (2y6), and every boot since 2sl has been
     * read against 0x008fd53f. */
    modeCount = rb_drr_build_modes(modeClkHz, htotal, vtotal,
                                   kRefreshRate1616, modeList,
                                   RB_DRR_MAX_MODES);
    if (modeCount == 0u) {
        modeRefuse = MODE_REFUSE_NO_LINES;
        return false;
    }

    /* Drop any non-native entry this raster will not accept, rather than
     * advertising it. rb_drr_check is asked per mode because it is what
     * modeApply will ask later, and a mode that would be refused at switch
     * time must not be in the list at all (rule 14: a refusal the user can
     * only discover by selecting it is indistinguishable from a bug). */
    {
        unsigned keep = 1;
        for (unsigned i = 1; i < modeCount; i++) {
            if (rb_drr_check(htotal, vtotal, modeCtl0,
                             modeList[i].lines) == RB_DRR_OK &&
                modeList[i].rate1616 != 0u && modeList[i].period_ns != 0u) {
                modeList[keep++] = modeList[i];
            }
        }
        modeCount = keep;
    }

    modeCur = kModeIDFast;
    modeArmed = true;
    modeRefuse = 0;
    return true;
}

void RBAMDFramebuffer::modeTeardown(void)
{
    /* Rule 37 asked of a mode: a kext that stops while the panel is at 60 Hz
     * would leave the raster at a refresh nothing advertises and nothing will
     * ever put back. Cheap to avoid, and the same reasoning drrTeardown()
     * already carries. */
    if (modeArmed && modeCur != kModeIDFast && blReady && blRegs) {
        (void)modeApply(kModeIDFast);
    }
    modeArmed = false;
}

/* The timing write, and the VBL retune, in that order and for a reason: the
 * register write is the half that can fail, and retuning the clock after a
 * failed write would leave our frame clock lying about the raster. If the
 * write is lost, the clock is never touched and the mode is refused. */
bool RBAMDFramebuffer::modeApply(IODisplayModeID want)
{
    if (!modeArmed)          { modeRefuse = MODE_REFUSE_NOT_ARMED; return false; }
    if (!blReady || !blRegs) { modeRefuse = MODE_REFUSE_NO_MAP; return false; }

    const int idx = modeIndexOf(want);
    if (idx < 0) { modeRefuse = MODE_REFUSE_BAD_MODE; return false; }

    volatile uint32_t *regs = blRegs;
    modeApplies++;

    if (modeList[idx].native) {
        /* NATIVE IS A RESTORE, NOT A WRITE. The firmware set this timing
         * before the kext existed, so the correct action is to put MIN/MAX/CTL
         * back to the values read at setup - CONTROL first, then MIN/MAX, the
         * mirror of the arming order, so a SEL bit is never left set over a
         * MIN of zero. */
        regs[kModeSeg2 + MODE_DW_V_TOTAL_CNTL] = modeCtl0;
        regs[kModeSeg2 + MODE_DW_V_TOTAL_MIN]  = modeMin0;
        regs[kModeSeg2 + MODE_DW_V_TOTAL_MAX]  = modeMax0;
        modeMinRb = regs[kModeSeg2 + MODE_DW_V_TOTAL_MIN] & RB_DRR_TOTAL_MASK;
        modeMaxRb = regs[kModeSeg2 + MODE_DW_V_TOTAL_MAX] & RB_DRR_TOTAL_MASK;
        modeCtlRb = regs[kModeSeg2 + MODE_DW_V_TOTAL_CNTL];
    } else {
        const uint32_t lines = modeList[idx].lines;
        /* Live, not the setup snapshot - rule 41 asked of a register. What the
         * raster was doing at start() is not what it is doing when a user
         * opens the Displays panel twenty minutes later. */
        const uint32_t htotal = regs[kModeSeg2 + MODE_DW_H_TOTAL] & RB_DRR_TOTAL_MASK;
        const uint32_t vtotal = regs[kModeSeg2 + MODE_DW_V_TOTAL] & RB_DRR_TOTAL_MASK;
        const uint32_t ctl0   = regs[kModeSeg2 + MODE_DW_V_TOTAL_CNTL];

        const int why = rb_drr_check(htotal, vtotal, ctl0, lines);
        if (why != RB_DRR_OK) { modeRefuse = (uint32_t)why; return false; }

        /* MIN and MAX first, CONTROL last. */
        regs[kModeSeg2 + MODE_DW_V_TOTAL_MIN] =
            (modeMin0 & ~RB_DRR_TOTAL_MASK) | (lines & RB_DRR_TOTAL_MASK);
        regs[kModeSeg2 + MODE_DW_V_TOTAL_MAX] =
            (modeMax0 & ~RB_DRR_TOTAL_MASK) | (lines & RB_DRR_TOTAL_MASK);
        regs[kModeSeg2 + MODE_DW_V_TOTAL_CNTL] = rb_drr_ctl_on(ctl0);

        modeMinRb = regs[kModeSeg2 + MODE_DW_V_TOTAL_MIN] & RB_DRR_TOTAL_MASK;
        modeMaxRb = regs[kModeSeg2 + MODE_DW_V_TOTAL_MAX] & RB_DRR_TOTAL_MASK;
        modeCtlRb = regs[kModeSeg2 + MODE_DW_V_TOTAL_CNTL];

        /* "The store executed" and "the store reached the chip" differ (2lq).
         * A lost write is put back rather than left half-applied. */
        if (modeMinRb != (lines & RB_DRR_TOTAL_MASK) ||
            modeMaxRb != (lines & RB_DRR_TOTAL_MASK)) {
            regs[kModeSeg2 + MODE_DW_V_TOTAL_CNTL] = ctl0;
            regs[kModeSeg2 + MODE_DW_V_TOTAL_MIN]  = modeMin0;
            regs[kModeSeg2 + MODE_DW_V_TOTAL_MAX]  = modeMax0;
            modeRefuse = MODE_REFUSE_WRITE_LOST;
            return false;
        }
    }

    /* The clock follows the timing, and only after the write that can fail has
     * succeeded - retuning after a failed write would leave the frame clock
     * lying about the raster (2y6). */
    vblRetune(modeList[idx].period_ns);

    if (modeCur != want) { modeSwitches++; }
    modeCur = want;
    modeRefuse = 0;
    return true;
}

/* The 16.16 rate for a mode id, used by getInformationForDisplayMode. Mode 100
 * keeps the value out of `.Display_boot`'s own live IOFBConfig rather than a
 * re-derived one: it is what the registry already carries and what every boot
 * since 2sl has been read against, and re-deriving it would move it by 0.01 Hz
 * for no gain. Mode 101's comes from the chip, because nothing published it. */
/* -1 when the id is not one of ours. One place decides, so the five query
 * methods and modeApply cannot disagree about it. */
int RBAMDFramebuffer::modeIndexOf(IODisplayModeID id) const
{
    const int i = (int)(id - kModeIDBase);
    if (i < 0) { return -1; }
    if (!modeArmed) { return i == 0 ? 0 : -1; }
    return ((unsigned)i < modeCount) ? i : -1;
}

UInt32 RBAMDFramebuffer::modeRate1616(IODisplayModeID id) const
{
    const int i = modeIndexOf(id);
    if (i < 0 || !modeArmed) { return kRefreshRate1616; }
    return modeList[i].rate1616 ? modeList[i].rate1616 : kRefreshRate1616;
}
