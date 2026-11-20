/* #8: the frame clock's initial period.
 *
 * The control that makes any of this meaningful is THIS BOARD: the conversion
 * has to reproduce kVBLPeriodNsFast from the constant the fbkext has used
 * since 2os, bit for bit, before the number it computes for a 90 Hz panel is
 * worth putting into a live frame clock. If it cannot rebuild 6 952 992 ns
 * from 143 833 mHz, it is wrong and #8's panel getting a plausible answer is a
 * coincidence (the same reasoning fbmode_test.c applies to the geometry).
 */
#include <stdio.h>
#include "rb_vblsrc.h"

static int fails;
#define ck(c, msg) do { if (!(c)) { printf("FAIL vblsrc: %s\n", (msg)); fails++; } } while (0)

int vblsrc_tests(void);
int vblsrc_tests(void)
{
    /* The fbkext's own constants, repeated so a change to either is a failing
     * test here rather than a silent divergence. */
    const uint32_t kThisBoardMhz = 143833u;
    const uint64_t kRefreshRate1616 = 0x008fd53fu;
    const uint64_t kVBLPeriodNsFast = 1000000000ULL * 65536ULL / kRefreshRate1616;

    fails = 0;

    /* THE CONTROL. This board, from the measured refresh, against the period
     * the kext has actually been running on. */
    ck(rb_vblsrc_period_ns(kThisBoardMhz) == kVBLPeriodNsFast,
       "143833 mHz reproduces kVBLPeriodNsFast EXACTLY - the reference board "
       "is the control for every other panel's answer");
    ck(kVBLPeriodNsFast == 6952507ull,
       "kVBLPeriodNsFast is 6952507 ns - pinned, so a change to the constant "
       "is a failing test and not a silent divergence");

    /* #8's panel. Not a measurement from that machine - the figure is their
     * reported 89871 mHz - so this exercises the arithmetic only, and is
     * labelled so nobody reads it as hardware evidence (rule 17). */
    ck(rb_vblsrc_period_ns(89871u) == 11127060ull,
       "#8's reported 89871 mHz gives 11127060 ns (~90 Hz), against the "
       "6952507 ns the constant would have imposed - a 60 % error in the "
       "period the frame clock ran at");
    ck(rb_vblsrc_period_ns(89871u) > rb_vblsrc_period_ns(kThisBoardMhz),
       "a slower panel gets a LONGER period - the direction of the defect");

    /* ROUNDING, which is the whole reason this is a function and not a divide.
     * THIS BOARD CANNOT TEST IT: at 143833 mHz truncation and rounding both
     * give 6952507, so the control above passes either way and says nothing
     * about the rounding - a check that would have passed for the wrong reason
     * (rule 48). #8's panel is where the two differ, by exactly 1 ns, so that
     * is where the discrimination lives. Both halves are stated, so reverting
     * to truncation fails here rather than passing quietly. */
    ck(1000000000000ull / 143833ull == 6952507ull,
       "at 143833 mHz truncation AGREES with rounding - the reference board "
       "is not the control for this term");
    ck(1000000000000ull / 89871ull == 11127059ull,
       "at 89871 mHz truncation gives 11127059, one less than rounding - so "
       "the 11127060 assertion above is known to discriminate");

    /* THE BOUND, both ends and both sides of each end. A refusal is 0 and
     * never a fallback period, so a caller cannot mistake it for a reading. */
    ck(rb_vblsrc_period_ns(RB_VBLSRC_MHZ_MIN) != 0u, "20 Hz is in range");
    ck(rb_vblsrc_period_ns(RB_VBLSRC_MHZ_MIN - 1u) == 0u, "just under refuses");
    ck(rb_vblsrc_period_ns(RB_VBLSRC_MHZ_MAX) != 0u, "400 Hz is in range");
    ck(rb_vblsrc_period_ns(RB_VBLSRC_MHZ_MAX + 1u) == 0u, "just over refuses");
    ck(rb_vblsrc_period_ns(0u) == 0u, "0 refuses - what measureMhz returns on "
                                      "failure, and it must not become a period");
    ck(rb_vblsrc_period_ns(0xffffffffu) == 0u, "an all-ones read refuses");

    /* ROUND TRIP over the whole range, every 1 Hz. Not a spot check: the
     * claim is that a published period reads back as the refresh that made
     * it, and one pair agreeing would not establish that (rule 18). */
    {
        unsigned bad = 0;
        for (uint32_t m = RB_VBLSRC_MHZ_MIN; m <= RB_VBLSRC_MHZ_MAX; m += 1000u) {
            uint64_t ns = rb_vblsrc_period_ns(m);
            uint32_t back = rb_vblsrc_mhz_from_ns(ns);
            if (ns == 0u || back == 0u) { bad++; continue; }
            /* Within 1 mHz: the two roundings can each move half a unit. */
            if (back > m ? back - m > 1u : m - back > 1u) { bad++; }
        }
        ck(bad == 0u, "every refresh from 20 to 400 Hz round-trips to within "
                      "1 mHz");
    }
    ck(rb_vblsrc_mhz_from_ns(0u) == 0u, "a zero period reads back as 0");
    ck(rb_vblsrc_mhz_from_ns(1ull) == 0u,
       "an absurdly short period reads back as 0, not as a huge refresh");
    ck(rb_vblsrc_mhz_from_ns(~0ull) == 0u, "an all-ones period reads back as 0");
    ck(rb_vblsrc_mhz_from_ns(kVBLPeriodNsFast) == kThisBoardMhz,
       "this board's period reads back as this board's refresh");

    /* THE SOURCE CODES. 0 must be the pre-existing behaviour, so a boot that
     * published 0 is a boot where nothing changed. */
    ck(RB_VBLSRC_CONST == 0u,
       "CONST is 0, so an unwritten field and 'the constant stands' agree "
       "rather than contradicting (rule 32)");

    /* DERIVED needs BOTH terms. A source code alone is a claim; a non-zero
     * period alone does not say where it came from. */
    ck(rb_vblsrc_derived(RB_VBLSRC_MODELIST, 11127224ull),
       "mode list with a real period IS derived");
    ck(rb_vblsrc_derived(RB_VBLSRC_MEASURED, 11127224ull),
       "measured with a real period IS derived");
    ck(!rb_vblsrc_derived(RB_VBLSRC_MEASURED, 0u),
       "MEASURED with a ZERO period is NOT derived - the case where the source "
       "code would otherwise read as a success it did not achieve");
    ck(!rb_vblsrc_derived(RB_VBLSRC_CONST, 6952992ull),
       "the constant is not derived even though its period is non-zero");
    ck(!rb_vblsrc_derived(RB_VBLSRC_MEAS_FAILED, 6952992ull),
       "a FAILED measurement with the constant's period is not derived");
    ck(!rb_vblsrc_derived(RB_VBLSRC_NO_MAP, 6952992ull),
       "no map is not derived");

    /* Every code is distinct, so one published number cannot mean two things. */
    {
        const uint32_t all[] = { RB_VBLSRC_CONST, RB_VBLSRC_MODELIST,
                                 RB_VBLSRC_MEASURED, RB_VBLSRC_MEAS_FAILED,
                                 RB_VBLSRC_NO_MAP };
        unsigned dup = 0, i, j;
        for (i = 0; i < 5; i++) {
            for (j = i + 1; j < 5; j++) { if (all[i] == all[j]) { dup++; } }
        }
        ck(dup == 0u, "the five source codes are pairwise distinct");
    }

    if (fails == 0) { printf("vblsrc: ok\n"); }
    return fails;
}
