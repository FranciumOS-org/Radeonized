/* 2y5: change the panel's refresh rate on a raster that is already scanning.
 *
 * WHY THIS SHAPE. The panel runs at clk/(H_TOTAL * V_TOTAL) and the active
 * window is set by OTG_V_BLANK_START_END independently of V_TOTAL, so a longer
 * V_TOTAL is a longer vertical blank and nothing else - same active raster,
 * same sync widths, same pixel clock, same DP link, no retraining. 60 Hz is
 * 2707 lines (59.989 Hz), computed by rb_drr.h and pinned by the hosttest
 * against the MEASURED 143836 mHz.
 *
 * OTG_V_TOTAL itself is NOT written. That is Linux's modeset path and it wants
 * the pipe locked; this pipe is live with the desktop on it. V_TOTAL_MIN/MAX
 * plus the MIN_SEL/MAX_SEL bits of V_TOTAL_CONTROL are the variable-refresh
 * path, which exists precisely to be written while scanning.
 *
 * WHAT THIS IS NOT YET: a display mode. macOS still advertises one mode at
 * 143.836 Hz and our VBL thread_call still ticks at that rate, so while the
 * raster is at 60 Hz the compositor is paced against a rate it is not running
 * at. That is why this reverts itself and why it is a measurement rather than
 * a feature - the mode list and the VBL clock have to move together with the
 * timing, and none of that is worth building before the panel has been shown
 * to accept 2707 lines at all.
 *
 * THE SAFETY ARGUMENT, since this is the first write this project has made to
 * the display pipe rather than to a surface address:
 *   - opt-in only, `-rbgpufbdrr`, never in rb_defaults.h;
 *   - it runs ONCE, from a thread_call, `rbgpudrrdelay` seconds after start,
 *     so the desktop and SSH are up before anything moves;
 *   - it reverts unconditionally after `rbgpudrrsecs`, on the same thread,
 *     with no dependency on anything observing it. A panel that will not sync
 *     to 2707 lines therefore goes black for a few seconds and comes back,
 *     rather than needing a power cycle (rule 12: a black screen we cannot
 *     diagnose from is the state to avoid);
 *   - the restore values are not inferred. Boot 254 read MIN/MAX/MID/CONTROL
 *     as four zeros, and `rb_drr_check` REFUSES if V_TOTAL_CONTROL is non-zero,
 *     because then the firmware is using DRR and what to restore is unknown;
 *   - write order is MIN/MAX first then CONTROL, and CONTROL first then
 *     MIN/MAX on the way back, so the window where a SEL bit is set over a
 *     zero MIN never exists;
 *   - and the third measurement is the control: the refresh must come back to
 *     where it started, or the revert did not work and we need to know that
 *     from the same boot rather than from the next one.
 */
#include "RBAMDFramebuffer.hpp"

#include <IOKit/IOLib.h>
#include <IOKit/IOTimerEventSource.h>
#include <kern/thread_call.h>
#include <mach/mach_time.h>

extern "C" {
#include "rb_drr.h"
}

/* Same BAR5 map and same segment base the backlight proved with
 * OTG0_CONTROL - this does not open a second window onto the chip. */
static const uint32_t kDrrSeg2 = 0x34c0;

/* dcn_3_1_2_offset.h:8767-8776, 0x1b2a, 0x1b4c; base_idx 2. The same values
 * rb_regs.h carries and the hosttest cross-checks. */
#define DRR_DW_H_TOTAL      0x1b2au
#define DRR_DW_V_TOTAL      0x1b2fu
#define DRR_DW_V_TOTAL_MIN  0x1b30u
#define DRR_DW_V_TOTAL_MAX  0x1b31u
#define DRR_DW_V_TOTAL_CNTL 0x1b33u
#define DRR_DW_FRAME_COUNT  0x1b4cu

/* Refusals that are ours rather than rb_drr.h's; numbered above its enum so
 * one published value can never mean two things. */
