/* SPDX-License-Identifier: MIT */
/* 2E5: the DIO link encoder's two pre-transmitter writes.
 *
 * The cases that matter are the ones a transcription gets wrong:
 *  - DP_CONFIG is a REG_SET with init 0 (the whole word), the scrambler is a
 *    REG_UPDATE (an RMW). Writing the scrambler the first way destroys three
 *    fields, so the two must NOT share a helper.
 *  - `lane_count - LANE_COUNT_ONE` with LANE_COUNT_ONE = 1, so two lanes
 *    write 1 and not 2.
 *  - instance 2 is DP2_* at 0x230b/0x231e, which is UNIPHY_C, which is 2E1's
 *    Type-C transmitter. An off-by-one here writes the panel's encoder or a
 *    neighbour's.
 *  - rb_enc_dw_max() must stay inside the aperture 2C0 already checks, or a
 *    write lands outside the mapping - and 2DD is what a READABLE wrong
 *    address costs when nothing is checking.
 */
#include <stdio.h>
#include <stdint.h>
#include "rb_enc.h"
#include "rb_dpalt.h"
#include "rb_dig.h"
#include "rb_dcn_deny.h"

int enc_tests(void);

int enc_tests(void)
{
    int bad = 0;

#define CHECK(c, msg) do { \
    if (!(c)) { printf("FAIL enc: %s\n", (msg)); bad++; } \
} while (0)

    /* --- the offsets, against dcn_3_1_2_offset.h ----------------------- */
    CHECK(rb_enc_dw_config(0) == 0x210bu, "regDP0_DP_CONFIG is 0x210b");
    CHECK(rb_enc_dw_config(1) == 0x220bu, "regDP1_DP_CONFIG is 0x220b");
    CHECK(rb_enc_dw_config(2) == 0x230bu, "regDP2_DP_CONFIG is 0x230b");
    CHECK(rb_enc_dw_config(3) == 0x240bu, "regDP3_DP_CONFIG is 0x240b");
    CHECK(rb_enc_dw_config(4) == 0x250bu, "regDP4_DP_CONFIG is 0x250b");
    CHECK(rb_enc_dw_scram(0) == 0x211eu, "regDP0_DP_DPHY_SCRAM_CNTL is 0x211e");
    CHECK(rb_enc_dw_scram(2) == 0x231eu, "regDP2_DP_DPHY_SCRAM_CNTL is 0x231e");
    CHECK(rb_enc_dw_scram(4) == 0x251eu, "regDP4_DP_DPHY_SCRAM_CNTL is 0x251e");
    CHECK(RB_ENC_COUNT == 5u, "five DIO link encoders, link_enc_regs[0..4]");

    /* The Type-C instance IS the phyid rb_dig.h sends, from link_regs(2, C).
     * These two headers are derived from different files - the resource file's
     * table and the ROM parse - so their agreement is a real check. */
    CHECK(RB_DIG_TYPEC_PHYID == 2u,
          "2E1's Type-C transmitter is phyid 2 (TRANSMITTER_UNIPHY_C)");
    CHECK(rb_enc_dw_config(RB_DIG_TYPEC_PHYID) == 0x230bu,
          "link encoder for the Type-C phyid is DP2_*, because link_regs() "
          "indexes LE_DCN31_REG_LIST(id) and DPCS_DCN31_REG_LIST(id) with "
          "the SAME id - one index, four names");

    /* --- the aperture relation ----------------------------------------- */
    CHECK(rb_enc_dw_max() == 0x251eu, "the highest dword is DP4's scrambler");
    CHECK(rb_enc_dw_max() <= rb_dpalt_dw_max(),
          "every encoder dword is inside the aperture 2C0 already checks "
          "separately (RDPCSTX4 at 0x2ca6), so this adds no mapping - if "
          "either constant moves, THIS is what catches it");

    /* --- the deny list -------------------------------------------------- */
    {
        unsigned i;
        int anyDenied = 0;
        for (i = 0; i < RB_ENC_COUNT; i++) {
            if (rb_dcn_is_denied(rb_enc_dw_config(i))) { anyDenied = 1; }
            if (rb_dcn_is_denied(rb_enc_dw_scram(i)))  { anyDenied = 1; }
        }
        CHECK(!anyDenied,
              "no encoder dword is on the dcn302/dcn312 danger list");
        /* And the list is not vacuous - the same check that passed above has
         * to be able to fail (rule 26 on a search). */
        CHECK(rb_dcn_is_denied(0x0288bu),
              "the deny list is live: 0x288b IS denied");
        CHECK(rb_dcn_is_denied(0x020e5u),
              "DIG0_DIG_LANE_ENABLE is denied - a DIG dword near ours that "
              "the list does catch, so the pass above is not a dead lookup");
    }

    /* --- DP_CONFIG: the whole word, and the -1 ------------------------- */
    CHECK(rb_enc_dp_config_word(2u) == 1u,
          "two lanes write 1: lane_count - LANE_COUNT_ONE, and "
          "LANE_COUNT_ONE is 1 (dc_dp_types.h:34) - NOT 2");
    CHECK(rb_enc_dp_config_word(1u) == 0u, "one lane writes 0");
    CHECK(rb_enc_dp_config_word(4u) == 3u, "four lanes write 3");
    CHECK((rb_enc_dp_config_word(4u) & ~RB_ENC_UDI_LANES_MASK) == 0u,
          "the word never sets a bit outside DP_UDI_LANES - DP_CONFIG has "
          "exactly one field, which is the only reason REG_SET's zeroing of "
          "'everything else' is harmless here");

    /* --- the scrambler: an RMW that preserves the other three fields --- */
    {
        /* BS_COUNT 0x3ff00 and KCODE 0x1000000 both set, ADVANCE clear. */
        const uint32_t live = 0x0103FF00u;
        const uint32_t out = rb_enc_scram_word(live);

        CHECK((out & RB_ENC_SCRAM_ADVANCE) != 0u,
              "DPHY_SCRAMBLER_ADVANCE (bit 4) is set");
        CHECK((out & 0x0003FF00u) == 0x0003FF00u,
              "DPHY_SCRAMBLER_BS_COUNT is PRESERVED - this is REG_UPDATE, an "
              "RMW, and DP_DPHY_SCRAM_CNTL has four fields");
        CHECK((out & 0x01000000u) == 0x01000000u,
              "DPHY_SCRAMBLER_KCODE is preserved");
        CHECK(out == (live | RB_ENC_SCRAM_ADVANCE),
              "and nothing else changes");
        CHECK(rb_enc_scram_word(out) == out,
              "idempotent: configuring twice is not a different write");
        /* The mistake this guards: using the DP_CONFIG helper's shape here. */
        CHECK(out != RB_ENC_SCRAM_ADVANCE,
              "the scrambler is NOT written as a bare field value - that "
              "would zero BS_COUNT and KCODE");
    }

    /* --- refusals, each on its own case -------------------------------- */
    CHECK(rb_enc_refuse(2u, 2u, 1, 1, 0) == RB_ENC_OK,
          "the Type-C instance with two lanes, armed, mapped: OK");
    CHECK(rb_enc_refuse(2u, 2u, 0, 1, 0) == RB_ENC_REFUSE_DISARMED,
          "disarmed refuses first");
    CHECK(rb_enc_refuse(2u, 2u, 1, 0, 0) == RB_ENC_REFUSE_NOREGS,
          "no aperture refuses before any instance arithmetic");
    CHECK(rb_enc_refuse(0u, 2u, 1, 1, 0) == RB_ENC_REFUSE_PANEL,
          "instance 0 is the panel's own link encoder and is refused BY "
          "NAME, as rb_dig.h refuses PHY0 and rb_pg.h refuses DOMAIN0");
    CHECK(rb_enc_refuse(5u, 2u, 1, 1, 0) == RB_ENC_REFUSE_INST,
          "instance 5 is past link_enc_regs[]");
    CHECK(rb_enc_refuse(2u, 3u, 1, 1, 0) == RB_ENC_REFUSE_LANES,
          "three lanes is not a DP lane count");
    CHECK(rb_enc_refuse(2u, 0u, 1, 1, 0) == RB_ENC_REFUSE_LANES,
          "zero lanes is refused, not treated as a default");
    CHECK(rb_enc_refuse(2u, 2u, 1, 1, 1) == RB_ENC_REFUSE_DENIED,
          "a denied dword refuses even when everything else is in order");

    /* Codes do not collide with the neighbouring tables, so a code read
     * against the wrong one cannot decode plausibly. */
    CHECK(RB_ENC_REFUSE_DISARMED > RB_DIG_REFUSE_HPDSEL,
          "rb_enc codes start above rb_dig's 30-37");

    /* --- the readback decides ------------------------------------------ */
    CHECK(rb_enc_applied(1u, RB_ENC_SCRAM_ADVANCE, 2u),
          "lanes read back as 1 and the advance bit set: applied");
    CHECK(!rb_enc_applied(0u, RB_ENC_SCRAM_ADVANCE, 2u),
          "the lane field reading 0 when 1 was asked for is NOT applied");
    CHECK(!rb_enc_applied(1u, 0u, 2u),
          "the advance bit clear is NOT applied, even with the lanes right - "
          "both writes have to have taken, since either alone is a half "
          "configure whose null would be unattributable");
    CHECK(rb_enc_applied(1u | 0x0103FF00u, 0x0103FF10u, 2u),
          "the other fields of either register do not affect the verdict");
    CHECK(!rb_enc_applied(0xFFFFFFFFu, 0xFFFFFFFFu, 1u),
          "an all-ones read - the signature of an unmapped or powered-down "
          "block - does not pass for one lane");

    /* --- 2E6: the change bit, which `applied` cannot supply ------------ */
    /* Boot 444's own words: before == after == target. */
    CHECK(rb_enc_applied(1u, 0x0101FF10u, 2u),
          "boot 444's AFTER words read as applied");
    CHECK(!rb_enc_changed(1u, 1u, 0x0101FF10u, 0x0101FF10u),
          "boot 444: applied AND unchanged - the encoder was already in the "
          "target state, so `applied` says nothing about our write");
    CHECK(rb_enc_changed(0u, 1u, 0x0101FF00u, 0x0101FF10u),
          "a write that moved both registers reads as changed");
    CHECK(rb_enc_changed(0u, 1u, 0x0101FF10u, 0x0101FF10u),
          "either register moving on its own is a change");
    CHECK(rb_enc_changed(1u, 1u, 0x0101FF00u, 0x0101FF10u),
          "...in either order");

    /* --- 2E6: the survey ------------------------------------------------ */
    {
        uint32_t cfg[RB_ENC_COUNT], scr[RB_ENC_COUNT];
        unsigned i;

        /* Boot 444 read instance 2 as 0x1 / 0x0101FF10. The hypothesis under
         * test is the firmware having brought up exactly instances 0 and 2 -
         * 2B4's HPD sink mask and 2B9's AUX_EN mask, both 0x05. */
        for (i = 0; i < RB_ENC_COUNT; i++) { cfg[i] = 0u; scr[i] = 0u; }
        cfg[0] = 1u; scr[0] = 0x0101FF10u;
        cfg[2] = 1u; scr[2] = 0x0101FF10u;
        CHECK(rb_enc_configured_mask(cfg, scr) == 0x05u,
              "instances 0 and 2 configured is mask 0x05 - the same pair "
              "2B4's HPD and 2B9's AUX_EN masks name");

        /* The control: the mask is not a constant. All five alike - the
         * reset-default outcome - must read 0x1f, and all five untouched 0. */
        for (i = 0; i < RB_ENC_COUNT; i++) {
            cfg[i] = 1u; scr[i] = 0x0101FF10u;
        }
        CHECK(rb_enc_configured_mask(cfg, scr) == 0x1Fu,
              "all five alike reads 0x1f, so the 0x05 case is not a constant");
        for (i = 0; i < RB_ENC_COUNT; i++) { cfg[i] = 0u; scr[i] = 0u; }
        CHECK(rb_enc_configured_mask(cfg, scr) == 0u,
              "five untouched encoders read 0");

        /* "Configured" is the CONJUNCTION, and each half alone is not it. */
        CHECK(!rb_enc_looks_configured(0u, 0x0101FF10u),
              "the advance bit without a lane count is not configured - a "
              "one-lane link stores 0 there, indistinguishable from untouched");
        CHECK(!rb_enc_looks_configured(1u, 0x0101FF00u),
              "a lane count without the advance bit is not configured");
        CHECK(rb_enc_looks_configured(1u, 0x0101FF10u),
              "boot 444's instance 2 reads configured");

        /* An absent registry key reads back as -1, which is 0xffffffff in
         * the low word - and before this check that satisfied BOTH field
         * tests and reported all five configured, which is the opposite of
         * the truth. Rule 14 at the tool boundary. */
        CHECK(!rb_enc_looks_configured(0xFFFFFFFFu, 0xFFFFFFFFu),
              "an all-ones pair is an unmapped block, not a configuration");
        CHECK(!rb_enc_looks_configured(0xFFFFFFFFu, 0x0101FF10u),
              "all-ones in either register alone is refused");
        CHECK(!rb_enc_looks_configured(1u, 0xFFFFFFFFu),
              "...in either position");
        for (i = 0; i < RB_ENC_COUNT; i++) {
            cfg[i] = 0xFFFFFFFFu; scr[i] = 0xFFFFFFFFu;
        }
        CHECK(rb_enc_configured_mask(cfg, scr) == 0u,
              "five unmapped encoders read 0, NOT 0x1f");
        /* The control, so the refusal is not swallowing real readings: a
         * configured instance beside unmapped ones still shows. */
        cfg[2] = 1u; scr[2] = 0x0101FF10u;
        CHECK(rb_enc_configured_mask(cfg, scr) == 0x04u,
              "the all-ones refusal does not swallow a real configuration");

        CHECK(rb_enc_configured_mask(NULL, scr) == 0u
              && rb_enc_configured_mask(cfg, NULL) == 0u,
              "a NULL survey is 0, not a read through NULL");
    }

#undef CHECK
    return bad;
}
