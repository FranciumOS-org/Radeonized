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
    CHECK(rb_digbe_dw_max() == 0x24b1u,
          "the highest dword is DIG4's EN_CNTL, not its DIG_BE_CNTL");

    /* regDIG0_DIG_BE_EN_CNTL 0x20b1 / DIG1 0x21b1, dcn_3_1_2_offset.h:9959
     * and :10227 - the same 0x100 stride, verified on two instances rather
     * than assumed from one. */
    CHECK(rb_digbe_en_dw(0) == 0x20b1u, "regDIG0_DIG_BE_EN_CNTL");
    CHECK(rb_digbe_en_dw(1) == 0x21b1u, "regDIG1_DIG_BE_EN_CNTL");
    CHECK(rb_digbe_en_dw(4) == 0x24b1u, "regDIG4_DIG_BE_EN_CNTL");
    CHECK(rb_digbe_en_dw(0) == rb_digbe_dw(0) + 1u,
          "EN_CNTL sits one dword above DIG_BE_CNTL");

    /* The aperture relation, not a remembered number: this block has to sit
     * inside what is already mapped or the reads land outside it. */
    /* ---- boot 447's own five words, the 2E8 reading ----
     * DIG0 0x00000100, DIG1/3/4 0x00010000, DIG2 0x20000000. */
    {
        uint32_t w[RB_DIGBE_COUNT];
        uint32_t p[RB_DIGBE_COUNT];
        uint32_t i;

        w[0] = 0x00000100u; w[1] = 0x00010000u; w[2] = 0x20000000u;
        w[3] = 0x00010000u; w[4] = 0x00010000u;

        CHECK(rb_digbe_fe_sel(w[0]) == RB_DIGBE_FE_DIGA,
              "boot 447: the panel's DIG0 is connected to DIG FE A");
        CHECK(rb_digbe_connected(w[0]) == 1,
              "boot 447: DIG0 connected - the positive control");
        CHECK(rb_digbe_connected(w[2]) == 0,
              "boot 447: DIG2 NOT connected, so 2E1's missing step is real");
        CHECK(rb_digbe_connected_mask(w) == 0x01u,
              "boot 447: only the panel's back end has a front end");

        /* DIG_MODE partitions EXACTLY as 2E7's DP_CONFIG did: DP SST on the
         * two the firmware brought up, LVDS on the three it did not. That is
         * what makes 1 the untouched value rather than 0 - three instances
         * agreeing, cross-checked against another register. */
        CHECK(rb_digbe_mode(w[0]) == RB_DIGBE_MODE_DP_SST, "DIG0 is DP SST");
        CHECK(rb_digbe_mode(w[2]) == RB_DIGBE_MODE_DP_SST, "DIG2 is DP SST");
        CHECK(rb_digbe_mode(w[1]) == RB_DIGBE_MODE_LVDS,
              "DIG1 reads LVDS - the untouched value, not a configuration");
        CHECK(rb_digbe_mode(w[3]) == RB_DIGBE_MODE_LVDS, "DIG3 untouched");
        CHECK(rb_digbe_mode(w[4]) == RB_DIGBE_MODE_LVDS, "DIG4 untouched");
        /* ... and every one of them is a VALID mode, which is the point of
         * rule 14 here: LVDS decodes cleanly on a board with no LVDS, so
         * DIG_MODE alone cannot say whether an instance was configured. */
        for (i = 0u; i < RB_DIGBE_COUNT; i++) {
            CHECK(rb_digbe_mode_valid(w[i]) == 1,
                  "all five decode to a mode Linux writes");
        }

        /* DIG_HPD_SELECT - the field the first decoder skipped, where the
         * rest of the answer was. 0-based DCN instance, so 2 IS the Type-C
         * port named by 2B4, 2B9 and 2E7. */
        CHECK(rb_digbe_hpd_sel(w[2]) == 2u,
              "boot 447: DIG2 is associated with HPD instance 2");
        CHECK(rb_digbe_hpd_sel(w[0]) == 0u,
              "boot 447: DIG0 is HPD instance 0 - the panel");
        CHECK(rb_digbe_hpd_sel(w[1]) == 0u,
              "an unwritten field reads 0, indistinguishable from HPD0");
        /* 2E1's atom hpdsel is this plus one: HPD instance 2 -> hpdsel 3,
         * which is the byte 2E4 actually sent. Pinned so the two
         * conventions cannot drift back together. */
        CHECK(rb_digbe_hpd_sel(w[2]) + 1u == 3u,
              "register field + 1 == the atom hpdsel 2E4 sent");

        /* The diff mask, with a control: identical arrays must give 0, and a
         * single changed instance must name only itself. */
        for (i = 0u; i < RB_DIGBE_COUNT; i++) { p[i] = w[i]; }
        CHECK(rb_digbe_diff_mask(w, p) == 0u,
              "nothing moved -> an empty diff mask");
        p[2] = 0x20000100u;
        CHECK(rb_digbe_diff_mask(w, p) == 0x04u,
              "DIG2 moving names instance 2 and nothing else");
        CHECK(rb_digbe_connected_mask(p) == 0x05u,
              "and that word would make DIG2 connected to DIG FE A");
        CHECK(rb_digbe_diff_mask(NULL, p) == 0u, "NULL is safe");
        CHECK(rb_digbe_diff_mask(w, NULL) == 0u, "NULL is safe");
    }

    /* ---- DIG_BE_EN_CNTL ---- */
    {
        uint32_t e[RB_DIGBE_COUNT];
        uint32_t i;

        for (i = 0u; i < RB_DIGBE_COUNT; i++) { e[i] = 0u; }
        CHECK(rb_digbe_enabled_mask(e) == 0u, "nothing enabled -> 0");

        e[0] = RB_DIGBE_EN_DIG_ENABLE | RB_DIGBE_EN_SYMCLK_ON;
        CHECK(rb_digbe_en_enabled(e[0]) == 1, "DIG_ENABLE is bit 0");
        CHECK(rb_digbe_en_symclk_on(e[0]) == 1, "DIG_SYMCLK_BE_ON is bit 8");
        CHECK(rb_digbe_enabled_mask(e) == 0x01u, "only instance 0 enabled");

        /* The two fields are independent: a back end can be enabled with its
         * symclk off, and nothing may assume the pair moves together. */
        e[2] = RB_DIGBE_EN_DIG_ENABLE;
        CHECK(rb_digbe_en_symclk_on(e[2]) == 0, "enabled with symclk off");
        CHECK(rb_digbe_enabled_mask(e) == 0x05u,
              "the mask 2E4's command would have to produce on instance 2");
        e[3] = RB_DIGBE_EN_SYMCLK_ON;
        CHECK(rb_digbe_en_enabled(e[3]) == 0, "symclk on without DIG_ENABLE");
        CHECK(rb_digbe_enabled_mask(e) == 0x05u,
              "and that does NOT count as enabled");

        /* The all-ones guard is LOAD-BEARING here and was DEAD in the
         * DIG_BE_CNTL decoder - this register has no one-hot or enum test to
         * fall back on, so without it an unmapped block reads as five
         * enabled transmitters (2E6's defect at the tool boundary, in the
         * kext). Its break fires. */
        CHECK(rb_digbe_en_word_plausible(0xFFFFFFFFu) == 0,
              "an all-ones word is not a reading of this register");
        for (i = 0u; i < RB_DIGBE_COUNT; i++) { e[i] = 0xFFFFFFFFu; }
        CHECK(rb_digbe_enabled_mask(e) == 0u,
              "an unmapped block is not five enabled transmitters");
        /* ... with a control proving the guard does not swallow a real
         * reading that sits beside unmapped ones. */
        e[0] = RB_DIGBE_EN_DIG_ENABLE;
        CHECK(rb_digbe_enabled_mask(e) == 0x01u,
              "a real enable beside all-ones words still reads");
        CHECK(rb_digbe_en_word_plausible(0xFFFFFFFEu) == 1,
              "and the guard rejects only all-ones");
        CHECK(rb_digbe_enabled_mask(NULL) == 0u, "NULL is safe");
    }

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

    /* ================================================================= *
     * 2EC: the WRITE. One case per guard, and a break per guard - 2E7
     * found one of this file's own guards dead by breaking it, so a guard
     * that no break can make matter is a result and not a formality.
     * ================================================================= */

    /* The word itself: an OR into the live field, inside an RMW. */
    {
        /* 2E8's own reading of DIG2: FE_SEL 0, DIG_MODE 0 (DP SST),
         * HPD_SELECT 2. Writing DIGC must leave the other two alone. */
        const uint32_t live = 0x20000000u;
        const uint32_t out  = rb_digbe_fe_word(live, RB_DIGBE_FE_DIGC);

        CHECK(rb_digbe_fe_sel(out) == RB_DIGBE_FE_DIGC,
              "DIGC lands in FE_SEL as 0x4, the one-hot - not as index 4");
        CHECK(out == 0x20000400u,
              "the word is boot 447's DIG2 with 0x400 ORed in, nothing else");
        CHECK(rb_digbe_hpd_sel(out) == 2u,
              "HPD_SELECT 2 survives the write - the firmware's own field, "
              "which a REG_SET would have destroyed");
        CHECK(rb_digbe_mode(out) == RB_DIGBE_MODE_DP_SST,
              "DIG_MODE survives too");
        CHECK(rb_digbe_fe_clean(live, out),
              "every bit outside FE_SEL is identical");

        /* It ACCUMULATES, as Linux's |= does. */
        CHECK(rb_digbe_fe_sel(rb_digbe_fe_word(0x20000400u,
                                               RB_DIGBE_FE_DIGB)) == 0x6u,
              "a second front end ORs in - the field accumulates (MST), it "
              "does not replace");
        /* And the panel's live word is untouched by a write of its own FE,
         * which is the idempotence a retry depends on. */
        CHECK(rb_digbe_fe_word(0x00000100u, RB_DIGBE_FE_DIGA) == 0x00000100u,
              "ORing a bit already set is a no-op on the whole word");
    }

    /* The refusals, in the order rb_digbe_fe_refuse() tests them. */
    {
        const uint32_t ok_word = 0x20000000u;   /* boot 447's DIG2 */
        const uint32_t inst = RB_DIGBE_TYPEC_INST;
        const uint32_t fe   = RB_DIGBE_TYPEC_FE;

        CHECK(rb_digbe_fe_refuse(inst, fe, ok_word, 1, 1, 0) == RB_DIGBE_OK,
              "the derived Type-C arm is accepted - the control, so the "
              "refusals below are not a function that always refuses");

        CHECK(rb_digbe_fe_refuse(inst, fe, ok_word, 0, 1, 0)
              == RB_DIGBE_REFUSE_DISARMED, "disarmed refuses");
        CHECK(rb_digbe_fe_refuse(inst, fe, ok_word, 1, 0, 0)
              == RB_DIGBE_REFUSE_NOREGS, "no writable aperture refuses");
        CHECK(rb_digbe_fe_refuse(RB_DIGBE_PANEL_INST, fe, ok_word, 1, 1, 0)
              == RB_DIGBE_REFUSE_PANEL,
              "instance 0 is the live panel's back end and is refused BY "
              "NAME, before any range test can let it through");
        CHECK(rb_digbe_fe_refuse(RB_DIGBE_COUNT, fe, ok_word, 1, 1, 0)
              == RB_DIGBE_REFUSE_INST, "instance 5 is out of range");
        CHECK(rb_digbe_fe_refuse(inst, RB_DIGBE_FE_INVALID, ok_word, 1, 1, 0)
              == RB_DIGBE_REFUSE_FE_ZERO,
              "FE 0 is the documented INVALID, and gets its own code rather "
              "than reading as 'not one-hot'");
        CHECK(rb_digbe_fe_refuse(inst, 0x6u, ok_word, 1, 1, 0)
              == RB_DIGBE_REFUSE_FE_HOT, "two bits name no front end");
        CHECK(rb_digbe_fe_refuse(inst, 0x20u, ok_word, 1, 1, 0)
              == RB_DIGBE_REFUSE_FE_RANGE,
              "a sixth front end: one-hot but this part has five "
              "(num_stream_encoder = 5)");
        CHECK(rb_digbe_fe_refuse(inst, RB_DIGBE_PANEL_FE, ok_word, 1, 1, 0)
              == RB_DIGBE_REFUSE_FE_PANEL,
              "DIGA is the panel's own front end (2E8: FE_SEL 0x1 on DIG0) "
              "and the field ACCUMULATES, so ORing it onto DIG2 would put "
              "the live desktop's stream encoder on a second back end");
        CHECK(rb_digbe_fe_refuse(inst, RB_DIGBE_FE_DIGE, ok_word, 1, 1, 0)
              == RB_DIGBE_OK,
              "DIGE is legal - the refusal above is about DIGA, not about "
              "'any front end but DIGC'");

        /* The one that is new in this header: check the target BEFORE
         * writing it. 2DD read a plausible wrong address; this would WRITE
         * one. */
        CHECK(rb_digbe_fe_refuse(inst, fe, 0xFFFFFFFFu, 1, 1, 0)
              == RB_DIGBE_REFUSE_WORD,
              "an all-ones live word is an unmapped block, not DIG_BE_CNTL: "
              "refused before the store, not decoded after it");
        CHECK(rb_digbe_fe_refuse(inst, fe, 0x00040000u, 1, 1, 0)
              == RB_DIGBE_REFUSE_WORD,
              "DIG_MODE 4 is not a mode dcn10_link_encoder_setup writes, so "
              "the dword is not this register");
        CHECK(rb_digbe_fe_refuse(inst, fe, 0x00010000u, 1, 1, 0)
              == RB_DIGBE_OK,
              "DIG_MODE 1 (LVDS) is a value this part really reads on the "
              "three dark instances, so the guard must not reject it");

        CHECK(rb_digbe_fe_refuse(inst, fe, ok_word, 1, 1, 1)
              == RB_DIGBE_REFUSE_DENIED, "a denied dword refuses");
    }

    /* The readback, and why `applied` needs `changed` beside it. */
    {
        CHECK(rb_digbe_fe_applied(0x20000400u, RB_DIGBE_FE_DIGC),
              "DIGC set in the readback is applied");
        CHECK(!rb_digbe_fe_applied(0x20000000u, RB_DIGBE_FE_DIGC),
              "FE_SEL 0 is not applied");
        CHECK(!rb_digbe_fe_applied(0x20000200u, RB_DIGBE_FE_DIGC),
              "DIGB set is not DIGC applied - the bit is tested, not "
              "non-zero-ness");
        CHECK(rb_digbe_fe_changed(0x20000000u, 0x20000400u),
              "the word moved: this is the half that is evidence of a store");
        CHECK(!rb_digbe_fe_changed(0x20000400u, 0x20000400u),
              "a word that did not move did not move, whatever `applied` "
              "says - 2E6's defect, where before == after == target made "
              "applied 1 with no control");
        /* Both together: the pair a boot must read. On DIG2 the before is 0
         * (2E8), so a genuine write is applied AND changed, and applied
         * without changed means the connection was already there. */
        CHECK(rb_digbe_fe_applied(0x20000400u, RB_DIGBE_FE_DIGC)
              && !rb_digbe_fe_changed(0x20000400u, 0x20000400u),
              "applied-but-unchanged is expressible, which is the whole "
              "reason there are two keys");

        CHECK(!rb_digbe_fe_clean(0x20000000u, 0x30000400u),
              "a store that moved HPD_SELECT is NOT clean - the check can "
              "fail, so it is not a constant 1");
    }

#undef CHECK
    return bad;
}
