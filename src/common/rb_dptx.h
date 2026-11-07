/* SPDX-License-Identifier: MIT */
#ifndef RB_DPTX_H
#define RB_DPTX_H

/* 2F4: the DP transmitter's link block - the PHY side of link training.
 *
 * WHAT THIS IS FOR. 2F3 made the SINK half work: a DPCD write lands and the
 * receiver stores it. The other half is this chip putting a training pattern
 * on the wire, and that is four registers in the DP back end.
 *
 * THE REFERENCE INSTANCE IS THE WHOLE IDENTIFICATION, and here it is better
 * than it has ever been: the firmware TRAINED THE eDP PANEL, which is running
 * this desktop right now, so DP0's `DP_LINK_CNTL` must read
 * `DP_LINK_TRAINING_COMPLETE` SET on this silicon at this firmware revision.
 * That is rule 47 asked of a write - the live word decodes as the register the
 * conclusion assumes - and it is discharged against a working link rather than
 * against a datasheet. `rb_dptx_ref_trained()` is that check and the write
 * refuses without it.
 *
 * THIS BLOCK IS SHARED WITH rb_enc.h, which owns two of its registers:
 * DP_CONFIG (0x210b) and DP_DPHY_SCRAM_CNTL (0x211e), both read and written by
 * 2EC/2E7. Documented here rather than discovered later - the stride and base
 * are the same and a second opinion about either would be a bug.
 *
 * OFFSETS, instance 0, stride 0x100 dwords, five instances, BASE_IDX 2, from
 * docs/linux-ref/dcn_3_1_2_offset.h by line:
 *     regDP0_DP_LINK_CNTL                   0x2108   (9723)
 *     regDP0_DP_VID_STREAM_CNTL             0x210c   (9731)
 *     regDP0_DP_DPHY_CNTL                   0x2117   (9753)
 *     regDP0_DP_DPHY_TRAINING_PATTERN_SEL   0x2118   (9755)
 *     regDP0_DP_DPHY_PRBS_CNTL              0x211d   (9765)
 * BASE_IDX 2 is DMU segment 0x34c0 - the same mapping HPD, AUX, OTG, HUBP and
 * the DIG blocks already use, so this needs no new aperture. It is NOT the
 * BASE_IDX 1 the DTO lives at (2EF/2F0), and the two must not be confused.
 *
 * RULE 46 IS DISCHARGED IN THE RESOURCE FILE, not by reasoning:
 * dcn31_resource.c builds its link encoders with dcn31_link_encoder_create(),
 * whose `.dp_set_phy_pattern` is `dcn10_link_encoder_dp_set_phy_pattern`
 * (dcn31_dio_link_encoder.c:354, identical to dcn20_link_encoder.c:369). So
 * the DCN10 link encoder IS this chip's link encoder for this operation - the
 * same relationship 2B8 found for the DCE110 AUX engine and 2B1 for the HUBP
 * triple-buffer writer.
 *
 * THE SEQUENCE, from dcn10_link_encoder_set_dp_phy_pattern_training_pattern()
 * (dcn10_link_encoder.c:209), in its order and with its register semantics:
 *     REG_WRITE (DP_DPHY_TRAINING_PATTERN_SEL, index);     <- WHOLE register
 *     REG_UPDATE(DP_LINK_CNTL,     DP_LINK_TRAINING_COMPLETE, 0);
 *     REG_UPDATE(DP_DPHY_CNTL,     DPHY_BYPASS,              0);
 *     REG_UPDATE(DP_DPHY_PRBS_CNTL, DPHY_PRBS_EN,            0);
 * The first is a plain write and the other three are read-modify-writes; that
 * difference is Linux's and is reproduced rather than tidied. `index` is 0 for
 * TPS1, 1 for TPS2, 2 for TPS3, 3 for TPS4 - which is NOT the DPCD's encoding
 * (DPCD 0x102 takes 1 for TPS1), and conflating the two would set the wrong
 * pattern on one side of the link. rb_dptx_hw_pattern() converts, and the
 * hosttest pins both directions.
 */

