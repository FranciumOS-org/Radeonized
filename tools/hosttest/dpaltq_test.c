/* 2EA: QUERY_DP_ALT (DMUB_CMD__VBIOS sub-type 26).
 *
 * What this pins, and why each is here rather than reasoned about:
 *  - the payload size is 4, NOT the transmitter's 60: Linux sizes this one
 *    off its own data struct and the two commands really do declare
 *    different sizes (2E2), so a copied constant would be wrong;
 *  - the OUT bytes are pre-filled with a sentinel, so NO_REPLY has its own
 *    code and cannot read as a real reply of three zeros;
 *  - the verdict's control is 2E1's ROM mask, an independent source.
 */
#include <stdio.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>

#include "rb_dpaltq.h"

int dpaltq_tests(void);

/* A reply the way the firmware would leave it: our header with RET cleared
 * and the three OUT bytes written. */
static void answer(uint8_t *buf, uint32_t phy, int usb, int dis, int dp4)
{
    rb_dpaltq_build(buf, phy, RB_DPALTQ_FILL);
    rb_dmub_put32(buf, 0, rb_dmub_get32(buf, 0) & ~RB_DMUB_HDR_RET_STATUS);
    buf[4u + RB_DPALTQ_OFF_IS_USB]      = (uint8_t)usb;
    buf[4u + RB_DPALTQ_OFF_ALT_DISABLE] = (uint8_t)dis;
    buf[4u + RB_DPALTQ_OFF_IS_DP4]      = (uint8_t)dp4;
}

