/* SPDX-License-Identifier: MIT */
/* 2EF: the DP DTO pixel-clock block, read-only - and above all the SEGMENT.
 *
 * What these cases exist to stop is one specific wrong boot: a read at
 * BASE_IDX 2, where 0x0080..0x0083 are the DOMAIN power-gating registers
 * rb_pg.h already reads, mistaken for the DTO because a power-gating word is
 * a perfectly plausible PIXEL_RATE_CNTL. So the base is pinned against THIS
 * MACHINE's own captured discovery table rather than against itself, with the
 * segment this kext has used for twenty milestones as the control in the next
 * slot of the same array.
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "rb_dpdto.h"
#include "rb_drr.h"
#include "rb_regs.h"
#include "rb_discovery.h"

int dpdto_tests(const struct rb_disc *real);

int dpdto_tests(const struct rb_disc *real)
{
    int bad = 0;

#define CHECK(c, msg) do { \
    if (!(c)) { printf("FAIL dpdto: %s\n", (msg)); bad++; } \
} while (0)

    /* ---------------------------------------------------- the segment bases
     *
     * The whole point of this file. `real` is tests/data/rembrandt-680m-*.bin
     * parsed - the table captured from the machine this runs on - and the DMU
     * entry carries all five segments, so the number the code uses and the
     * number already proved on hardware come out of the SAME array. If the
     * binary is missing the pin cannot be made, and that is a FAILURE rather
     * than a quiet pass: a check that silently skips is not a check (rule 26 -
     * the search is an instrument, so it is verified against a known-present
     * case, which here is base[2]). */
    if (!real) {
        printf("FAIL dpdto: no real discovery table was passed, so "
               "RB_DPDTO_SEG1 could not be pinned against the chip's own "
               "table - tests/data/rembrandt-680m-*.bin is missing\n");
        bad++;
    } else {
        const struct rb_ip *dmu = rb_disc_find(real, RB_HWID_DMU, 0);
        CHECK(dmu != NULL, "the captured table has a DMU IP entry");
        if (dmu) {
            CHECK(dmu->nbase >= 3,
                  "the DMU entry carries at least three segments, so base[1] "
                  "exists in it and is not an index past the end");
            /* THE CONTROL, first: the segment every hardware reading in this
             * kext has come through. If this does not match, the pin below
             * is meaningless whatever it says. */
            CHECK(dmu->base[2] == RB_DPDTO_SEG2_CONTROL,
                  "base[2] is 0x34c0 - the DMU segment this kext already "
                  "reads, which is what makes the same array's base[1] "
                  "trustworthy");
            CHECK(dmu->base[1] == RB_DPDTO_SEG1,
                  "base[1] is 0xc0 - BASE_IDX 1, where the DTO lives");
            CHECK(dmu->base[1] != dmu->base[2],
                  "the two segments are different numbers, so reading at one "
                  "cannot accidentally be reading at the other");
        }
    }

    /* ------------------------------------------------------- the offsets
     *
     * Written down per instance from docs/linux-ref/dcn_3_1_2_offset.h, not
     * computed by the formula under test - a formula checked against itself
     * proves nothing (hpd_test.c's and pg_test.c's shape):
     *   OTG0_PIXEL_RATE_CNTL 0x0080  DP_DTO0_PHASE 0x0081
     *   DP_DTO0_MODULO       0x0082  OTG0_PHYPLL_PIXEL_RATE_CNTL 0x0083
     *   OTG1 0x0084 / DP_DTO1_PHASE 0x0085 / MODULO 0x0086
     *   OTG2 0x0088 / DP_DTO2_PHASE 0x0089 / MODULO 0x008a
     *   OTG3 0x008c / DP_DTO3_PHASE 0x008d / MODULO 0x008e
     * Unlike the DOMAIN table in rb_pg.h, this stride DOES hold for all four
     * - which is a reading of the header, taken rather than assumed. */
    {
        static const uint32_t want_cntl[4]   = { 0x0080, 0x0084, 0x0088, 0x008c };
        static const uint32_t want_phase[4]  = { 0x0081, 0x0085, 0x0089, 0x008d };
        static const uint32_t want_modulo[4] = { 0x0082, 0x0086, 0x008a, 0x008e };
        uint32_t i;
        for (i = 0; i < RB_DPDTO_COUNT; i++) {
            CHECK(rb_dpdto_dw(i, RB_DPDTO_REG_RATE_CNTL) == want_cntl[i],
                  "OTGn_PIXEL_RATE_CNTL offset matches the header");
            CHECK(rb_dpdto_dw(i, RB_DPDTO_REG_PHASE) == want_phase[i],
                  "DP_DTOn_PHASE offset matches the header");
            CHECK(rb_dpdto_dw(i, RB_DPDTO_REG_MODULO) == want_modulo[i],
                  "DP_DTOn_MODULO offset matches the header");
        }
        CHECK(rb_dpdto_dw(RB_DPDTO_COUNT, 0) == RB_DPDTO_DW_INVALID,
              "a fifth pipe is refused - DCN 3.1.2 has four and the table "
              "ends at OTG3");
        CHECK(rb_dpdto_dw(0, RB_DPDTO_REG_COUNT) == RB_DPDTO_DW_INVALID,
              "a register past the group is refused rather than read out of "
              "the group");
        CHECK(rb_dpdto_dw_max() == 0x008fu,
              "the aperture bound covers OTG3's last dword and no more");
        /* The bound is only useful if it is ABOVE everything rb_dpdto_dw can
         * return, which is the property the caller relies on. */
        for (i = 0; i < RB_DPDTO_COUNT; i++) {
            uint32_t r;
            for (r = 0; r < RB_DPDTO_REG_COUNT; r++) {
                CHECK(rb_dpdto_dw(i, r) <= rb_dpdto_dw_max(),
                      "every dword this header names is inside the bound it "
                      "publishes");
            }
        }
    }

    /* --------------------------------------------------- the field decodes
     *
     * From dcn_3_1_2_sh_mask.h: DP_DTO0_ENABLE bit 4, DPDTO0_ENABLE_STATUS
     * bit 7, PIPE0_DTO_SRC_SEL bit 12, OTG0_PIXEL_RATE_SOURCE bits 1:0. */
    {
        CHECK(rb_dpdto_enabled(0x00000010u) == 1, "DP_DTO0_ENABLE is bit 4");
        CHECK(rb_dpdto_enabled(0x00000008u) == 0,
              "DTBCLK_DTO0_ENABLE at bit 3 is NOT the DP DTO enable - the two "
              "are adjacent and are different clocks");
        CHECK(rb_dpdto_enable_status(0x00000080u) == 1,
              "DPDTO0_ENABLE_STATUS is bit 7");
        CHECK(rb_dpdto_enable_status(0x00000040u) == 0,
              "DTBCLKDTO0_ENABLE_STATUS at bit 6 is a different block's "
              "status");
        CHECK(rb_dpdto_src_sel(0x00001000u) == 1, "PIPE0_DTO_SRC_SEL is bit 12");
        CHECK(rb_dpdto_rate_source(0x00000003u) == 3,
              "OTG0_PIXEL_RATE_SOURCE is the low two bits");
        CHECK(rb_dpdto_rate_source(0xfffffffcu) == 0,
              "and nothing above them leaks into it");
    }

    /* --------------------------------------------- the raster -> pixel clock
     *
     * rb_dpdto.h computes NO pixel clock of its own: it compares against the
     * one `rb_drr.h` has formed since 2y6 and the framebuffer already keeps
     * as `modeClkHz`. So the number the whole base check rests on is pinned
     * HERE, where it is defined, and by hand rather than by restating the
     * expression under test:
     *   2106 * 1129         = 2 377 674 pixels per frame
     *   2 377 674 * 143 830 = 341 980 851 420, +500, / 1000
     *                       = 341 980 851 Hz
     * taken at the ROUNDED 143.830 Hz this project's prose uses. The rate the
     * framebuffer actually runs at is 143.832 992 6 Hz, pinned below, and it
     * gives 341 987 984 Hz. Both are here deliberately: the two differ by
     * 7 kHz, the 342 011 761 Hz derived from boot 255's own raster is 31 kHz
     * from them, and the base check's window is 1.71 MHz - so the window does
     * not care which of this project's instruments produced the reference,
     * which is the property that matters. */
    {
        CHECK(rb_drr_clk_from_mhz(143830u, 2106u, 1129u) == 341980851u,
              "the raster times the ROUNDED refresh gives a pixel clock "
              "inside the window - the figure the journal's prose uses");
        /* The zero cases are a CONTRACT pin, not guard coverage, and saying
         * so is the point. 2EF's break-it run deleted the equivalent zero
         * checks from an earlier copy of this arithmetic and then deleted
         * their apparent replacement, and neither deletion failed anything:
         * a zero factor makes the product zero and returning a zero product
         * IS returning 0. Here the 0 is load-bearing for a different reason -
         * rb_drr.h's callers read it as "no clock derivable", which is what
         * becomes RB_DPDTO_BASE_NO_EXPECTED. Counting these as coverage of a
         * guard would be a test passing for the wrong reason (rule 48). */
        CHECK(rb_drr_clk_from_mhz(143830u, 0u, 1129u) == 0,
              "a zero H_TOTAL yields no reference clock");
        CHECK(rb_drr_clk_from_mhz(143830u, 2106u, 0u) == 0,
              "a zero V_TOTAL yields no reference clock");
        CHECK(rb_drr_clk_from_mhz(0u, 2106u, 1129u) == 0,
              "an unmeasured refresh yields no reference clock");
    }

    /* ------------------------------------------ the raster offsets, PINNED
     *
     * rb_dpdto.h carries its own copies of OTG0_H_TOTAL and OTG0_V_TOTAL so
     * the sampler does not depend on another setup path having run, and a
     * fourth copy of a number is only tolerable if something checks it. This
     * is that check: against rb_regs.h's own table, by NAME, in the one place
     * both headers are visible (rb_gpuname.h's RCC_CONFIG_MEMSIZE alias has
     * the same licence for the same reason).
     *
     * The lookup is by name rather than by index, because an index into that
     * table is exactly the thing a later insertion silently changes. And the
     * search is checked against a known-present case - if the name is not
     * found at all, that is a FAILURE, not a pass with nothing compared
     * (rule 26 - a search is an instrument). */
    {
        const uint32_t n = (uint32_t)(sizeof(rb_reg_defs) /
                                      sizeof(rb_reg_defs[0]));
        uint32_t i;
        int found_h = 0, found_v = 0;
        for (i = 0; i < n; i++) {
            if (rb_reg_defs[i].name == NULL) { continue; }
            if (strcmp(rb_reg_defs[i].name, "OTG0_H_TOTAL") == 0) {
                found_h = 1;
                CHECK(rb_reg_defs[i].reg == RB_DPDTO_DW_OTG0_H_TOTAL,
                      "RB_DPDTO_DW_OTG0_H_TOTAL matches rb_regs.h's table");
                CHECK(rb_reg_defs[i].base_idx == 2,
                      "and it is BASE_IDX 2 - the segment the sampler adds "
                      "to it, which is NOT the segment the DTO lives at");
            }
            if (strcmp(rb_reg_defs[i].name, "OTG0_V_TOTAL") == 0) {
                found_v = 1;
                CHECK(rb_reg_defs[i].reg == RB_DPDTO_DW_OTG0_V_TOTAL,
                      "RB_DPDTO_DW_OTG0_V_TOTAL matches rb_regs.h's table");
                CHECK(rb_reg_defs[i].base_idx == 2,
                      "and it is BASE_IDX 2 too");
            }
        }
        CHECK(found_h && found_v,
              "both raster registers were FOUND in rb_regs.h - a lookup that "
              "matches nothing compares nothing and must not pass quietly");
        CHECK(RB_DPDTO_TOTAL_MASK == RB_DRR_TOTAL_MASK,
              "the raster mask agrees with rb_drr.h's, which masks the same "
              "two registers for the same reason");
    }

    /* --------------------------------------- the vblank period -> millihertz
     *
     * The framebuffer holds a PERIOD, not a rate, and the first draft of these
     * two cases failed - correctly, and worth recording. It assumed the
     * panel's rate was 143.830 Hz because that is what the journal and the
     * reference card both round it to, and used 6 952 653 ns. The framebuffer
     * does not: `kRefreshRate1616` is 0x008fd53f, which is 9 426 239 / 65 536
     * = **143.832 992 6 Hz**, and `kVBLPeriodNsFast` is 1e9 * 65536 / that =
     * **6 952 507 ns**. The constant had to be READ rather than recalled -
     * which is the recurring error of this project in its smallest possible
     * form, caught here by a test rather than by a boot.
     *
     * The payoff is a cross-check that could not have been arranged: the
     * raster taken at 143 833 mHz gives
     *   2106 * 1129 * 143833 / 1000 = 341 987 984 Hz
     * which is the ~341 987 984 Hz this project MEASURED directly, to the
     * hertz. The two paths to the panel's pixel clock now agree exactly. */
    {
        CHECK(rb_dpdto_mhz_from_period_ns(6952507ull) == 143833u,
              "the framebuffer's own vblank period converts back to the "
              "refresh its 16.16 constant encodes");
        CHECK(rb_dpdto_mhz_from_period_ns(16666666ull) == 60000u,
              "and 60 Hz round-trips too - the 60 Hz mode is the other rate "
              "this project runs the clock at");
        CHECK(rb_dpdto_mhz_from_period_ns(0ull) == 0,
              "a period of zero yields no rate rather than dividing by it");
        /* The round trip that actually matters: period -> mHz -> pixel clock
         * IS the whole RB_DPDTO_REF_SRC_RASTER path, so it is pinned end to
         * end against a number measured by a different instrument. */
        CHECK(rb_drr_clk_from_mhz(rb_dpdto_mhz_from_period_ns(6952507ull),
                                  2106u, 1129u) == 341987984u,
              "period -> refresh -> pixel clock reproduces the pixel clock "
              "this project measured, to the hertz");
    }

    /* ----------------------------------------------------- the base verdict
     *
     * Each of the six outcomes gets a case, because the whole purpose of
     * separate codes is that the boot can be read without re-deriving them
     * (rule 14 - a refusal is a code). */
    {
        const uint32_t expected = 341987984u;
        /* A confirmed read: PHASE is the pixel clock in hertz, MODULO the DP
         * reference in hertz (600 MHz used here only as a plausible value -
         * what the chip actually holds is the thing this read is FOR, and the
         * bound is wide for exactly that reason). */
        uint32_t s1[RB_DPDTO_REG_COUNT] =
            { 0x00001090u, 341987984u, 600000000u, 0u };
        uint32_t s2[RB_DPDTO_REG_COUNT] =
            { 0x00000000u, 0x10000000u, 0x00000000u, 0x10000000u };

        CHECK(rb_dpdto_base_verdict(s1, s2, expected) ==
              RB_DPDTO_BASE_CONFIRMED,
              "pipe 0's PHASE reproducing the live raster confirms the base");

        /* THE ALIAS CONTROL. Two bases returning the same four words means
         * the segment arithmetic did nothing, and it outranks everything
         * else - including a PHASE that would otherwise confirm. */
        {
            uint32_t a[RB_DPDTO_REG_COUNT];
            uint32_t i;
            for (i = 0; i < RB_DPDTO_REG_COUNT; i++) { a[i] = s1[i]; }
            CHECK(rb_dpdto_base_verdict(s1, a, expected) ==
                  RB_DPDTO_BASE_ALIASED,
                  "identical words at both bases is ALIASED even when the "
                  "phase would have passed - a correct-looking number read "
                  "at the wrong address is the failure this whole file is "
                  "about");
        }

        /* The three ways the arithmetic can decline, each distinct. */
        {
            uint32_t z[RB_DPDTO_REG_COUNT] = { 0x1000u, 0u, 600000000u, 0u };
            CHECK(rb_dpdto_base_verdict(z, s2, expected) ==
                  RB_DPDTO_BASE_PHASE_ZERO,
                  "PHASE 0 says NOTHING about the base - the firmware may "
                  "have brought the panel up on a PHYPLL, which is a correct "
                  "base with an unprogrammed DTO");
        }
        {
            /* 0x10000000 - the power-gating-shaped word, 268 MHz, 21 % away
             * from the panel's clock. The impostor this check exists for. */
            uint32_t w[RB_DPDTO_REG_COUNT] =
                { 0x1000u, 0x10000000u, 600000000u, 0u };
            CHECK(rb_dpdto_base_verdict(w, s2, expected) ==
                  RB_DPDTO_BASE_PHASE_WRONG,
                  "a DOMAIN-shaped word in PHASE is rejected");
        }
        {
            uint32_t m[RB_DPDTO_REG_COUNT] =
                { 0x1000u, 341987984u, 7u, 0u };
            CHECK(rb_dpdto_base_verdict(m, s2, expected) ==
                  RB_DPDTO_BASE_MODULO_BAD,
                  "a MODULO that cannot be a reference clock gets its OWN "
                  "code, because at that point PHASE already matched the "
                  "raster and the finding is about the clock, not the base");
        }
        CHECK(rb_dpdto_base_verdict(s1, s2, 0u) ==
              RB_DPDTO_BASE_NO_EXPECTED,
              "no derivable raster means the instrument had no reference - "
              "which is not the same answer as a wrong address");
        CHECK(rb_dpdto_base_verdict(NULL, s2, expected) ==
              RB_DPDTO_BASE_NOT_SAMPLED,
              "nothing sampled is its own code, never a verdict");

        /* THE TOLERANCE, both sides of the edge. 0.5 % of 341 980 851 is
         * 1 709 904 Hz. Checked at the boundary and one hertz past it, so the
         * window is pinned rather than merely exercised. */
        {
            const uint32_t win = 1709939u;
            uint32_t e[RB_DPDTO_REG_COUNT];
            uint32_t i;
            for (i = 0; i < RB_DPDTO_REG_COUNT; i++) { e[i] = s1[i]; }
            e[RB_DPDTO_REG_PHASE] = expected + win;
            CHECK(rb_dpdto_base_verdict(e, s2, expected) ==
                  RB_DPDTO_BASE_CONFIRMED, "the top of the window passes");
            e[RB_DPDTO_REG_PHASE] = expected + win + 1u;
            CHECK(rb_dpdto_base_verdict(e, s2, expected) ==
                  RB_DPDTO_BASE_PHASE_WRONG, "one hertz past it does not");
            e[RB_DPDTO_REG_PHASE] = expected - win;
            CHECK(rb_dpdto_base_verdict(e, s2, expected) ==
                  RB_DPDTO_BASE_CONFIRMED, "the bottom of the window passes");
            e[RB_DPDTO_REG_PHASE] = expected - win - 1u;
            CHECK(rb_dpdto_base_verdict(e, s2, expected) ==
                  RB_DPDTO_BASE_PHASE_WRONG, "and one hertz below it does not");
            /* Both of this project's independent derivations of the panel's
             * clock must pass against the third, or the window is too tight
             * to survive which instrument measured it. */
            e[RB_DPDTO_REG_PHASE] = 342011761u;
            CHECK(rb_dpdto_base_verdict(e, s2, expected) ==
                  RB_DPDTO_BASE_CONFIRMED,
                  "boot 255's raster-derived clock passes against the live "
                  "raster's");
            e[RB_DPDTO_REG_PHASE] = 341987984u;
            CHECK(rb_dpdto_base_verdict(e, s2, expected) ==
                  RB_DPDTO_BASE_CONFIRMED,
                  "and so does the measured one");
        }

        /* The reference clock is only reportable once the base is confirmed:
         * a MODULO read at an unproven address is not a `dprefclk`. */
        CHECK(rb_dpdto_ref_hz(s1, RB_DPDTO_BASE_CONFIRMED) == 600000000u,
              "a confirmed base yields the reference clock");
        CHECK(rb_dpdto_ref_hz(s1, RB_DPDTO_BASE_PHASE_ZERO) == 0,
              "an unconfirmed base yields nothing to publish as one");
        CHECK(rb_dpdto_ref_hz(s1, RB_DPDTO_BASE_ALIASED) == 0,
              "and an aliased read yields nothing");
    }

    /* ------------------------------------------------------ the pipe verdict
     *
     * The second, INDEPENDENT question. It must be answerable when the base
     * check declines and must not change its answer because of it - that
     * separation is the point. */
    {
        CHECK(rb_dpdto_pipe_verdict(0x00000090u) == RB_DPDTO_PIPE_DTO,
              "enable set and the hardware's own status bit agreeing is the "
              "DTO driving");
        CHECK(rb_dpdto_pipe_verdict(0x00000010u) == RB_DPDTO_PIPE_DTO_ASKED,
              "enable set with the status bit clear is what was ASKED, not "
              "what happened - and it is not the same reading");
        CHECK(rb_dpdto_pipe_verdict(0x00000000u) ==
              RB_DPDTO_PIPE_OTHER_SOURCE,
              "no enable means this pipe's clock comes from somewhere else");
        CHECK(rb_dpdto_pipe_verdict(0x00000008u) ==
              RB_DPDTO_PIPE_OTHER_SOURCE,
              "DTBCLK's enable is somewhere else by this block's reckoning");
    }

#undef CHECK
    return bad;
}