#include <stdint.h>

#define RB_DPTX_COUNT              5u
#define RB_DPTX_DW_STRIDE          0x100u

#define RB_DPTX_DW_LINK_CNTL       0x2108u
#define RB_DPTX_DW_VID_STREAM      0x210cu
#define RB_DPTX_DW_DPHY_CNTL       0x2117u
#define RB_DPTX_DW_TP_SEL          0x2118u
#define RB_DPTX_DW_PRBS_CNTL       0x211du

/* rb_enc.h owns these two of the same block. Named so the overlap is a fact in
 * the code rather than a thing to rediscover. */
#define RB_DPTX_DW_DP_CONFIG_ENC   0x210bu
#define RB_DPTX_DW_SCRAM_ENC       0x211eu

#define RB_DPTX_DW_INVALID         0u

/* DP_LINK_CNTL. TRAINING_COMPLETE is OURS to write; LINK_STATUS is the
 * hardware's and is never written here - which is what makes it usable as a
 * verdict at all (rule 20). */
#define RB_DPTX_TRAINING_COMPLETE  0x00000010u
#define RB_DPTX_LINK_STATUS        0x00000100u
#define RB_DPTX_EMBEDDED_PANEL     0x00020000u

/* DP_VID_STREAM_CNTL */
#define RB_DPTX_VID_STREAM_ENABLE  0x00000001u

/* DP_DPHY_CNTL */
#define RB_DPTX_DPHY_FEC_EN        0x00000010u
#define RB_DPTX_DPHY_FEC_READY     0x00000020u
#define RB_DPTX_DPHY_FEC_ACTIVE    0x00000040u
#define RB_DPTX_DPHY_SCRAMBLER_SEL 0x00000100u
#define RB_DPTX_DPHY_BYPASS        0x00010000u
#define RB_DPTX_DPHY_SKEW_BYPASS   0x01000000u

/* DP_DPHY_TRAINING_PATTERN_SEL - two bits, the whole register as far as
 * anything here is concerned. */
#define RB_DPTX_TP_SEL_MASK        0x00000003u

/* DP_DPHY_PRBS_CNTL */
#define RB_DPTX_PRBS_EN            0x00000001u
#define RB_DPTX_PRBS_SEL_MASK      0x00000030u

enum rb_dptx_reg {
    RB_DPTX_REG_LINK_CNTL = 0,
    RB_DPTX_REG_VID_STREAM,
    RB_DPTX_REG_DPHY_CNTL,
    RB_DPTX_REG_TP_SEL,
    RB_DPTX_REG_PRBS_CNTL,
    RB_DPTX_REG_COUNT
};

static inline uint32_t rb_dptx_reg_base(enum rb_dptx_reg which)
{
    switch (which) {
    case RB_DPTX_REG_LINK_CNTL:  return RB_DPTX_DW_LINK_CNTL;
    case RB_DPTX_REG_VID_STREAM: return RB_DPTX_DW_VID_STREAM;
    case RB_DPTX_REG_DPHY_CNTL:  return RB_DPTX_DW_DPHY_CNTL;
    case RB_DPTX_REG_TP_SEL:     return RB_DPTX_DW_TP_SEL;
    case RB_DPTX_REG_PRBS_CNTL:  return RB_DPTX_DW_PRBS_CNTL;
    default:                     return RB_DPTX_DW_INVALID;
    }
}

static inline uint32_t rb_dptx_dw(uint32_t inst, enum rb_dptx_reg which)
{
    const uint32_t base = rb_dptx_reg_base(which);
    if (!base || inst >= RB_DPTX_COUNT) { return RB_DPTX_DW_INVALID; }
    return base + inst * RB_DPTX_DW_STRIDE;
}

/* The highest dword this block touches, for the aperture check - asserted in
 * the hosttest rather than stated in a comment at the call site. */
