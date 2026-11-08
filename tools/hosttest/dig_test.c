/* SPDX-License-Identifier: MIT */
/* 2E2: the DMUB DIG1 transmitter command for the Type-C port.
 *
 * Every constant here is pinned against the file it was read from, and the
 * board's five derived values are pinned against 2E1's ROM parse - which is
 * itself corroborated by three hardware readings that were not used to make
 * it (2B4's HPD instance 2, 2B9's AUX_HPD_SEL 2, 2C1's lit PHY0 being the
 * panel on UNIPHY_A).
 *
 * The case that matters most is the hpdsel one. The payload comment in
 * dmub_cmd.h reads "=2: HPD2", our port is HPD instance 2, and the correct
 * byte is 3 - so a plausible-looking 2 must FAIL here.
 */
#include <stdio.h>
#include <stdint.h>
#include "rb_dig.h"
#include "rb_dmub.h"
#include "rb_dpalt.h"

int dig_tests(void);

int dig_tests(void)
{
    int bad = 0;

#define CHECK(c, msg) do { \
    if (!(c)) { printf("FAIL dig: %s\n", (msg)); bad++; } \
} while (0)

    /* --- the constants, each against its source ------------------------ */

    CHECK(RB_DMUB_CMD_VBIOS_DIG1_TRANSMITTER == 1u,
          "DMUB_CMD__VBIOS_DIG1_TRANSMITTER_CONTROL is sub-type 1");
    CHECK(RB_DIG_PAYLOAD == 60u,
          "payload_bytes is sizeof(union) - sizeof(header) = 60, and 60 fits "
          "the header's 6-bit field");
    CHECK(RB_DIG_PAYLOAD <= RB_DMUB_CMD_SIZE - 4u,
          "the payload cannot exceed the 64-byte slot less its header");
    CHECK(RB_DIG_ACTION_ENABLE == 1u,
          "ATOM_TRANSMITTER_ACTION_ENABLE is 1 (atomfirmware.h:4385) and so "
          "is TRANSMITTER_CONTROL_ENABLE (bios_parser_types.h:68)");
    CHECK(RB_DIG_DIGMODE_DP == 0u,
          "ATOM_TRANSMITTER_DIGMODE_V6_DP is 0 (atombios.h:1588)");

    /* 2E1's derivation, value by value. */
    CHECK(RB_DIG_TYPEC_PHYID == 2u,
          "the Type-C port is TRANSMITTER_UNIPHY_C, and phy_id_to_atom is "
          "the identity, so phyid is 2");
    CHECK(RB_DIG_TYPEC_HPDSEL == 3u,
          "HPD instance 2 PLUS ONE. dmub_cmd.h's '=2: HPD2' names HPD "
          "instance 1 - the other DisplayPort connector on this die");
    CHECK(RB_DIG_TYPEC_HPDSEL != RB_DIG_TYPEC_HPD_INST,
          "the payload byte is NOT the DCN HPD block instance");
    CHECK(RB_DIG_TYPEC_LANES == 2u,
          "2BB read 2 lanes out of the sink's own DPCD, which is what DP alt "
          "mode leaves for DP");
    CHECK(RB_DIG_TYPEC_SYMCLK == (0x14u * 27000u) / 10u,
          "symclk_10khz is LINK_RATE_HIGH2 x LINK_RATE_REF_FREQ_IN_KHZ / 10");
    CHECK(RB_DIG_TYPEC_SYMCLK == 54000u, "which is 54000 for HBR2");
    CHECK(RB_DIG_HPO_DP_NONE == 244u,
          "(uint8_t)(ENGINE_ID_DIGA - ENGINE_ID_HPO_DP_0) = (uint8_t)(0 - 12)"
          " - what Linux sends on every non-HPO DP enable");

    /* The PHY index is also the RDPCSTX instance (dcn31_resource.c:419), so
     * the product register must be addressable for the target. */
    CHECK(RB_DIG_TYPEC_PHYID < RB_DPALT_COUNT,
          "RDPCSTX2 exists in the five-instance PHY block rb_dpalt.h maps");
    CHECK(rb_dpalt_dw(RB_DIG_TYPEC_PHYID) == 0x2946u + 2u * 0xd8u,
          "the target's PHY_CNTL6 is the third RDPCSTX instance");

    /* --- the parameter set -------------------------------------------- */
    {
        struct rb_dig_params p;
        rb_dig_typec_enable(&p);

        CHECK(p.phyid == 2u && p.action == 1u && p.digmode == 0u &&
              p.lanenum == 2u && p.symclk_10khz == 54000u &&
              p.hpdsel == 3u,
              "rb_dig_typec_enable fills the derived values");
        CHECK(p.digfe_sel == 0u,
              "digfe_sel is 0 because dig_encoder_sel_to_atom returns 0 "
              "unconditionally - the DIG FE-to-BE connection is a separate "
              "register write, not part of this command");
        CHECK(p.connobj_id == 0u,
              "connobj_id is 0 because dcn10_link_encoder_enable_dp_output "
              "never assigns it and cntl is `{ 0 }`");
        CHECK(p.hpo_instance == 244u,
              "HPO_instance is reproduced, not normalised to 0");

        /* --- refusals, each its own code ------------------------------ */
        CHECK(rb_dig_refuse(&p, 1, 1, 1) == RB_DIG_OK,
              "the derived Type-C ENABLE is sent when armed, mapped and the "
              "control PHY reads lit");
        CHECK(rb_dig_refuse(&p, 0, 1, 1) == RB_DIG_REFUSE_DISARMED,
              "disarmed refuses with its own code");
        CHECK(rb_dig_refuse(&p, 1, 0, 1) == RB_DIG_REFUSE_NOREGS,
              "no aperture refuses with its own code - the product cannot be "
              "read, so the send would be unjudgeable (rule 14)");
        CHECK(rb_dig_refuse(&p, 1, 1, 0) == RB_DIG_REFUSE_DARKREF,
              "a control PHY that does not read lit refuses: a difference "
              "read out of a block not answering as measured is "
              "uninterpretable, which is 2zw's whole shape");

        /* Every refusal code must be distinct from OK and from each other,
         * or the published field cannot separate them. */
        CHECK(RB_DIG_REFUSE_DISARMED != RB_DIG_REFUSE_NOREGS &&
              RB_DIG_REFUSE_PANEL    != RB_DIG_REFUSE_PHY &&
              RB_DIG_REFUSE_ACTION   != RB_DIG_REFUSE_LANES &&
              RB_DIG_REFUSE_HPDSEL   != RB_DIG_REFUSE_DARKREF &&
              RB_DIG_REFUSE_DISARMED != RB_DIG_OK,
              "the refusal codes are distinct");
        /* And clear of rb_pg.h's 20-25, so one published field can never be
         * decoded against the wrong table. */
        CHECK(RB_DIG_REFUSE_DISARMED > 25u,
              "the dig refusals do not collide with the DOMAIN refusals");

        /* The panel is refused BY NAME, as rb_pg.h refuses DOMAIN0. */
        p.phyid = RB_DIG_PHY_PANEL;
        CHECK(rb_dig_refuse(&p, 1, 1, 1) == RB_DIG_REFUSE_PANEL,
              "PHY0 is the eDP panel (2E1's display path 0, UNIPHY_A, and "
              "2C1's only lit PHY) and is refused by name, not by range");
        p.phyid = 6u;
        CHECK(rb_dig_refuse(&p, 1, 1, 1) == RB_DIG_REFUSE_PHY,
              "UNIPHYG is outside the payload's documented 0..5");
        p.phyid = RB_DIG_TYPEC_PHYID;

        p.action = 7u;   /* ATOM_TRANSMITTER_ACTION_INIT */
        CHECK(rb_dig_refuse(&p, 1, 1, 1) == RB_DIG_REFUSE_ACTION,
              "an action this file has not reasoned about is refused rather "
              "than passed through");
        p.action = RB_DIG_ACTION_ENABLE;

        p.lanenum = 3u;
        CHECK(rb_dig_refuse(&p, 1, 1, 1) == RB_DIG_REFUSE_LANES,
              "3 is not a dc_lane_count");
        p.lanenum = 8u;
        CHECK(rb_dig_refuse(&p, 1, 1, 1) == RB_DIG_REFUSE_LANES,
              "8 is a dc_lane_count but never a DP one - LANE_COUNT_DP_MAX "
              "is LANE_COUNT_FOUR");
        p.lanenum = RB_DIG_TYPEC_LANES;

        p.hpdsel = 0u;
        CHECK(rb_dig_refuse(&p, 1, 1, 1) == RB_DIG_REFUSE_HPDSEL,
              "hpdsel 0 is 'HPD is not assigned' - a command about nothing "
              "on a connector we have measured a sink on");
        p.hpdsel = 7u;
        CHECK(rb_dig_refuse(&p, 1, 1, 1) == RB_DIG_REFUSE_HPDSEL,
              "atomfirmware.h defines HPD1_SEL..HPD6_SEL only");
        p.hpdsel = RB_DIG_TYPEC_HPDSEL;
        CHECK(rb_dig_refuse(&p, 1, 1, 1) == RB_DIG_OK,
              "and the derived value is accepted after all that poking");
    }

    /* --- the wire format --------------------------------------------- */
    {
        struct rb_dig_params p;
        uint8_t buf[RB_DMUB_CMD_SIZE];
        uint32_t i, h;

        rb_dig_typec_enable(&p);
        rb_dig_build_transmitter(buf, &p);

        h = rb_dmub_get32(buf, 0);
        CHECK(rb_dmub_hdr_type(h) == RB_DMUB_CMD_VBIOS,
              "the header carries DMUB_CMD__VBIOS");
        CHECK(rb_dmub_hdr_sub(h) == RB_DMUB_CMD_VBIOS_DIG1_TRANSMITTER,
              "and sub-type 1");
        CHECK(rb_dmub_hdr_payload(h) == RB_DIG_PAYLOAD,
              "and payload_bytes 60");
        CHECK(rb_dmub_hdr_ret(h) == 0u,
              "ret_status stays 0: this sub-type has no reply, and asking "
              "for one then reading its absence as failure is 2D9's trap");
        CHECK(rb_dmub_hdr_plausible(h) == 1,
              "the command we build is one the ring scanner would accept as "
              "a header - the control for the whole address derivation");

        /* The payload bytes at their documented offsets. */
        CHECK(buf[4 + 0] == 2u,  "byte 0 is phyid 2");
        CHECK(buf[4 + 1] == 1u,  "byte 1 is action ENABLE");
        CHECK(buf[4 + 2] == 0u,  "byte 2 is digmode DP");
        CHECK(buf[4 + 3] == 2u,  "byte 3 is lanenum 2");
        CHECK(rb_dmub_get32(buf, 4 + 4) == 54000u,
              "bytes 4-7 are symclk_10khz, little-endian");
        CHECK(buf[4 + 8] == 3u,  "byte 8 is hpdsel 3, NOT 2");
        CHECK(buf[4 + 9] == 0u,  "byte 9 is digfe_sel 0");
        CHECK(buf[4 + 10] == 0u, "byte 10 is connobj_id 0");
        CHECK(buf[4 + 11] == 244u,
              "byte 11 is HPO_instance, and it is the ONLY byte in which "
              "v1_7 and v1_6 differ for an ENABLE - v1_6 has `reserved` "
              "there. 2E1's ROM says v1_7, so 244 it is");

        /* Everything past the named fields is zero, which is what both
         * senders' memset leaves. A stray byte in a reserved field is a
         * different command from the one with evidence behind it. */
        for (i = 4 + 12; i < RB_DMUB_CMD_SIZE; i++) {
            if (buf[i] != 0u) {
                printf("FAIL dig: byte %u past the named fields is not zero\n",
                       i);
                bad++;
                break;
            }
        }

        /* A null pointer must zero the buffer rather than leave it, so a
         * refused build can never be pushed as a half-written command. */
        for (i = 0; i < RB_DMUB_CMD_SIZE; i++) { buf[i] = 0xa5u; }
        rb_dig_build_transmitter(buf, 0);
        CHECK(buf[0] == 0u && buf[RB_DMUB_CMD_SIZE - 1u] == 0u,
              "a build with no parameters leaves no stale bytes behind");
    }

    /* --- the verdict, against 2C1's own measured words ---------------- */
    {
        /* Boot 297, read off this chip: PHY0 lit, PHY1 and PHY2-4 dark. */
        const uint32_t lit   = 0x00194444u;
        const uint32_t dark2 = 0x00063333u;
        const uint32_t dark1 = 0x00023333u;

        CHECK(rb_dig_phy_lit(lit) == 1,
              "2C1's PHY0 word is what a PHY in operation reads - four MPLLs "
              "on, PSTATE 0 on every lane, ref clock enabled");
        CHECK(rb_dig_phy_lit(dark2) == 0,
              "2C1's PHY2 word is dark - this is the target's before-state");
        CHECK(rb_dig_phy_lit(dark1) == 0,
              "and PHY1, which differs from PHY2 only in the alt-mode bits, "
              "is dark too: the alt-mode axis must not reach this verdict");

        CHECK(rb_dig_verdict(dark2, dark2) == RB_DIG_V_STILL,
              "byte-identical is STILL, and on its own it is NOT 'the "
              "firmware discards VBIOS commands' - the encoder setup this "
              "file omits is a live alternative");
        CHECK(rb_dig_verdict(dark2, lit) == RB_DIG_V_LIT,
              "dark to lit is the answer the whole experiment is for");
        CHECK(rb_dig_verdict(dark2, dark2 | RB_DPALT_REF_CLK_REQ)
              == RB_DIG_V_MOVED,
              "a change that is not a lit PHY is its own third outcome - the "
              "firmware acted and the command or the setup is incomplete");
        CHECK(rb_dig_verdict(lit, lit) == RB_DIG_V_STILL,
              "an already-lit PHY that does not move is STILL, not LIT: the "
              "verdict is a TRANSITION, so it cannot be satisfied by a PHY "
              "that was already up when we found it");
    }

    /* 2FF: the hpdsel override. The point of these cases is that the
     * override cannot widen the gate and cannot silently fail. */
    {
        struct rb_dig_params p;

        rb_dig_typec_enable(&p);
        CHECK(p.hpdsel == RB_DIG_TYPEC_HPDSEL,
              "the producer's derived hpdsel is what an untouched set "
              "carries - a boot that sets nothing must send what boot 468 "
              "sent");

        CHECK(rb_dig_set_hpdsel(&p, 2u) == 1 && p.hpdsel == 2u,
              "2 is accepted: it is dmub_cmd.h's own reading of the field "
              "and the other side of the A/B");

        rb_dig_typec_enable(&p);
        CHECK(rb_dig_set_hpdsel(&p, 6u) == 1 && p.hpdsel == 6u,
              "6 is the top of atomfirmware.h's HPD1_SEL..HPD6_SEL");

        /* THE BOUNDS ARE THE GATE'S BOUNDS. An override that could set a
         * value rb_dig_refuse() would reject would let a caller post a
         * payload past the gate, which is the whole reason the setter exists
         * instead of a bare assignment. */
        rb_dig_typec_enable(&p);
        CHECK(rb_dig_set_hpdsel(&p, 0u) == 0 &&
              p.hpdsel == RB_DIG_TYPEC_HPDSEL,
              "0 is refused AND the derived value is left intact - a "
              "half-applied override is worse than none");
        /* EACH out-of-range case re-produces the set. Without this the
         * cases share state: the break-it run that allowed 0 through made
         * the PRECEDING case leave p.hpdsel at 0, and this one then failed
         * for that reason rather than for anything about 7. A case that can
         * fail for a reason other than the one it names is not coverage
         * (rule 48), and it hides whichever guard it was written for. */
        rb_dig_typec_enable(&p);
        CHECK(rb_dig_set_hpdsel(&p, 7u) == 0 &&
              p.hpdsel == RB_DIG_TYPEC_HPDSEL,
              "7 is past HPD6_SEL and is refused without disturbing the set");
        rb_dig_typec_enable(&p);
        CHECK(rb_dig_set_hpdsel(&p, 0xffffffffu) == 0 &&
              p.hpdsel == RB_DIG_TYPEC_HPDSEL,
              "a wrapped value is refused too");
        CHECK(rb_dig_set_hpdsel(0, 2u) == 0, "NULL is refused");

        /* Every value the setter ACCEPTS must pass the gate, and every value
         * it REFUSES must be one the gate would have refused. Checked as a
         * sweep rather than at the edges, because the two bounds are written
         * out twice in two files and the failure mode is them drifting
         * apart. */
        for (uint32_t h = 0; h < 10u; h++) {
            int took;
            uint32_t why;
            rb_dig_typec_enable(&p);
            took = rb_dig_set_hpdsel(&p, h);
            why  = rb_dig_refuse(&p, 1, 1, 1);
            if (took && why == RB_DIG_REFUSE_HPDSEL) {
                printf("FAIL dig: hpdsel %u accepted but refused by the "
                       "gate\n", h);
                bad++;
            }
            if (!took && h != 0u && h <= 6u) {
                printf("FAIL dig: hpdsel %u refused but inside the gate\n",
                       h);
                bad++;
            }
        }
    }

#undef CHECK
    return bad;
}