int dpaltq_tests(void)
{
    int bad = 0;
#define CHECK(c, m) do { if (!(c)) { printf("FAIL dpaltq: %s\n", (m)); bad++; } } while (0)

    /* ---- the wire format ---- */
    {
        uint8_t b[RB_DMUB_CMD_SIZE];
        uint32_t h;

        rb_dpaltq_build(b, 2u, RB_DPALTQ_FILL);
        h = rb_dmub_get32(b, 0);

        CHECK(rb_dmub_hdr_type(h) == RB_DMUB_CMD_VBIOS,
              "type is DMUB_CMD__VBIOS (128)");
        CHECK(rb_dmub_hdr_sub(h) == 26u,
              "sub-type 26 is TRANSMITTER_QUERY_DP_ALT");
        CHECK(rb_dmub_hdr_payload(h) == 4u,
              "payload_bytes is 4 - sizeof(query_dp_alt.data), NOT the "
              "transmitter's union-sized 60 (2E2)");
        CHECK(rb_dmub_hdr_ret(h) == 1u,
              "RET_STATUS is set on the way out, as the caps query does");

        CHECK(b[4u + RB_DPALTQ_OFF_PHYID] == 2u, "phy_id is the IN field");
        CHECK(b[4u + RB_DPALTQ_OFF_IS_USB] == RB_DPALTQ_FILL &&
              b[4u + RB_DPALTQ_OFF_ALT_DISABLE] == RB_DPALTQ_FILL &&
              b[4u + RB_DPALTQ_OFF_IS_DP4] == RB_DPALTQ_FILL,
              "all three OUT bytes carry the sentinel");

        /* Nothing beyond the payload. A stray byte in a 64-byte slot is
         * what a struct crossing the boundary would produce. */
        {
            unsigned i, nz = 0;
            for (i = 8u; i < RB_DMUB_CMD_SIZE; i++) { if (b[i]) { nz++; } }
            CHECK(nz == 0u, "bytes past the 4-byte payload are zero");
        }

        rb_dpaltq_build(b, 0u, RB_DPALTQ_FILL_LINUX);
        CHECK(b[4u + RB_DPALTQ_OFF_IS_USB] == 0u,
              "the Linux-exact control fills with 0, byte for byte");
    }

    /* ---- the decode, and the sentinel's whole point ---- */
    {
        uint8_t b[RB_DMUB_CMD_SIZE];
        struct rb_dpaltq_reply r;

        /* Nothing answered: the slot comes back as we wrote it. */
        rb_dpaltq_build(b, 2u, RB_DPALTQ_FILL);
        CHECK(rb_dpaltq_decode(b, 2u, RB_DPALTQ_FILL, &r) ==
              RB_DPALTQ_NO_REPLY,
              "three sentinels back is NO_REPLY, not a reply of zeros");
        CHECK(!rb_dpaltq_ret_cleared(b),
              "and RET_STATUS is still set on an unanswered slot");

        /* THE CONTROL FOR THE DEVIATION, and it refuted the comment that
         * introduced it. The sentinel test compares against `fill`, so
         * under Linux's zero fill it fires on an unanswered slot too - but
         * it ALSO fires on a genuine reply of three zeros, which is a
         * perfectly possible answer (a non-USB-C phy). So the zero fill
         * cannot tell the two apart in either direction; the sentinel's
         * value is that under it NO_REPLY is a TRUE statement. */
        rb_dpaltq_build(b, 2u, RB_DPALTQ_FILL_LINUX);
        CHECK(rb_dpaltq_decode(b, 2u, RB_DPALTQ_FILL_LINUX, &r) ==
              RB_DPALTQ_NO_REPLY,
              "under the zero fill an unanswered slot reads NO_REPLY");
        answer(b, 2u, 0, 0, 0);
        CHECK(rb_dpaltq_decode(b, 2u, RB_DPALTQ_FILL_LINUX, &r) ==
              RB_DPALTQ_NO_REPLY,
              "and so does a GENUINE all-zero reply - the zero fill cannot "
              "separate them, which is the whole reason for the sentinel");
        /* The same genuine reply under the sentinel is read correctly. */
        CHECK(rb_dpaltq_decode(b, 2u, RB_DPALTQ_FILL, &r) == RB_DPALTQ_OK &&
              r.is_usb == 0u,
              "the sentinel run reads that same reply as the answer it is");

        /* A real answer. */
        answer(b, 2u, 1, 0, 0);
        CHECK(rb_dpaltq_decode(b, 2u, RB_DPALTQ_FILL, &r) == RB_DPALTQ_OK,
              "a written reply decodes");
        CHECK(r.is_usb == 1u && r.is_dp_alt_disable == 0u && r.is_dp4 == 0u,
              "fields land in their own offsets");
        CHECK(rb_dpaltq_ret_cleared(b),
              "the firmware clearing RET_STATUS is readable separately");

        /* Register noise announces itself rather than decoding. */
        answer(b, 2u, 1, 0, 0);
        b[4u + RB_DPALTQ_OFF_IS_DP4] = 0x5au;
        CHECK(rb_dpaltq_decode(b, 2u, RB_DPALTQ_FILL, &r) ==
              RB_DPALTQ_BAD_REPLY, "a byte that is neither 0 nor 1 refuses");

        /* A reply written for a different phy means the slot is not ours. */
        answer(b, 3u, 1, 0, 0);
        CHECK(rb_dpaltq_decode(b, 2u, RB_DPALTQ_FILL, &r) ==
              RB_DPALTQ_PHY_ECHO, "a foreign phy_id echo refuses");

        /* ...but the echo check must not fire on a slot that simply did not
         * answer: phy_id is an IN field and is unchanged there, so NO_REPLY
         * has to win. That ordering is the test. */
        rb_dpaltq_build(b, 2u, RB_DPALTQ_FILL);
        b[4u + RB_DPALTQ_OFF_PHYID] = 2u;
        CHECK(rb_dpaltq_decode(b, 2u, RB_DPALTQ_FILL, &r) ==
              RB_DPALTQ_NO_REPLY, "NO_REPLY is decided before the echo");

        CHECK(rb_dpaltq_decode(NULL, 2u, RB_DPALTQ_FILL, &r) ==
              RB_DPALTQ_BAD_REPLY, "a NULL slot refuses");
        CHECK(rb_dpaltq_decode(b, 2u, RB_DPALTQ_FILL, NULL) ==
              RB_DPALTQ_BAD_REPLY, "a NULL output refuses");
    }

    /* ---- the sweep verdict ---- */
    {
        struct rb_dpaltq_reply r[RB_DPALTQ_PHY_COUNT];
        uint32_t refuse[RB_DPALTQ_PHY_COUNT];
        unsigned i;

        /* 2E1's ROM read, stated as the mask the journal records. */
        CHECK(RB_DPALTQ_ROM_USB_MASK == 0x1Cu,
              "DP_IS_USB_C set on display paths 2/3/4 and clear on 0/1 "
              "(2E1, firmware/680m-vbios.rom)");

        memset(r, 0, sizeof(r));
        for (i = 0; i < RB_DPALTQ_PHY_COUNT; i++) {
            refuse[i] = RB_DPALTQ_NO_REPLY;
        }
        CHECK(rb_dpaltq_verdict(r, refuse) == RB_DPALTQ_V_NONE,
              "nothing answered is NONE");
        CHECK(rb_dpaltq_usb_mask(r, refuse) == 0u,
              "and the mask is empty, not built from unanswered replies");

        /* All five answered with the ROM's pattern: the reply path is
         * verified against an independent source. */
        for (i = 0; i < RB_DPALTQ_PHY_COUNT; i++) {
            refuse[i] = RB_DPALTQ_OK;
            r[i].phy_id = i;
            r[i].is_usb = (i >= 2u) ? 1u : 0u;
        }
        CHECK(rb_dpaltq_usb_mask(r, refuse) == 0x1Cu, "usb mask is 0x1C");
        CHECK(rb_dpaltq_verdict(r, refuse) == RB_DPALTQ_V_ROM_MATCH,
              "answered and matching the ROM is ROM_MATCH");

        /* 2BF's own control: uniform is_usb carries no information. */
        for (i = 0; i < RB_DPALTQ_PHY_COUNT; i++) { r[i].is_usb = 1u; }
        CHECK(rb_dpaltq_verdict(r, refuse) == RB_DPALTQ_V_UNIFORM,
              "all five USB-C is a refusal, not a finding");
        for (i = 0; i < RB_DPALTQ_PHY_COUNT; i++) { r[i].is_usb = 0u; }
        CHECK(rb_dpaltq_verdict(r, refuse) == RB_DPALTQ_V_UNIFORM,
              "none USB-C is the same refusal");

        /* A partial answer that is non-uniform is a real reading, and it is
         * NOT a ROM match - the ROM pattern is a claim about all five. */
        for (i = 0; i < RB_DPALTQ_PHY_COUNT; i++) {
            refuse[i] = (i < 3u) ? RB_DPALTQ_OK : RB_DPALTQ_NO_REPLY;
            r[i].is_usb = (i == 2u) ? 1u : 0u;
        }
        CHECK(rb_dpaltq_verdict(r, refuse) == RB_DPALTQ_V_DISAGREES,
              "three answered, phy 2 USB-C: a reading, but not ROM_MATCH");
        CHECK(rb_dpaltq_usb_mask(r, refuse) == 0x04u,
              "the mask covers only the phys that answered");

        /* FOUR OF FIVE, with those four matching the ROM. This case exists
         * because the "answered == all five" term in ROM_MATCH fired on
         * NOTHING when it was first broken (2E7's rule: a break that
         * changes no assertion is a result about the guard). It is
         * reachable: usb can only name answered phys, so usb == 0x1C needs
         * phys 2,3,4 answered - and with phy 0 answering too the set is
         * 0x1D, non-uniform and not complete. ROM_MATCH is a claim about
         * all five, so four agreeing is a reading and not the verification. */
        for (i = 0; i < RB_DPALTQ_PHY_COUNT; i++) {
            refuse[i] = (i == 1u) ? RB_DPALTQ_NO_REPLY : RB_DPALTQ_OK;
            r[i].is_usb = (i >= 2u) ? 1u : 0u;
        }
        CHECK(rb_dpaltq_usb_mask(r, refuse) == 0x1Cu,
              "four answered, the ROM's three USB-C phys among them");
        CHECK(rb_dpaltq_verdict(r, refuse) == RB_DPALTQ_V_DISAGREES,
              "four of five is NOT the ROM verification - the ROM read "
              "covers all five display paths");

        /* One answered phy is uniform by construction. */
        for (i = 0; i < RB_DPALTQ_PHY_COUNT; i++) {
            refuse[i] = (i == 2u) ? RB_DPALTQ_OK : RB_DPALTQ_NO_REPLY;
        }
        CHECK(rb_dpaltq_verdict(r, refuse) == RB_DPALTQ_V_UNIFORM,
              "a single answered phy cannot be non-uniform");
    }

    /* ---- the question the sweep exists for ---- */
    {
        struct rb_dpaltq_reply r[RB_DPALTQ_PHY_COUNT];
        uint32_t refuse[RB_DPALTQ_PHY_COUNT];
        unsigned i;

        memset(r, 0, sizeof(r));
        for (i = 0; i < RB_DPALTQ_PHY_COUNT; i++) {
            refuse[i] = RB_DPALTQ_OK;
            r[i].phy_id = i;
            r[i].is_usb = (i >= 2u) ? 1u : 0u;
        }

        r[2].is_dp_alt_disable = 1u;
        CHECK(rb_dpaltq_dp_granted(r, refuse, 2u) == 0,
              "alt disabled on the Type-C phy: DP is NOT granted, which "
              "would agree with RDPCSTX2_PHY_CNTL6 and make the external "
              "display a PD/firmware matter");
        r[2].is_dp_alt_disable = 0u;
        CHECK(rb_dpaltq_dp_granted(r, refuse, 2u) == 1,
              "alt enabled: DP IS granted and 2C1's register is the wrong "
              "one for this part");

        /* -1, never a plausible 0 or 1, wherever it cannot say. */
        CHECK(rb_dpaltq_dp_granted(r, refuse, 0u) == -1,
              "the panel's phy is not USB-C, so the question does not apply");
        refuse[2] = RB_DPALTQ_NO_REPLY;
        CHECK(rb_dpaltq_dp_granted(r, refuse, 2u) == -1,
              "a phy that did not answer cannot say");
        refuse[2] = RB_DPALTQ_OK;
        CHECK(rb_dpaltq_dp_granted(r, refuse, RB_DPALTQ_PHY_COUNT) == -1,
              "an out-of-range phy refuses rather than reading past the end");
        CHECK(rb_dpaltq_dp_granted(NULL, refuse, 2u) == -1, "NULL refuses");
    }

#undef CHECK
    return bad;
}