static inline uint32_t rb_dptx_dw_max(void)
{
    return RB_DPTX_DW_PRBS_CNTL + (RB_DPTX_COUNT - 1u) * RB_DPTX_DW_STRIDE;
}

static inline int rb_dptx_trained(uint32_t link_cntl)
{ return (link_cntl & RB_DPTX_TRAINING_COMPLETE) ? 1 : 0; }

static inline int rb_dptx_link_status(uint32_t link_cntl)
{ return (link_cntl & RB_DPTX_LINK_STATUS) ? 1 : 0; }

static inline int rb_dptx_embedded(uint32_t link_cntl)
{ return (link_cntl & RB_DPTX_EMBEDDED_PANEL) ? 1 : 0; }

static inline int rb_dptx_stream_on(uint32_t vid)
{ return (vid & RB_DPTX_VID_STREAM_ENABLE) ? 1 : 0; }

static inline int rb_dptx_bypass(uint32_t dphy)
{ return (dphy & RB_DPTX_DPHY_BYPASS) ? 1 : 0; }

static inline int rb_dptx_prbs_on(uint32_t prbs)
{ return (prbs & RB_DPTX_PRBS_EN) ? 1 : 0; }

static inline uint32_t rb_dptx_pattern(uint32_t tp_sel)
{ return tp_sel & RB_DPTX_TP_SEL_MASK; }

/* ===================== THE TWO PATTERN ENCODINGS =======================
 *
 * They are NOT the same and this is the whole reason these two functions
 * exist. The hardware register takes 0..3 for TPS1..TPS4
 * (dcn10_link_encoder_dp_set_phy_pattern's switch); DPCD 0x102 takes
 * 1 for TPS1, 2 for TPS2, 3 for TPS3, 7 for TPS4 (drm_dp.h:603-609), with 0
 * meaning "training off". Passing one where the other belongs sets a
 * DIFFERENT pattern on each side of the link, which would look exactly like a
 * sink that cannot achieve clock recovery.
 */
#define RB_DPTX_TPS1  1u
#define RB_DPTX_TPS2  2u
#define RB_DPTX_TPS3  3u
#define RB_DPTX_TPS4  4u

/* TPSn -> the hardware register's value. 0 on a bad n, which the caller must
 * not treat as TPS1 - rb_dptx_tps_valid() is the gate. */
static inline uint32_t rb_dptx_hw_pattern(uint32_t tps)
{
    if (tps < RB_DPTX_TPS1 || tps > RB_DPTX_TPS4) { return 0; }
    return tps - 1u;
}

/* TPSn -> DPCD 0x102's value. TPS4 is 7, not 4 - the one place these two
 * encodings are not even a constant apart. */
static inline uint32_t rb_dptx_dpcd_pattern(uint32_t tps)
{
    switch (tps) {
    case RB_DPTX_TPS1: return 1u;
    case RB_DPTX_TPS2: return 2u;
    case RB_DPTX_TPS3: return 3u;
    case RB_DPTX_TPS4: return 7u;
    default:           return 0u;
    }
}

static inline int rb_dptx_tps_valid(uint32_t tps)
{ return tps >= RB_DPTX_TPS1 && tps <= RB_DPTX_TPS4; }

/* ===================== THE WRITE ======================================= */

/* The reference check: the panel's own instance must read as a trained link,
 * because the firmware trained it and it is scanning out right now. Without
 * this the block is unidentified and a write goes somewhere unknown. */
#define RB_DPTX_PANEL_INST   0u

static inline int rb_dptx_ref_trained(uint32_t panel_link_cntl)
{
    return rb_dptx_trained(panel_link_cntl) &&
           rb_dptx_link_status(panel_link_cntl);
}

#define RB_DPTX_W_OK              0u
#define RB_DPTX_W_NOT_ARMED      60u
#define RB_DPTX_W_NO_REGS        61u
#define RB_DPTX_W_PANEL_INST     62u  /* DIG0/DP0 - the panel, by name     */
#define RB_DPTX_W_INST_RANGE     63u
#define RB_DPTX_W_REF_UNTRAINED  64u  /* the block is NOT identified       */
#define RB_DPTX_W_TPS_RANGE      65u
#define RB_DPTX_W_STREAM_LIVE    66u  /* a video stream is running on it   */

