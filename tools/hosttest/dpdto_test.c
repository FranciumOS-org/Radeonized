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
#include "rb_dcn_deny.h"
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

    /* ================= 2F1: THE WRITE SIDE =============================
     *
     * Every number here is boot 452's own, read off the chip: pipe 0's rate
     * word 0x80000090, the three dark pipes' 0x8000c000, MODULO 600 000 000.
     * The point of pinning them is that the refusals and the verdict are
     * being checked against the words the hardware actually returns, not
     * against invented ones (2CC's practice). */
    {
        const uint32_t dark = 0x8000c000u;   /* pipes 1-3, boot 452         */
        const uint32_t live = 0x80000090u;   /* pipe 0, enable + status     */
        const uint32_t ref  = 600000000u;    /* MODULO[0], boot 452         */
        const uint32_t pix  = 148500000u;    /* 2BD's preferred timing      */

        /* ----------------------------------------------- the gate that matters
         *
         * An unproven base must refuse, because the other candidate base is
         * the power gating of the pipe scanning the panel. Checked for EVERY
         * base code, not just one, since the write is only allowed on the
         * single value CONFIRMED. */
        CHECK(rb_dpdto_write_refuse(1, pix, ref, dark, 1, 1,
                                    RB_DPDTO_BASE_CONFIRMED)
              == RB_DPDTO_W_OK,
              "a confirmed base, a dark pipe and a plausible clock is allowed");
        {
            const int codes[] = { RB_DPDTO_BASE_NOT_SAMPLED,
                                  RB_DPDTO_BASE_PHASE_ZERO,
                                  RB_DPDTO_BASE_PHASE_WRONG,
                                  RB_DPDTO_BASE_MODULO_BAD,
                                  RB_DPDTO_BASE_ALIASED,
                                  RB_DPDTO_BASE_NO_EXPECTED };
            unsigned k;
            for (k = 0; k < sizeof(codes) / sizeof(codes[0]); k++) {
                CHECK(rb_dpdto_write_refuse(1, pix, ref, dark, 1, 1, codes[k])
                      == RB_DPDTO_W_BASE_UNPROVEN,
                      "every base verdict but CONFIRMED refuses the write");
            }
        }

        /* The panel's pipe, by name.
         *
         * The break-it run found the ORDER of this check and the range check
         * to be INERT - swapping them changed no assertion, because pipe 0
         * passes `inst >= RB_DPDTO_COUNT` either way, so the panel check
         * fires whichever comes first. Recorded rather than dressed up: a
         * break that fires nothing is a result (rule 48), and the first
         * draft's comment claimed an ordering guarantee that guards nothing.
         * The CHECK stays, because what it pins is the one that matters -
         * pipe 0 gets PANEL_PIPE and not PIPE_RANGE, so a log naming the
         * refusal names a decision and not an off-by-one. */
        CHECK(rb_dpdto_write_refuse(0, pix, ref, dark, 1, 1,
                                    RB_DPDTO_BASE_CONFIRMED)
              == RB_DPDTO_W_PANEL_PIPE,
              "pipe 0 is the panel and is refused BY NAME");
        CHECK(RB_DPDTO_FIRST_FREE_PIPE != RB_DPDTO_PANEL_PIPE,
              "the default target is not the panel's pipe");
        CHECK(rb_dpdto_write_refuse(4, pix, ref, dark, 1, 1,
                                    RB_DPDTO_BASE_CONFIRMED)
              == RB_DPDTO_W_PIPE_RANGE,
              "a pipe past the fourth is out of range, not the panel");

        /* A pipe whose DTO is already running is a clock something may be
         * using. Pipe 0's own live word is the case, which is a second,
         * independent refusal of the panel. */
        CHECK(rb_dpdto_write_refuse(1, pix, ref, live, 1, 1,
                                    RB_DPDTO_BASE_CONFIRMED)
              == RB_DPDTO_W_PIPE_LIVE,
              "a pipe whose DP DTO is already enabled is refused");

        CHECK(rb_dpdto_write_refuse(1, pix, ref, dark, 0, 1,
                                    RB_DPDTO_BASE_CONFIRMED)
              == RB_DPDTO_W_NOT_ARMED, "an unarmed boot writes nothing");
        CHECK(rb_dpdto_write_refuse(1, pix, ref, dark, 1, 0,
                                    RB_DPDTO_BASE_CONFIRMED)
              == RB_DPDTO_W_NO_REGS, "and so does one with no mapping");

        /* The reference about to be COPIED. 0x10000000 is the impostor 2F0
         * actually found in the alias slot, and it is inside the wide
         * reference band - so this is not what catches a wrong base, which is
         * exactly why the base verdict is a separate gate ahead of it. */
        CHECK(rb_dpdto_write_refuse(1, pix, 0, dark, 1, 1,
                                    RB_DPDTO_BASE_CONFIRMED)
              == RB_DPDTO_W_REF_BAD, "a zero reference cannot be copied");
        CHECK(rb_dpdto_write_refuse(1, pix, 1000u, dark, 1, 1,
                                    RB_DPDTO_BASE_CONFIRMED)
              == RB_DPDTO_W_REF_BAD, "nor can one below the reference band");
        CHECK(rb_dpdto_ref_usable(ref), "600 MHz is a usable reference");
        CHECK(rb_dpdto_ref_usable(0x10000000u),
              "0x10000000 IS inside the band - the base gate is what "
              "excludes it, not this one");

        /* The asked clock. The ceiling is the live reference rather than a
         * constant, which is the half that is actually load-bearing: a PHASE
         * at or above MODULO is not a ratio this block can make. */
        CHECK(rb_dpdto_write_refuse(1, 1000000u, ref, dark, 1, 1,
                                    RB_DPDTO_BASE_CONFIRMED)
              == RB_DPDTO_W_PIX_RANGE, "1 MHz is below any DP pixel clock");
        CHECK(rb_dpdto_write_refuse(1, ref, ref, dark, 1, 1,
                                    RB_DPDTO_BASE_CONFIRMED)
              == RB_DPDTO_W_PIX_RANGE,
              "a clock equal to the reference is a ratio of 1, not a mode");
        CHECK(rb_dpdto_write_refuse(1, ref + 1u, ref, dark, 1, 1,
                                    RB_DPDTO_BASE_CONFIRMED)
              == RB_DPDTO_W_PIX_RANGE, "and above it is not a clock at all");
        /* Both of this sink's own timings, from the EDID 2BD read off it, and
         * the panel's own clock, must all be accepted - a bound that refused
         * one of the modes it exists for would be useless. */
        CHECK(rb_dpdto_write_refuse(1, 148500000u, ref, dark, 1, 1,
                                    RB_DPDTO_BASE_CONFIRMED) == RB_DPDTO_W_OK,
              "2BD's preferred timing, 148.5 MHz, is allowed");
        CHECK(rb_dpdto_write_refuse(1, 228000000u, ref, dark, 1, 1,
                                    RB_DPDTO_BASE_CONFIRMED) == RB_DPDTO_W_OK,
              "and its 100 Hz mode, 228 MHz");
        CHECK(rb_dpdto_write_refuse(1, 343000000u, ref, dark, 1, 1,
                                    RB_DPDTO_BASE_CONFIRMED) == RB_DPDTO_W_OK,
              "and the panel's own PHASE, which is the fastest clock this "
              "chip is known to drive");

        /* ------------------------------------------------------ the RMW word
         *
         * Reproducing REG_UPDATE_2: the two fields placed, everything else as
         * read. The dark pipes' 0x8000c000 carries DTBCLK_DTO_DIV 8 in the top
         * nibble and DIO_FIFO_ERROR 3 at bits 14-15, and BOTH must survive -
         * losing the divider would be programming a different register. */
        CHECK(rb_dpdto_rate_word(dark, 1) == 0x8000d010u,
              "the enable and SRC_SEL go in and the divider and error bits "
              "survive");
        CHECK(rb_dpdto_rate_word(dark, 0) == 0x8000c010u,
              "SRC_SEL 0 reproduces the firmware's own value instead");
        CHECK((rb_dpdto_rate_word(dark, 1) & 0xf0000000u) ==
              (dark & 0xf0000000u),
              "DTBCLK_DTO_DIV is preserved, not rewritten");
        CHECK(rb_dpdto_rate_word(dark, 1) != dark,
              "a real write MUST move a dark pipe's word, which is what makes "
              "`changed` worth publishing beside `applied`");

        /* ------------------------------------------------------- the triple */
        CHECK(rb_dpdto_rate_applied(0x8000d010u, 1),
              "enable set and SRC_SEL where asked is applied");
        CHECK(!rb_dpdto_rate_applied(0x8000c010u, 1),
              "SRC_SEL missing is not applied, even with the enable set");
        CHECK(!rb_dpdto_rate_applied(0x8000d000u, 1),
              "nor is SRC_SEL alone with no enable");
        CHECK(rb_dpdto_rate_applied(0x8000c010u, 0),
              "and asking for SRC_SEL 0 is satisfied by its being 0");
        CHECK(rb_dpdto_rate_changed(dark, 0x8000d010u), "the word moved");
        CHECK(!rb_dpdto_rate_changed(dark, dark), "and an unmoved word has not");
        CHECK(rb_dpdto_rate_clean(dark, 0x8000d010u),
              "nothing outside the two fields moved");
        CHECK(rb_dpdto_rate_clean(dark, 0x8000d090u),
              "and the hardware's own status bit changing is not dirt - it is "
              "the verdict");
        CHECK(!rb_dpdto_rate_clean(dark, 0x9000d010u),
              "but the divider changing under us is");

        /* ------------------------------------------------------- the verdict
         *
         * It reads DPDTO_ENABLE_STATUS, a bit we do not write, and it needs a
         * TRANSITION - so a pipe already reporting a clock cannot satisfy it
         * (rb_dig_verdict()'s requirement, for the same reason). */
        CHECK(rb_dpdto_prog_verdict(dark, 0x8000d090u) ==
              RB_DPDTO_PROG_STARTED,
              "the status bit going 0 -> 1 is a clock, and it is a bit we did "
              "not write");
        CHECK(rb_dpdto_prog_verdict(dark, 0x8000d010u) == RB_DPDTO_PROG_STILL,
              "the enable we wrote reading back alone is NOT the verdict");
        CHECK(rb_dpdto_prog_verdict(live, live) == RB_DPDTO_PROG_WAS_ON,
              "a pipe whose status was already set cannot satisfy it");
        CHECK(RB_DPDTO_PROG_NOT_RUN < 0,
              "never programmed is its own code, never a verdict");

        /* ------------------------------------------- the aperture inequality
         *
         * The relation that makes the aperture question already answered: the
         * highest dword this write touches is below the segment every mapping
         * in this kext has had to clear. Asserted here instead of stated in a
         * comment at the call site. */
        CHECK(rb_dpdto_write_dw_max() < RB_DPDTO_SEG2_CONTROL,
              "every dword the write touches is below the segment the rest of "
              "this kext works at, so no mapping that works can be too short");
        CHECK(rb_dpdto_write_dw_max() == RB_DPDTO_SEG1 + 0x8fu,
              "and that highest dword is 0xc0 + 0x8f, as the comment says");

        /* THE DENY LIST IS THE WRONG INSTRUMENT HERE, and this pins why
         * rather than leaving it to a comment: it is a BASE_IDX 2 list, and
         * asking it about a BASE_IDX 1 offset returns 0 for the wrong reason
         * - which would read exactly like a check that had been done (rule
         * 26 on an instrument, and rule 14 on its answer). */
        CHECK(RB_DCN_DENY_BASE_IDX == 2u,
              "the deny list is indexed by BASE_IDX 2 offsets");
        CHECK(!rb_dcn_is_denied(rb_dpdto_dw(1, RB_DPDTO_REG_PHASE)),
              "so it passes a DTO offset for the wrong reason, and the base "
              "verdict is the check that counts");
        CHECK(rb_dcn_is_denied(0x89u),
              "while the list itself is non-empty - the control for the line "
              "above");
    }

    /* ================= 2F2: THE WORD THE CHIP RETURNED ===================
     *
     * Boot 455 ran the write and these are its OWN numbers, not a model.
     * `dmubbl dto` was invoked twice; the second invocation was refused by
     * the already-live guard, and the word it refused on is what pins the
     * first one's effect.
     *
     *   pipe 1 before (2F0 and 2F1, two boots, pipes 1-3 byte-identical):
     *       0x8000c000
     *   pipe 1 after:
     *       0x8000d090
     *
     * The whole finding is the arithmetic below, which is why it is an
     * assertion and not a sentence in the journal. */
    {
        const uint32_t dark = 0x8000c000u;   /* measured, two boots          */
        const uint32_t now  = 0x8000d090u;   /* measured, after the write    */
        const uint32_t wrote = rb_dpdto_rate_word(dark, 1);

        /* THE VERDICT. The live word differs from the word this kext writes
         * by EXACTLY DPDTO_ENABLE_STATUS - one bit, which rb_dpdto_rate_word
         * does not set and cannot set, since it only ever touches
         * DP_DTO_ENABLE and SRC_SEL. Anything else in this difference would
         * mean something besides our store moved the register, and that is
         * the reading this assertion exists to catch. */
        CHECK((now ^ wrote) == RB_DPDTO_DP_DTO_STATUS,
              "the live word is the word we wrote plus the hardware's own "
              "status bit, and nothing else moved");
        CHECK(!rb_dpdto_enable_status(dark) && rb_dpdto_enable_status(now),
              "and that bit went 0 -> 1, which 2F0 attested both states of "
              "on this chip before anything was programmed");
        CHECK(rb_dpdto_prog_verdict(dark, now) == RB_DPDTO_PROG_STARTED,
              "so the verdict over that transition is STARTED");

        /* The RMW's duty, in the measured word rather than the modelled one:
         * the divider and the error bits came through untouched. */
        CHECK((now & RB_DPDTO_DTBCLK_DIV) == (dark & RB_DPDTO_DTBCLK_DIV),
              "DTBCLK_DTO_DIV survived the read-modify-write");
        CHECK(rb_dpdto_fifo_error(now) == rb_dpdto_fifo_error(dark) &&
              rb_dpdto_fifo_error(now) == 3u,
              "and so did DIO_FIFO_ERROR, which stayed 3 across a DTO being "
              "started - so it does not clear on the DTO alone");
        CHECK(rb_dpdto_rate_applied(now, 1) && rb_dpdto_rate_changed(dark, now),
              "applied AND changed, so the readback carries evidence of the "
              "store rather than agreeing with a value already present "
              "(rule 20)");

        /* WHY THE SECOND INVOCATION WAS REFUSED, and that it was refused for
         * THIS reason and not another. The arguments are boot 454's: base
         * CONFIRMED, armed, regs present, pipe 1, 148.5 MHz, ref 600 MHz. */
        CHECK(rb_dpdto_write_refuse(1u, 148500000u, 600000000u, now,
                                    1, 1, RB_DPDTO_BASE_CONFIRMED)
              == RB_DPDTO_W_PIPE_LIVE,
              "a second write to the pipe we just started is refused as "
              "PIPE_LIVE");
        CHECK(rb_dpdto_write_refuse(1u, 148500000u, 600000000u, dark,
                                    1, 1, RB_DPDTO_BASE_CONFIRMED)
              == RB_DPDTO_W_OK,
              "and the same call against the dark word it replaced is "
              "allowed - so PIPE_LIVE is the live word's doing, not another "
              "gate's");

        /* ------------------------------------- THE IN-BOOT CONTROL
         *
         * Taken from the PERIODIC sample rather than the write path, in the
         * same window, so it is not our own store read back (rule 20) and
         * not a comparison across boots (rule 44): the two pipes we did NOT
         * name still hold the dark word byte for byte, and the panel's holds
         * exactly what 2F0 measured. The change is confined to the pipe the
         * dial named, which is what makes "our write did this" a reading
         * rather than a sequence of events. */
        CHECK(rb_dpdto_enable_status(0x80000090u) &&
              !rb_dpdto_enable_status(0x8000c000u),
              "boot 455's other three pipes decode as the panel driving and "
              "two still dark - the control for the pipe that moved");

        /* The divergence we chose, in the chip's own word: SRC_SEL is 1 here
         * where the firmware left pipe 0's at 0. Stated as a difference
         * between two measured words; what the field SELECTS is still not
         * established (rule 17). */
        CHECK((now & RB_DPDTO_SRC_SEL) &&
              !(0x80000090u & RB_DPDTO_SRC_SEL),
              "SRC_SEL is set on the pipe we programmed and clear on the "
              "panel's, which is Linux's value against the firmware's");
    }

#undef CHECK
    return bad;
}
