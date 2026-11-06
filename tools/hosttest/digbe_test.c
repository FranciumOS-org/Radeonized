/* 2E7: DIG_BE_CNTL decode. The offsets are PINNED against the vendored
 * header, and the aperture relation against rb_enc's, because 2DD is what a
 * readable wrong address costs. */
#include <stdio.h>
#include <stdint.h>
#include <stddef.h>

#include "rb_digbe.h"
#include "rb_enc.h"
#include "rb_dpalt.h"
#include "rb_dcn_deny.h"

int digbe_tests(void);

int digbe_tests(void)
{
    int bad = 0;
#define CHECK(c, m) do { if (!(c)) { printf("FAIL digbe: %s\n", (m)); bad++; } } while (0)

    /* --- offsets, straight off dcn_3_1_2_offset.h --------------------- */
    CHECK(rb_digbe_dw(0) == 0x20b0u, "regDIG0_DIG_BE_CNTL");
    CHECK(rb_digbe_dw(1) == 0x21b0u, "regDIG1_DIG_BE_CNTL");
    CHECK(rb_digbe_dw(2) == 0x22b0u, "regDIG2_DIG_BE_CNTL");
    CHECK(rb_digbe_dw(3) == 0x23b0u, "regDIG3_DIG_BE_CNTL");
    CHECK(rb_digbe_dw(4) == 0x24b0u, "regDIG4_DIG_BE_CNTL");
    CHECK(rb_digbe_dw_max() == 0x24b0u, "the highest dword is DIG4's");

    /* The aperture relation, not a remembered number: this block has to sit
     * inside what is already mapped or the reads land outside it. */
    CHECK(rb_digbe_dw_max() <= rb_enc_dw_max(),
          "DIG_BE_CNTL is below rb_enc_dw_max(), so no new mapping");
    CHECK(rb_digbe_dw_max() <= rb_dpalt_dw_max(),
          "...and below the dpalt bound that sizes the aperture");

    /* None of these is on the dcn302/dcn312 deny list, with a control so the
     * check cannot be a constant 0 (rule 26). */
    {
        uint32_t i;
        for (i = 0; i < RB_DIGBE_COUNT; i++) {
            CHECK(!rb_dcn_is_denied(rb_digbe_dw(i)),
                  "no DIG_BE_CNTL dword is on the deny list");
        }
        CHECK(rb_dcn_is_denied(0x288bu),
              "control: the deny list is live in this build");
    }

    /* --- the one-hot field, which is NOT an index --------------------- */
    CHECK(rb_digbe_fe_sel(0x00000400u) == RB_DIGBE_FE_DIGC,
          "FE_SEL 0x4 in bits 14:8 decodes as DIGC");
    CHECK(rb_digbe_fe_sel(0x00000100u) == RB_DIGBE_FE_DIGA, "DIGA is 0x1");
    CHECK(rb_digbe_fe_sel(0x00000800u) == RB_DIGBE_FE_DIGD, "DIGD is 0x8");
    CHECK(rb_digbe_fe_sel(0x00000000u) == RB_DIGBE_FE_INVALID,
          "zero is INVALID - unconnected, which is the reading in question");
    CHECK(rb_digbe_fe_sel(0xFFFF80FFu) == 0u,
          "bits outside 14:8 do not leak into the selection");

    /* A one-hot value with two bits set names no front end and must NOT be
     * decoded to the lowest - reading it as an index is 2E1's hpdsel trap
     * with the arrow reversed. */
    CHECK(rb_digbe_fe_one_hot(RB_DIGBE_FE_DIGC), "0x4 is one-hot");
    CHECK(!rb_digbe_fe_one_hot(0u), "0 is not a one-hot selection");
    CHECK(!rb_digbe_fe_one_hot(RB_DIGBE_FE_DIGA | RB_DIGBE_FE_DIGC),
          "two bits set is not a front end this project can name");
    CHECK(!rb_digbe_fe_one_hot(0x7Fu), "every bit set is not one-hot");

    /* --- DIG_MODE, from dcn10_link_encoder_setup's switch ------------- */
    CHECK(rb_digbe_mode(0x00000000u) == RB_DIGBE_MODE_DP_SST,
          "mode 0 is DP/eDP SST, agreeing with 2E1's DP = 0");
    CHECK(rb_digbe_mode(0x00030000u) == RB_DIGBE_MODE_TMDS_HDMI,
          "mode 3 is TMDS-HDMI");
    CHECK(rb_digbe_mode(0x00050000u) == RB_DIGBE_MODE_DP_MST, "mode 5 is MST");
    CHECK(rb_digbe_mode(0xFFF8FFFFu) == 0u,
          "bits outside 18:16 do not leak into the mode");

    /* --- mode validity, where the plausibility guard is NOT subsumed --- */
    CHECK(rb_digbe_mode_valid(0x00000000u), "mode 0 (DP SST) is valid");
    CHECK(rb_digbe_mode_valid(0x00050000u), "mode 5 (DP MST) is valid");
    CHECK(!rb_digbe_mode_valid(0x00040000u),
          "mode 4 is not a mode dcn10_link_encoder_setup ever writes");
    CHECK(!rb_digbe_mode_valid(0x00060000u), "nor is 6");
    CHECK(!rb_digbe_mode_valid(0x00070000u),
          "nor 7, which is what an unmapped block decodes to");
    /* 2E7: all-ones is rejected by the ENUM test, not by a plausibility
     * guard - two breaks proved such a guard dead here, where rb_enc needs
     * one. Kept as coverage of the test that actually rejects it. */
    CHECK(!rb_digbe_mode_valid(0xFFFFFFFFu),
          "an all-ones word decodes to mode 7 and is refused by the enum");

    /* --- the verdict and the survey ----------------------------------- */
    CHECK(!rb_digbe_connected(0x00000000u),
          "a zero word is NOT connected - the step is not done");
    CHECK(rb_digbe_connected(0x00000400u),
          "DIGC selected is connected");
    CHECK(!rb_digbe_connected(0xFFFFFFFFu),
          "an all-ones word is refused because FE_SEL 0x7f is not one-hot - "
          "no separate plausibility guard is needed here, unlike rb_enc");
    CHECK(!rb_digbe_connected(0x00000500u),
          "a multi-bit selection is refused, not decoded to its low bit");

    {
        uint32_t w[RB_DIGBE_COUNT];
        uint32_t i;

        /* The hypothesis 2E7 names: the firmware connected the same pair it
         * configured, instances 0 and 2, which is 2B4/2B9/2E7's mask 0x05. */
        for (i = 0; i < RB_DIGBE_COUNT; i++) { w[i] = 0u; }
        w[0] = 0x00000100u;            /* DIGA */
        w[2] = 0x00000400u;            /* DIGC */
        CHECK(rb_digbe_connected_mask(w) == 0x05u,
              "0 and 2 connected is mask 0x05, the pair three other "
              "instruments already name");

        /* The controls, so the mask cannot pass by being a constant. */
        for (i = 0; i < RB_DIGBE_COUNT; i++) { w[i] = 0u; }
        CHECK(rb_digbe_connected_mask(w) == 0u,
              "five unconnected back ends read 0 - the OTHER outcome, which "
              "leaves 2E1's write to make");
        for (i = 0; i < RB_DIGBE_COUNT; i++) { w[i] = 0x00000100u; }
        CHECK(rb_digbe_connected_mask(w) == 0x1Fu,
              "all five connected reads 0x1f, so 0x05 is not a constant");
        for (i = 0; i < RB_DIGBE_COUNT; i++) { w[i] = 0xFFFFFFFFu; }
        CHECK(rb_digbe_connected_mask(w) == 0u,
              "five unmapped words read 0, NOT 0x1f - an absent key must not "
              "decode as every back end connected (2E6's tool defect)");
        w[2] = 0x00000400u;
        CHECK(rb_digbe_connected_mask(w) == 0x04u,
              "the all-ones refusal does not swallow a real connection");

        CHECK(rb_digbe_connected_mask(NULL) == 0u,
              "a NULL survey is 0, not a read through NULL");
    }

#undef CHECK
    return bad;
}
