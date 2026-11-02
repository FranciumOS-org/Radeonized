/* SPDX-License-Identifier: MIT */
#ifndef RB_DPCD_H
#define RB_DPCD_H

/* 2BA: a native AUX read of the sink's DPCD.
 *
 * THE FIRST WRITE THIS PROJECT MAKES OUTSIDE THE PANEL'S OWN PIPE, and the
 * user's decision (new write surface in the display path). What makes it a
 * bounded risk rather than a gamble is that it happens on the engine bound to
 * HPD2 - which drives nothing this desktop depends on - while the panel's
 * engine is not touched. 2B9 read `arb` 0 on both, so nothing holds the lock.
 *
 * THE ARBITRATION IS HARDWARE, NOT HOPE. `AUX_ARB_CONTROL.AUX_REG_RW_CNTL_
 * STATUS` (bits 2-3) says who owns the engine, and Linux's acquire_engine()
 * REFUSES when it reads DMCU_CAN_ACCESS_AUX (2). So "the DMCUB also
 * arbitrates for this block" - 2B8's reason for not writing - is a condition
 * the hardware publishes and we can decline on, rather than a race we have to
 * win. rb_aux_may_acquire() is that refusal, and it is the whole reason this
 * is attemptable at all.
 *
 * EVERY CONSTANT HERE IS READ, NOT RECALLED. The action byte is
 * I2CAUX_TRANSACTION_ACTION_DP_READ = 0x90 from
 * docs/linux-ref/dc_ddc_types.h; the reply codes are that file's
 * aux_transaction_reply (ACK 0, NACK 1, DEFER 2); the request layout is
 * dce_aux.c's COMPOSE_AUX_SW_DATA_* macros; the field masks are
 * dcn_3_1_2_sh_mask.h. Rule 17's standing warning is about substituting a
 * value and narrating it as hardware - the mirror is substituting a
 * remembered constant for one in the tree.
 *
 * WHAT IS NOT REPRODUCED, AND IT HAS A DETECTABLE SIGNATURE. Linux's
 * read_channel_reply() sets AUX_SW_INDEX 0, AUX_SW_AUTOINCREMENT_DISABLE 1
 * and AUX_SW_DATA_RW 1, then reads AUX_SW_DATA in a loop and expects
 * SUCCESSIVE bytes - with the auto-increment nominally DISABLED. Either the
 * index advances on read regardless of that bit, or the field means something
 * narrower than its name. **This project does not know which**, so the
 * sequence is transcribed exactly rather than reasoned about, and the
 * failure mode is made VISIBLE instead of guessed at:
 * rb_dpcd_reply_suspect_norepeat() flags a reply whose bytes are all
 * identical, which is what a stuck index looks like and which a real DPCD
 * block at address 0 is not (byte 0 is the revision, byte 1 the max link
 * rate, byte 2 the lane count - they differ).
 *
 * THERE IS NO WRITE TO THE SINK HERE. Native READ only: no DPCD write, no
 * I2C-over-AUX, no link training. Reading DPCD cannot change the sink's
 * state, so the worst outcome at the far end is no answer.
 */

#include <stdint.h>

/* AUX_ARB_CONTROL */
#define RB_AUX_ARB_RW_STATUS_MASK   0x0000000Cu
#define RB_AUX_ARB_RW_STATUS_SHIFT  2u
#define RB_AUX_ARB_SW_USE_REQ       0x00010000u
#define RB_AUX_ARB_SW_DONE_USING    0x00020000u

/* dce_aux.c's own names for the two values of that field. */
#define RB_AUX_ARB_SW_CAN_ACCESS    1u
#define RB_AUX_ARB_DMCU_CAN_ACCESS  2u

/* AUX_INTERRUPT_CONTROL / AUX_SW_CONTROL / AUX_SW_DATA / AUX_SW_STATUS */
#define RB_AUX_INT_SW_DONE_ACK      0x00000002u
#define RB_AUX_SWC_GO               0x00000001u
#define RB_AUX_SWC_START_DELAY_MASK 0x000000F0u
#define RB_AUX_SWC_WR_BYTES_MASK    0x001F0000u
#define RB_AUX_SWC_WR_BYTES_SHIFT   16u
#define RB_AUX_SWD_RW               0x00000001u
#define RB_AUX_SWD_DATA_MASK        0x0000FF00u
#define RB_AUX_SWD_DATA_SHIFT       8u
#define RB_AUX_SWD_INDEX_MASK       0x001F0000u
#define RB_AUX_SWD_INDEX_SHIFT      16u
#define RB_AUX_SWD_AUTOINC_DIS      0x80000000u

/* dc_ddc_types.h: I2CAUX_TRANSACTION_ACTION_DP_READ. */
#define RB_DPCD_ACTION_READ         0x90u

/* dc_ddc_types.h: enum aux_transaction_reply, after the >> 4. */
#define RB_DPCD_REPLY_ACK           0x0u
#define RB_DPCD_REPLY_NACK          0x1u
#define RB_DPCD_REPLY_DEFER         0x2u

#define RB_DPCD_MAX_READ            16u

static inline uint32_t rb_aux_arb_owner(uint32_t arb)
{ return (arb & RB_AUX_ARB_RW_STATUS_MASK) >> RB_AUX_ARB_RW_STATUS_SHIFT; }

/* Linux refuses on exactly one value, so this does too - and it is a REFUSAL
 * and not a wait, because waiting for the DMCUB to finish is a race with
 * firmware we cannot see. */