#define DRR_REFUSE_NOT_ARMED   10u
#define DRR_REFUSE_NO_MAP      11u
#define DRR_REFUSE_NO_BASE     12u
#define DRR_REFUSE_MEAS_BEFORE 13u
#define DRR_REFUSE_WRITE_LOST  14u

/* Mirrors the plugin's measured sampler exactly (2rn): latch both endpoints ON
 * a FRAME_COUNT edge, never over a fixed window, because a fixed window
 * measures the sleep and not the raster. Bounded at 1.2 s of 100 us polls,
 * which covers 16 frames at 60 Hz (266 ms) with room, where the plugin's
 * 400 ms bound would not have. */
uint32_t RBAMDFramebuffer::measureMhz(volatile uint32_t *regs)
{
    const uint32_t kFrames  = 16;
    const uint32_t kPollUs  = 100;
    const uint32_t kMaxPoll = 12000;
    uint32_t polls = 0;
    uint32_t prev = regs[kDrrSeg2 + DRR_DW_FRAME_COUNT];
    uint64_t t0, t1, ns = 0;
    uint32_t frames = 0;

    while (regs[kDrrSeg2 + DRR_DW_FRAME_COUNT] == prev) {
        if (++polls >= kMaxPoll) { return 0; }
        IODelay(kPollUs);
    }
    t0 = mach_absolute_time();
    t1 = t0;
    prev = regs[kDrrSeg2 + DRR_DW_FRAME_COUNT];
    while (frames < kFrames) {
        if (++polls >= kMaxPoll) { return 0; }
        IODelay(kPollUs);
        const uint32_t now = regs[kDrrSeg2 + DRR_DW_FRAME_COUNT];
        if (now != prev) {
            frames += now - prev;   /* may step by more than one */
            prev = now;
            t1 = mach_absolute_time();
        }
    }
    absolutetime_to_nanoseconds(t1 - t0, &ns);
    if (!frames || !ns) { return 0; }
    return (uint32_t)((uint64_t)frames * 1000000000000ull / ns);
}

/* Three steps, each re-arming the next, rather than one callout that sleeps.
 *
 * The first version of this did `IOSleep(drrSecs * 1000)` in the callout, and
 * that is wrong here for a specific reason: our own VBL delivery is a
 * thread_call that must fire every 6944 us, and thread_call callouts run on a
 * shared worker pool. Occupying a worker for eight seconds to wait out an
 * experiment puts the panel's frame clock behind a sleep. So the wait is a
 * DEADLINE on the next step instead, and the only blocking left is the bounded
 * poll inside drrMeasureMhz (~300 ms worst case, the same busy-wait the
 * plugin's sampler has always done once at startup).
 *
 * Each step is also the revert's last chance: step 2 reverts whatever step 1
 * wrote, and it is armed before step 1 touches a register. */
