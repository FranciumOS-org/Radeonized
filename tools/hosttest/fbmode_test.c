/* 3fb: the framebuffer's geometry, derived from the chip (issue #4).
 *
 * THE CONTROL IS THIS BOARD. The four numbers that were hard-coded were
 * measured here, so the derivation has to reproduce all four bit for bit from
 * registers alone before anything it computes for another panel is worth
 * writing into an IOFramebuffer. If it cannot rebuild 1920 / 1080 / 7680 /
 * 8294400 from the live words in rb_otg.h and rb_hubp.h, it is wrong and the
 * second panel's answer is a coincidence.
 *
 * THE SECOND PANEL IS NOT EVIDENCE AND IS NOT PRESENTED AS SUCH. Issue #4
 * gives the Legion's panel size, 2560x1600, but not its register words, so the
 * 2560x1600 inputs below are CONSTRUCTED from ordinary timings for that size
 * and exercise the arithmetic only. They are labelled so nobody reads them as
 * a measurement from that machine (rule 17). What they do establish is that
 * the arithmetic is not accidentally pinned to 1920x1080 - which is the whole
 * defect - and that a padded stride is carried through rather than collapsed
 * to width * 4.
 */
#include <stdio.h>
#include <string.h>

#include "rb_fbmode.h"

#define CHECK(cond, what) do { \
    printf((cond) ? "ok   %s\n" : "FAIL %s\n", (what)); \
    failures += !(cond); \
} while (0)

/* LIVE, this board. h/v blank words from rb_otg.h:23,26; totals from the card
 * and drr_test.c's anchor; pitch from RBFBHubpPitch read on boot 567 and
 * documented as RB_HUBP_EXPECT_PITCH in rb_hubp.h:338. */
/* The TOTALS ARE THE LIVE PUBLISHED VALUES, read off the running machine on
 * boot 567: RBFBOtgHTotal 2106, RBFBOtgVTotal 1129, raw reads published at
 * RBAMDFramebufferVBL.cpp:697. The first draft of this test used 2105/1128 to
 * match a "register is total-1" claim invented in rb_fbmode.h, which made the
 * control pass for the wrong reason while the code carried a compensating +1
 * (rule 48). rb_drr.h and drr_test.c use 2106/1129 directly, and they are
 * what reproduce the refresh measured on an OTG0_FRAME_COUNT edge. */
#define LIVE_H_TOTAL   2106u    /* RBFBOtgHTotal, live, boot 567 */
#define LIVE_V_TOTAL   1129u    /* RBFBOtgVTotal, live, boot 567 */
#define LIVE_HBLANK    0x007f07ffu
#define LIVE_VBLANK    0x0014044cu
#define LIVE_PITCH     0x0000077fu

int fbmode_tests(void);

