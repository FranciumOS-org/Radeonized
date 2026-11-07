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

/* dc_ddc_types.h: enum i2caux_transaction_action, verbatim. MOT is
 * "middle of transaction" - the bit that keeps an I2C transaction open across
 * several AUX transactions, which is how a 128-byte EDID is read 16 bytes at
 * a time from one I2C read. */
#define RB_DPCD_ACTION_READ         0x90u   /* DP_READ                     */
#define RB_DPCD_ACTION_WRITE        0x80u   /* DP_WRITE  (2F3)             */
#define RB_I2C_ACTION_WRITE         0x00u   /* I2C_WRITE                   */
#define RB_I2C_ACTION_READ          0x10u   /* I2C_READ                    */
#define RB_I2C_ACTION_WRITE_MOT     0x40u   /* I2C_WRITE_MOT               */
#define RB_I2C_ACTION_READ_MOT      0x50u   /* I2C_READ_MOT                */

/* The EDID's I2C slave address on the DDC bus. */
#define RB_EDID_I2C_ADDR            0x50u
#define RB_EDID_BLOCK_LEN           128u

/* A real sink DEFERs an I2C-over-AUX transaction routinely while its own DDC
 * bus is busy - it is not an error and a single-shot read would report a
 * working monitor as silent. Bounded, because the mirror of that mistake is
 * spinning forever on a sink that always defers. */
#define RB_AUX_DEFER_RETRIES        8u

/* 2F6: THE SAME IS TRUE OF A NATIVE DPCD TRANSACTION, and until 2F6 this
 * project only believed it of the I2C path. `auxDpcdRead` was SINGLE-SHOT:
 * one transaction, and a DEFER reply returned false. Boot 458's
 * `dptrain go` reported `SinkAfterOk 0` and 2F4 called it the sink going
 * silent; boot 458's `dpcdread 0x202 1` then came back
 * `reply 2 (DEFER)` - so what the verdict read as silence was a sink saying
 * "not yet" exactly once, with the EDID path's own comment about that
 * mistake sitting twenty lines away in the same file.
 *
 * DEFER AND NACK ARE DIFFERENT ANSWERS. A NACK is the sink declining; a DEFER
 * is flow control, and the correct response to flow control is to wait and
 * ask again. `dpcdread`'s text conflated them too.
 *
 * WHERE THIS DIVERGES FROM LINUX, deliberately. dce_aux.c:822-827 bounds a
 * native defer with a CONJUNCTION - give up only when
 * `aux_defer_retries >= AUX_MIN_DEFER_RETRIES` (7) *AND*
 * `defer_time_in_ms >= AUX_MAX_DEFER_TIMEOUT_MS` (50), inside an outer
 * `AUX_MAX_RETRIES` (7) loop, so in practice the count binds first there
 * too. Ours is the count alone at RB_AUX_DEFER_DELAY_US apart, which is
 * ~8 ms rather than Linux's 50 ms ceiling: TIGHTER IN TIME, and on purpose,
 * because every caller here is a setProperties dial and this is a spin, not
 * a sleep. The 50 ms term is therefore NOT implemented - a guard that could
 * never fire is not a guard (rule 48) - and if a sink is ever seen to need
 * longer than 8 ms, the change is to the delay or to IOSleep, as a decision
 * with a reason and not a quiet widening. */
#define RB_AUX_DEFER_DELAY_US       1000u

/* dc_ddc_types.h: enum aux_transaction_reply, after the >> 4. */
#define RB_DPCD_REPLY_ACK           0x0u
#define RB_DPCD_REPLY_NACK          0x1u
#define RB_DPCD_REPLY_DEFER         0x2u

#define RB_DPCD_MAX_READ            16u

