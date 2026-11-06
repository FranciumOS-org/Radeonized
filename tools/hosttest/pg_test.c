/* SPDX-License-Identifier: MIT */
/* 2DC: the DOMAIN power-gating readback, and the DMUB_CMD__VBIOS
 * DOMAIN_CONTROL encoder it exists to judge.
 *
 * The question these pin is the one 2DB left with no instrument: does this
 * firmware EXECUTE an inbox command? Every readable quantity was spent - the
 * caps query is unanswered at both candidate addresses, an ack advances over
 * whatever the slot held, the ring read-back only proves our own write stuck,
 * and the ABM backlight is a no-op when ABM was never initialised. What is
 * left is 2D9's finding: a VBIOS sub-type has no reply and its product is a
 * REGISTER.
 */
#include <stdio.h>
#include <stdint.h>
#include "rb_pg.h"
#include "rb_dmub.h"

int pg_tests(void);

int pg_tests(void)
{
    int bad = 0;

#define CHECK(c, msg) do { \
    if (!(c)) { printf("FAIL pg: %s\n", (msg)); bad++; } \
} while (0)

    /* The offsets, against docs/linux-ref/dcn_3_1_2_offset.h read directly:
     *   regDOMAIN0_PG_CONFIG 0x0080  regDOMAIN0_PG_STATUS 0x0081
     *   regDOMAIN1_         0x0082                       0x0083
     *   regDOMAIN2_         0x0084                       0x0085
     *   regDOMAIN3_         0x0086                       0x0087
     * Pinned per instance, not as the formula they are computed by - a
     * formula checked against itself proves nothing (rule 26 for arithmetic,
     * the shape hpd_test.c records). */
    {
        static const uint32_t want_cfg[4] = { 0x0080, 0x0082, 0x0084, 0x0086 };
        static const uint32_t want_sts[4] = { 0x0081, 0x0083, 0x0085, 0x0087 };
        uint32_t i;

        CHECK(RB_PG_DOMAIN_COUNT == 4u, "DOMAIN0..3 are the exposed block");
        for (i = 0; i < RB_PG_DOMAIN_COUNT; i++) {
            CHECK(rb_pg_config_dw(i) == want_cfg[i],
                  "DOMAINn_PG_CONFIG matches the vendored offset header");
            CHECK(rb_pg_status_dw(i) == want_sts[i],
                  "DOMAINn_PG_STATUS matches the vendored offset header");
        }

        /* The bound an aperture check uses must be the real highest dword,
         * so it is asserted against the literal rather than recomputed. */
        CHECK(rb_pg_dw_max() == 0x0087u,
              "rb_pg_dw_max is DOMAIN3_PG_STATUS, the highest dword read");

        /* THE STRIDE MUST NOT BE EXTENDED. regDOMAIN16_PG_CONFIG is 0x0089
         * and continuing the stride would reach 0x0088, which is a different
         * register. This asserts the file does NOT claim to reach it, so
         * raising the count later fails here instead of reading the wrong
         * address on a chip. */
        CHECK(rb_pg_config_dw(RB_PG_DOMAIN_COUNT) == 0x0088u,
              "one past the block is 0x0088, which is NOT DOMAIN16 (0x0089) "
              "- the stride is broken there and the count must stay 4");
    }

    /* The fields, against docs/linux-ref/dcn_3_1_2_sh_mask.h:
     *   DOMAIN0_PG_STATUS__DOMAIN_PGFSM_PWR_STATUS   shift 0x1e,
     *                                                mask 0xC0000000
     *   DOMAIN0_PG_STATUS__DOMAIN_DESIRED_PWR_STATE  shift 0x1c,
     *                                                mask 0x10000000
     *   DOMAIN0_PG_CONFIG__DOMAIN_POWER_FORCEON      mask 0x00000001
     *   DOMAIN0_PG_CONFIG__DOMAIN_POWER_GATE         mask 0x00000100 */
    {
        CHECK(rb_pg_fsm(0xc0000000u) == 3u, "PGFSM is bits 31:30");
        CHECK(rb_pg_fsm(0x80000000u) == 2u, "PGFSM 0x80000000 is 2");
        CHECK(rb_pg_fsm(0x40000000u) == 1u, "PGFSM 0x40000000 is 1");
        CHECK(rb_pg_fsm(0x00000000u) == 0u, "PGFSM 0 is 0");
        /* The DESIRED bit must not bleed into the FSM field and vice versa. */
        CHECK(rb_pg_fsm(0x10000000u) == 0u,
              "DOMAIN_DESIRED_PWR_STATE is not part of the FSM field");
        CHECK(rb_pg_desired(0x10000000u) == 1u, "DESIRED is bit 28");
        CHECK(rb_pg_desired(0xc0000000u) == 0u,
              "the FSM field is not part of DESIRED");

        CHECK(rb_pg_forced_on(0x00000001u) == 1, "FORCEON is bit 0");
        CHECK(rb_pg_forced_on(0x00000100u) == 0, "GATE is not FORCEON");
        CHECK(rb_pg_gate_requested(0x00000100u) == 1, "GATE is bit 8");
        CHECK(rb_pg_gate_requested(0x00000001u) == 0, "FORCEON is not GATE");
    }

    /* THE REFERENCE METHOD. No constant for "powered" exists in rb_pg.h on
     * purpose: the polarity of PGFSM_PWR_STATUS is defined in Linux's
     * dcn20_hwseq.c, which is NOT vendored, and recalling it is how a
     * backwards verdict gets printed confidently (2BA's "every constant is
     * read, not recalled"). DOMAIN0 drives the live panel, so its own value
     * is the definition of powered - and the test must pass whichever
     * polarity the hardware actually uses. Both are exercised. */
    {
        /* Polarity A: suppose "powered" reads 0 in the FSM field. */
        uint32_t a[4] = { 0x00000000u, 0x80000000u, 0x80000000u, 0x00000000u };
        /* Polarity B: suppose "powered" reads 2. Same shape, inverted. */
        uint32_t b[4] = { 0x80000000u, 0x00000000u, 0x00000000u, 0x80000000u };

        CHECK(rb_pg_like_reference(a[3], a[RB_PG_DOMAIN_REFERENCE]) == 1,
              "a domain matching the reference reads as powered (polarity A)");
        CHECK(rb_pg_like_reference(a[1], a[RB_PG_DOMAIN_REFERENCE]) == 0,
              "a domain differing from the reference does not (polarity A)");
        CHECK(rb_pg_like_reference(b[3], b[RB_PG_DOMAIN_REFERENCE]) == 1,
              "...and the identical conclusion under polarity B");
        CHECK(rb_pg_like_reference(b[1], b[RB_PG_DOMAIN_REFERENCE]) == 0,
              "...in both directions, so no polarity is assumed anywhere");

        /* The candidate is a domain that is NOT the reference and NOT already
         * in the reference's state - otherwise a power-up request is a no-op
         * and its null would mean nothing (rule 39). */
        CHECK(rb_pg_candidate(a) == 1,
              "the first domain unlike the reference is the candidate");
        CHECK(rb_pg_candidate(b) == 1,
              "...independent of polarity");

        /* THE TEST MUST DECLARE ITSELF UNAVAILABLE rather than pick one
         * anyway. If every domain already matches the reference there is no
         * gated domain to power up on this boot. */
        {
            uint32_t all_same[4] = { 0x11u, 0x11u, 0x11u, 0x11u };
            CHECK(rb_pg_candidate(all_same) == -1,
                  "every domain already in the reference state means the test "
                  "is NOT AVAILABLE, not that domain 1 will do");
        }
        CHECK(rb_pg_candidate(0) == -1, "a null sample refuses");

        /* THE REFERENCE CAN NEVER BE THE CANDIDATE, and the reason is
         * structural rather than a branch: it is compared against ITSELF, so
         * it always matches and is never "unlike the reference". Asserting
         * the `continue` in rb_pg_candidate would pass whether or not that
         * line existed - caught by a break-it check that deleted it and saw
         * nothing fail, which is 2BC's "a case that passes for the wrong
         * reason is not coverage". So the PROPERTY is asserted, over values
         * chosen to differ wildly, and the panel's domain is protected
         * against a hand-named instance by rb_pg_sel_refuse() below, which
         * does fail when broken. */
        {
            uint32_t ref_unlike_all[4] =
                { 0xc0000000u, 0x00000000u, 0x00000000u, 0x00000000u };
            CHECK(rb_pg_like_reference(ref_unlike_all[RB_PG_DOMAIN_REFERENCE],
                                       ref_unlike_all[RB_PG_DOMAIN_REFERENCE])
                      == 1,
                  "the reference always matches itself, which is WHY it can "
                  "never be selected as a gated candidate");
            CHECK(rb_pg_candidate(ref_unlike_all) == 1,
                  "with the reference unlike every other domain, the first "
                  "OTHER domain is still what is returned - never 0");
        }
    }

    /* The hand-named selector's refusals are distinct codes, not a bool -
     * "out of range" and "that is the panel's domain" point at different
     * mistakes (rule 14). */
    {
        CHECK(rb_pg_sel_refuse(1u) == RB_PG_SEL_OK, "DOMAIN1 is selectable");
        CHECK(rb_pg_sel_refuse(3u) == RB_PG_SEL_OK, "DOMAIN3 is selectable");
        CHECK(rb_pg_sel_refuse(0u) == RB_PG_SEL_IS_REFERENCE,
              "DOMAIN0 is refused as the live panel's domain, with its own "
              "code");
        CHECK(rb_pg_sel_refuse(4u) == RB_PG_SEL_OUT_OF_RANGE,
              "DOMAIN4 is outside the block this file can address");
        CHECK(rb_pg_sel_refuse(16u) == RB_PG_SEL_OUT_OF_RANGE,
              "DOMAIN16 is refused: its offset is not on this stride");
    }

    /* THE VERDICT IS A DIFFERENCE, and it is a MASK so two changes cannot
     * cancel into a count of zero. */
    {
        uint32_t before[4] = { 0x00u, 0x80000000u, 0x80000000u, 0x80000000u };
        uint32_t same[4]   = { 0x00u, 0x80000000u, 0x80000000u, 0x80000000u };
        uint32_t one[4]    = { 0x00u, 0x00000000u, 0x80000000u, 0x80000000u };
        uint32_t two[4]    = { 0x00u, 0x00000000u, 0x00000000u, 0x80000000u };

        CHECK(rb_pg_moved_mask(before, same) == 0u,
              "nothing moved is an empty mask - the firmware did not execute");
        CHECK(rb_pg_moved_mask(before, one) == 0x2u,
              "DOMAIN1 moving sets bit 1, naming WHICH domain changed");
        CHECK(rb_pg_moved_mask(before, two) == 0x6u,
              "two domains moving sets two bits rather than summing to one");
        CHECK(rb_pg_moved_mask(0, same) == 0u, "a null sample moves nothing");
        CHECK(rb_pg_moved_mask(before, 0) == 0u, "...either side");
    }

    /* THE ENCODER. docs/linux-ref/dmub_cmd.h:
     *   DMUB_CMD__VBIOS = 128, DMUB_CMD__VBIOS_DOMAIN_CONTROL = 28
     *   struct dmub_rb_cmd_domain_control_data {
     *       uint8_t inst : 6; uint8_t power_gate : 1; uint8_t reserved[3]; }
     * so the payload is 4 bytes and both fields are in the FIRST one. */
    {
        uint8_t buf[RB_DMUB_CMD_SIZE];
        uint32_t i, h;

        rb_dmub_build_domain_control(buf, 1u, 0u);
        h = rb_dmub_get32(buf, 0);

        CHECK(rb_dmub_hdr_type(h) == 128u, "type is DMUB_CMD__VBIOS");
        CHECK(rb_dmub_hdr_sub(h) == 28u, "sub_type is DOMAIN_CONTROL");
        CHECK(rb_dmub_hdr_payload(h) == 4u,
              "payload_bytes is sizeof(domain_control_data) == 4");

        /* NO ret_status: this is a command, not a query, and 2D9's finding is
         * that its product is a register. Asking for a reply the ABI never
         * promised and then reading its absence as failure is rule 14. */
        CHECK(rb_dmub_hdr_ret(h) == 0u,
              "ret_status is CLEAR - a VBIOS command returns no data");

        CHECK(buf[4] == 0x01u,
              "inst 1 with power_gate 0 packs as 0x01 in the payload's first "
              "byte");
        for (i = 5; i < RB_DMUB_CMD_SIZE; i++) {
            CHECK(buf[i] == 0u, "reserved[3] and the rest of the slot are 0");
        }

        /* power_gate is bit 6, above the 6-bit inst field. */
        rb_dmub_build_domain_control(buf, 1u, 1u);
        CHECK(buf[4] == 0x41u, "power_gate sets bit 6, so inst 1 gated is "
                               "0x41 and never 0x81 or 0x11");
        rb_dmub_build_domain_control(buf, 3u, 0u);
        CHECK(buf[4] == 0x03u, "inst 3 power-up is 0x03");
        rb_dmub_build_domain_control(buf, 63u, 1u);
        CHECK(buf[4] == 0x7fu, "the maximum inst 63 gated is 0x7f");

        /* An inst that does not fit the 6-bit field must not silently wrap
         * into a DIFFERENT domain - the caller is refused first. */
        CHECK(rb_dmub_domain_inst_valid(63u) == 1, "63 fits six bits");
        CHECK(rb_dmub_domain_inst_valid(64u) == 0,
              "64 does not fit, and truncating it would name domain 0 - the "
              "live panel's - so it is refused");

        /* The command this project would actually send must read as a
         * plausible header to our own scanner, or the ring scan would score
         * our own traffic as garbage. */
        rb_dmub_build_domain_control(buf, 1u, 0u);
        CHECK(rb_dmub_hdr_plausible(rb_dmub_get32(buf, 0)) == 1,
              "our own DOMAIN_CONTROL reads as a plausible command header");
    }

#undef CHECK
    return bad;
}