void RBAMDFramebuffer::drrRun(void)
{
    drrRuns++;

    if (!drrArmed)           { drrRefuse = DRR_REFUSE_NOT_ARMED; return; }
    if (!blReady || !blRegs) { drrRefuse = DRR_REFUSE_NO_MAP; return; }

    volatile uint32_t *regs = blRegs;

    if (drrStep == 0u) {
        /* Live, not the blSetup() snapshot: rule 41's question asked of a
         * register. What the raster was doing at start() is not what it is
         * doing now, and this computes a value to WRITE from it. */
        const uint32_t htotal = regs[kDrrSeg2 + DRR_DW_H_TOTAL] & RB_DRR_TOTAL_MASK;
        const uint32_t vtotal = regs[kDrrSeg2 + DRR_DW_V_TOTAL] & RB_DRR_TOTAL_MASK;
        const uint32_t ctl0   = regs[kDrrSeg2 + DRR_DW_V_TOTAL_CNTL];

        drrVTotalLive = vtotal;
        drrCtl0 = ctl0;
        drrMin0 = regs[kDrrSeg2 + DRR_DW_V_TOTAL_MIN];
        drrMax0 = regs[kDrrSeg2 + DRR_DW_V_TOTAL_MAX];

        const int why = rb_drr_check(htotal, vtotal, ctl0, drrLines);
        if (why != RB_DRR_OK) { drrRefuse = (uint32_t)why; return; }

        /* Measured before anything is written, and a measurement that fails is
         * a refusal rather than a baseline of zero to compare against
         * (rule 14). Nothing has been written yet, so returning here leaves
         * the chip untouched. */
        drrMhzBefore = measureMhz(regs);
        if (drrMhzBefore == 0u) { drrRefuse = DRR_REFUSE_MEAS_BEFORE; return; }

        /* Arm step 2 BEFORE writing, so the revert is already scheduled when
         * the raster changes. If the panel drops sync, the thing that puts it
         * back is already on the clock. */
        drrStep = 1u;
        uint64_t deadline = 0;
        clock_interval_to_deadline(drrSecs, NSEC_PER_SEC, &deadline);
        thread_call_enter_delayed(drrCall, deadline);

        /* MIN and MAX first, CONTROL last. The reverse order would arm a SEL
         * bit over a MIN of zero. */
        regs[kDrrSeg2 + DRR_DW_V_TOTAL_MIN] =
            (drrMin0 & ~RB_DRR_TOTAL_MASK) | (drrLines & RB_DRR_TOTAL_MASK);
        regs[kDrrSeg2 + DRR_DW_V_TOTAL_MAX] =
            (drrMax0 & ~RB_DRR_TOTAL_MASK) | (drrLines & RB_DRR_TOTAL_MASK);
        regs[kDrrSeg2 + DRR_DW_V_TOTAL_CNTL] = rb_drr_ctl_on(ctl0);

        drrMinRb = regs[kDrrSeg2 + DRR_DW_V_TOTAL_MIN] & RB_DRR_TOTAL_MASK;
        drrMaxRb = regs[kDrrSeg2 + DRR_DW_V_TOTAL_MAX] & RB_DRR_TOTAL_MASK;
        drrCtlRb = regs[kDrrSeg2 + DRR_DW_V_TOTAL_CNTL];

        /* "The store executed" and "the store reached the chip" are different
         * facts - 2lq in its register form, the same check blSetLevel makes.
         * A lost write is reported and the revert still runs. */
        if (drrMinRb != (drrLines & RB_DRR_TOTAL_MASK) ||
            drrMaxRb != (drrLines & RB_DRR_TOTAL_MASK)) {
            drrRefuse = DRR_REFUSE_WRITE_LOST;
        }
        return;
    }

    if (drrStep == 1u) {
        drrMhzDuring = measureMhz(regs);

        /* CONTROL first, then MIN/MAX - the mirror of the arming order, for
         * the same reason. */
        regs[kDrrSeg2 + DRR_DW_V_TOTAL_CNTL] = drrCtl0;
        regs[kDrrSeg2 + DRR_DW_V_TOTAL_MIN]  = drrMin0;
        regs[kDrrSeg2 + DRR_DW_V_TOTAL_MAX]  = drrMax0;
        drrReverted = (regs[kDrrSeg2 + DRR_DW_V_TOTAL_CNTL] == drrCtl0) &&
                      (regs[kDrrSeg2 + DRR_DW_V_TOTAL_MIN] == drrMin0) &&
                      (regs[kDrrSeg2 + DRR_DW_V_TOTAL_MAX] == drrMax0);

        drrStep = 2u;
        uint64_t deadline = 0;
        clock_interval_to_deadline(500, NSEC_PER_MSEC, &deadline);
        thread_call_enter_delayed(drrCall, deadline);
        return;
    }

    /* The control. If this does not come back to drrMhzBefore the revert did
     * not take, and that has to be a finding from THIS boot rather than a
     * question for the next one. */
    drrMhzAfter = measureMhz(regs);
    drrStep = 3u;
    if (drrRefuse == 0u) { drrDone = true; }
}

static void drrThunk(thread_call_param_t p0, thread_call_param_t)
{
    RBAMDFramebuffer *fb = static_cast<RBAMDFramebuffer *>(p0);
    if (fb) { fb->drrRun(); }
}