static inline int rb_aux_may_acquire(uint32_t arb)
{ return rb_aux_arb_owner(arb) != RB_AUX_ARB_DMCU_CAN_ACCESS; }

static inline uint32_t rb_aux_arb_acquire_word(uint32_t arb)
{ return (arb | RB_AUX_ARB_SW_USE_REQ) & ~RB_AUX_ARB_SW_DONE_USING; }

static inline uint32_t rb_aux_arb_release_word(uint32_t arb)
{ return (arb | RB_AUX_ARB_SW_DONE_USING) & ~RB_AUX_ARB_SW_USE_REQ; }

/* The request, byte for byte out of dce_aux.c's COMPOSE_AUX_SW_DATA_*:
 *     byte0 = action | ((address & 0xF0000) >> 16)
 *     byte1 = (address & 0xFF00) >> 8
 *     byte2 = address & 0xFF
 *     byte3 = length - 1          (only when length is non-zero)
 * and AUX_SW_WR_BYTES is 3 for a zero length, 4 otherwise - "the 4 bit
 * header and the 20 bit address (that is 3 byte)" plus a length byte.
 * Returns the number of bytes to program, or 0 on a refusal.
 */
static inline uint32_t rb_dpcd_read_request(uint32_t address, uint32_t length,
                                            uint8_t *out, uint32_t out_len)
{
    if (!out || out_len < 4u) { return 0; }
    if (length == 0u || length > RB_DPCD_MAX_READ) { return 0; }
    if (address > 0xFFFFFu) { return 0; }   /* 20-bit DPCD address space */
    out[0] = (uint8_t)(RB_DPCD_ACTION_READ | ((address & 0xF0000u) >> 16));
    out[1] = (uint8_t)((address & 0xFF00u) >> 8);
    out[2] = (uint8_t)(address & 0xFFu);
    out[3] = (uint8_t)(length - 1u);
    return 4u;
}

static inline uint32_t rb_aux_swc_request_word(uint32_t wr_bytes)
{
    /* START_DELAY 0, as Linux passes request->delay and dce_aux_transfer
     * leaves it 0 for a native read. */
    return (wr_bytes << RB_AUX_SWC_WR_BYTES_SHIFT) & RB_AUX_SWC_WR_BYTES_MASK;
}

static inline uint32_t rb_aux_swd_write_word(uint32_t index, uint8_t byte,
                                             int autoinc_disable)
{
    uint32_t v = ((index << RB_AUX_SWD_INDEX_SHIFT) & RB_AUX_SWD_INDEX_MASK) |
                 (((uint32_t)byte << RB_AUX_SWD_DATA_SHIFT) &
                  RB_AUX_SWD_DATA_MASK);
    if (autoinc_disable) { v |= RB_AUX_SWD_AUTOINC_DIS; }
    return v;                               /* AUX_SW_DATA_RW 0 = write */
}

static inline uint32_t rb_aux_swd_read_setup_word(void)
{
    /* Linux: INDEX 0, AUTOINCREMENT_DISABLE 1, DATA_RW 1. Transcribed, not
     * reasoned about - see the header comment on what is not reproduced. */
    return RB_AUX_SWD_AUTOINC_DIS | RB_AUX_SWD_RW;
}

static inline uint8_t rb_aux_swd_byte(uint32_t swd)
{ return (uint8_t)((swd & RB_AUX_SWD_DATA_MASK) >> RB_AUX_SWD_DATA_SHIFT); }

/* The first replied byte is the status nibble, shifted down by 4. */
static inline uint32_t rb_dpcd_reply_code(uint8_t first_byte)
{ return (uint32_t)first_byte >> 4; }

/* How many of the replied bytes are payload. The status byte is one of them,
 * so a count of 1 is an ACK with no data - which is NOT a failure and must
 * not be reported as one (rule 14: the two look identical in a byte count). */
static inline uint32_t rb_dpcd_payload_bytes(uint32_t replied)
{ return replied ? replied - 1u : 0u; }

/* THE SIGNATURE OF THE ONE THING NOT REPRODUCED. All bytes identical is what
 * a stuck AUX_SW_DATA index looks like; a real DPCD block at address 0 has a
 * revision, a max link rate and a lane count that differ. Returns 1 when the
 * reply should be distrusted. One byte cannot be judged, so it is not. */
static inline int rb_dpcd_reply_suspect_norepeat(const uint8_t *buf,
                                                 uint32_t n)
{
    if (!buf || n < 2u) { return 0; }
    for (uint32_t i = 1; i < n; i++) {
        if (buf[i] != buf[0]) { return 0; }
    }
    return 1;
}

/* DPCD 0x00000..2, the only three bytes this milestone claims to interpret.
 * DP_DPCD_REV is BCD: 0x11 is DP 1.1, 0x12 is 1.2, 0x14 is 1.4. */
static inline uint32_t rb_dpcd_rev_major(uint8_t rev) { return rev >> 4; }
static inline uint32_t rb_dpcd_rev_minor(uint8_t rev) { return rev & 0xfu; }
static inline uint32_t rb_dpcd_lane_count(uint8_t b2) { return b2 & 0x1fu; }

/* A DPCD revision outside the range any sink reports is how a reply that is
 * really register noise announces itself - the control on the whole read. */
static inline int rb_dpcd_rev_plausible(uint8_t rev)
{
    const uint32_t maj = rb_dpcd_rev_major(rev);
    const uint32_t min = rb_dpcd_rev_minor(rev);
    if (maj != 1u) { return 0; }
    return min <= 4u;
}

#endif /* RB_DPCD_H */
