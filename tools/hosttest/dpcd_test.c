/* SPDX-License-Identifier: MIT */
/* 2BA: the native AUX read of DPCD. Every constant checked against the file
 * it came from, and the refusals checked from both sides - this is the first
 * write this project makes outside the panel's own pipe.
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
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

    printf("dpcd: 2BA's request layout, the DMCUB arbitration refusal, the "
           "reply decode and the stuck-index signature%s\n",
           bad ? " FAILED" : ": passed");
    return bad;
}
