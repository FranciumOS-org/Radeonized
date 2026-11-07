/* SPDX-License-Identifier: MIT */
/* 2F4: the DP transmitter's link block - the PHY side of link training.
 *
 * What these cases exist to stop, in order of how much a boot would cost:
 *
 *   1. CONFLATING THE TWO PATTERN ENCODINGS. The hardware register takes
 *      0..3 for TPS1..TPS4; DPCD 0x102 takes 1, 2, 3, 7. Getting one wrong
 *      sets a DIFFERENT pattern on each side of the link, which looks exactly
 *      like a sink that cannot achieve clock recovery - a wrong answer that
 *      would be believed.
 *   2. Writing with the block UNIDENTIFIED. The reference is the panel's own
 *      instance, which the firmware trained and which is scanning out now.
 *   3. Touching the panel's transmitter at all.
 *   4. Disagreeing with rb_enc.h about a block they share.
 */
#include <stdio.h>
#include <stdint.h>
#include "rb_dptx.h"
#include "rb_enc.h"
#include "rb_dpcd.h"

int dptx_tests(void);

int dptx_tests(void)
{
    int bad = 0;
#define CHECK(cond, msg) do { if (!(cond)) { \
        printf("FAIL dptx: %s\n", (msg)); bad++; } } while (0)

    /* ---------------- the offsets, against dcn_3_1_2_offset.h ---------- */
    CHECK(rb_dptx_dw(0, RB_DPTX_REG_LINK_CNTL) == 0x2108u,
          "DP0_DP_LINK_CNTL is 0x2108");
    CHECK(rb_dptx_dw(0, RB_DPTX_REG_VID_STREAM) == 0x210cu,
          "DP0_DP_VID_STREAM_CNTL is 0x210c");
    CHECK(rb_dptx_dw(0, RB_DPTX_REG_DPHY_CNTL) == 0x2117u,
          "DP0_DP_DPHY_CNTL is 0x2117");
    CHECK(rb_dptx_dw(0, RB_DPTX_REG_TP_SEL) == 0x2118u,
          "DP0_DP_DPHY_TRAINING_PATTERN_SEL is 0x2118");
    CHECK(rb_dptx_dw(0, RB_DPTX_REG_PRBS_CNTL) == 0x211du,
          "DP0_DP_DPHY_PRBS_CNTL is 0x211d");
    /* The instance this project cares about, spelled out: DIG2 is the Type-C
     * port's (four instruments agree - 2B4/2B9/2E8/2E1). */
    CHECK(rb_dptx_dw(2, RB_DPTX_REG_TP_SEL) == 0x2318u,
          "DP2's training pattern register is 0x2318 - the stride is 0x100");
    CHECK(rb_dptx_dw(4, RB_DPTX_REG_LINK_CNTL) == 0x2508u,
          "and the fifth instance's LINK_CNTL is 0x2508");
    /* Rule 19: out of range refuses with a value that reads nothing rather
     * than indexing into some other block. */
    CHECK(rb_dptx_dw(RB_DPTX_COUNT, RB_DPTX_REG_TP_SEL) == RB_DPTX_DW_INVALID,
          "an out-of-range instance refuses");
    CHECK(rb_dptx_dw(0, RB_DPTX_REG_COUNT) == RB_DPTX_DW_INVALID,
          "and so does an out-of-range register");

    /* ---------------- THE SHARED BLOCK -------------------------------
     *
     * rb_enc.h owns two registers of this same block. A second opinion about
     * the stride or either offset would be a bug, so the two headers are
     * checked against each other rather than each against a comment. */
    CHECK(RB_DPTX_DW_DP_CONFIG_ENC == RB_ENC_DW_DP_CONFIG_0,
          "rb_dptx and rb_enc agree on DP_CONFIG's offset");
    CHECK(RB_DPTX_DW_SCRAM_ENC == RB_ENC_DW_SCRAM_0,
          "and on DP_DPHY_SCRAM_CNTL's");
    CHECK(RB_DPTX_DW_STRIDE == RB_ENC_DW_STRIDE &&
          RB_DPTX_COUNT == RB_ENC_COUNT,
          "and on the stride and the instance count");

    /* THE APERTURE, discharged by a measurement that already happened: 2E7
     * read rb_enc's SCRAM_CNTL on all five instances off this chip, and that
     * is the HIGHEST dword of this block. So every dword 2F4 touches is
     * inside a range already proven mapped - not merely assumed to be. */
    CHECK(rb_dptx_dw_max() < rb_enc_dw_max(),
          "every dword this block touches is below the highest one 2E7 "
          "already read on hardware");
    CHECK(rb_dptx_dw_max() == 0x251du,
          "and that highest dword is 0x251d, as the header says");

    /* ---------------- THE TWO PATTERN ENCODINGS ----------------------
     *
     * The case that would otherwise cost a boot and be misread as a sink
     * failure. Asserted in BOTH directions and then asserted to DIFFER. */
    CHECK(rb_dptx_hw_pattern(RB_DPTX_TPS1) == 0u &&
          rb_dptx_hw_pattern(RB_DPTX_TPS2) == 1u &&
          rb_dptx_hw_pattern(RB_DPTX_TPS3) == 2u &&
          rb_dptx_hw_pattern(RB_DPTX_TPS4) == 3u,
          "the hardware register takes 0..3 for TPS1..TPS4");
    CHECK(rb_dptx_dpcd_pattern(RB_DPTX_TPS1) == 1u &&
          rb_dptx_dpcd_pattern(RB_DPTX_TPS2) == 2u &&
          rb_dptx_dpcd_pattern(RB_DPTX_TPS3) == 3u &&
          rb_dptx_dpcd_pattern(RB_DPTX_TPS4) == 7u,
          "DPCD 0x102 takes 1, 2, 3, 7 - and TPS4 is 7, not 4");
    {
        uint32_t t;
        for (t = RB_DPTX_TPS1; t <= RB_DPTX_TPS4; t++) {
            CHECK(rb_dptx_hw_pattern(t) != rb_dptx_dpcd_pattern(t),
                  "the two encodings DIFFER for every pattern, so one can "
                  "never be substituted for the other");
        }
    }
    /* DPCD 0 means training off, and the hardware's 0 means TPS1. The two
     * zeros mean opposite things, which is the sharpest form of the trap. */
    CHECK(rb_dptx_dpcd_pattern(0u) == 0u &&
          rb_dptx_hw_pattern(RB_DPTX_TPS1) == 0u,
          "DPCD 0 is training OFF while the hardware's 0 is TPS1 - the two "
          "zeros mean opposite things");
    CHECK(RB_DPCD_TRAIN_PATTERN_DISABLE == 0u &&
          RB_DPCD_TRAIN_PATTERN_1 == rb_dptx_dpcd_pattern(RB_DPTX_TPS1),
          "and rb_dpcd.h's own names agree with the conversion");
    /* A bad pattern number must not fall through as TPS1. */
    CHECK(!rb_dptx_tps_valid(0u) && !rb_dptx_tps_valid(5u) &&
          rb_dptx_tps_valid(RB_DPTX_TPS1) && rb_dptx_tps_valid(RB_DPTX_TPS4),
          "0 and 5 are not patterns; 1 and 4 are");

    /* ---------------- the decoders ------------------------------------ */
    CHECK(rb_dptx_trained(RB_DPTX_TRAINING_COMPLETE) &&
          !rb_dptx_trained(0u),
          "TRAINING_COMPLETE is bit 4");
    CHECK(rb_dptx_link_status(RB_DPTX_LINK_STATUS) &&
          !rb_dptx_link_status(RB_DPTX_TRAINING_COMPLETE),
          "LINK_STATUS is bit 8 and is a DIFFERENT bit from "
          "TRAINING_COMPLETE - the one we write and the one we do not");
    CHECK(rb_dptx_bypass(RB_DPTX_DPHY_BYPASS) && !rb_dptx_bypass(0u),
          "DPHY_BYPASS is bit 16");
    CHECK(rb_dptx_prbs_on(RB_DPTX_PRBS_EN) && !rb_dptx_prbs_on(0u),
          "PRBS_EN is bit 0");
    CHECK(rb_dptx_pattern(0xfffffffcu | 0x2u) == 0x2u,
          "the pattern field is the low two bits only");

    /* ---------------- THE REFERENCE CHECK ---------------------------- */
    {
        const uint32_t trained = RB_DPTX_TRAINING_COMPLETE |
                                 RB_DPTX_LINK_STATUS;
        CHECK(rb_dptx_ref_trained(trained),
              "a word with both TRAINING_COMPLETE and LINK_STATUS is a "
              "trained link");
        /* BOTH bits are required, and each alone is checked, because a
         * conjunction can be correct while one term carries no information
         * (rule 48). */
        CHECK(!rb_dptx_ref_trained(RB_DPTX_TRAINING_COMPLETE),
              "TRAINING_COMPLETE alone is not enough - that bit is one a "
              "driver writes, so it can be set with no link");
        CHECK(!rb_dptx_ref_trained(RB_DPTX_LINK_STATUS),
              "and LINK_STATUS alone is not enough either");
        CHECK(!rb_dptx_ref_trained(0u), "and zero is not a trained link");
    }

    /* ---------------- THE REFUSALS, each for its own reason ----------- */
    {
        const uint32_t ref = RB_DPTX_TRAINING_COMPLETE | RB_DPTX_LINK_STATUS;
        const uint32_t dark = 0u;          /* no video stream on that inst */

        CHECK(rb_dptx_write_refuse(2u, RB_DPTX_TPS1, ref, dark, 1, 1)
              == RB_DPTX_W_OK,
              "TPS1 on DIG2 with the panel trained and no stream is allowed "
              "- the control for every refusal below");
        CHECK(rb_dptx_write_refuse(2u, RB_DPTX_TPS1, ref, dark, 0, 1)
              == RB_DPTX_W_NOT_ARMED, "unarmed is refused");
        CHECK(rb_dptx_write_refuse(2u, RB_DPTX_TPS1, ref, dark, 1, 0)
              == RB_DPTX_W_NO_REGS, "no register mapping is refused");
        CHECK(rb_dptx_write_refuse(RB_DPTX_PANEL_INST, RB_DPTX_TPS1, ref,
                                   dark, 1, 1) == RB_DPTX_W_PANEL_INST,
              "the PANEL's transmitter is refused by name");
        /* ...and by name even when the reference is also bad, so it is
         * refused for being the panel and not incidentally. */
        CHECK(rb_dptx_write_refuse(RB_DPTX_PANEL_INST, RB_DPTX_TPS1, 0u,
                                   dark, 1, 1) == RB_DPTX_W_PANEL_INST,
              "and is refused AS THE PANEL even when the reference check "
              "would also have failed");
        CHECK(rb_dptx_write_refuse(RB_DPTX_COUNT, RB_DPTX_TPS1, ref, dark,
                                   1, 1) == RB_DPTX_W_INST_RANGE,
              "an out-of-range instance is refused");
        CHECK(rb_dptx_write_refuse(2u, RB_DPTX_TPS1, 0u, dark, 1, 1)
              == RB_DPTX_W_REF_UNTRAINED,
              "an UNIDENTIFIED block is refused - rule 47 asked of a write");
        CHECK(rb_dptx_write_refuse(2u, 0u, ref, dark, 1, 1)
              == RB_DPTX_W_TPS_RANGE,
              "a pattern number that is not TPS1..TPS4 is refused, and 0 in "
              "particular is NOT taken as TPS1");
        CHECK(rb_dptx_write_refuse(2u, RB_DPTX_TPS1, ref,
                                   RB_DPTX_VID_STREAM_ENABLE, 1, 1)
              == RB_DPTX_W_STREAM_LIVE,
              "a transmitter with a VIDEO STREAM running is refused - "
              "something else owns it");
    }

    /* ---------------- THE THREE READ-MODIFY-WRITES ------------------- */
    {
        /* A word with every neighbouring field set, so a clobber shows. */
        const uint32_t lc = 0xffffffffu;
        const uint32_t out = rb_dptx_link_cntl_word(lc);
        CHECK(!(out & RB_DPTX_TRAINING_COMPLETE),
              "the LINK_CNTL write clears TRAINING_COMPLETE");
        CHECK((out | RB_DPTX_TRAINING_COMPLETE) == lc,
              "and clears NOTHING else - LINK_STATUS and "
              "EMBEDDED_PANEL_MODE survive");
        CHECK(rb_dptx_link_status(out) && rb_dptx_embedded(out),
              "named explicitly: the status bit we must not disturb is still "
              "set after our own write");

        const uint32_t dc = 0xffffffffu;
        const uint32_t dout = rb_dptx_dphy_cntl_word(dc);
        CHECK(!(dout & RB_DPTX_DPHY_BYPASS) &&
              (dout | RB_DPTX_DPHY_BYPASS) == dc,
              "the DPHY_CNTL write clears BYPASS and nothing else - FEC_EN "
              "and the scrambler select survive");
        CHECK((dout & RB_DPTX_DPHY_FEC_EN) &&
              (dout & RB_DPTX_DPHY_SCRAMBLER_SEL),
              "named explicitly, because 2EC's lesson was a neighbour");

        const uint32_t pc = 0xffffffffu;
        const uint32_t pout = rb_dptx_prbs_cntl_word(pc);
        CHECK(!(pout & RB_DPTX_PRBS_EN) && (pout | RB_DPTX_PRBS_EN) == pc,
              "the PRBS_CNTL write clears PRBS_EN and leaves PRBS_SEL");
        CHECK((pout & RB_DPTX_PRBS_SEL_MASK) == RB_DPTX_PRBS_SEL_MASK,
              "PRBS_SEL in particular, which shares the register");

        /* And the clean check itself, both ways. */
        CHECK(rb_dptx_rmw_clean(0xffffffffu, out, RB_DPTX_TRAINING_COMPLETE),
              "rb_dptx_rmw_clean accepts a write that moved only its field");
        CHECK(!rb_dptx_rmw_clean(0xffffffffu, out & ~RB_DPTX_LINK_STATUS,
                                 RB_DPTX_TRAINING_COMPLETE),
              "and REJECTS one that also cleared a neighbour");
    }

    /* ---------------- THE SINK-SIDE VERDICT ------------------------- */
    {
        /* drm_dp.h's layout: lane 0 low nibble, lane 1 high nibble. */
        CHECK(rb_dptx_lane_status(0x71u, 0u) == 0x1u,
              "lane 0's status is the low nibble");
        CHECK(rb_dptx_lane_status(0x71u, 1u) == 0x7u,
              "and lane 1's is the high nibble - a byte where the two "
              "DIFFER, so the shift is pinned");
        /* This sink is TWO lanes (2BB read its DPCD). */
        CHECK(rb_dptx_cr_done(0x11u, 2u),
              "CR_DONE on both lanes of a 2-lane link");
        CHECK(!rb_dptx_cr_done(0x01u, 2u),
              "and NOT when only lane 0 has it - the second lane is really "
              "checked");
        CHECK(!rb_dptx_cr_done(0x10u, 2u),
              "nor when only lane 1 has it");
        CHECK(rb_dptx_cr_done(0x01u, 1u),
              "while a 1-lane link needs only lane 0");
        CHECK(!rb_dptx_cr_done(0xffu, 0u),
              "zero lanes is not 'all lanes done' - a vacuous truth would "
              "report CR on a link with no lanes (rule 14)");
        CHECK(!rb_dptx_cr_done(0xffu, 3u),
              "and one status byte cannot answer for three lanes");
        /* EQ and SYMBOL_LOCKED are NOT part of CR - the CR phase checks one
         * bit and a conjunction of all three would never pass at that
         * stage. */
        CHECK(rb_dptx_cr_done(0x11u, 2u) &&
              !(0x11u & (RB_DPTX_LANE_EQ_DONE | RB_DPTX_LANE_SYM_LOCKED)),
              "CR_DONE is satisfied with EQ and SYMBOL_LOCKED both clear, "
              "which is exactly the state the CR phase ends in");

        CHECK(rb_dptx_verdict(0, 1, 1, 1, 0u, 0x11u, 2u)
              == RB_DPTX_V_NO_STORE,
              "if our own registers did not take, the verdict is NO_STORE "
              "whatever the sink says");
        CHECK(rb_dptx_verdict(1, 0, 1, 1, 0u, 0x11u, 2u)
              == RB_DPTX_V_NO_STORE,
              "and an UNCLEAN write is NO_STORE too - a write that disturbed "
              "a neighbour is not a success");
        CHECK(rb_dptx_verdict(1, 1, 1, 1, 0u, 0x11u, 2u)
              == RB_DPTX_V_CR_DONE,
              "clean, applied, something MOVED and CR on every lane is "
              "CR_DONE");
        CHECK(rb_dptx_verdict(1, 1, 1, 1, 0u, 0x44u, 2u)
              == RB_DPTX_V_SINK_MOVED,
              "a sink status that CHANGED without CR is SINK_MOVED - which "
              "is still evidence the pattern reached it");
        CHECK(rb_dptx_verdict(1, 1, 1, 1, 0x44u, 0x44u, 2u)
              == RB_DPTX_V_PATTERN_SET,
              "an unchanged sink status is PATTERN_SET - our side only");

        /* ---- 2F5: the two readings boot 458 was reported as PATTERN_SET -- */
        CHECK(rb_dptx_verdict(1, 1, 1, 0, 0u, 0x11u, 2u)
              == RB_DPTX_V_SINK_BLIND,
              "a FAILED sink read is SINK_BLIND, NOT the expected reading - "
              "2F4 returned PATTERN_SET here and boot 458's after-read DID "
              "fail, so the run's own instrument failure printed as its "
              "expected outcome");
        CHECK(rb_dptx_verdict(1, 1, 0, 1, 0u, 0x11u, 2u)
              == RB_DPTX_V_NO_OP,
              "and a write where NOT ONE BIT MOVED is NO_OP even with CR "
              "done on every lane - rule 17: a write that wrote nothing "
              "cannot be credited with the sink's state");
        CHECK(rb_dptx_verdict(0, 1, 0, 1, 0u, 0x11u, 2u)
              == RB_DPTX_V_NO_STORE,
              "NO_STORE still outranks NO_OP: registers that did not take "
              "is the worse fact");

        /* ---- 2F5: boot 458's OWN four words, verbatim ------------------- */
        {
            /* dptrain go, boot 458, from ioreg: every before == after. */
            const uint32_t tp_b = 0x00000000u, tp_a = 0x00000000u;
            const uint32_t lc_b = 0x00000000u, lc_a = 0x00000000u;
            const uint32_t dc_b = 0x00000000u, dc_a = 0x00000000u;
            const uint32_t pc_b = 0x7fffff00u, pc_a = 0x7fffff00u;

            CHECK(!rb_dptx_moved(tp_b, tp_a, lc_b, lc_a, dc_b, dc_a,
                                 pc_b, pc_a),
                  "boot 458's four measured word pairs moved NOTHING");
            CHECK(rb_dptx_pattern(tp_a) == rb_dptx_hw_pattern(RB_DPTX_TPS1) &&
                  !rb_dptx_trained(lc_a) && !rb_dptx_bypass(dc_a) &&
                  !rb_dptx_prbs_on(pc_a),
                  "and `applied` was nonetheless HONEST on all four - every "
                  "target state was already present on a dark transmitter, "
                  "which is precisely the rule-20 defect");
            CHECK(!rb_dptx_pattern_observable(RB_DPTX_TPS1, tp_b),
                  "TPS1 is UNOBSERVABLE in a TP_SEL that reads 0, because "
                  "TPS1 encodes as hardware pattern 0 - the one pattern the "
                  "dial defaulted to");
            CHECK(rb_dptx_pattern_observable(RB_DPTX_TPS2, tp_b) &&
                  rb_dptx_pattern_observable(RB_DPTX_TPS3, tp_b) &&
                  rb_dptx_pattern_observable(RB_DPTX_TPS4, tp_b),
                  "TPS2/3/4 encode as 1/2/3 and ARE observable against the "
                  "same dark register");
            CHECK(!rb_dptx_pattern_observable(RB_DPTX_TPS2, 0x1u),
                  "and observability is against the LIVE word, not the "
                  "pattern: TPS2 into a TP_SEL already reading 1 is a no-op "
                  "too");
            CHECK(!rb_dptx_pattern_observable(0u, tp_b) &&
                  !rb_dptx_pattern_observable(5u, tp_b),
                  "an invalid TPS is never observable, whatever the live "
                  "word");
        }

        /* `moved` fires on each of the four independently. */
        CHECK(rb_dptx_moved(0u, 1u, 0u, 0u, 0u, 0u, 0u, 0u) &&
              rb_dptx_moved(0u, 0u, 0u, 1u, 0u, 0u, 0u, 0u) &&
              rb_dptx_moved(0u, 0u, 0u, 0u, 0u, 1u, 0u, 0u) &&
              rb_dptx_moved(0u, 0u, 0u, 0u, 0u, 0u, 0u, 1u),
              "a change in ANY of the four registers alone is `moved`");
    }

#undef CHECK
    printf("dptx: 2F4's DP link block - the offsets against rb_enc's shared "
           "ones, the two pattern encodings, the panel reference, the "
           "refusals, the three RMWs, the sink-side verdict and 2F5's "
           "moved/observable terms%s\n",
           bad ? " FAILED" : ": passed");
    return bad;
}
