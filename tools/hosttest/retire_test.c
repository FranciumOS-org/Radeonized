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

/* 2CF: `rb_retire_vbl_offer()` now also carries the framebuffer's live vblank
 * period, because the interior occasions are placed inside it. The existing
 * cases pass the measured native period so none of them is accidentally in
 * the "the framebuffer did not say" branch - the one case that exercises a
 * zero says so by name. */
#define VBLNS 6952507u

int retire_tests(void)
{
    int bad = 0;

    /* Unarmed: the vblank never takes over, and the offer is COUNTED rather
     * than dropped, so "the handover did nothing" is distinguishable from
     * "the vblank never offered" (rule 14). */
    {
        struct rb_retire r;
        rb_retire_reset(&r, 0u);
        if (rb_retire_vbl_offer(&r, 100u, VBLNS) != 0) {
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
        if (!rb_retire_vbl_offer(&r, 1000u, VBLNS) || r.handoffs != 1u ||
            r.src != RB_RETIRE_SRC_VBL) {
            printf("FAIL retire: first vblank did not hand over\n"); bad++;
        }
        if (!rb_retire_vbl_offer(&r, 1100u, VBLNS) || r.handoffs != 1u) {
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
        (void)rb_retire_vbl_offer(&r, 1000u, VBLNS);
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
        if (!rb_retire_vbl_offer(&r, 20000u, VBLNS) || r.handoffs != 2u ||
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
        (void)rb_retire_vbl_offer(&r, hi, VBLNS);
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
        (void)rb_retire_vbl_offer(&r, 100u, VBLNS);
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
        if (rb_retire_vbl_offer(NULL, 1u, VBLNS) != 0 ||
            rb_retire_timer_should(NULL, 1u, QUIET) != 0 ||
            rb_retire_sub_active(NULL) != 0 ||
            rb_retire_sub_period_ns(NULL) != 0u ||
            rb_retire_sub_should(NULL) != 0 ||
            rb_retire_sub_deadline(NULL, 100u, 0u) != 0u) {
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
            (void)rb_retire_vbl_offer(&r, t, VBLNS);
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

    /* 2CF: SUB-VBLANK RETIRE OCCASIONS.
     *
     * The thing these cases exist to protect is not the feature, it is the A
     * side: `sub` 1 must be pre-2CF byte for byte, because that is the
     * shipping configuration and the control for every reading taken against
     * it (rule 39). Everything else is arithmetic that 2C5-2C8 already fixed
     * on hardware, and the one genuinely new claim - that the occasions are
     * anchored on the DISPLAY rather than on the previous arm - is pinned by
     * the deadline cases, because that anchor is the entire difference from
     * 2zc's two beating timers. */
    {
        const uint64_t native = 6952507u;   /* the measured native period */
        const uint64_t sixty  = 16669443u;  /* boot 413's 60 Hz period */

        /* THE A SIDE. Default 1, inert everywhere, and no interior occasion
         * can be taken however the state machine is driven. */
        {
            struct rb_retire r;
            rb_retire_reset(&r, 1u);
            if (r.sub != 1u) {
                printf("FAIL 2CF: the default must be 1 - a build that "
                       "subdivides by default has no A side\n"); bad++;
            }
            (void)rb_retire_vbl_offer(&r, 1000u, native);
            if (rb_retire_sub_active(&r) || rb_retire_sub_period_ns(&r) ||
                rb_retire_sub_should(&r) ||
                rb_retire_sub_deadline(&r, 100u, 0u)) {
                printf("FAIL 2CF: sub=1 placed an interior occasion\n"); bad++;
            }
            if (r.subRetires || r.subSkipped) {
                printf("FAIL 2CF: sub=1 moved an interior counter\n"); bad++;
            }
        }

        /* INERT IN THE TIMER-DRIVEN REGIME. Below native the tick dial owns
         * the quantum (2C5-2C8 measured it there), and two mechanisms for one
         * job is how 2zc's beat happened. */
        {
            struct rb_retire r;
            rb_retire_reset(&r, 1u);
            r.sub = 4u;
            r.subPeriodNs = sixty;
            if (r.src != RB_RETIRE_SRC_TIMER) {
                printf("FAIL 2CF: reset must start in TIMER\n"); bad++;
            }
            if (rb_retire_sub_active(&r) || rb_retire_sub_should(&r)) {
                printf("FAIL 2CF: interior occasions placed while the TIMER "
                       "drives - that is 2zc's configuration\n"); bad++;
            }
        }

        /* INERT UNARMED, and inert when the framebuffer reported no period.
         * The second is the one that matters in the field: an older
         * framebuffer paired with this plugin passes no period, and that must
         * read as "place none", never as "place them at zero" - a zero
         * interval thread_call is a spin. */
        {
            struct rb_retire r;
            rb_retire_reset(&r, 0u);
            r.sub = 4u; r.src = RB_RETIRE_SRC_VBL; r.subPeriodNs = native;
            if (rb_retire_sub_active(&r)) {
                printf("FAIL 2CF: unarmed build placed interior occasions\n");
                bad++;
            }
            rb_retire_reset(&r, 1u);
            r.sub = 4u; r.src = RB_RETIRE_SRC_VBL; r.subPeriodNs = 0u;
            if (rb_retire_sub_active(&r) || rb_retire_sub_period_ns(&r) ||
                rb_retire_sub_deadline(&r, 100u, 0u)) {
                printf("FAIL 2CF: a zero vblank period must place NOTHING, "
                       "not an occasion every zero nanoseconds\n"); bad++;
            }
        }

        /* 2C8's THRESHOLD, REPRODUCED BY THE DIAL RATHER THAN RETYPED.
         * `tick = vbl/8` is where the eight-queue spread is exactly one frame,
         * measured at 60 Hz as 2084 us against a 16669443 ns period. sub=8
         * must land on that number from the period alone - if it does not, the
         * dial is not the quantity 2C5-2C8 measured. */
        {
            struct rb_retire r;
            rb_retire_reset(&r, 1u);
            r.src = RB_RETIRE_SRC_VBL; r.sub = 8u; r.subPeriodNs = sixty;
            if (rb_retire_sub_period_ns(&r) != 2083680u) {
                printf("FAIL 2CF: sub=8 at 60 Hz must be 2C8's vbl/8 "
                       "threshold (2083680 ns), got %llu\n",
                       (unsigned long long)rb_retire_sub_period_ns(&r)); bad++;
            }
            r.subPeriodNs = native;
            if (rb_retire_sub_period_ns(&r) != 869063u) {
                printf("FAIL 2CF: sub=8 at native must be 869063 ns\n"); bad++;
            }
            /* And sub=4 is half the subdivision, not a quarter of it - the
             * off-by-one a reader would most plausibly introduce. */
            r.sub = 4u;
            if (rb_retire_sub_period_ns(&r) != native / 4u) {
                printf("FAIL 2CF: sub=4 must divide the period by 4\n"); bad++;
            }
        }

        /* THE RUN: `sub` occasions per period, of which the VBLANK IS THE
         * FIRST. sub=4 therefore yields exactly three interior retires and
         * then COUNTS the refusals - a full run is what a vblank that stopped
         * arriving looks like from inside the timer, and it must be
         * distinguishable from an interior occasion that never ran (rule 14).
         * Counting the vblank again would retire twice at the same phase. */
        {
            struct rb_retire r;
            int i, got = 0;
            rb_retire_reset(&r, 1u);
            r.sub = 4u;
            (void)rb_retire_vbl_offer(&r, 1000u, native);
            for (i = 0; i < 10; i++) {
                if (rb_retire_sub_should(&r)) { got++; }
            }
            if (got != 3) {
                printf("FAIL 2CF: sub=4 gave %d interior retires, want 3 "
                       "(the vblank is the first of the four)\n", got); bad++;
            }
            if (r.subRetires != 3u || r.subSkipped != 7u) {
                printf("FAIL 2CF: run accounting %llu/%llu, want 3/7\n",
                       (unsigned long long)r.subRetires,
                       (unsigned long long)r.subSkipped); bad++;
            }
            /* The next vblank re-opens the run - otherwise the subdivision
             * works for exactly one frame after boot. */
            (void)rb_retire_vbl_offer(&r, 1000u + native, native);
            if (r.subRun != 0u || !rb_retire_sub_should(&r)) {
                printf("FAIL 2CF: the vblank did not re-open the run\n");
                bad++;
            }
        }

        /* THE ANCHOR, which is the whole design. Deadline k must be
         * `lastVbl + k * (period/sub)` - measured from the DISPLAY's own
         * timestamp, never walked forward from the previous arm. Two
         * independent periods is 2zc, which cost a third of the frame rate;
         * one period subdivided and re-anchored every frame cannot drift by
         * construction. */
        {
            struct rb_retire r;
            const uint64_t L = 1000000u;
            const uint64_t P = 400u;          /* abstract abs units */
            rb_retire_reset(&r, 1u);
            r.sub = 4u; r.subPeriodNs = native;
            (void)rb_retire_vbl_offer(&r, L, native);
            if (rb_retire_sub_deadline(&r, P, L) != L + P) {
                printf("FAIL 2CF: first interior slot is not lastVbl + "
                       "period/sub\n"); bad++;
            }
            r.subRun = 1u;
            if (rb_retire_sub_deadline(&r, P, L) != L + 2u * P) {
                printf("FAIL 2CF: second interior slot is not anchored on "
                       "lastVbl - that is a free-running timer (2zc)\n");
                bad++;
            }
            r.subRun = 2u;
            if (rb_retire_sub_deadline(&r, P, L) != L + 3u * P) {
                printf("FAIL 2CF: third interior slot misplaced\n"); bad++;
            }
            /* The run's last slot is the next VBLANK, so there is nothing
             * left for the timer to arm. */
            r.subRun = 3u;
            if (rb_retire_sub_deadline(&r, P, L) != 0u) {
                printf("FAIL 2CF: a full run must arm NOTHING - the next "
                       "occasion is the vblank itself\n"); bad++;
            }
            /* A SLOT ALREADY PAST ARMS NOTHING. Arming a thread_call into the
             * past fires a catch-up burst - the drift defect 2xe fixed once in
             * this same timer - and a burst of retires is 2zc in miniature.
             * The anchor is a timestamp taken on the vblank thread, so a late
             * fire is ordinary here, not exceptional. */
            r.subRun = 0u;
            if (rb_retire_sub_deadline(&r, P, L + P) != 0u ||
                rb_retire_sub_deadline(&r, P, L + 10u * P) != 0u) {
                printf("FAIL 2CF: a slot in the past must refuse, not clamp "
                       "forward (2xe's catch-up burst)\n"); bad++;
            }
        }

        /* 2D0: THE TWO ARM REFUSALS ARE DISTINCT, and boot 424 is why.
         * `vblret_sublate` climbed at 146/s - one per vblank, i.e. every run
         * COMPLETING - and read as "slots missed" because one counter carried
         * both causes. A header whose every other refusal is a code had this
         * one merged into a deadline-or-zero. */
        {
            struct rb_retire r;
            const uint64_t L = 1000000u, P = 400u;
            uint64_t d = 1234u;
            rb_retire_reset(&r, 1u);
            r.sub = 4u; r.subPeriodNs = native;
            (void)rb_retire_vbl_offer(&r, L, native);

            if (rb_retire_sub_arm(&r, P, L, &d) != RB_SUB_ARM_OK ||
                d != L + P) {
                printf("FAIL 2D0: a placeable slot must report OK with its "
                       "deadline\n"); bad++;
            }
            /* The run completing: the ordinary end of a frame. */
            r.subRun = 3u; d = 1234u;
            if (rb_retire_sub_arm(&r, P, L, &d) != RB_SUB_ARM_FULL || d != 0u) {
                printf("FAIL 2D0: a complete run must report FULL, not "
                       "LATE - 146/s of FULL read as a fault on boot 424\n");
                bad++;
            }
            /* A slot genuinely missed. */
            r.subRun = 0u; d = 1234u;
            if (rb_retire_sub_arm(&r, P, L + 10u * P, &d) != RB_SUB_ARM_LATE ||
                d != 0u) {
                printf("FAIL 2D0: a past slot must report LATE\n"); bad++;
            }
            /* Not in use at all is a third answer, not a miss. */
            r.sub = 1u; d = 1234u;
            if (rb_retire_sub_arm(&r, P, L, &d) != RB_SUB_ARM_OFF || d != 0u) {
                printf("FAIL 2D0: sub=1 must report OFF, not FULL or LATE\n");
                bad++;
            }
            /* FULL and LATE must not be the same value, or the split is
             * cosmetic and the plugin's two counters are one again. */
            if (RB_SUB_ARM_FULL == RB_SUB_ARM_LATE ||
                RB_SUB_ARM_OK == RB_SUB_ARM_FULL ||
                RB_SUB_ARM_OFF == RB_SUB_ARM_LATE) {
                printf("FAIL 2D0: the arm codes collide\n"); bad++;
            }
        }

        /* 2D0: BOOT 424's OWN LADDER, as arithmetic rather than as a story.
         * The per-queue column measured 1 quantum per queue with the FIRST
         * interval costing 2, at both quanta - which is where 2C7's
         * unexplained coefficient 8 comes from: 2 + 6 x 1. Pinned here so a
         * later change to the subdivision cannot quietly stop reproducing the
         * measurement that justified it. */
        {
            struct rb_retire r;
            rb_retire_reset(&r, 1u);
            r.src = RB_RETIRE_SRC_VBL; r.sub = 4u; r.subPeriodNs = native;
            /* The interior quantum IS what the ladder stepped by: boot 424
             * read 1.852 ms steady against this 1.738 ms spacing. */
            if (rb_retire_sub_period_ns(&r) != 1738126u) {
                printf("FAIL 2D0: sub=4 at native must be boot 424's measured "
                       "1738126 ns, got %llu\n",
                       (unsigned long long)rb_retire_sub_period_ns(&r)); bad++;
            }
            /* 8 quanta of spread: the first interval 2, the next six 1 each. */
            {
                const uint64_t q = rb_retire_sub_period_ns(&r);
                const uint64_t spread = 2u * q + 6u * q;
                /* boot 424 measured 14.582 ms; allow 10 % for scheduling. */
                if (spread < 13000000u || spread > 16000000u) {
                    printf("FAIL 2D0: 8 quanta at sub=4 should be boot 424's "
                           "~14.6 ms spread, got %llu ns\n",
                           (unsigned long long)spread); bad++;
                }
            }
        }

        /* THE WATCHDOG STILL OWNS A DEAD VBLANK. Interior fires must not
         * refresh `lastVbl`, or a stopped vblank looks live forever and 2xe's
         * one-frame-per-ten-seconds desktop comes back with no way out. */
        {
            struct rb_retire r;
            rb_retire_reset(&r, 1u);
            r.sub = 4u;
            (void)rb_retire_vbl_offer(&r, 1000u, native);
            (void)rb_retire_sub_should(&r);
            (void)rb_retire_sub_should(&r);
            if (r.lastVbl != 1000u) {
                printf("FAIL 2CF: an interior occasion moved lastVbl - the "
                       "watchdog can never fire again\n"); bad++;
            }
            if (!rb_retire_timer_should(&r, 1000u + QUIET, QUIET) ||
                r.src != RB_RETIRE_SRC_TIMER || r.takebacks != 1u) {
                printf("FAIL 2CF: the watchdog did not take back a dead "
                       "vblank\n"); bad++;
            }
            if (rb_retire_sub_active(&r)) {
                printf("FAIL 2CF: interior occasions survived the "
                       "takeback\n"); bad++;
            }
        }

        /* A MODE SWITCH MOVES THE SUBDIVISION ON THE NEXT OFFER, with nothing
         * to invalidate - the period arrives with every offer precisely so
         * that 2z1's runtime mode change needs no second code path. */
        {
            struct rb_retire r;
            rb_retire_reset(&r, 1u);
            r.sub = 4u;
            (void)rb_retire_vbl_offer(&r, 1000u, native);
            if (rb_retire_sub_period_ns(&r) != native / 4u) {
                printf("FAIL 2CF: native subdivision wrong\n"); bad++;
            }
            (void)rb_retire_vbl_offer(&r, 2000u, sixty);
            if (rb_retire_sub_period_ns(&r) != sixty / 4u) {
                printf("FAIL 2CF: the subdivision did not follow the mode\n");
                bad++;
            }
        }

        /* The clamp, and that it REFUSES nothing silently at the edges. */
        {
            if (rb_retire_sub_clamp(0u) != RB_RETIRE_SUB_MIN ||
                rb_retire_sub_clamp(1u) != 1u ||
                rb_retire_sub_clamp(8u) != RB_RETIRE_SUB_MAX ||
                rb_retire_sub_clamp(9u) != RB_RETIRE_SUB_MAX ||
                rb_retire_sub_clamp(0xFFFFFFFFu) != RB_RETIRE_SUB_MAX) {
                printf("FAIL 2CF: sub clamp\n"); bad++;
            }
            /* sub above the bound must be inert rather than trusted, because
             * the global is written by setProperties and read on two threads:
             * a value that got past the setter must still not place 600
             * occasions in a frame. */
            {
                struct rb_retire r;
                rb_retire_reset(&r, 1u);
                r.src = RB_RETIRE_SRC_VBL;
                r.sub = 600u; r.subPeriodNs = native;
                if (rb_retire_sub_active(&r)) {
                    printf("FAIL 2CF: an out-of-range sub was honoured\n");
                    bad++;
                }
            }
        }
    }

    /* ---- 2D5: ONE QUANTUM DERIVES BOTH MEASURED DEFAULTS ---------------
     *
     * The whole claim of 2D5 is that `sub` 4 at native and `tick` 2084 at
     * 60 Hz are one constant seen twice. These two assertions are that claim,
     * pinned to the periods the two boots actually reported, and they are the
     * reason this is not a changed literal: break the constant and BOTH fail.
     */
    {
        const uint64_t native = 6952507ull;   /* boot 424/426, measured */
        const uint64_t hz60   = 16669443ull;  /* boot 413, measured */

        /* 2D0 read `vblret_subns` 1738126 off the chip at sub 4. The
         * derivation must reproduce the COUNT and the realised quantum. */
        if (rb_retire_sub_auto(native) != 4u ||
            (native / rb_retire_sub_auto(native)) != 1738126ull) {
            printf("FAIL 2D5: native does not derive sub 4 / 1738126 ns "
                   "(got %u / %llu)\n",
                   rb_retire_sub_auto(native),
                   (unsigned long long)(native / rb_retire_sub_auto(native)));
            bad++;
        }

        /* 2C8's vbl/8 threshold, and 2CF already hosttests 2083680 as the
         * 60 Hz value. The DERIVED tick must land on the measured 2084 us. */
        if (rb_retire_sub_auto(hz60) != 8u ||
            (hz60 / rb_retire_sub_auto(hz60)) != 2083680ull ||
            rb_retire_tick_us_auto(hz60) != 2083u) {
            printf("FAIL 2D5: 60 Hz does not derive 8 / 2083680 ns / 2083 us "
                   "(got %u / %llu / %u)\n",
                   rb_retire_sub_auto(hz60),
                   (unsigned long long)(hz60 / rb_retire_sub_auto(hz60)),
                   rb_retire_tick_us_auto(hz60));
            bad++;
        }

        /* THE REALISED QUANTUM IS NEVER LARGER THAN THE TARGET. The constant
         * is the slower of the two measured quanta and the division rounds
         * the COUNT up, so every period must come out at or under it - that
         * is what makes a never-measured mode safe rather than hopeful. */
        {
            uint64_t ps[6] = { 6952507ull, 10000000ull, 13333333ull,
                               16669443ull, 20000000ull, 1000000ull };
            unsigned i;
            for (i = 0; i < 6u; i++) {
                uint32_t occ = rb_retire_sub_auto(ps[i]);
                uint64_t q = ps[i] / occ;
                if (!occ) { printf("FAIL 2D5: zero occasions\n"); bad++; continue; }
                /* The clamp at 8 is the one case that may exceed it, and it
                 * must be exactly the case where 8 is not enough. */
                if (q > RB_RETIRE_QUANTUM_NS && occ != RB_RETIRE_SUB_MAX) {
                    printf("FAIL 2D5: period %llu quantum %llu over target "
                           "with occ %u below the clamp\n",
                           (unsigned long long)ps[i],
                           (unsigned long long)q, occ);
                    bad++;
                }
            }
        }

        /* A ZERO PERIOD IS PRE-2CF, BYTE FOR BYTE - the framebuffer did not
         * say, so place nothing. This is the rule-38 A side: an old
         * framebuffer passes no period and must not get a derived default. */
        if (rb_retire_sub_auto(0ull) != 1u ||
            rb_retire_tick_us_auto(0ull) != 0u ||
            rb_retire_tick_auto_ok(0ull) != 0) {
            printf("FAIL 2D5: a zero period does not fall back to pre-2CF\n");
            bad++;
        }

        /* 2zc's REFUSAL: no derived tick at native or faster. -rbgpunoretirevbl
         * at native is the dangerous combination and 2000 us cost -34 % there,
         * so the derivation must decline and say so with a 0 rather than
         * return a plausible 1738. */
        if (rb_retire_tick_auto_ok(native) != 0 ||
            rb_retire_tick_us_auto(native) != 0u ||
            rb_retire_tick_us_auto(6000000ull) != 0u) {
            printf("FAIL 2D5: a native period got a derived tick (2zc)\n");
            bad++;
        }
        /* ...and the control, so that refusal cannot be a constant 0: just
         * past the bound it must derive. */
        if (rb_retire_tick_auto_ok(8000001ull) != 1 ||
            rb_retire_tick_us_auto(8000001ull) == 0u) {
            printf("FAIL 2D5: the tick refusal is unconditional\n");
            bad++;
        }

        /* The derived sub must survive the setter it will be handed to, and
         * `rb_retire_sub_active()` must accept it at both measured periods -
         * a default that the activity gate then refuses is 2A9's shape. */
        {
            struct rb_retire r;
            rb_retire_reset(&r, 1u);
            r.src = RB_RETIRE_SRC_VBL;
            r.sub = rb_retire_sub_clamp(rb_retire_sub_auto(native));
            r.subPeriodNs = native;
            if (r.sub != 4u || !rb_retire_sub_active(&r) ||
                rb_retire_sub_period_ns(&r) != 1738126ull) {
                printf("FAIL 2D5: the derived native default is not active\n");
                bad++;
            }
        }
    }

    printf("retire: the 2zd handover - unarmed decline, single handoff, the "
           "watchdog takeback, a non-advancing clock, role-dependent period "
           "and 200 interleaved frames, plus 2zf's mode gate, 2C2's drain clamp and 2C4's tick dial and 2CF's sub-vblank occasions and 2D0's split arm codes against boot 424's ladder%s\n", bad ? " FAILED" : ": passed");
    return bad;
}
