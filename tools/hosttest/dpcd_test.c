/* SPDX-License-Identifier: MIT */
/* 2BA: the native AUX read of DPCD. Every constant checked against the file
 * it came from, and the refusals checked from both sides - this is the first
 * write this project makes outside the panel's own pipe.
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "rb_aux.h"
#include "rb_dpcd.h"

int dpcd_tests(void);

int dpcd_tests(void)
{
    int bad = 0;

    /* THE ARBITRATION REFUSAL, which is what makes this attemptable: Linux
     * declines on exactly DMCU_CAN_ACCESS_AUX (2) and so must we. Boot 293
     * read arb 0 on both live engines, so owner 0 must be allowed or the
     * read can never run. */
    {
        if (rb_aux_arb_owner(0x00000000u) != 0u ||
            rb_aux_arb_owner(0x00000004u) != 1u ||
            rb_aux_arb_owner(0x00000008u) != 2u ||
            rb_aux_arb_owner(0x0000000Cu) != 3u) {
            printf("FAIL dpcd: RW_STATUS is not bits 2-3\n"); bad++;
        }
        if (!rb_aux_may_acquire(0x00000000u)) {
            printf("FAIL dpcd: refused the live arb word 0 - boot 293's own "
                   "reading, so the read could never run\n"); bad++;
        }
        if (!rb_aux_may_acquire(0x00000004u)) {
            printf("FAIL dpcd: refused SW_CAN_ACCESS\n"); bad++;
        }
        if (rb_aux_may_acquire(0x00000008u)) {
            printf("FAIL dpcd: ACQUIRED an engine the DMCUB owns\n"); bad++;
        }
        /* The DMCU value must be refused whatever else is set in the word. */
        if (rb_aux_may_acquire(0xffffffffu & ~0x4u)) {
            printf("FAIL dpcd: DMCU refusal depends on other bits\n"); bad++;
        }
        /* Acquire sets REQ and clears DONE_USING; release does the reverse,
         * and each must preserve the rest of the word (the priority and the
         * DMCU's own bits are not ours to clear). */
        const uint32_t keep = 0x01000003u;
        if (rb_aux_arb_acquire_word(keep) != (keep | 0x00010000u)) {
            printf("FAIL dpcd: acquire word 0x%x\n",
                   rb_aux_arb_acquire_word(keep)); bad++;
        }
        if (rb_aux_arb_release_word(keep | 0x00010000u) !=
            (keep | 0x00020000u)) {
            printf("FAIL dpcd: release word 0x%x\n",
                   rb_aux_arb_release_word(keep | 0x00010000u)); bad++;
        }
    }

    /* The request bytes, against dce_aux.c's COMPOSE_AUX_SW_DATA_* and
     * dc_ddc_types.h's DP_READ = 0x90. */
    {
        uint8_t req[4];
        memset(req, 0xaa, sizeof(req));
        if (rb_dpcd_read_request(0x00000u, 16u, req, sizeof(req)) != 4u) {
            printf("FAIL dpcd: a 16-byte read of 0 was refused\n"); bad++;
        }
        if (req[0] != 0x90u || req[1] != 0x00u || req[2] != 0x00u ||
            req[3] != 0x0fu) {
            printf("FAIL dpcd: request %02x %02x %02x %02x want 90 00 00 0f\n",
                   req[0], req[1], req[2], req[3]); bad++;
        }
        /* A 20-bit address, so the top nibble rides in byte 0 with the
         * action - the one part of the layout easy to get wrong. */
        if (rb_dpcd_read_request(0xABCDEu, 1u, req, sizeof(req)) != 4u ||
            req[0] != 0x9Au || req[1] != 0xBCu || req[2] != 0xDEu ||
            req[3] != 0x00u) {
            printf("FAIL dpcd: 0xABCDE gave %02x %02x %02x %02x\n",
                   req[0], req[1], req[2], req[3]); bad++;
        }
        /* Refusals: a zero length, over the buffer, past 20 bits, no room. */
        if (rb_dpcd_read_request(0u, 0u, req, sizeof(req)) != 0u ||
            rb_dpcd_read_request(0u, 17u, req, sizeof(req)) != 0u ||
            rb_dpcd_read_request(0x100000u, 1u, req, sizeof(req)) != 0u ||
            rb_dpcd_read_request(0u, 1u, req, 3u) != 0u ||
            rb_dpcd_read_request(0u, 1u, 0, 4u) != 0u) {
            printf("FAIL dpcd: a bad request was accepted\n"); bad++;
        }
    }

    /* The register words we will actually store. */
    {
        if (rb_aux_swc_request_word(4u) != 0x00040000u) {
            printf("FAIL dpcd: WR_BYTES word 0x%x want 0x40000\n",
                   rb_aux_swc_request_word(4u)); bad++;
        }
        /* WR_BYTES is 5 bits, so a count that would overflow the field must
         * not corrupt the neighbours. */
        if ((rb_aux_swc_request_word(0xffu) & ~RB_AUX_SWC_WR_BYTES_MASK)
            != 0u) {
            printf("FAIL dpcd: WR_BYTES overflowed its field\n"); bad++;
        }
        if (rb_aux_swd_write_word(0u, 0x90u, 1) != 0x80009000u) {
            printf("FAIL dpcd: SW_DATA first word 0x%x\n",
                   rb_aux_swd_write_word(0u, 0x90u, 1)); bad++;
        }
        if (rb_aux_swd_write_word(0u, 0x0fu, 0) != 0x00000f00u) {
            printf("FAIL dpcd: SW_DATA later word 0x%x\n",
                   rb_aux_swd_write_word(0u, 0x0fu, 0)); bad++;
        }
        /* DATA_RW must be 0 on every write word, or the engine reads where
         * we meant to write. */
        if ((rb_aux_swd_write_word(0u, 0xffu, 1) & RB_AUX_SWD_RW) != 0u) {
            printf("FAIL dpcd: a write word set DATA_RW\n"); bad++;
        }
        if ((rb_aux_swd_read_setup_word() & RB_AUX_SWD_RW) == 0u) {
            printf("FAIL dpcd: the read setup did not set DATA_RW\n"); bad++;
        }
        if (rb_aux_swd_byte(0x0000ab00u) != 0xabu) {
            printf("FAIL dpcd: byte extraction\n"); bad++;
        }
    }

    /* The reply. */
    {
        if (rb_dpcd_reply_code(0x00u) != RB_DPCD_REPLY_ACK ||
            rb_dpcd_reply_code(0x10u) != RB_DPCD_REPLY_NACK ||
            rb_dpcd_reply_code(0x20u) != RB_DPCD_REPLY_DEFER) {
            printf("FAIL dpcd: reply code is not the high nibble\n"); bad++;
        }
        /* An ACK with ONE replied byte is the status byte and no payload -
         * a legitimate outcome, not an error. */
        if (rb_dpcd_payload_bytes(1u) != 0u ||
            rb_dpcd_payload_bytes(17u) != 16u ||
            rb_dpcd_payload_bytes(0u) != 0u) {
            printf("FAIL dpcd: payload count\n"); bad++;
        }
    }

    /* THE SIGNATURE OF THE ONE THING NOT REPRODUCED: a stuck AUX_SW_DATA
     * index returns the same byte every time. A real DPCD 0x00 block does
     * not, so this must flag the former and pass the latter. */
    {
        const uint8_t stuck[4] = { 0x11, 0x11, 0x11, 0x11 };
        const uint8_t real[4]  = { 0x14, 0x1e, 0x84, 0x01 };
        if (!rb_dpcd_reply_suspect_norepeat(stuck, 4u)) {
            printf("FAIL dpcd: a stuck index was not flagged\n"); bad++;
        }
        if (rb_dpcd_reply_suspect_norepeat(real, 4u)) {
            printf("FAIL dpcd: a real DPCD block was flagged\n"); bad++;
        }
        /* One byte cannot be judged either way, and must not read as stuck. */
        if (rb_dpcd_reply_suspect_norepeat(real, 1u) ||
            rb_dpcd_reply_suspect_norepeat(0, 4u)) {
            printf("FAIL dpcd: judged a reply too short to judge\n"); bad++;
        }
        /* All zeros IS all-the-same, and a sink that replies 16 zeros is
         * indistinguishable from a stuck index - so it must be flagged, not
         * excused. */
        const uint8_t zeros[8] = { 0 };
        if (!rb_dpcd_reply_suspect_norepeat(zeros, 8u)) {
            printf("FAIL dpcd: all-zero reply not flagged\n"); bad++;
        }
    }

    /* The revision plausibility control: real sinks report 1.0-1.4. */
    {
        if (!rb_dpcd_rev_plausible(0x11u) || !rb_dpcd_rev_plausible(0x14u) ||
            !rb_dpcd_rev_plausible(0x10u)) {
            printf("FAIL dpcd: a real DPCD revision was refused\n"); bad++;
        }
        if (rb_dpcd_rev_plausible(0x00u) || rb_dpcd_rev_plausible(0xffu) ||
            rb_dpcd_rev_plausible(0x21u) || rb_dpcd_rev_plausible(0x15u)) {
            printf("FAIL dpcd: register noise passed as a revision\n"); bad++;
        }
        if (rb_dpcd_rev_major(0x14u) != 1u || rb_dpcd_rev_minor(0x14u) != 4u) {
            printf("FAIL dpcd: BCD revision split\n"); bad++;
        }
        /* DP_MAX_LANE_COUNT is the low 5 bits - the upper bits are
         * capability flags and reading them as a lane count would give 129
         * lanes from a perfectly ordinary sink. */
        if (rb_dpcd_lane_count(0x84u) != 4u ||
            rb_dpcd_lane_count(0x81u) != 1u) {
            printf("FAIL dpcd: lane count is not the low 5 bits\n"); bad++;
        }
    }

    /* 2BC: the general encoder, the I2C actions and the EDID checks. */
    {
        uint8_t req[4];
        /* A ZERO length gives THREE bytes, not four - dce_aux.c's
         * `length = request->length ? 4 : 3`. 2BA's DPCD-only encoder refused
         * length 0, which would have made the MOT protocol unexpressible. */
        memset(req, 0xaa, sizeof(req));
        if (rb_aux_request_bytes(RB_I2C_ACTION_READ, 0x50u, 0u,
                                 req, sizeof(req)) != 3u) {
            printf("FAIL dpcd: a zero-length request was not 3 bytes\n"); bad++;
        }
        if (req[0] != 0x10u || req[1] != 0x00u || req[2] != 0x50u ||
            req[3] != 0xaau) {
            printf("FAIL dpcd: zero-length wrote a 4th byte or a bad header\n");
            bad++;
        }
        /* The four I2C actions, against dc_ddc_types.h as vendored. */
        if (RB_I2C_ACTION_WRITE != 0x00u || RB_I2C_ACTION_READ != 0x10u ||
            RB_I2C_ACTION_WRITE_MOT != 0x40u ||
            RB_I2C_ACTION_READ_MOT != 0x50u) {
            printf("FAIL dpcd: an I2C action constant is wrong\n"); bad++;
        }
        /* The EDID slave address shares its value with READ_MOT (0x50), which
         * is a coincidence worth pinning so a future edit cannot swap them. */
        if (RB_EDID_I2C_ADDR != 0x50u || RB_EDID_BLOCK_LEN != 128u) {
            printf("FAIL dpcd: EDID address or block length\n"); bad++;
        }
        if (rb_aux_request_bytes(RB_I2C_ACTION_READ_MOT, 0x50u, 16u,
                                 req, sizeof(req)) != 4u ||
            req[0] != 0x50u || req[2] != 0x50u || req[3] != 0x0fu) {
            printf("FAIL dpcd: READ_MOT request %02x %02x %02x %02x\n",
                   req[0], req[1], req[2], req[3]); bad++;
        }
        /* The low nibble of byte 0 belongs to the ADDRESS, so an action that
         * sets it would silently corrupt the top address nibble. */
        if (rb_aux_request_bytes(0x91u, 0u, 1u, req, sizeof(req)) != 0u) {
            printf("FAIL dpcd: an action with a low nibble was accepted\n");
            bad++;
        }
        /* And a 3-byte buffer is enough for a zero-length request but not a
         * sized one - the bound differs by form. */
        if (rb_aux_request_bytes(RB_I2C_ACTION_READ, 0x50u, 0u, req, 3u) != 3u ||
            rb_aux_request_bytes(RB_I2C_ACTION_READ, 0x50u, 1u, req, 3u) != 0u) {
            printf("FAIL dpcd: out_len bound wrong for one of the forms\n");
            bad++;
        }
    }

    /* THE MOT CHOICE, which is the one that affects the sink's own bus state:
     * every chunk but the last keeps the I2C transaction open, and the last
     * MUST drop MOT or the sink's DDC bus stays held and the next reader
     * finds it busy. */
    {
        for (uint32_t c = 0; c < 7u; c++) {
            if (rb_edid_read_action(c, 8u) != RB_I2C_ACTION_READ_MOT) {
                printf("FAIL dpcd: chunk %u of 8 dropped MOT early\n", c);
                bad++;
            }
        }
        if (rb_edid_read_action(7u, 8u) != RB_I2C_ACTION_READ) {
            printf("FAIL dpcd: the LAST chunk kept MOT set\n"); bad++;
        }
        /* A single-chunk read is the last chunk, so it must not set MOT. */
        if (rb_edid_read_action(0u, 1u) != RB_I2C_ACTION_READ) {
            printf("FAIL dpcd: a one-chunk read set MOT\n"); bad++;
        }
        if (!rb_dpcd_reply_is_defer(RB_DPCD_REPLY_DEFER) ||
            rb_dpcd_reply_is_defer(RB_DPCD_REPLY_ACK)) {
            printf("FAIL dpcd: defer detection\n"); bad++;
        }
    }

    /* The two EDID checks, which must be INDEPENDENT: a block can pass the
     * header and fail the sum, which is exactly what a partial chunked read
     * looks like. */
    {
        uint8_t e[128];
        memset(e, 0, sizeof(e));
        e[0] = 0x00; e[1] = 0xff; e[2] = 0xff; e[3] = 0xff;
        e[4] = 0xff; e[5] = 0xff; e[6] = 0xff; e[7] = 0x00;
        /* Make the block sum to zero mod 256 by setting the checksum byte. */
        uint32_t sum = 0;
        for (unsigned i = 0; i < 127u; i++) { sum += e[i]; }
        e[127] = (uint8_t)((256u - (sum & 0xffu)) & 0xffu);
        if (!rb_edid_header_ok(e, 128u)) {
            printf("FAIL dpcd: a valid EDID header was rejected\n"); bad++;
        }
        if (!rb_edid_checksum_ok(e, 128u)) {
            printf("FAIL dpcd: a valid EDID checksum was rejected\n"); bad++;
        }
        /* One byte of the header wrong is not an EDID. */
        e[3] = 0xfe;
        if (rb_edid_header_ok(e, 128u)) {
            printf("FAIL dpcd: a broken header passed\n"); bad++;
        }
        e[3] = 0xff;
        /* HEADER OK, SUM WRONG - the partial-read case, and the reason both
         * checks exist. Without this the two could be one function. */
        e[50] ^= 0x01;
        if (!rb_edid_header_ok(e, 128u)) {
            printf("FAIL dpcd: header check depends on the payload\n"); bad++;
        }
        if (rb_edid_checksum_ok(e, 128u)) {
            printf("FAIL dpcd: a corrupt block passed the checksum\n"); bad++;
        }
        if (rb_edid_header_ok(e, 4u) ||
            rb_edid_checksum_ok(0, 128u) || rb_edid_header_ok(0, 128u)) {
            printf("FAIL dpcd: a short or NULL block passed a check\n"); bad++;
        }
        e[50] ^= 0x01;   /* restore the valid block */
        if (!rb_edid_checksum_ok(e, 128u)) {
            printf("FAIL dpcd: the restore of the test block failed\n"); bad++;
        }
        /* THE LENGTH GUARD, ISOLATED. The first version of this case asserted
         * rb_edid_checksum_ok(e, 64) on a CORRUPT block and passed whether or
         * not the guard existed - the function sums a fixed 128 bytes and the
         * corruption made it fail for the wrong reason, so removing the guard
         * caused no FAIL. It had to be asserted on a block whose 128 bytes ARE
         * valid, where only the guard can make it refuse. That is rule 26 on a
         * test: a case that passes for the wrong reason is not coverage, and
         * the break-it check is what exposed it. */
        if (rb_edid_checksum_ok(e, 127u)) {
            printf("FAIL dpcd: a 127-byte partial read passed the checksum\n");
            bad++;
        }
        if (rb_edid_checksum_ok(e, 0u) || rb_edid_checksum_ok(e, 64u)) {
            printf("FAIL dpcd: a short partial read passed the checksum\n");
            bad++;
        }
        /* An all-zero block has a zero sum - so the checksum ALONE would
         * accept 128 zeros, which is why the header check is not optional. */
        memset(e, 0, sizeof(e));
        if (!rb_edid_checksum_ok(e, 128u)) {
            printf("FAIL dpcd: the zero-block checksum changed\n"); bad++;
        }
        if (rb_edid_header_ok(e, 128u)) {
            printf("FAIL dpcd: 128 zeros passed the header check\n"); bad++;
        }
    }

    /* ============== 2F3: THE NATIVE DPCD WRITE =========================
     *
     * The first write this project makes to the SINK. These cases carry the
     * safety case, so each is a refusal that must fire for its OWN reason -
     * a write refused incidentally by a later gate is rule 48's "passes for
     * the wrong reason" and would read like a check that had been done.
     */