static inline uint32_t rb_dptx_write_refuse(uint32_t inst, uint32_t tps,
                                            uint32_t panel_link_cntl,
                                            uint32_t live_vid,
                                            int armed, int have_regs)
{
    if (!armed)     { return RB_DPTX_W_NOT_ARMED; }
    if (!have_regs) { return RB_DPTX_W_NO_REGS; }
    if (inst == RB_DPTX_PANEL_INST) { return RB_DPTX_W_PANEL_INST; }
    if (inst >= RB_DPTX_COUNT)      { return RB_DPTX_W_INST_RANGE; }
    /* Rule 47 asked of a write: the live word must decode as the register the
     * conclusion assumes, measured against the link the firmware trained. */
    if (!rb_dptx_ref_trained(panel_link_cntl)) {
        return RB_DPTX_W_REF_UNTRAINED;
    }
    if (!rb_dptx_tps_valid(tps))    { return RB_DPTX_W_TPS_RANGE; }
    /* A training pattern replaces the video stream on that transmitter. If
     * one is running, something else owns this instance and we are not it. */
    if (rb_dptx_stream_on(live_vid)) { return RB_DPTX_W_STREAM_LIVE; }
    return RB_DPTX_W_OK;
}

/* The three read-modify-writes, each keeping every field but its own. */
static inline uint32_t rb_dptx_link_cntl_word(uint32_t live)
{ return live & ~RB_DPTX_TRAINING_COMPLETE; }

static inline uint32_t rb_dptx_dphy_cntl_word(uint32_t live)
{ return live & ~RB_DPTX_DPHY_BYPASS; }

static inline uint32_t rb_dptx_prbs_cntl_word(uint32_t live)
{ return live & ~RB_DPTX_PRBS_EN; }

/* `clean` for an RMW: nothing but the field moved. Separate from `applied`,
 * because a write that lands AND disturbs a neighbour is not a success and
 * 2EC paid for learning that. */
static inline int rb_dptx_rmw_clean(uint32_t before, uint32_t after,
                                    uint32_t field)
{ return ((before & ~field) == (after & ~field)) ? 1 : 0; }

/* ===================== THE SINK-SIDE VERDICT ===========================
 *
 * The transmitter's own registers can only say our stores landed. Whether a
 * pattern reached the SINK is in the sink's DPCD 0x202/0x203, which we do not
 * write at all - the strongest verdict available, and the same shape as
 * DPDTO_ENABLE_STATUS in 2F0/2F2.
 *
 * drm_dp.h:812-815: LANE0_1_STATUS, lane 0 in the low nibble and lane 1 in
 * the high one, CR_DONE 1<<0, CHANNEL_EQ_DONE 1<<1, SYMBOL_LOCKED 1<<2.
 */
#define RB_DPTX_LANE_CR_DONE       0x1u
#define RB_DPTX_LANE_EQ_DONE       0x2u
#define RB_DPTX_LANE_SYM_LOCKED    0x4u

static inline uint32_t rb_dptx_lane_status(uint32_t byte, uint32_t lane)
{ return (lane & 1u) ? ((byte >> 4) & 0x7u) : (byte & 0x7u); }

static inline int rb_dptx_cr_done(uint32_t byte, uint32_t lanes)
{
    uint32_t i;
    if (!lanes || lanes > 2u) { return 0; }   /* one status byte is 2 lanes */
    for (i = 0; i < lanes; i++) {
        if (!(rb_dptx_lane_status(byte, i) & RB_DPTX_LANE_CR_DONE)) {
            return 0;
        }
    }
    return 1;
}