/* ===================== 2F3: THE NATIVE DPCD WRITE =======================
 *
 * THE FIRST WRITE THIS PROJECT MAKES TO THE SINK. Everything before it wrote
 * to this chip; an EDID read writes an offset byte over AUX (2BD), but that
 * goes to the monitor's DDC EEPROM and not to its DP receiver. These
 * addresses change the receiver's own state, and link training is nothing but
 * a sequence of them.
 *
 * WHAT MAKES IT BOUNDED. Three things, each a refusal with its own code:
 *   - The engine is the one bound to HPD2, which drives nothing this desktop
 *     depends on. The PANEL's engine is refused BY NAME, twice over - by HPD
 *     and by instance - because a DPCD write on the link the display is
 *     running on is the way to lose it. Same shape as pipe 0 in rb_dpdto.h.
 *   - The ADDRESS IS ALLOWLISTED. Not "refuse the dangerous ones" - only the
 *     link-configuration block below is writable at all, and everything else
 *     in the 20-bit space is refused, including the read-only capability and
 *     status blocks. A NACK from the sink would be the fallback; a refusal
 *     here is cheaper and says which address it was.
 *   - Nothing in the allowlist does anything on its own. 0x100/0x101 set a
 *     rate and a lane count the sink only acts on once a training pattern is
 *     set; 0x103..0x106 are drive settings; and the sink is not displaying
 *     anything in any case, because no boot of this machine has ever trained
 *     this link (2F2's measurement: the firmware's GOP lights no external
 *     display on this chassis, on either GPU, and the sink's LED never leaves
 *     standby).
 *
 * THE VERDICT IS THE SINK'S OWN ACK, which is a reply we do not compose -
 * rule 20's requirement, since a readback of a byte we just wrote is a value
 * known in advance to agree. The read-after-write is published beside it with
 * `changed` against the byte read BEFORE, so the pair says whether the sink
 * stored it as well as whether it answered.
 *
 * ADDRESSES ARE drm_dp.h's, BY LINE, not from memory:
 *   0x100 DP_LINK_BW_SET (588), 0x101 DP_LANE_COUNT_SET (598),
 *   0x102 DP_TRAINING_PATTERN_SET (603), 0x103..0x106 DP_TRAINING_LANE0..3_SET
 *   (628-631), 0x107 DP_DOWNSPREAD_CTRL (652),
 *   0x108 DP_MAIN_LINK_CHANNEL_CODING_SET (657), 0x600 DP_SET_POWER (1023).
 */
#define RB_DPCD_LINK_BW_SET            0x100u
#define RB_DPCD_LANE_COUNT_SET         0x101u
#define RB_DPCD_TRAINING_PATTERN_SET   0x102u
#define RB_DPCD_TRAINING_LANE0_SET     0x103u
#define RB_DPCD_TRAINING_LANE3_SET     0x106u
#define RB_DPCD_DOWNSPREAD_CTRL        0x107u
#define RB_DPCD_LINK_CODING_SET        0x108u
#define RB_DPCD_SET_POWER              0x600u

/* Values, also drm_dp.h's. Named because the hosttest asserts the allowlist
 * accepts what training actually sends, which a bare number could not say. */
#define RB_DPCD_LINK_BW_1_62           0x06u
#define RB_DPCD_LINK_BW_2_7            0x0au
#define RB_DPCD_LINK_BW_5_4            0x14u
#define RB_DPCD_TRAIN_PATTERN_DISABLE  0x00u
#define RB_DPCD_TRAIN_PATTERN_1        0x01u
#define RB_DPCD_TRAIN_PATTERN_2        0x02u
#define RB_DPCD_LANE_COUNT_MASK        0x0fu
#define RB_DPCD_ENHANCED_FRAME_EN      0x80u
#define RB_DPCD_SET_ANSI_8B10B         0x01u
#define RB_DPCD_SET_POWER_D0           0x01u
#define RB_DPCD_SET_POWER_D3           0x02u

/* Read-only blocks, named so the hosttest can assert they are refused rather
 * than merely absent from the allowlist - a test that passes because a number
 * happens not to be listed is rule 48's "right answer for the wrong reason". */
#define RB_DPCD_REV_ADDR               0x000u
#define RB_DPCD_MAX_LINK_RATE_ADDR     0x001u
#define RB_DPCD_LANE0_1_STATUS         0x202u
#define RB_DPCD_LANE_ALIGN_STATUS      0x204u
#define RB_DPCD_ADJUST_REQUEST_LANE0_1 0x206u

/* The one and only set of addresses this project will write to a sink.
 * Everything not named here is refused, including every address above the
 * 20-bit AUX space. */
static inline int rb_dpcd_write_allowed(uint32_t address)
{
    if (address >= RB_DPCD_TRAINING_LANE0_SET &&
        address <= RB_DPCD_TRAINING_LANE3_SET) {
        return 1;
    }
    switch (address) {
    case RB_DPCD_LINK_BW_SET:
    case RB_DPCD_LANE_COUNT_SET:
    case RB_DPCD_TRAINING_PATTERN_SET:
    case RB_DPCD_DOWNSPREAD_CTRL:
    case RB_DPCD_LINK_CODING_SET:
    case RB_DPCD_SET_POWER:
        return 1;
    default:
        return 0;
    }
}

/* Which actions carry a payload, and therefore count it into
 * AUX_SW_WR_BYTES. 2BA's auxTransact tested two I2C forms by equality and so
 * would have encoded a native DP write with the READ length and no data -
 * silently, since the engine would have ACKed a well-formed 4-byte request
 * that wrote nothing. This predicate is the fix and the thing the hosttest
 * pins. */
static inline int rb_aux_action_is_write(uint32_t action)
{
    return action == RB_I2C_ACTION_WRITE ||
           action == RB_I2C_ACTION_WRITE_MOT ||
           action == RB_DPCD_ACTION_WRITE;
}

