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
    modeLines60 = 0;
    modeMhz60 = 0;
    modeRate60 = 0;
    modePeriod60 = 0;
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

    modeLines60 = rb_drr_lines_for_mhz(modeClkHz, htotal, 60000u);
    if (modeLines60 == 0u ||
        rb_drr_check(htotal, vtotal, modeCtl0, modeLines60) != RB_DRR_OK) {
        modeRefuse = MODE_REFUSE_NO_LINES;
        return false;
    }

    /* What the chip will DO at that line count, not the 60000 that was asked
     * for. Advertising the request instead of the achievable value is how a
     * mode list comes to lie about the panel. */
    modeMhz60 = rb_drr_mhz(modeClkHz, htotal, modeLines60);
    modeRate60 = rb_drr_rate1616(modeMhz60);
    modePeriod60 = rb_drr_period_ns(modeRate60);
    if (modeRate60 == 0u || modePeriod60 == 0u) {
        modeRefuse = MODE_REFUSE_NO_CLK;
        return false;
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
    if (want != kModeIDFast && want != kModeID60) {
        modeRefuse = MODE_REFUSE_BAD_MODE;
        return false;
    }

    volatile uint32_t *regs = blRegs;
    modeApplies++;

    if (want == kModeID60) {
        /* Live, not the setup snapshot - rule 41's question asked of a
         * register. This computes nothing from them, but it refuses on them,
         * and what the raster was doing at start() is not what it is doing
         * when a user opens the Displays panel twenty minutes later. */
        const uint32_t htotal = regs[kModeSeg2 + MODE_DW_H_TOTAL] & RB_DRR_TOTAL_MASK;
        const uint32_t vtotal = regs[kModeSeg2 + MODE_DW_V_TOTAL] & RB_DRR_TOTAL_MASK;
        const uint32_t ctl0   = regs[kModeSeg2 + MODE_DW_V_TOTAL_CNTL];

        const int why = rb_drr_check(htotal, vtotal, ctl0, modeLines60);
        if (why != RB_DRR_OK) { modeRefuse = (uint32_t)why; return false; }

        /* MIN and MAX first, CONTROL last - the reverse order would arm a SEL
         * bit over a MIN of zero, which is a frame length of nothing. */
        regs[kModeSeg2 + MODE_DW_V_TOTAL_MIN] =
            (modeMin0 & ~RB_DRR_TOTAL_MASK) | (modeLines60 & RB_DRR_TOTAL_MASK);
        regs[kModeSeg2 + MODE_DW_V_TOTAL_MAX] =
            (modeMax0 & ~RB_DRR_TOTAL_MASK) | (modeLines60 & RB_DRR_TOTAL_MASK);
        regs[kModeSeg2 + MODE_DW_V_TOTAL_CNTL] = rb_drr_ctl_on(ctl0);

        modeMinRb = regs[kModeSeg2 + MODE_DW_V_TOTAL_MIN] & RB_DRR_TOTAL_MASK;
        modeMaxRb = regs[kModeSeg2 + MODE_DW_V_TOTAL_MAX] & RB_DRR_TOTAL_MASK;
        modeCtlRb = regs[kModeSeg2 + MODE_DW_V_TOTAL_CNTL];

        /* "The store executed" and "the store reached the chip" are different
         * facts (2lq in its register form). A lost write is put back rather
         * than left half-applied, because the alternative is a raster at a
         * length nothing advertises. */
        if (modeMinRb != (modeLines60 & RB_DRR_TOTAL_MASK) ||
            modeMaxRb != (modeLines60 & RB_DRR_TOTAL_MASK)) {
            regs[kModeSeg2 + MODE_DW_V_TOTAL_CNTL] = ctl0;
            regs[kModeSeg2 + MODE_DW_V_TOTAL_MIN]  = modeMin0;
            regs[kModeSeg2 + MODE_DW_V_TOTAL_MAX]  = modeMax0;
            modeRefuse = MODE_REFUSE_WRITE_LOST;
            return false;
        }

        vblRetune(modePeriod60);
    } else {
        /* CONTROL first, then MIN/MAX - the mirror, for the same reason. The
         * restore values are the ones read at setup, which boot 254 and boot
         * 255 both found to be zeros; they are READ rather than inferred. */
        regs[kModeSeg2 + MODE_DW_V_TOTAL_CNTL] = modeCtl0;
        regs[kModeSeg2 + MODE_DW_V_TOTAL_MIN]  = modeMin0;
        regs[kModeSeg2 + MODE_DW_V_TOTAL_MAX]  = modeMax0;
        modeMinRb = regs[kModeSeg2 + MODE_DW_V_TOTAL_MIN] & RB_DRR_TOTAL_MASK;
        modeMaxRb = regs[kModeSeg2 + MODE_DW_V_TOTAL_MAX] & RB_DRR_TOTAL_MASK;
        modeCtlRb = regs[kModeSeg2 + MODE_DW_V_TOTAL_CNTL];

        vblRetune(kVBLPeriodNsFast);
    }

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
UInt32 RBAMDFramebuffer::modeRate1616(IODisplayModeID id) const
{
    if (id == kModeID60 && modeArmed) { return modeRate60; }
    return kRefreshRate1616;
}
