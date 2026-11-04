/* SPDX-License-Identifier: MIT */
#include "rb_retire.h"
#include <stdio.h>

/* 2zd: the handover between the two frame-retirement drivers. The invariant
 * that matters is not "the vblank drives it" - it is that RETIREMENT NEVER
 * STOPS, because 2xe measured what stopping looks like: one frame per ten
 * seconds. The two drivers are in different kernel collections and cannot see
 * each other, so the only thing standing between a disabled vblank and that
 * desktop is this state machine, and it is asserted here before either kext
 * runs it. */

#define QUIET 1000u   /* abstract units; the kext passes an absolute-time gap */

int retire_tests(void)
{
    int bad = 0;

    /* Unarmed: the vblank never takes over, and the offer is COUNTED rather
     * than dropped, so "the handover did nothing" is distinguishable from
     * "the vblank never offered" (rule 14). */
    {
        struct rb_retire r;
        rb_retire_reset(&r, 0u);
        if (rb_retire_vbl_offer(&r, 100u) != 0) {
            printf("FAIL retire: unarmed vblank took over\n"); bad++;
        }
        if (r.vblDeclined != 1u || r.vblRetires != 0u || r.handoffs != 0u ||
            r.src != RB_RETIRE_SRC_TIMER) {
            printf("FAIL retire: unarmed offer not accounted\n"); bad++;
        }
        if (!rb_retire_timer_should(&r, 200u, QUIET) || r.timerRetires != 1u) {
            printf("FAIL retire: unarmed timer stopped retiring\n"); bad++;
        }
    }

    /* Armed: the first vblank hands over exactly once, and while the vblank
     * stays live the timer retires NOTHING - otherwise the two timers are both
     * still calling into the scheduler and 2zc's beat is not removed. */
    {
        struct rb_retire r;
        rb_retire_reset(&r, 1u);
        if (!rb_retire_vbl_offer(&r, 1000u) || r.handoffs != 1u ||
            r.src != RB_RETIRE_SRC_VBL) {
            printf("FAIL retire: first vblank did not hand over\n"); bad++;
        }
        if (!rb_retire_vbl_offer(&r, 1100u) || r.handoffs != 1u) {
            printf("FAIL retire: handoff counted twice\n"); bad++;
        }
        /* A tick inside the quiet window must not retire. */
        if (rb_retire_timer_should(&r, 1500u, QUIET) != 0 ||
            r.timerRetires != 0u) {
            printf("FAIL retire: timer retired while the vblank was live\n");
            bad++;
        }
        if (r.vblRetires != 2u) {
            printf("FAIL retire: vblank retires miscounted\n"); bad++;
        }
    }

    /* THE SAFETY PROPERTY. The vblank goes quiet; the timer takes the job
     * back, once, and keeps retiring on every tick thereafter. A takeback
     * that were not a state change would be rediscovered every tick and the
     * counter would climb forever. */
    {
        struct rb_retire r;
        rb_retire_reset(&r, 1u);
        (void)rb_retire_vbl_offer(&r, 1000u);
        if (!rb_retire_timer_should(&r, 1000u + QUIET, QUIET) ||
            r.takebacks != 1u || r.src != RB_RETIRE_SRC_TIMER) {
            printf("FAIL retire: timer did not take back a dead vblank\n");
            bad++;
        }
        if (!rb_retire_timer_should(&r, 1000u + QUIET * 9u, QUIET) ||
            r.takebacks != 1u) {
            printf("FAIL retire: takeback counted more than once\n"); bad++;
        }
        if (r.timerRetires != 2u) {
            printf("FAIL retire: timer retires miscounted after takeback\n");
            bad++;
        }
        /* And the vblank can come back - a mode switch disables and re-enables
         * it, which is a thing boot 259 did on purpose. */
        if (!rb_retire_vbl_offer(&r, 20000u) || r.handoffs != 2u ||
            r.src != RB_RETIRE_SRC_VBL) {
            printf("FAIL retire: vblank could not resume after a takeback\n");
            bad++;
        }
    }

    /* The gap is a FORWARD difference. A clock that has not advanced past the
     * last vblank must read as 0, not as 2^64 - something, which would take
     * the job back on the very next tick and reinstate the beat. */
    {
        struct rb_retire r;
        rb_retire_reset(&r, 1u);
        /* Near the top of the range, but far enough from it that this test's
         * OWN arithmetic does not wrap - the first version of this case added
         * QUIET to 0xffffff..ff00 and landed BEHIND lastVbl, so it was the
         * assertion that overflowed and not the code under it. */
        const uint64_t hi = 0xffffffffffff0000ull;
        (void)rb_retire_vbl_offer(&r, hi);
        if (rb_retire_timer_should(&r, hi, QUIET) != 0 ||
            rb_retire_timer_should(&r, 10u, QUIET) != 0) {
            printf("FAIL retire: a non-advancing clock read as a huge gap\n");
            bad++;
        }
        /* ...but a genuine forward gap past the window still takes back. */
        if (!rb_retire_timer_should(&r, hi + QUIET, QUIET)) {
            printf("FAIL retire: forward gap did not take back\n"); bad++;
        }
    }

    /* The period follows the role: fast while the timer drives, watchdog while
     * the vblank does. A demoted timer that kept the fast period would still
     * be the thing it was demoted for being. */
    {
        struct rb_retire r;
        rb_retire_reset(&r, 1u);
        if (rb_retire_timer_period_us(&r, 6944u, 50000u) != 6944u) {
            printf("FAIL retire: timer-driven period\n"); bad++;
        }
        (void)rb_retire_vbl_offer(&r, 100u);
        if (rb_retire_timer_period_us(&r, 6944u, 50000u) != 50000u) {
            printf("FAIL retire: watchdog period\n"); bad++;
        }
        if (rb_retire_timer_period_us(NULL, 6944u, 50000u) != 6944u) {
            printf("FAIL retire: NULL must fall back to the fast period\n");
            bad++;
        }
    }

    /* Every entry point NULL-safe: the kext calls through on paths where the
     * state was never set up. */
    {
        rb_retire_reset(NULL, 1u);
        if (rb_retire_vbl_offer(NULL, 1u) != 0 ||
            rb_retire_timer_should(NULL, 1u, QUIET) != 0) {
            printf("FAIL retire: NULL args not safe\n"); bad++;
        }
    }

    /* THE WHOLE INVARIANT, over an interleaving rather than a case: drive 200
     * vblanks with ticks between them, then stop the vblank, and assert that
     * retirement continues and that no step was ever retired twice. */
    {
        struct rb_retire r;
        uint64_t t = 0u;
        unsigned i;
        rb_retire_reset(&r, 1u);
        for (i = 0; i < 200u; i++) {
            t += QUIET / 4u;
            (void)rb_retire_vbl_offer(&r, t);
            t += QUIET / 4u;
            (void)rb_retire_timer_should(&r, t, QUIET);
        }
        if (r.vblRetires != 200u || r.timerRetires != 0u) {
            printf("FAIL retire: live vblank did not hold the job for 200 "
                   "frames (vbl=%llu timer=%llu)\n",
                   (unsigned long long)r.vblRetires,
                   (unsigned long long)r.timerRetires);
            bad++;
        }
        /* The vblank stops. Every subsequent tick must retire. */
        for (i = 0; i < 10u; i++) {
            t += QUIET;
            if (!rb_retire_timer_should(&r, t, QUIET)) {
                printf("FAIL retire: RETIREMENT STOPPED at tick %u after the "
                       "vblank went quiet - this is the 2xe desktop\n", i);
                bad++;
                break;
            }
        }
        if (r.timerRetires != 10u || r.takebacks != 1u) {
            printf("FAIL retire: watchdog bookkeeping after the vblank died\n");
            bad++;
        }
    }

    /* 2zf: the mode gate. The vblank may only take the job where it fires at
     * least as often as the timer would, because boot 267 measured the
     * handover as +24 % at native and -42 % at 60 Hz, pinned at exactly half
     * the vblank rate - the retire wants a RATE and only the native vblank
     * supplies it. */
    {
        const uint64_t native = 6952507u;          /* the measured native period */
        if (!rb_retire_vbl_suits(native, native)) {
            printf("FAIL retire: native mode refused by its own period\n"); bad++;
        }
        if (!rb_retire_vbl_suits(native - 50000u, native)) {
            printf("FAIL retire: a FASTER mode refused\n"); bad++;
        }
        /* Spread-spectrum moves the derived period boot to boot (2y6), so a
         * period a hair longer than native is still native. */
        if (!rb_retire_vbl_suits(native + (native / 32u), native)) {
            printf("FAIL retire: slack too tight for a re-derived period\n");
            bad++;
        }
        if (rb_retire_vbl_suits(16665556u, native)) {
            printf("FAIL retire: 60 Hz accepted - this is boot 267's -42 %%\n");
            bad++;
        }
        if (rb_retire_vbl_suits(8331041u, native)) {
            printf("FAIL retire: 120 Hz accepted - boot 267 measured -20 %%\n");
            bad++;
        }
        if (rb_retire_vbl_suits(0u, native) || rb_retire_vbl_suits(native, 0u)) {
            printf("FAIL retire: a zero period must never arm the handover\n");
            bad++;
        }
    }

    /* 2C2: the retire-drain clamp. The dial decides how many times one
     * offer polls `checkTimestamps()`; a value that escaped the bound would
     * put an unbounded loop on the vblank thread_call (rule 31), and 0 would
     * stop retirement altogether, which is the one value that must map to
     * today's behaviour rather than to nothing. */
    {
        if (rb_retire_drain_clamp(0u) != 1u) {
            printf("FAIL retire: drain 0 must clamp to 1, not stop the "
                   "retirement\n");
            bad++;
        }
        if (rb_retire_drain_clamp(1u) != 1u) {
            printf("FAIL retire: drain 1 must stay 1 - it is the A side\n");
            bad++;
        }
        if (rb_retire_drain_clamp(RB_RETIRE_DRAIN_MAX) !=
            RB_RETIRE_DRAIN_MAX) {
            printf("FAIL retire: the max must be reachable\n");
            bad++;
        }
        if (rb_retire_drain_clamp(RB_RETIRE_DRAIN_MAX + 1u) !=
            RB_RETIRE_DRAIN_MAX) {
            printf("FAIL retire: over the max must clamp DOWN\n");
            bad++;
        }
        if (rb_retire_drain_clamp(0xFFFFFFFFu) != RB_RETIRE_DRAIN_MAX) {
            printf("FAIL retire: a huge drain must clamp, not wrap\n");
            bad++;
        }
        /* The bound has to be small enough that the worst case is bounded
         * CPU and not a stall: at the panel's 143.83 Hz, MAX polls per
         * vblank is MAX*144 calls a second. Assert the ORDER, so raising the
         * constant to something that changes the risk fails here. */
        if (RB_RETIRE_DRAIN_MAX > 16u) {
            printf("FAIL retire: DRAIN_MAX %u would be over 2300 calls/s on "
                   "the vblank thread - rule 31\n",
                   (unsigned)RB_RETIRE_DRAIN_MAX);
            bad++;
        }
    }

    /* 2C4: THE TICK DIAL. */
    {
        /* The default must survive the clamp untouched - it is the A side of
         * every A/B and 2zc measured it as an interior optimum. */
        if (rb_tick_us_clamp(6944u) != 6944u) {
            printf("FAIL tick: 6944 is the measured optimum and must pass "
                   "through unchanged\n");
            bad++;
        }
        /* The value the experiment actually sets. If this were clamped the
         * B side would silently become the A side - 2C2's degenerate A/B. */
        if (rb_tick_us_clamp(3472u) != 3472u) {
            printf("FAIL tick: 3472 is half the native period and is the B "
                   "side of the 2C4 experiment - it must be reachable\n");
            bad++;
        }
        if (rb_tick_us_clamp(RB_TICK_US_MIN) != RB_TICK_US_MIN ||
            rb_tick_us_clamp(RB_TICK_US_MAX) != RB_TICK_US_MAX) {
            printf("FAIL tick: both bounds must be reachable\n");
            bad++;
        }
        if (rb_tick_us_clamp(0u) != RB_TICK_US_MIN) {
            printf("FAIL tick: 0 must clamp UP - a zero period is an "
                   "unbounded timer loop, not a fast one\n");
            bad++;
        }
        if (rb_tick_us_clamp(0xFFFFFFFFu) != RB_TICK_US_MAX) {
            printf("FAIL tick: a huge period must clamp, not wrap\n");
            bad++;
        }
        /* The verdict is a CODE and the two directions must not merge -
         * rule 14. A single bool here is what the drain's `drain_refused`
         * was, and it could not tell a typo'd 50 from a typo'd 50000000. */
        if (rb_tick_us_verdict(6944u) != RB_TICK_OK) {
            printf("FAIL tick: the default must verdict OK\n");
            bad++;
        }
        if (rb_tick_us_verdict(RB_TICK_US_MIN - 1u) != RB_TICK_TOO_FAST) {
            printf("FAIL tick: under the floor must say TOO_FAST\n");
            bad++;
        }
        if (rb_tick_us_verdict(RB_TICK_US_MAX + 1u) != RB_TICK_TOO_SLOW) {
            printf("FAIL tick: over the ceiling must say TOO_SLOW\n");
            bad++;
        }
        if (RB_TICK_TOO_FAST == RB_TICK_TOO_SLOW) {
            printf("FAIL tick: the two refusal codes must differ or the "
                   "code is a bool with extra steps\n");
            bad++;
        }
        /* The floor has to bound the worst case. At RB_TICK_US_MIN the timer
         * fires 1000000/MIN times a second on a thread_call; assert the
         * ORDER so lowering the constant to something that changes the risk
         * fails here rather than on the machine (rule 31). */
        if (RB_TICK_US_MIN < 100u) {
            printf("FAIL tick: a %u us floor is over 10000 ticks/s - "
                   "rule 31\n", (unsigned)RB_TICK_US_MIN);
            bad++;
        }
        /* 2C4's premise, asserted so it cannot drift: the dial is inert
         * while the vblank drives, because the timer runs at the watchdog
         * period then. A null result in that configuration is the dial not
         * being connected, not the tick not mattering (rule 39) - which is
         * why `tickset` has to print `vblret_src`. */
        {
            struct rb_retire r;
            rb_retire_reset(&r, 1u);
            r.src = RB_RETIRE_SRC_VBL;
            if (rb_retire_timer_period_us(&r, 3472u, 50000u) != 50000u) {
                printf("FAIL tick: with the vblank driving, the timer must "
                       "run at the WATCHDOG period - the dial is inert "
                       "there and the tool must say so\n");
                bad++;
            }
            r.src = RB_RETIRE_SRC_TIMER;
            if (rb_retire_timer_period_us(&r, 3472u, 50000u) != 3472u) {
                printf("FAIL tick: with the timer driving, the dial IS the "
                       "period - this is the whole experiment\n");
                bad++;
            }
        }
    }

    printf("retire: the 2zd handover - unarmed decline, single handoff, the "
           "watchdog takeback, a non-advancing clock, role-dependent period "
           "and 200 interleaved frames, plus 2zf's mode gate, 2C2's drain clamp and 2C4's tick dial%s\n", bad ? " FAILED" : ": passed");
    return bad;
}
