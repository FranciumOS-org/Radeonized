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

    printf("retire: the 2zd handover - unarmed decline, single handoff, the "
           "watchdog takeback, a non-advancing clock, role-dependent period "
           "and 200 interleaved frames, plus 2zf's mode gate%s\n", bad ? " FAILED" : ": passed");
    return bad;
}