int fbmode_tests(void)
{
    int failures = 0;
    struct rb_fbmode_in in;
    struct rb_fbmode_out out;
    uint32_t why;

    printf("-- framebuffer geometry from the chip (3fb, issue #4) --\n");

    /* 1. The control: this board, every one of the four frozen constants. */
    memset(&in, 0, sizeof(in));
    memset(&out, 0, sizeof(out));
    in.h_total           = LIVE_H_TOTAL;
    in.v_total           = LIVE_V_TOTAL;
    in.h_blank_start_end = LIVE_HBLANK;
    in.v_blank_start_end = LIVE_VBLANK;
    in.surface_pitch     = LIVE_PITCH;
    why = rb_fbmode_derive(&in, &out);
    CHECK(why == RB_FBMODE_OK, "this board's live registers derive OK");
    CHECK(out.width == 1920,  "width rebuilds kWidth 1920 from OTG H blank");
    CHECK(out.height == 1080, "height rebuilds kHeight 1080 from OTG V blank");
    CHECK(out.bytes_per_row == 7680,
          "bytes_per_row rebuilds kBytesPerRow 7680 from HUBP pitch");
    CHECK(out.aperture_bytes == 8294400,
          "aperture rebuilds kApertureBytes 8294400, the value read live from "
          "IOFBMemorySize");
    CHECK(out.padded == 0,
          "and it agrees this board's rows are NOT padded, which the old "
          "constants' comment asserted from IOFBMemorySize");

    /* 2. The field split, stated separately so a wrong shift cannot hide
     *    behind a right answer. */
    {
        uint32_t s = 0, e = 0;
        CHECK(rb_fbmode_active(LIVE_HBLANK, &s, &e) == 1920 &&
              s == 2047 && e == 127,
              "H blank 0x007f07ff splits START 2047 / END 127 (rb_otg.h:23)");
        CHECK(rb_fbmode_active(LIVE_VBLANK, &s, &e) == 1080 &&
              s == 1100 && e == 20,
              "V blank 0x0014044c splits START 1100 / END 20 (rb_otg.h:26)");
    }

    /* 3. CONSTRUCTED, not measured: a 2560x1600 panel, unpadded. Issue #4's
     *    panel size with ordinary timings, to show the arithmetic is not
     *    pinned to 1920x1080. */
    memset(&in, 0, sizeof(in));
    memset(&out, 0, sizeof(out));
    in.h_total           = 2720u;
    in.v_total           = 1646u;
    in.h_blank_start_end = (2679u) | (119u << 16);  /* 2679-119 = 2560 */
    in.v_blank_start_end = (1620u) | (20u  << 16);  /* 1620-20  = 1600 */
    in.surface_pitch     = 2560u - 1u;
    why = rb_fbmode_derive(&in, &out);
    CHECK(why == RB_FBMODE_OK, "constructed 2560x1600 derives OK");
    CHECK(out.width == 2560 && out.height == 1600,
          "and gives 2560x1600, not this board's 1920x1080 - the defect in #4");
    CHECK(out.bytes_per_row == 10240 && out.aperture_bytes == 16384000,
          "with row 10240 and aperture 16384000");

    /* 4. A PADDED stride is carried, not collapsed to width * 4. This is why
     *    the pitch is read at all: width * 4 would render a sheared image
     *    rather than failing. */
    in.surface_pitch = 2688u - 1u;                  /* padded to 2688 px */
    why = rb_fbmode_derive(&in, &out);
    CHECK(why == RB_FBMODE_OK && out.padded == 1 &&
          out.bytes_per_row == 10752 &&
          out.aperture_bytes == 10752u * 1600u,
          "a padded pitch 2688 gives row 10752 and sets padded=1");
    CHECK(out.width == 2560,
          "and padding does not change the ACTIVE width");

    /* 5. Every refusal, by code, and each for a reason the hardware can
     *    actually produce (rule 14: refusals are codes, not bools). */
    memset(&in, 0, sizeof(in));
    in.h_total = LIVE_H_TOTAL; in.v_total = LIVE_V_TOTAL;
    in.h_blank_start_end = LIVE_HBLANK; in.v_blank_start_end = LIVE_VBLANK;
    in.surface_pitch = LIVE_PITCH;

    {
        struct rb_fbmode_in t = in;
        t.h_blank_start_end = 0u;
        CHECK(rb_fbmode_derive(&t, &out) == RB_FBMODE_E_HBLANK,
              "a zeroed H blank word refuses 1, not a 0-wide mode");
        t = in; t.v_blank_start_end = 0u;
        CHECK(rb_fbmode_derive(&t, &out) == RB_FBMODE_E_VBLANK,
              "a zeroed V blank word refuses 2");
        /* START below END: a register block read through a dead map. */
        t = in; t.h_blank_start_end = (127u) | (2047u << 16);
        CHECK(rb_fbmode_derive(&t, &out) == RB_FBMODE_E_HBLANK,
              "H START below END refuses 1 rather than underflowing");
        t = in; t.h_blank_start_end = (700u) | (60u << 16);   /* 640 wide */
        CHECK(rb_fbmode_derive(&t, &out) == RB_FBMODE_OK,
              "640 wide is accepted - the bound is inclusive");
        t = in; t.h_blank_start_end = (699u) | (60u << 16);   /* 639 */
        CHECK(rb_fbmode_derive(&t, &out) == RB_FBMODE_E_WIDTH,
              "639 wide refuses 3");
        t = in; t.v_blank_start_end = (499u) | (20u << 16);   /* 479 */
        CHECK(rb_fbmode_derive(&t, &out) == RB_FBMODE_E_HEIGHT,
              "479 high refuses 4");
        /* active at or above its own total is arithmetically impossible */
        t = in; t.h_total = 1920u;
        CHECK(rb_fbmode_derive(&t, &out) == RB_FBMODE_E_HTOTAL,
              "width 1920 against H_TOTAL 1920 refuses 5 - active cannot "
              "equal its own total");
        t = in; t.h_total = 1921u;
        CHECK(rb_fbmode_derive(&t, &out) == RB_FBMODE_OK,
              "and H_TOTAL 1921 is accepted, so the bound is exactly one "
              "pixel wide and not an off-by-one either way");
        t = in; t.v_total = 1080u;
        CHECK(rb_fbmode_derive(&t, &out) == RB_FBMODE_E_VTOTAL,
              "height 1080 against V_TOTAL 1080 refuses 6");
        t = in; t.h_total = 0u;
        CHECK(rb_fbmode_derive(&t, &out) == RB_FBMODE_E_HTOTAL,
              "a zero H_TOTAL refuses 5");
        t = in; t.surface_pitch = 1000u;
        CHECK(rb_fbmode_derive(&t, &out) == RB_FBMODE_E_PITCH_LOW,
              "a pitch below the active width refuses 7 - not that surface");
        t = in; t.surface_pitch = 0xffffu;
        CHECK(rb_fbmode_derive(&t, &out) == RB_FBMODE_E_PITCH_HIGH,
              "an absurd pitch refuses 8");
    }

    /* 6. *out is untouched on refusal, so a caller that ignores the code
     *    cannot inherit a half-built geometry.
     *
     * 3fb's break-it sweep caught this check passing for the wrong reason.
     * It originally refused only via a zeroed H blank word, which returns at
     * the FIRST guard - so a write inserted anywhere after that guard left
     * the test green, and the sweep's "write *out before validating" break
     * FIRED NOTHING. A guard that only covers the earliest exit is not
     * coverage of "no exit writes" (rule 48). Every refusal is now driven
     * through this check, early and late, so a stray write before any one of
     * them is caught by the one that follows it. */
    {
        struct rb_fbmode_in bad[6];
        const char *names[6];
        int k;
        bad[0] = in; bad[0].h_blank_start_end = 0u;        names[0] = "H blank zero";
        bad[1] = in; bad[1].v_blank_start_end = 0u;        names[1] = "V blank zero";
        bad[2] = in; bad[2].h_blank_start_end = (699u) | (60u << 16);
                                                            names[2] = "width 639";
        bad[3] = in; bad[3].h_total = 1920u;                names[3] = "H_TOTAL too small";
        bad[4] = in; bad[4].surface_pitch = 1000u;          names[4] = "pitch below width";
        bad[5] = in; bad[5].surface_pitch = 0xffffu;        names[5] = "pitch absurd";
        for (k = 0; k < 6; k++) {
            char what[128];
            memset(&out, 0xAB, sizeof(out));
            (void)rb_fbmode_derive(&bad[k], &out);
            snprintf(what, sizeof(what),
                     "refusing on %s writes nothing into *out", names[k]);
            CHECK(out.width == 0xABABABABu &&
                  out.height == 0xABABABABu &&
                  out.pitch_px == 0xABABABABu &&
                  out.bytes_per_row == 0xABABABABu &&
                  out.aperture_bytes == 0xABABABABu &&
                  out.padded == 0xABABABABu, what);
        }
    }

    /* 7. NULL in, NULL out. */
    CHECK(rb_fbmode_derive(NULL, &out) != RB_FBMODE_OK,
          "a NULL input refuses");
    CHECK(rb_fbmode_derive(&in, NULL) != RB_FBMODE_OK,
          "a NULL output refuses");

    /* 8. Every refusal code is distinct - one published number can never mean
     *    two things, and this is checked rather than eyeballed. */
    {
        const uint32_t codes[] = {
            RB_FBMODE_OK, RB_FBMODE_E_HBLANK, RB_FBMODE_E_VBLANK,
            RB_FBMODE_E_WIDTH, RB_FBMODE_E_HEIGHT, RB_FBMODE_E_HTOTAL,
            RB_FBMODE_E_VTOTAL, RB_FBMODE_E_PITCH_LOW, RB_FBMODE_E_PITCH_HIGH,
            RB_FBMODE_E_APERTURE,
        };
        const int n = (int)(sizeof(codes) / sizeof(codes[0]));
        int i, j, dup = 0;
        for (i = 0; i < n; i++) {
            for (j = i + 1; j < n; j++) {
                if (codes[i] == codes[j]) { dup = 1; }
            }
        }
        CHECK(!dup, "the ten outcome codes are pairwise distinct");
    }

    return failures;
}