void RBAMDFramebuffer::drrReset(void)
{
    drrArmed = false;
    drrDone = false;
    drrReverted = false;
    drrRefuse = DRR_REFUSE_NOT_ARMED;
    drrLines = 0;
    drrSecs = 0;
    drrDelay = 0;
    drrRuns = 0;
    drrMhzBefore = 0;
    drrMhzDuring = 0;
    drrMhzAfter = 0;
    drrMinRb = 0;
    drrMaxRb = 0;
    drrCtlRb = 0;
    drrCtl0 = 0;
    drrVTotalLive = 0;
    drrCall = nullptr;
    drrStep = 0;
    drrMin0 = 0;
    drrMax0 = 0;
}

bool RBAMDFramebuffer::drrSetup(void)
{
    if (!argEnabled("-rbgpufbdrr")) { return false; }

    /* 2y6: the two features would fight over V_TOTAL_MIN/MAX, and the fight
     * would MISREPORT itself - with the mode table holding 60 Hz,
     * rb_drr_check() sees a non-zero V_TOTAL_CONTROL and refuses with
     * RB_DRR_FW_DRR, which reads "the firmware owns DRR" when in fact we do.
     * A published value that can mean two things is rule 14's complaint, so
     * this declines outright and says which. The mode table is the better of
     * the two in any case: it is the same write with somebody listening. */
    if (modeArmed) { drrRefuse = 27u; return false; }

    /* Defaults are the measured 60 Hz, a short visit and enough delay that a
     * boot which goes black is still a boot somebody can reach over SSH. */
    int lines = 2707, secs = 8, delay = 45;
    (void)PE_parse_boot_argn("rbgpudrrlines", &lines, sizeof(lines));
    (void)PE_parse_boot_argn("rbgpudrrsecs", &secs, sizeof(secs));
    (void)PE_parse_boot_argn("rbgpudrrdelay", &delay, sizeof(delay));

    if (lines < 1130 || lines > (int)RB_DRR_TOTAL_MASK) { lines = 2707; }
    if (secs < 2 || secs > 60)                          { secs = 8; }
    if (delay < 10 || delay > 600)                      { delay = 45; }

    drrLines = (uint32_t)lines;
    drrSecs  = (uint32_t)secs;
    drrDelay = (uint32_t)delay;

    /* Rule 38: this half needs the backlight's BAR5 map, and says so rather
     * than discovering it in the callout. */
    if (!blReady || !blRegs) { drrRefuse = DRR_REFUSE_NO_MAP; return false; }

    drrCall = thread_call_allocate(drrThunk, this);
    if (!drrCall) { drrRefuse = DRR_REFUSE_NO_BASE; return false; }

    uint64_t deadline = 0;
    clock_interval_to_deadline(drrDelay, NSEC_PER_SEC, &deadline);
    drrArmed = true;
    drrRefuse = 0;
    thread_call_enter_delayed(drrCall, deadline);
    return true;
}

void RBAMDFramebuffer::drrTeardown(void)
{
    if (!drrCall) { return; }
    thread_call_cancel(drrCall);

    /* Rule 37, asked of a half-finished experiment: if step 1 had written the
     * timing and step 2 had not yet put it back, cancelling the callout would
     * leave the raster at a refresh nothing advertises and nothing will ever
     * revert. So the revert happens here too, in the same order, against the
     * values step 0 recorded. Cheap, and the alternative is a panel left at
     * 60 Hz because a kext stopped. */
    if (drrStep == 1u && blReady && blRegs) {
        blRegs[kDrrSeg2 + DRR_DW_V_TOTAL_CNTL] = drrCtl0;
        blRegs[kDrrSeg2 + DRR_DW_V_TOTAL_MIN]  = drrMin0;
        blRegs[kDrrSeg2 + DRR_DW_V_TOTAL_MAX]  = drrMax0;
        drrReverted = (blRegs[kDrrSeg2 + DRR_DW_V_TOTAL_CNTL] == drrCtl0);
        drrStep = 3u;
    }

    thread_call_free(drrCall);
    drrCall = nullptr;
}