/* ---- 2F5: did any BIT actually move? (rule 20, paid for again) --------
 *
 * Boot 458 ran `dptrain go` and every one of the four registers read back
 * before == after: TP_SEL 0 -> 0, LINK_CNTL 0 -> 0, DPHY_CNTL 0 -> 0,
 * PRBS_CNTL 0x7fffff00 -> 0x7fffff00. `applied` was 1 and `clean` was 1 and
 * BOTH were honest, because every target state was ALREADY PRESENT on a dark
 * transmitter: TPS1 converts to hardware pattern 0, and the three RMWs each
 * clear a bit that a dark instance does not have set. So the whole Linux TPS1
 * sequence is a NO-OP here, and `applied 1` was exactly the defect rule 20
 * names - a readback agreeing with a target that was already there.
 *
 * `moved` is the missing term. It is NOT a success criterion (a correct write
 * of a value that is already present legitimately moves nothing); it is the
 * term that stops such a write being reported as evidence about the sink.
 */
static inline int rb_dptx_moved(uint32_t tp_b, uint32_t tp_a,
                                uint32_t lc_b, uint32_t lc_a,
                                uint32_t dc_b, uint32_t dc_a,
                                uint32_t pc_b, uint32_t pc_a)
{
    return (tp_b != tp_a || lc_b != lc_a ||
            dc_b != dc_a || pc_b != pc_a) ? 1 : 0;
}

/* Whether THIS pattern can be distinguished from the dark state at all. The
 * register's dark value is 0 and TPS1 encodes as 0, so a TPS1 write is
 * unobservable in TP_SEL by construction - the one pattern the dial defaulted
 * to. TPS2/3/4 encode as 1/2/3 and are observable. */
static inline int rb_dptx_pattern_observable(uint32_t tps, uint32_t tp_live)
{
    if (!rb_dptx_tps_valid(tps)) { return 0; }
    return (rb_dptx_pattern(tp_live) != rb_dptx_hw_pattern(tps)) ? 1 : 0;
}

/* What the transmitter write established, as a code. CR_DONE is the prize and
 * is NOT expected until the transmitter is also enabled at the link rate -
 * saying so here is what keeps a negative from being read as a broken block.
 *
 * 2F5 added NO_OP and SINK_BLIND, both of which boot 458 was misreported as
 * PATTERN_SET: nothing moved on our side, AND the sink's after-read failed,
 * and the 2F4 verdict absorbed both into the reading it called "expected".
 */
#define RB_DPTX_V_NOT_RUN       (-1)
#define RB_DPTX_V_NO_STORE        0   /* our own registers did not take   */
#define RB_DPTX_V_PATTERN_SET     1   /* they did; the sink shows nothing */
#define RB_DPTX_V_SINK_MOVED      2   /* the sink's status byte CHANGED   */
#define RB_DPTX_V_CR_DONE         3   /* clock recovery on every lane     */
#define RB_DPTX_V_NO_OP           4   /* took, but not one bit moved      */
#define RB_DPTX_V_SINK_BLIND      5   /* our side took; the sink unread   */

static inline int rb_dptx_verdict(int applied, int clean, int moved,
                                  int sink_read_ok,
                                  uint32_t sink_before, uint32_t sink_after,
                                  uint32_t lanes)
{
    if (!applied || !clean) { return RB_DPTX_V_NO_STORE; }
    /* Before anything about the sink: a write that moved no bit cannot be
     * credited with the sink's state, whatever that state is. Rule 17 - two
     * numbers are not a mechanism, and here one of them is not even a
     * change. So NO_OP wins over CR_DONE deliberately. */
    if (!moved) { return RB_DPTX_V_NO_OP; }
    if (!sink_read_ok) { return RB_DPTX_V_SINK_BLIND; }
    if (rb_dptx_cr_done(sink_after, lanes)) { return RB_DPTX_V_CR_DONE; }
    if (sink_after != sink_before) { return RB_DPTX_V_SINK_MOVED; }
    return RB_DPTX_V_PATTERN_SET;
}

#endif /* RB_DPTX_H */