/* THE WRITE'S REFUSALS. Codes, not a bool (rule 14), and numbered clear of
 * auxDpcdRefuse's 1..9 so one field can carry either and a reader can tell
 * which half refused. */
#define RB_DPCD_W_OK              0u
#define RB_DPCD_W_NOT_ARMED      20u
#define RB_DPCD_W_NO_REGS        21u
#define RB_DPCD_W_PANEL_HPD      22u  /* HPD 0 - the panel, by name        */
#define RB_DPCD_W_PANEL_ENGINE   23u  /* AUX 0 - the panel, independently  */
#define RB_DPCD_W_NO_ENGINE      24u  /* no engine serves that HPD         */
#define RB_DPCD_W_ADDR_DENIED    25u  /* not in the allowlist              */
#define RB_DPCD_W_VALUE_RANGE    26u  /* not a byte                        */

#define RB_DPCD_PANEL_HPD         0u
#define RB_DPCD_PANEL_AUX         0u

static inline uint32_t rb_dpcd_write_refuse(uint32_t hpd, uint32_t inst,
                                            uint32_t address, uint32_t value,
                                            int armed, int have_regs,
                                            uint32_t aux_count)
{
    if (!armed)     { return RB_DPCD_W_NOT_ARMED; }
    if (!have_regs) { return RB_DPCD_W_NO_REGS; }
    /* The panel twice, by two different facts, so neither alone is load
     * bearing: the connector it is on and the engine that serves it. */
    if (hpd == RB_DPCD_PANEL_HPD)       { return RB_DPCD_W_PANEL_HPD; }
    if (inst == RB_DPCD_PANEL_AUX)      { return RB_DPCD_W_PANEL_ENGINE; }
    if (inst >= aux_count)              { return RB_DPCD_W_NO_ENGINE; }
    if (!rb_dpcd_write_allowed(address)) { return RB_DPCD_W_ADDR_DENIED; }
    if (value > 0xffu)                  { return RB_DPCD_W_VALUE_RANGE; }
    return RB_DPCD_W_OK;
}

/* The verdict. The sink's ACK is the part we do not compose; `changed` is the
 * part that says it STORED the byte rather than merely answering. Separate,
 * because a sink can ACK a write to a field it ignores - which is a real and
 * reportable outcome, not a failure to be smoothed over. */
#define RB_DPCD_W_V_NOT_RUN     (-1)
#define RB_DPCD_W_V_NO_ANSWER     0   /* no ACK                            */
#define RB_DPCD_W_V_ACK_STORED    1   /* ACK, and the byte reads back      */
#define RB_DPCD_W_V_ACK_IGNORED   2   /* ACK, and the byte did not MOVE    */
#define RB_DPCD_W_V_ACK_BLIND     3   /* ACK, and the read-back failed     */
/* 2F6: ACK, the byte MOVED, and not to what we wrote. Until boot 459 this
 * collapsed into ACK_IGNORED, whose text says "the byte did not change" -
 * and the run it was printed for had `changed 1` beside it, so the tool
 * contradicted its own fields in the same paragraph. `dpcdwrite 0x102 0x01`
 * took the sink from 0x02 to 0x00: it neither stored nor ignored, it
 * CLEARED. That is a third behaviour and the most informative of the three,
 * since the value it lands on is the sink's own choice. */
#define RB_DPCD_W_V_ACK_OTHER     4   /* ACK, moved, to a THIRD value      */

static inline int rb_dpcd_write_verdict(int acked, int read_ok,
                                        int before_ok,
                                        uint32_t wrote, uint32_t before,
                                        uint32_t after)
{
    if (!acked)  { return RB_DPCD_W_V_NO_ANSWER; }
    if (!read_ok) { return RB_DPCD_W_V_ACK_BLIND; }
    if (after == wrote) { return RB_DPCD_W_V_ACK_STORED; }
    /* Not what we wrote. Whether that is "unchanged" or "changed to
     * something else" needs the BEFORE, and without a valid before-read the
     * two cannot be told apart - so it stays ACK_IGNORED, which is the
     * weaker claim, rather than guessing. */
    if (before_ok && after != before) { return RB_DPCD_W_V_ACK_OTHER; }
    return RB_DPCD_W_V_ACK_IGNORED;
}

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
/* The general encoder. A ZERO length is legal and gives THREE bytes, not
 * four - dce_aux.c's `length = request->length ? 4 : 3`, "the 4 bit header
 * and the 20 bit address (that is 3 byte). If the requested length is non
 * zero this means an addition byte specifying the length is required." That
 * form is how an I2C transaction is ended with an address-only request, so
 * refusing length 0 outright (as 2BA's DPCD-only encoder did) would make the
 * MOT protocol unexpressible. */
