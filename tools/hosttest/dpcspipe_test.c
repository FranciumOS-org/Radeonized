/* SPDX-License-Identifier: MIT */
/* 300: RDPCSPIPE_PHY_CNTL6, the other DP-alt register.
 *
 * The cases that matter here are the ones that stop this decoder making the
 * two claims it is not entitled to:
 *
 *   1. that instance 2 exists. Upstream's table has five entries and says in
 *      a comment that only two are real, so a decoder that accepted index 2
 *      would hand back instance 0's address under instance 2's name - which
 *      is precisely how the RDPCSTX word got read as this one for five boots.
 *   2. that an agreement identifies UNIPHY_C. One matching bit across two
 *      candidates is not an identification (rule 18), so the agreement test
 *      is checked to be an OR over instances and nothing more.
 *
 * And the offsets are pinned individually: they are 0xd8 apart, which is the
 * RDPCSTX stride, so a formula would look right and be a coincidence.
 */
#include <stdio.h>
#include <stdint.h>
#include "rb_dpcspipe.h"

int dpcspipe_tests(void);

int dpcspipe_tests(void)
{
    int bad = 0;

    /* dpcs_4_2_0_offset.h:11961-11964, read directly. */
    {
        if (rb_dpcspipe_dw(0) != 0x2d73u) {
            printf("FAIL dpcspipe: inst0 = 0x%x want 0x2d73\n",
                   rb_dpcspipe_dw(0));
            bad++;
        }
        if (rb_dpcspipe_dw(1) != 0x2e4bu) {
            printf("FAIL dpcspipe: inst1 = 0x%x want 0x2e4b\n",
                   rb_dpcspipe_dw(1));
            bad++;
        }
        if (rb_dpcspipe_dw_max() != 0x2e4bu) {
            printf("FAIL dpcspipe: dw_max = 0x%x want 0x2e4b\n",
                   rb_dpcspipe_dw_max());
            bad++;
        }
        /* THE ALIAS TRAP. Upstream defines indexes 2, 3 and 4 as copies of
         * 0 and 1. If this decoder answered them it would be reporting one
         * register under another's name. */
        if (rb_dpcspipe_dw(2) != RB_DPCSPIPE_DW_INVALID ||
            rb_dpcspipe_dw(3) != RB_DPCSPIPE_DW_INVALID ||
            rb_dpcspipe_dw(4) != RB_DPCSPIPE_DW_INVALID) {
            printf("FAIL dpcspipe: an aliased index was answered\n");
            bad++;
        }
        if (RB_DPCSPIPE_COUNT != 2u) {
            printf("FAIL dpcspipe: COUNT is not 2\n");
            bad++;
        }
        /* The two are 0xd8 apart, and that must NOT have been used to derive
         * them - checked by asserting the gap exists while the values stay
         * pinned above, so a later "simplification" to base + i * stride
         * fails the pinned offsets rather than passing this one. */
        if (rb_dpcspipe_dw(1) - rb_dpcspipe_dw(0) != 0xd8u) {
            printf("FAIL dpcspipe: the two instances are not 0xd8 apart\n");
            bad++;
        }
    }

    /* Each field alone, so a swapped pair cannot pass. */
    {
        if (!rb_dpcspipe_dp4(0x00010000u) ||
            rb_dpcspipe_disabled(0x00010000u) ||
            rb_dpcspipe_disable_acked(0x00010000u)) {
            printf("FAIL dpcspipe: DP4 alone decoded wrong\n");
            bad++;
        }
        if (!rb_dpcspipe_disabled(0x00020000u) ||
            rb_dpcspipe_dp4(0x00020000u) ||
            rb_dpcspipe_disable_acked(0x00020000u)) {
            printf("FAIL dpcspipe: DISABLE alone decoded wrong\n");
            bad++;
        }
        if (!rb_dpcspipe_disable_acked(0x00040000u) ||
            rb_dpcspipe_dp4(0x00040000u) ||
            rb_dpcspipe_disabled(0x00040000u)) {
            printf("FAIL dpcspipe: DISABLE_ACK alone decoded wrong\n");
            bad++;
        }
        /* Bits outside the three must not reach any of them. RDPCSTX's own
         * MPLL and PSTATE nibbles live in the low bits, and this register has
         * NO such fields - a leak would re-create the exact conflation this
         * file exists to end. */
        if (rb_dpcspipe_dp4(0xfff8ffffu & ~0x00070000u) ||
            rb_dpcspipe_disabled(0x00003333u) ||
            rb_dpcspipe_disable_acked(0x00003333u)) {
            printf("FAIL dpcspipe: a bit outside the three fields leaked\n");
            bad++;
        }
    }

    /* The per-instance verdict, including the named ambiguity. */
    {
        if (rb_dpcspipe_verdict(0x00000000u) != RB_DPCSPIPE_NOT_DISABLED) {
            printf("FAIL dpcspipe: DISABLE clear is not NOT_DISABLED\n");
            bad++;
        }
        if (rb_dpcspipe_verdict(0x00020000u) != RB_DPCSPIPE_SETTLING) {
            printf("FAIL dpcspipe: set-not-acked is not SETTLING\n");
            bad++;
        }
        if (rb_dpcspipe_verdict(0x00060000u) != RB_DPCSPIPE_DISABLED) {
            printf("FAIL dpcspipe: set-and-acked is not DISABLED\n");
            bad++;
        }
        /* ACK without DISABLE is still not disabled - the gate is DISABLE. */
        if (rb_dpcspipe_verdict(0x00040000u) != RB_DPCSPIPE_NOT_DISABLED) {
            printf("FAIL dpcspipe: ACK alone read as disabled\n");
            bad++;
        }
        if (RB_DPCSPIPE_NOT_SAMPLED != 0) {
            printf("FAIL dpcspipe: NOT_SAMPLED is not 0\n");
            bad++;
        }
        if (rb_dpcspipe_verdict(0x00000000u) == RB_DPCSPIPE_NOT_SAMPLED) {
            printf("FAIL dpcspipe: a sampled zero reads as NOT_SAMPLED\n");
            bad++;
        }
    }

    /* THE AGREEMENT TEST. It is an OR over instances and must stay one. */
    {
        uint32_t w[RB_DPCSPIPE_COUNT];

        /* The DMUB says alt_disable 0 for PHY2 (boot 469, ROM-matched). An
         * instance with DISABLE clear agrees. */
        w[0] = 0x00060000u;   /* disabled */
        w[1] = 0x00000000u;   /* not disabled */
        if (!rb_dpcspipe_agrees_with_dmub(w, RB_DPCSPIPE_COUNT, 0)) {
            printf("FAIL dpcspipe: an agreeing instance was not found\n");
            bad++;
        }
        /* Agreement on EITHER instance counts, so the mirrored case must
         * also pass - otherwise the test has smuggled in a claim about which
         * instance is UNIPHY_C. */
        w[0] = 0x00000000u;
        w[1] = 0x00060000u;
        if (!rb_dpcspipe_agrees_with_dmub(w, RB_DPCSPIPE_COUNT, 0)) {
            printf("FAIL dpcspipe: agreement is instance-ordered\n");
            bad++;
        }
        /* Both disabled, DMUB says granted: no agreement. This is the larger
         * finding and must not be reported as agreement. */
        w[0] = 0x00060000u;
        w[1] = 0x00020000u;
        if (rb_dpcspipe_agrees_with_dmub(w, RB_DPCSPIPE_COUNT, 0)) {
            printf("FAIL dpcspipe: both-disabled reported as agreeing\n");
            bad++;
        }
        /* And the other polarity: if the DMUB said disabled, a disabled
         * instance agrees and a clear one does not. */
        if (!rb_dpcspipe_agrees_with_dmub(w, RB_DPCSPIPE_COUNT, 1)) {
            printf("FAIL dpcspipe: disabled-side agreement missed\n");
            bad++;
        }
        w[0] = 0x00000000u; w[1] = 0x00000000u;
        if (rb_dpcspipe_agrees_with_dmub(w, RB_DPCSPIPE_COUNT, 1)) {
            printf("FAIL dpcspipe: clear instances agreed with 'disabled'\n");
            bad++;
        }
        if (rb_dpcspipe_agrees_with_dmub(0, RB_DPCSPIPE_COUNT, 0) ||
            rb_dpcspipe_agrees_with_dmub(w, 0, 0) ||
            rb_dpcspipe_agrees_with_dmub(w, RB_DPCSPIPE_COUNT + 1u, 0)) {
            printf("FAIL dpcspipe: bad args not refused by the agreement\n");
            bad++;
        }
    }

    /* The sweep: DIFFERS is the only control, UNIFORM is not an error. */
    {
        uint32_t w[RB_DPCSPIPE_COUNT];

        w[0] = 0x00060000u; w[1] = 0x00060000u;
        if (rb_dpcspipe_sweep_verdict(w, RB_DPCSPIPE_COUNT)
            != RB_DPCSPIPE_SWEEP_UNIFORM) {
            printf("FAIL dpcspipe: identical is not UNIFORM\n");
            bad++;
        }
        w[1] = 0x00000000u;
        if (rb_dpcspipe_sweep_verdict(w, RB_DPCSPIPE_COUNT)
            != RB_DPCSPIPE_SWEEP_DIFFERS) {
            printf("FAIL dpcspipe: differing is not DIFFERS\n");
            bad++;
        }
        /* A difference OUTSIDE the three fields is not a difference in this
         * register's state. The bit goes on instance 0, because the
         * comparison treats w[0] differently from the rest and a case that
         * puts it elsewhere passes whether or not w[0] is masked - the exact
         * hole rule 48's break-it run found in 2FC. */
        w[0] = 0x80000000u; w[1] = 0x00000000u;
        if (rb_dpcspipe_sweep_verdict(w, RB_DPCSPIPE_COUNT)
            != RB_DPCSPIPE_SWEEP_UNIFORM) {
            printf("FAIL dpcspipe: an undecoded bit made the sweep DIFFER\n");
            bad++;
        }
        /* ...and a real difference past that stray bit must still DIFFER, so
         * the case above cannot pass by making every verdict UNIFORM. */
        w[1] = 0x00020000u;
        if (rb_dpcspipe_sweep_verdict(w, RB_DPCSPIPE_COUNT)
            != RB_DPCSPIPE_SWEEP_DIFFERS) {
            printf("FAIL dpcspipe: a field difference past a stray bit is "
                   "not DIFFERS\n");
            bad++;
        }
        if (rb_dpcspipe_sweep_verdict(0, RB_DPCSPIPE_COUNT)
                != RB_DPCSPIPE_SWEEP_BAD_ARGS ||
            rb_dpcspipe_sweep_verdict(w, 0) != RB_DPCSPIPE_SWEEP_BAD_ARGS ||
            rb_dpcspipe_sweep_verdict(w, RB_DPCSPIPE_COUNT + 1u)
                != RB_DPCSPIPE_SWEEP_BAD_ARGS) {
            printf("FAIL dpcspipe: bad args not refused by the sweep\n");
            bad++;
        }
        if (RB_DPCSPIPE_SWEEP_NOT_SAMPLED != 0 ||
            RB_DPCSPIPE_SWEEP_DIFFERS == 0) {
            printf("FAIL dpcspipe: sweep NOT_SAMPLED/DIFFERS numbering\n");
            bad++;
        }
    }

    if (!bad) {
        printf("ok dpcspipe: 2 real instances, the alias trap and the DMUB "
               "agreement\n");
    }
    return bad;
}