#define WCHK(cond, msg) do { if (!(cond)) { \
        printf("FAIL dpcd/2F3: %s\n", (msg)); bad++; } } while (0)
    {
        /* ---------------- THE BUG THAT WAS THERE ----------------------
         *
         * auxTransact tested `action == I2C_WRITE || action ==
         * I2C_WRITE_MOT` by equality, so a native DP write would have been
         * encoded with the READ length and NO payload - and silently, since
         * the engine ACKs a well-formed 4-byte request that writes nothing.
         * This predicate is the fix; these five lines are why it exists. */
        WCHK(rb_aux_action_is_write(RB_DPCD_ACTION_WRITE),
             "a native DP write is a write - the whole point of 2F3");
        WCHK(!rb_aux_action_is_write(RB_DPCD_ACTION_READ),
             "a native DP read is not");
        WCHK(rb_aux_action_is_write(RB_I2C_ACTION_WRITE) &&
             rb_aux_action_is_write(RB_I2C_ACTION_WRITE_MOT),
             "both I2C write forms still are - 2BC's EDID offset byte "
             "depends on it");
        WCHK(!rb_aux_action_is_write(RB_I2C_ACTION_READ) &&
             !rb_aux_action_is_write(RB_I2C_ACTION_READ_MOT),
             "and neither I2C read form is");

        /* ---------------- THE ALLOWLIST -------------------------------
         *
         * Named addresses, so the test says WHICH register it accepted
         * rather than which number (rule 17's shape: a bare constant cannot
         * say what it is). Everything training sends must pass. */
        WCHK(rb_dpcd_write_allowed(RB_DPCD_LINK_BW_SET) &&
             rb_dpcd_write_allowed(RB_DPCD_LANE_COUNT_SET) &&
             rb_dpcd_write_allowed(RB_DPCD_TRAINING_PATTERN_SET) &&
             rb_dpcd_write_allowed(RB_DPCD_DOWNSPREAD_CTRL) &&
             rb_dpcd_write_allowed(RB_DPCD_LINK_CODING_SET) &&
             rb_dpcd_write_allowed(RB_DPCD_SET_POWER),
             "every register link training writes is allowed");
        for (uint32_t a = RB_DPCD_TRAINING_LANE0_SET;
             a <= RB_DPCD_TRAINING_LANE3_SET; a++) {
            WCHK(rb_dpcd_write_allowed(a),
                 "all four TRAINING_LANEn_SET are allowed");
        }
        /* The read-only blocks, by name. A sink would NACK these; being
         * refused HERE is what makes the allowlist the instrument. */
        WCHK(!rb_dpcd_write_allowed(RB_DPCD_REV_ADDR) &&
             !rb_dpcd_write_allowed(RB_DPCD_MAX_LINK_RATE_ADDR),
             "the receiver capability block is NOT writable");
        WCHK(!rb_dpcd_write_allowed(RB_DPCD_LANE0_1_STATUS) &&
             !rb_dpcd_write_allowed(RB_DPCD_LANE_ALIGN_STATUS) &&
             !rb_dpcd_write_allowed(RB_DPCD_ADJUST_REQUEST_LANE0_1),
             "nor is the link status block training READS");
        /* The addresses either side of the allowed run, so the bounds are
         * pinned rather than the interior only. */
        WCHK(!rb_dpcd_write_allowed(0x0ffu) &&
             !rb_dpcd_write_allowed(0x109u),
             "the allowed run is bounded at both ends - 0x0ff and 0x109 are "
             "refused");
        WCHK(!rb_dpcd_write_allowed(RB_DPCD_SET_POWER - 1u) &&
             !rb_dpcd_write_allowed(RB_DPCD_SET_POWER + 1u),
             "and SET_POWER is a single address, not a run");
        WCHK(!rb_dpcd_write_allowed(0xfffffu) &&
             !rb_dpcd_write_allowed(0x700u),
             "the eDP block and the top of the 20-bit space are refused");

        /* ---------------- THE ENCODER ---------------------------------
         *
         * dce_aux.c's COMPOSE macros, for a one-byte write to LINK_BW_SET.
         * The action's low nibble must be clear or it would corrupt the
         * address, which rb_aux_request_bytes already refuses - asserted
         * here against the real action byte rather than assumed. */
        {
            uint8_t req[4] = { 0, 0, 0, 0 };
            const uint32_t n = rb_dpcd_write_request(RB_DPCD_LINK_BW_SET, 1u,
                                                     req, sizeof(req));
            WCHK(n == 4u, "a one-byte DPCD write composes a four-byte header");
            WCHK(req[0] == (uint8_t)RB_DPCD_ACTION_WRITE,
                 "byte 0 is the DP_WRITE action, address bits 19:16 being 0");
            WCHK(req[1] == 0x01u && req[2] == 0x00u,
                 "bytes 1-2 are the address, 0x100, big-endian");
            WCHK(req[3] == 0x00u, "byte 3 is length - 1");
            WCHK((RB_DPCD_ACTION_WRITE & 0x0fu) == 0u,
                 "and the action's low nibble is clear, so it cannot corrupt "
                 "the address");
        }
        /* The encoder refuses a denied address on its own, so the allowlist
         * is enforced in TWO independent places and neither is load-bearing
         * alone. */
        {
            uint8_t req[4] = { 0xaa, 0xaa, 0xaa, 0xaa };
            WCHK(rb_dpcd_write_request(RB_DPCD_LANE0_1_STATUS, 1u, req,
                                       sizeof(req)) == 0u,
                 "the encoder refuses a denied address independently of "
                 "rb_dpcd_write_refuse");
            WCHK(rb_dpcd_write_request(RB_DPCD_LINK_BW_SET, 0u, req,
                                       sizeof(req)) == 0u,
                 "and refuses a zero-length write, which has no payload to "
                 "carry");
        }

        /* ---------------- THE REFUSALS, each for its own reason -------- */
        const uint32_t okHpd = 2u, okInst = 2u;
        WCHK(rb_dpcd_write_refuse(okHpd, okInst, RB_DPCD_LINK_BW_SET,
                                  RB_DPCD_LINK_BW_5_4, 1, 1, RB_AUX_COUNT)
             == RB_DPCD_W_OK,
             "HBR2 to LINK_BW_SET on HPD2/AUX2 is allowed - the control for "
             "every refusal below");
        WCHK(rb_dpcd_write_refuse(okHpd, okInst, RB_DPCD_LINK_BW_SET, 0x14u,
                                  0, 1, RB_AUX_COUNT)
             == RB_DPCD_W_NOT_ARMED, "unarmed is refused");
        WCHK(rb_dpcd_write_refuse(okHpd, okInst, RB_DPCD_LINK_BW_SET, 0x14u,
                                  1, 0, RB_AUX_COUNT)
             == RB_DPCD_W_NO_REGS, "no register mapping is refused");
        /* THE PANEL, twice, by two different facts. */
        WCHK(rb_dpcd_write_refuse(RB_DPCD_PANEL_HPD, okInst,
                                  RB_DPCD_LINK_BW_SET, 0x14u, 1, 1,
                                  RB_AUX_COUNT) == RB_DPCD_W_PANEL_HPD,
             "the panel's HPD is refused by name");
        WCHK(rb_dpcd_write_refuse(okHpd, RB_DPCD_PANEL_AUX,
                                  RB_DPCD_LINK_BW_SET, 0x14u, 1, 1,
                                  RB_AUX_COUNT) == RB_DPCD_W_PANEL_ENGINE,
             "and the panel's ENGINE is refused separately, so neither check "
             "is load-bearing alone");
        /* ...and the panel wins over a denied address, so it is refused for
         * being the panel and not incidentally. */
        WCHK(rb_dpcd_write_refuse(RB_DPCD_PANEL_HPD, okInst,
                                  RB_DPCD_LANE0_1_STATUS, 0x14u, 1, 1,
                                  RB_AUX_COUNT) == RB_DPCD_W_PANEL_HPD,
             "the panel is refused AS THE PANEL even when the address would "
             "also have been denied");
        WCHK(rb_dpcd_write_refuse(okHpd, RB_AUX_COUNT, RB_DPCD_LINK_BW_SET,
                                  0x14u, 1, 1, RB_AUX_COUNT)
             == RB_DPCD_W_NO_ENGINE,
             "an HPD no engine serves is refused - rb_aux_for_hpd's own "
             "out-of-range answer");
        WCHK(rb_dpcd_write_refuse(okHpd, okInst, RB_DPCD_LANE_ALIGN_STATUS,
                                  0x00u, 1, 1, RB_AUX_COUNT)
             == RB_DPCD_W_ADDR_DENIED, "a denied address is refused");
        WCHK(rb_dpcd_write_refuse(okHpd, okInst, RB_DPCD_LINK_BW_SET, 0x100u,
                                  1, 1, RB_AUX_COUNT)
             == RB_DPCD_W_VALUE_RANGE,
             "a value that is not a byte is refused - AUX_SW_DATA carries "
             "eight bits and the rest would be dropped silently");
        WCHK(rb_dpcd_write_refuse(okHpd, okInst, RB_DPCD_LINK_BW_SET, 0xffu,
                                  1, 1, RB_AUX_COUNT) == RB_DPCD_W_OK,
             "while 0xff - the largest byte - is allowed, so the bound is on "
             "the ninth bit and not on 0xff itself");

        /* The values training actually sends, through the full refusal. */
        WCHK(rb_dpcd_write_refuse(okHpd, okInst, RB_DPCD_LANE_COUNT_SET,
                                  2u | RB_DPCD_ENHANCED_FRAME_EN, 1, 1,
                                  RB_AUX_COUNT) == RB_DPCD_W_OK &&
             rb_dpcd_write_refuse(okHpd, okInst, RB_DPCD_TRAINING_PATTERN_SET,
                                  RB_DPCD_TRAIN_PATTERN_1, 1, 1,
                                  RB_AUX_COUNT) == RB_DPCD_W_OK &&
             rb_dpcd_write_refuse(okHpd, okInst, RB_DPCD_LINK_CODING_SET,
                                  RB_DPCD_SET_ANSI_8B10B, 1, 1,
                                  RB_AUX_COUNT) == RB_DPCD_W_OK,
             "2 lanes with enhanced framing, training pattern 1 and 8b/10b "
             "coding all pass - this sink is 2 lanes HBR2 (2BB)");

        /* ---------------- THE VERDICT ---------------------------------
         *
         * Four outcomes, not a bool: a sink can ACK a write to a field it
         * ignores, and that is a finding rather than a failure. */
        WCHK(rb_dpcd_write_verdict(0, 1, 0x14u, 0x14u)
             == RB_DPCD_W_V_NO_ANSWER,
             "no ACK is NO_ANSWER even when the byte reads back - which is "
             "what a stale readback of an already-set field looks like");
        WCHK(rb_dpcd_write_verdict(1, 1, 0x14u, 0x14u)
             == RB_DPCD_W_V_ACK_STORED, "ACK and the byte reads back");
        WCHK(rb_dpcd_write_verdict(1, 1, 0x14u, 0x0au)
             == RB_DPCD_W_V_ACK_IGNORED,
             "ACK and it does not - a real outcome with its own code");
        WCHK(rb_dpcd_write_verdict(1, 0, 0x14u, 0xffffffffu)
             == RB_DPCD_W_V_ACK_BLIND,
             "ACK with no read-back is BLIND, never silently STORED - the "
             "refused read's 0xffffffff must not be compared as a byte");
    }
#undef WCHK

    printf("dpcd: 2BA's request layout, the DMCUB arbitration refusal, the "
           "reply decode, the stuck-index signature, 2BC's I2C/MOT "
           "encoding and EDID checks and 2F3's write allowlist, encoder, "
           "panel refusals and verdict%s\n",
           bad ? " FAILED" : ": passed");
    return bad;
}