static inline uint32_t rb_aux_request_bytes(uint32_t action, uint32_t address,
                                            uint32_t length, uint8_t *out,
                                            uint32_t out_len)
{
    if (!out || out_len < 3u) { return 0; }
    if (length > RB_DPCD_MAX_READ) { return 0; }
    if (address > 0xFFFFFu) { return 0; }   /* 20 bits, per the COMPOSE macros */
    if (action & 0x0Fu) { return 0; }        /* the low nibble is the address */
    out[0] = (uint8_t)(action | ((address & 0xF0000u) >> 16));
    out[1] = (uint8_t)((address & 0xFF00u) >> 8);
    out[2] = (uint8_t)(address & 0xFFu);
    if (length == 0u) { return 3u; }
    if (out_len < 4u) { return 0; }
    out[3] = (uint8_t)(length - 1u);
    return 4u;
}

/* 2BA's DPCD read, now a wrapper. It keeps the stricter rule that a zero
 * length is meaningless for a DPCD read even though the encoder allows it. */
static inline uint32_t rb_dpcd_read_request(uint32_t address, uint32_t length,
                                            uint8_t *out, uint32_t out_len)
{
    if (length == 0u) { return 0; }
    if (out_len < 4u) { return 0; }
    return rb_aux_request_bytes(RB_DPCD_ACTION_READ, address, length,
                                out, out_len);
}

/* A DPCD write request: the 4-byte header, then the payload. Returns the
 * header length, as rb_aux_request_bytes does - the caller adds `length` to
 * get AUX_SW_WR_BYTES, which is exactly what dce_aux.c does. */
static inline uint32_t rb_dpcd_write_request(uint32_t address, uint32_t length,
                                             uint8_t *out, uint32_t out_len)
{
    if (length == 0u) { return 0; }
    if (out_len < 4u) { return 0; }
    if (!rb_dpcd_write_allowed(address)) { return 0; }
    return rb_aux_request_bytes(RB_DPCD_ACTION_WRITE, address, length,
                                out, out_len);
}

/* Which action each step of an EDID block read uses. The LAST read drops MOT,
 * which is what ends the I2C transaction - leaving MOT set on the final chunk
 * holds the sink's DDC bus open, and the next reader finds it busy. */
static inline uint32_t rb_edid_read_action(uint32_t chunk, uint32_t chunks)
{
    if (chunk + 1u >= chunks) { return RB_I2C_ACTION_READ; }
    return RB_I2C_ACTION_READ_MOT;
}

static inline int rb_dpcd_reply_is_defer(uint32_t code)
{ return code == RB_DPCD_REPLY_DEFER; }

/* 2F6: should the caller ask again? Only a DEFER earns a retry, and only
 * while the bound holds. A NACK must NOT be retried - the sink has answered -
 * and neither must a transaction that failed without a reply at all, which is
 * the engine's problem and not the sink's. */
static inline int rb_aux_defer_retry(uint32_t code, uint32_t tries)
{
    if (!rb_dpcd_reply_is_defer(code)) { return 0; }
    return (tries < RB_AUX_DEFER_RETRIES) ? 1 : 0;
}

/* How a bounded-out defer differs from every other failure, as a code the
 * tool can print. `defers` is the count, so a read that exhausted the bound
 * is distinguishable from one that was NACKed once (rule 14). */
#define RB_AUX_DEFER_V_NONE         0   /* no defer happened                */
#define RB_AUX_DEFER_V_RECOVERED    1   /* deferred, then answered          */
#define RB_AUX_DEFER_V_EXHAUSTED    2   /* deferred to the bound and lost   */

static inline uint32_t rb_aux_defer_verdict(uint32_t defers, int ok)
{
    if (!defers) { return RB_AUX_DEFER_V_NONE; }
    return ok ? RB_AUX_DEFER_V_RECOVERED : RB_AUX_DEFER_V_EXHAUSTED;
}

/* EDID block 0 sanity, before any field of it is believed: the eight-byte
 * header is fixed by the standard, and the whole 128-byte block sums to zero
 * modulo 256. Two independent checks, because a half-read block can satisfy
 * the header and a corrupt one can satisfy neither. */
static inline int rb_edid_header_ok(const uint8_t *b, uint32_t n)
{
    static const uint8_t hdr[8] = { 0x00, 0xff, 0xff, 0xff,
                                    0xff, 0xff, 0xff, 0x00 };
    if (!b || n < 8u) { return 0; }
    for (unsigned i = 0; i < 8u; i++) {
        if (b[i] != hdr[i]) { return 0; }
    }
    return 1;
}

static inline int rb_edid_checksum_ok(const uint8_t *b, uint32_t n)
{
    if (!b || n < RB_EDID_BLOCK_LEN) { return 0; }
    uint32_t sum = 0;
    for (unsigned i = 0; i < RB_EDID_BLOCK_LEN; i++) { sum += b[i]; }
    return (sum & 0xffu) == 0u;
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
