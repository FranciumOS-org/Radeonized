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
        /* 2DE: seven rows, each written down from the header. DOMAIN16..18
         * are NOT on the DOMAIN0..3 stride - 0x0089 where the formula would
         * say 0x0088 - which is the whole reason this is a table. */
        static const uint32_t want_cfg[7] =
            { 0x0080, 0x0082, 0x0084, 0x0086, 0x0089, 0x008b, 0x008d };
        static const uint32_t want_sts[7] =
            { 0x0081, 0x0083, 0x0085, 0x0087, 0x008a, 0x008c, 0x008e };
        static const uint32_t want_inst[7] = { 0, 1, 2, 3, 16, 17, 18 };
        uint32_t i;

        CHECK(RB_PG_DOMAIN_COUNT == 7u,
              "DOMAIN0..3 and DOMAIN16..18 are the exposed rows");
        for (i = 0; i < RB_PG_DOMAIN_COUNT; i++) {
            CHECK(rb_pg_config_dw(i) == want_cfg[i],
                  "DOMAINn_PG_CONFIG matches the vendored offset header");
            CHECK(rb_pg_status_dw(i) == want_sts[i],
                  "DOMAINn_PG_STATUS matches the vendored offset header");
            CHECK(rb_pg_slot_inst(i) == want_inst[i],
                  "the slot carries the DOMAIN NUMBER the command needs");
            CHECK(rb_pg_slot_for_inst(want_inst[i]) == (int)i,
                  "and the inverse round-trips, so neither direction is "
                  "arithmetic");
        }

        /* THE FORMULA IS WRONG PAST SLOT 3, asserted explicitly: the naive
         * 0x0080 + slot*2 gives 0x0088 at slot 4, which is a DIFFERENT
         * register, and the table gives DOMAIN16's real 0x0089. This case is
         * what fails if anyone replaces the table with a loop. */
        CHECK(rb_pg_config_dw(4u) != (RB_PG_DW_DOMAIN0_PG_CONFIG +
                                      4u * RB_PG_DOMAIN_STRIDE),
              "slot 4 is NOT where continuing the stride would land");
        CHECK((RB_PG_DW_DOMAIN0_PG_CONFIG + 4u * RB_PG_DOMAIN_STRIDE)
                  == 0x0088u,
              "...and the value it would have landed on is 0x0088");
        CHECK(rb_pg_config_dw(4u) == 0x0089u, "DOMAIN16_PG_CONFIG is 0x0089");

        /* The bound an aperture check uses must be the real highest dword. */
        CHECK(rb_pg_dw_max() == 0x008eu,
              "rb_pg_dw_max is DOMAIN18_PG_STATUS, the highest dword read");

        /* A slot off the end is REFUSED with a value no aperture contains,
         * not clamped to a plausible row (rule 14). */
        CHECK(rb_pg_config_dw(RB_PG_DOMAIN_COUNT) == RB_PG_SLOT_BAD,
              "a slot past the table refuses rather than computing one");
        CHECK(rb_pg_status_dw(RB_PG_DOMAIN_COUNT) == RB_PG_SLOT_BAD,
              "...both accessors");
        CHECK(rb_pg_slot_inst(RB_PG_DOMAIN_COUNT) == RB_PG_SLOT_BAD,
              "...and the instance lookup");
        CHECK(rb_pg_slot_for_inst(4u) == -1,
              "DOMAIN4 does not exist on this chip and has no slot");
        CHECK(rb_pg_slot_for_inst(RB_PG_SLOT_BAD) == -1,
              "the refusal value itself is not a domain number");
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
        uint32_t a[7] = { 0x00000000u, 0x80000000u, 0x80000000u,
                          0x00000000u, 0x00000000u, 0x00000000u,
                          0x00000000u };
        /* Polarity B: suppose "powered" reads 2. Same shape, inverted. */
        uint32_t b[7] = { 0x80000000u, 0x00000000u, 0x00000000u,
                          0x80000000u, 0x80000000u, 0x80000000u,
                          0x80000000u };

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
            uint32_t all_same[7] = { 0x11u, 0x11u, 0x11u, 0x11u,
                                     0x11u, 0x11u, 0x11u };
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
            uint32_t ref_unlike_all[7] =
                { 0xc0000000u, 0x00000000u, 0x00000000u, 0x00000000u,
                  0x00000000u, 0x00000000u, 0x00000000u };
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
              "DOMAIN4 does not exist on this chip - refused, NOT read as "
              "slot 4, which is DOMAIN16");
        CHECK(rb_pg_sel_refuse(16u) == RB_PG_SEL_OK,
              "DOMAIN16 is selectable since 2DE wrote its offsets down");
        CHECK(rb_pg_sel_refuse(18u) == RB_PG_SEL_OK, "DOMAIN18 too");
        CHECK(rb_pg_sel_refuse(19u) == RB_PG_SEL_OUT_OF_RANGE,
              "DOMAIN19 is past the header's last row");
        CHECK(rb_pg_sel_refuse(63u) == RB_PG_SEL_OUT_OF_RANGE,
              "the payload field's maximum is still not a domain");
    }

    /* THE VERDICT IS A DIFFERENCE, and it is a MASK so two changes cannot
     * cancel into a count of zero. */
    {
        uint32_t before[7] = { 0x00u, 0x80000000u, 0x80000000u, 0x80000000u,
                               0x80000000u, 0x80000000u, 0x80000000u };
        uint32_t same[7]   = { 0x00u, 0x80000000u, 0x80000000u, 0x80000000u,
                               0x80000000u, 0x80000000u, 0x80000000u };
        uint32_t one[7]    = { 0x00u, 0x00000000u, 0x80000000u, 0x80000000u,
                               0x80000000u, 0x80000000u, 0x80000000u };
        uint32_t two[7]    = { 0x00u, 0x00000000u, 0x00000000u, 0x80000000u,
                               0x80000000u, 0x80000000u, 0x80000000u };

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

    /* 2DD: the sample-readable guard, which exists because boot 436 read
     * eight zero words off a wrong address and the reference check turned
     * them into refusal 24 - "no domain differs from DOMAIN0", true of the
     * words and false of the chip. */
    {
        uint32_t st[RB_PG_DOMAIN_COUNT];
        uint32_t cf[RB_PG_DOMAIN_COUNT];
        uint32_t i;

        for (i = 0; i < RB_PG_DOMAIN_COUNT; i++) { st[i] = 0u; cf[i] = 0u; }
        CHECK(rb_pg_sample_readable(st, cf) == 0,
              "all eight words zero is an UNREADABLE sample - boot 436's "
              "exact reading from pgSample() without the DMU segment base");

        /* One bit anywhere is enough: the guard's job is to catch a block
         * that is not being read at all, not to judge any field. */
        st[RB_PG_DOMAIN_COUNT - 1] = 0x00000001u;
        CHECK(rb_pg_sample_readable(st, cf) == 1,
              "a single set bit in the LAST status word makes the sample "
              "readable - the loop covers every domain, not just DOMAIN0");
        st[RB_PG_DOMAIN_COUNT - 1] = 0u;
        cf[RB_PG_DOMAIN_COUNT - 1] = 0x00000001u;
        CHECK(rb_pg_sample_readable(st, cf) == 1,
              "CONFIG alone also makes it readable - both arrays are read");
        cf[RB_PG_DOMAIN_COUNT - 1] = 0u;

        /* The control: a sample this project has actually seen from a live
         * block must pass. DOMAIN0 scans the panel, so a powered reference
         * with one gated neighbour is the shape the experiment wants, and
         * the guard must not refuse it. */
        st[0] = 0x40000000u;   /* reference, PGFSM 01 */
        st[1] = 0x80000000u;   /* unlike it, PGFSM 10 */
        st[2] = 0x40000000u;
        st[3] = 0x40000000u;
        CHECK(rb_pg_sample_readable(st, cf) == 1,
              "a plausible live sample passes - the guard is not a constant 0");
        CHECK(rb_pg_candidate(st) == 1,
              "and the candidate is still DOMAIN1, so the guard does not "
              "disturb the verdict it protects");

        CHECK(rb_pg_sample_readable(0, cf) == 0,
              "a NULL array is unreadable rather than a dereference");
        CHECK(rb_pg_sample_readable(st, 0) == 0,
              "either NULL refuses");
    }

    /* 2DE: BOOT 437's OWN SAMPLE, which is why this milestone exists.
     * All four pipe domains read 0x10000000 identically - DESIRED set, PGFSM
     * 00 - so there is no gated pipe and a power-UP has nothing to act on.
     * The test must stay unavailable on that sample and become available the
     * moment a DOMAIN16..18 row differs. */
    {
        uint32_t st[RB_PG_DOMAIN_COUNT];
        uint32_t cf[RB_PG_DOMAIN_COUNT];
        uint32_t i;

        for (i = 0; i < RB_PG_DOMAIN_COUNT; i++) {
            st[i] = 0x10000000u;
            cf[i] = 0x00000000u;
        }
        CHECK(rb_pg_sample_readable(st, cf) == 1,
              "boot 437's sample is readable - 0x10000000 is a real word");
        CHECK(rb_pg_candidate(st) == -1,
              "with every domain in the reference state the test is NOT "
              "AVAILABLE, which is exactly refusal 24 on boot 437");

        /* One gated DOMAIN16 and the whole thing becomes available - with no
         * power-DOWN of a working pipe, which is the alternative 2DE exists
         * to avoid. */
        st[4] = 0x40000000u;
        CHECK(rb_pg_candidate(st) == 4,
              "a DOMAIN16 unlike the reference is found, at SLOT 4");
        CHECK(rb_pg_slot_inst((uint32_t)rb_pg_candidate(st)) == 16u,
              "and the command carries DOMAIN NUMBER 16, not the slot 4 - "
              "the conflation this file is built to prevent");
        CHECK(rb_pg_sel_refuse(
                  rb_pg_slot_inst((uint32_t)rb_pg_candidate(st)))
                  == RB_PG_SEL_OK,
              "the number the candidate produces survives the hand-named "
              "selector, so the auto path cannot pick something the manual "
              "path would refuse");

        /* A pipe gated as well: the FIRST unlike domain wins, and it is a
         * pipe, so DOMAIN16 is not preferred over a nearer candidate. */
        st[2] = 0x40000000u;
        CHECK(rb_pg_candidate(st) == 2,
              "the first unlike domain in slot order is still the answer");

        /* The encoder must carry 16 as 16. */
        {
            uint8_t buf[RB_DMUB_CMD_SIZE];
            rb_dmub_build_domain_control(buf, 16u, 0u);
            CHECK(buf[4] == 0x10u,
                  "DOMAIN16 power-up packs as 0x10 - inside the 6-bit field "
                  "and clear of the power_gate bit at 0x40");
            CHECK(rb_dmub_domain_inst_valid(18u) == 1,
                  "DOMAIN18 fits the payload field");
        }
    }

#undef CHECK
    return bad;
}
