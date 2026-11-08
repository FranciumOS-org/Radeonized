/* rb_strprog.h - THE STREAM WRITE on a DIO stream encoder instance.
 *
 * 305. The write half of rb_strenc.h, which stays read-only. Freestanding C,
 * no writes performed here: this file BUILDS a list and the caller performs
 * it, so the hosttest decides everything the kext decides.
 *
 * Licensed by boot 474 and by nothing else. The arithmetic that fills
 * DP_MSA_TIMING_PARAM1..4 comes from rb_strenc_derive_msa(), which in that
 * boot reproduced DIG0's four live MSA words BIT FOR BIT from OTG0's eight
 * registers - including PARAM3, whose sync and polarity inputs were read
 * rather than assumed for the first time. A derivation that cannot reproduce
 * the panel does not get to program the external port (rule 47 asked of a
 * write), so rb_strprog_build() takes the mismatch mask as an argument and
 * refuses unless it is zero.
 *
 * FOUR LINUX FILES FOR ONE OPERATION, all vendored under docs/linux-ref/:
 *   dcn31_resource.c:1323   builds with dcn30_dio_stream_encoder_construct
 *                           over a struct dcn10_stream_enc_registers (:361)
 *   dcn30_dio_stream_encoder.c:832  dcn30_str_enc_funcs, which points
 *                           .dp_set_stream_attribute and .dp_unblank at
 *                           DCN20's and .dig_connect_to_otg at DCN10's
 *   dcn20_stream_encoder.c:468/562  the unblank, and the one extra attribute
 *                           write (DP_SEC_FRAMING4.DP_SST_SDP_SPLITTING)
 *   dcn10_stream_encoder.c:245/1491 the attribute body and the OTG connect
 *
 * THE MISSING STEP THIS FILE FOUND. `DIG_FE_CNTL.DIG_SOURCE_SELECT` - the
 * front end to OTG mux - is a PLAIN INDEX (enc1_dig_connect_to_otg,
 * dcn10_stream_encoder.c:1497: `REG_UPDATE(DIG_FE_CNTL, DIG_SOURCE_SELECT,
 * tg_inst)`). It appears nowhere in src/ or tools/ and has NEVER been
 * written: everything this project has done is
 * `DIG_BE_CNTL.DIG_FE_SOURCE_SELECT` (2EC), a DIFFERENT register and field,
 * which rb_digbe.h:27 records as a ONE-HOT MASK and which cost a misread for
 * being read as an index. Two adjacent muxes, opposite encodings - rule 33's
 * shape exactly. Boot 474 read DIG2's DIG_SOURCE_SELECT as 0, i.e. OTG0, the
 * PANEL's pipe. It must be 1.
 *
 * That 0 cannot be distinguished from unprogrammed, because 0 is also the
 * reset value - the same ambiguity 2FC hit on the DIO mux, where
 * ENCODER_TYPE_DIG is zero. The write is still correct either way, and the
 * caller publishes `changed` beside `applied` so a no-op is visible as one
 * (rule 20).
 */
#ifndef RB_STRPROG_H
#define RB_STRPROG_H

#include <stdint.h>
#include "rb_strenc.h"

/* Field masks, dcn_3_1_2_sh_mask.h. Each is quoted because a wrong mask on a
 * register whose other fields hold live state is not a cosmetic error. */
#define RB_STRPROG_M_PIXEL_ENCODING  0x00000007u /* DP_PIXEL_FORMAT          */
#define RB_STRPROG_M_COMPONENT_DEPTH 0x07000000u
#define RB_STRPROG_M_MSA_MISC0       0xff000000u /* DP_MSA_COLORIMETRY       */
#define RB_STRPROG_M_SDP_SPLITTING   0x00000001u /* DP_SEC_FRAMING4          */
#define RB_STRPROG_M_VID_M_N_GEN_EN  0x00000100u /* DP_VID_TIMING            */
#define RB_STRPROG_M_VID_N_MUL       0x00000c00u
#define RB_STRPROG_M_VID_N           0x00ffffffu
#define RB_STRPROG_M_VID_M           0x00ffffffu
#define RB_STRPROG_M_STREAM_ENABLE   0x00000001u /* DP_VID_STREAM_CNTL       */
#define RB_STRPROG_M_STEER_RESET     0x00000001u /* DP_STEER_FIFO            */
#define RB_STRPROG_M_DIG_START       0x00000400u /* DIG_FE_CNTL              */
#define RB_STRPROG_M_DIG_SOURCE_SEL  0x00000007u

/* MSA_MISC1 bits 6 and 7: bit 6 selects VSC SDP colorimetry (not used on an
 * 8b/10b SST RGB link) and bit 7 is the YCbCr range. sRGB clears both
 * (dcn10_stream_encoder.c:310, :370). Read-modify-write: the rest of that
 * register is not ours. */
#define RB_STRPROG_M_MSA_MISC_CLR    0x000000c0u

/* RGB444 and 8bpc, taken off THE PANEL'S OWN LIVE WORD rather than from an
 * enum header this tree does not vendor (rule 25). Boot 474: instance 0's
 * DP_PIXEL_FORMAT is 0x01000000, so DP_PIXEL_ENCODING is 0 and
 * DP_COMPONENT_DEPTH is 1, and DP_MSA_COLORIMETRY is 0x20000000, so MISC0 is
 * 0x20 = colorimetry_bpc 1 << 5 for 8bpc with no bit 3, i.e. full-range
 * sRGB. Three constants, all measured on a working stream. */
#define RB_STRPROG_PIXEL_ENCODING_RGB444  0u
#define RB_STRPROG_COMPONENT_DEPTH_8BPC   1u
#define RB_STRPROG_MSA_MISC0_8BPC_SRGB    0x20u

enum rb_strprog_err {
    RB_STRPROG_OK        = 0,
    RB_STRPROG_E_ARGS    = 1,  /* NULL, or no room for the writes          */
    RB_STRPROG_E_INST    = 2,  /* instance >= RB_STRENC_COUNT              */
    RB_STRPROG_E_PANEL   = 3,  /* instance 0 - DIG0 is the panel's, BY NAME */
    RB_STRPROG_E_MSA     = 4,  /* the derivation did not reproduce the panel */
    RB_STRPROG_E_DERIVE  = 5,  /* the target pipe's own derivation refused   */
    RB_STRPROG_E_MVID    = 6,  /* m_vid 0 - never a valid M                 */
    RB_STRPROG_E_OTG0    = 7,  /* tg_inst 0 - the panel's pipe, BY NAME     */
    RB_STRPROG_E_OTG     = 8,  /* tg_inst outside the mux field             */
    RB_STRPROG_E_PHASE   = 9   /* not a phase this file knows               */
};

/* Refusals the CALLER adds, above the builder's 1..8 so one published field
 * can carry either without ambiguity (rule 14: refusals are codes, and two
 * refusal spaces sharing a field is how "not armed" gets read as "the panel
 * was refused"). The hosttest pins the two ranges apart. */
#define RB_STRPROG_W_OK             0u
#define RB_STRPROG_W_NOT_ARMED      50u
#define RB_STRPROG_W_NO_REGS        51u
#define RB_STRPROG_W_APERTURE       52u
#define RB_STRPROG_W_NO_DERIVATION  53u  /* no derivation for that tg_inst */
#define RB_STRPROG_W_STATUS_STUCK   54u  /* STATUS never cleared after ENABLE 0 */
#define RB_STRPROG_W_FIRST          50u

/* One write. `whole` is Linux's REG_SET (write the register, zeroing every
 * other field) against REG_UPDATE (read, modify, write back) - the four MSA
 * timing registers are REG_SET and the rest are REG_UPDATE, and on a register
 * holding live state the difference is not cosmetic. `delay_us` is the
 * udelay that FOLLOWS the write, which the unblank depends on: 1 us after
 * DIG_START, 10 after the FIFO reset, 100 for the DIG/DP logic to prime
 * (dcn20_stream_encoder.c:518-536). */
struct rb_strprog_write {
    uint32_t dw;
    uint32_t mask;
    uint32_t value;      /* already shifted into place */
    uint32_t whole;
    uint32_t delay_us;
    const char *what;    /* for the published trace - rule 14 */
};

#define RB_STRPROG_MAX_WRITES 24u

/* THE TWO PHASES, AND WHY THEY ARE TWO.
 *
 * Upstream has two functions, not one: `dp_set_stream_attribute` configures
 * the encoder and `dp_unblank` turns the stream on, and LINK TRAINING HAPPENS
 * BETWEEN THEM (`enable_link_dp` trains, then unblanks). 305 collapsed both
 * into one nineteen-write list ending in `DP_VID_STREAM_ENABLE` 1, and boot
 * 477 paid for it: with the stream enabled, `dptrain` refuses with
 * RB_DPTX_W_STREAM_LIVE (66), "a video stream is running on that
 * transmitter". The chain locked itself out of its own next step.
 *
 * So the phases are separate operations here too, and the ATTRIBUTE phase
 * deliberately ends by writing ENABLE 0 - which upstream never needs, because
 * upstream has never enabled a stream at this point, and we may have. That is
 * the recovery from boot 477's state.
 *
 * The caller runs: ATTRIBUTE -> train the link -> UNBLANK. */
enum rb_strprog_phase {
    RB_STRPROG_PHASE_ATTRIBUTE = 0,
    RB_STRPROG_PHASE_UNBLANK   = 1
};

/* The built plan. The two wait points are RETURNED rather than hardcoded as
 * indexes into the list: they differ per phase, and a constant naming a
 * position in a list that someone will reorder is exactly the kind that
 * drifts silently. -1 means "this phase has no such wait". */
struct rb_strprog_plan {
    struct rb_strprog_write w[RB_STRPROG_MAX_WRITES];
    uint32_t n;
    int wait_off_idx;   /* after this write, wait for STATUS == 0 */
    int wait_on_idx;    /* after this write, read STATUS back     */
};

static inline uint32_t rb_strprog_shift(uint32_t mask)
{
    uint32_t s = 0;
    if (!mask) { return 0; }
    while (!(mask & 1u)) { mask >>= 1; s++; }
    return s;
}

static inline uint32_t rb_strprog_field(uint32_t mask, uint32_t v)
{
    return (v << rb_strprog_shift(mask)) & mask;
}

/* Build one phase's write list. Returns RB_STRPROG_OK, or a refusal code with
 * plan->n = 0 - a refused build never leaves a partial list, because a
 * partially applied stream write is worse than none.
 *
 * `msa_mismatch` is rb_strenc_msa_mismatch() for PIPE 0 in THIS boot's
 * sample: the derivation checked against the live panel. Zero is the licence
 * to write. `derive_rc` is rb_strenc_derive_msa()'s code for the TARGET pipe,
 * which is a different question - the panel says the arithmetic is right, the
 * target pipe says it had a raster to apply it to.
 */
static inline int rb_strprog_build(uint32_t inst,
                                   uint32_t tg_inst,
                                   const struct rb_strenc_msa *msa,
                                   uint32_t msa_mismatch,
                                   int derive_rc,
                                   uint32_t m_vid,
                                   enum rb_strprog_phase phase,
                                   struct rb_strprog_plan *plan)
{
    uint32_t c = 0;
    const uint32_t base = inst * RB_STRENC_INST_STRIDE;

    if (!msa || !plan) { return RB_STRPROG_E_ARGS; }
    plan->n = 0;
    plan->wait_off_idx = -1;
    plan->wait_on_idx = -1;
    if (phase != RB_STRPROG_PHASE_ATTRIBUTE &&
        phase != RB_STRPROG_PHASE_UNBLANK) {
        return RB_STRPROG_E_PHASE;
    }
    /* DIG0 is the panel's live working DP stream - the control every
     * experiment on this line is measured against. Refused BY NAME here, and
     * the tool refuses it again on the way in. */
    if (inst == RB_STRENC_PANEL_INST) { return RB_STRPROG_E_PANEL; }
    if (inst >= RB_STRENC_COUNT) { return RB_STRPROG_E_INST; }
    /* OTG0 drives the panel. Connecting another front end to it is how a
     * working desktop gets taken down by a write meant for a dark port. */
    if (tg_inst == 0u) { return RB_STRPROG_E_OTG0; }
    if (tg_inst > RB_STRPROG_M_DIG_SOURCE_SEL) { return RB_STRPROG_E_OTG; }

    if (msa_mismatch != 0u) { return RB_STRPROG_E_MSA; }
    if (derive_rc != (int)RB_STRENC_DERIVE_OK) { return RB_STRPROG_E_DERIVE; }
    if (m_vid == 0u) { return RB_STRPROG_E_MVID; }

#define RB_STRPROG_ADD(reg, msk, val, whl, dly, txt)                        \
    do {                                                                    \
        if (c >= RB_STRPROG_MAX_WRITES) { plan->n = 0;                      \
            return RB_STRPROG_E_ARGS; }                                     \
        plan->w[c].dw = (reg) + base; plan->w[c].mask = (msk);              \
        plan->w[c].value = (val); plan->w[c].whole = (whl);                 \
        plan->w[c].delay_us = (dly); plan->w[c].what = (txt); c++;          \
    } while (0)

    if (phase == RB_STRPROG_PHASE_ATTRIBUTE) {
        /* enc1_dig_connect_to_otg (dcn10_stream_encoder.c:1497). A PLAIN
         * INDEX, not the one-hot mask the BE-to-FE mux uses. Never written by
         * this project before 305. */
        RB_STRPROG_ADD(RB_STRENC_DW_DIG_FE_CNTL, RB_STRPROG_M_DIG_SOURCE_SEL,
                       rb_strprog_field(RB_STRPROG_M_DIG_SOURCE_SEL, tg_inst),
                       0u, 0u, "DIG_FE_CNTL.DIG_SOURCE_SELECT");

        /* enc1_stream_encoder_dp_set_stream_attribute
         * (dcn10_stream_encoder.c:245). Only the RGB444/8bpc/sRGB path; every
         * YCbCr and VSC-SDP branch upstream takes is unreachable here because
         * the three constants are the panel's own measured values. */
        RB_STRPROG_ADD(RB_STRENC_DW_PIXEL_FORMAT,
                       RB_STRPROG_M_PIXEL_ENCODING | RB_STRPROG_M_COMPONENT_DEPTH,
                       rb_strprog_field(RB_STRPROG_M_PIXEL_ENCODING,
                                        RB_STRPROG_PIXEL_ENCODING_RGB444) |
                       rb_strprog_field(RB_STRPROG_M_COMPONENT_DEPTH,
                                        RB_STRPROG_COMPONENT_DEPTH_8BPC),
                       0u, 0u, "DP_PIXEL_FORMAT RGB444 8bpc");
        /* REG_SET: upstream zeroes the rest of this register (:432). */
        RB_STRPROG_ADD(RB_STRENC_DW_MSA_COLORIM, RB_STRPROG_M_MSA_MISC0,
                       rb_strprog_field(RB_STRPROG_M_MSA_MISC0,
                                        RB_STRPROG_MSA_MISC0_8BPC_SRGB),
                       1u, 0u, "DP_MSA_COLORIMETRY MISC0 8bpc sRGB");
        RB_STRPROG_ADD(RB_STRENC_DW_MSA_MISC, RB_STRPROG_M_MSA_MISC_CLR,
                       0u, 0u, 0u, "DP_MSA_MISC1 bits 6,7 clear");
        RB_STRPROG_ADD(RB_STRENC_DW_MSA_PARAM1, 0xffffffffu, msa->param1,
                       1u, 0u, "DP_MSA_TIMING_PARAM1 HTOTAL/VTOTAL");
        RB_STRPROG_ADD(RB_STRENC_DW_MSA_PARAM2, 0xffffffffu, msa->param2,
                       1u, 0u, "DP_MSA_TIMING_PARAM2 HSTART/VSTART");
        RB_STRPROG_ADD(RB_STRENC_DW_MSA_PARAM3, 0xffffffffu, msa->param3,
                       1u, 0u, "DP_MSA_TIMING_PARAM3 sync widths/polarities");
        RB_STRPROG_ADD(RB_STRENC_DW_MSA_PARAM4, 0xffffffffu, msa->param4,
                       1u, 0u, "DP_MSA_TIMING_PARAM4 HWIDTH/VHEIGHT");
        /* enc2's one extra write (dcn20_stream_encoder.c:577). SST, single
         * stream, no splitting - written rather than left alone because
         * "already 0" and "not programmed" are the same word. */
        RB_STRPROG_ADD(RB_STRENC_DW_SEC_FRAMING4, RB_STRPROG_M_SDP_SPLITTING,
                       0u, 0u, 0u, "DP_SEC_FRAMING4.DP_SST_SDP_SPLITTING 0");
        /* NOT upstream's: leave the stream DISABLED so the link can be
         * trained. Boot 477's refusal 66 is the reason this write exists. */
        RB_STRPROG_ADD(RB_STRENC_DW_VID_STREAM_CNTL, RB_STRPROG_M_STREAM_ENABLE,
                       0u, 0u, 0u,
                       "DP_VID_STREAM_ENABLE 0 - leave it trainable");
        plan->wait_off_idx = (int)(c - 1u);
    } else {
        /* enc2_stream_encoder_dp_unblank (dcn20_stream_encoder.c:468).
         * Upstream's own first act is to make sure the stream is off and WAIT
         * for STATUS to follow, before resetting the steer FIFO. */
        RB_STRPROG_ADD(RB_STRENC_DW_VID_STREAM_CNTL, RB_STRPROG_M_STREAM_ENABLE,
                       0u, 0u, 0u, "DP_VID_STREAM_ENABLE 0 (then wait STATUS 0)");
        plan->wait_off_idx = (int)(c - 1u);

        /* M/N with GEN_EN dropped while the initial values go in: the auto
         * measurement needs one full 0x8000 symbol cycle to kick in, which is
         * why M is PRIMED and not programmed. The panel's own M drifted
         * 0xa2a6 -> 0xa2a4 -> 0xa2a7 across boots 472-474, which is what
         * GEN_EN does and the reason none of those values may be copied. */
        RB_STRPROG_ADD(RB_STRENC_DW_VID_TIMING, RB_STRPROG_M_VID_M_N_GEN_EN,
                       0u, 0u, 0u, "DP_VID_TIMING.M_N_GEN_EN 0");
        RB_STRPROG_ADD(RB_STRENC_DW_VID_N, RB_STRPROG_M_VID_N,
                       rb_strprog_field(RB_STRPROG_M_VID_N, RB_STRENC_N_VID),
                       0u, 0u, "DP_VID_N 0x8000");
        RB_STRPROG_ADD(RB_STRENC_DW_VID_M, RB_STRPROG_M_VID_M,
                       rb_strprog_field(RB_STRPROG_M_VID_M, m_vid),
                       0u, 0u, "DP_VID_M primed");
        /* n_multiply is 0: it is 1 only for YCbCr420 or two-pixel-per-
         * container modes (is_two_pixels_per_containter, :459), and this is
         * RGB444 SST. */
        RB_STRPROG_ADD(RB_STRENC_DW_VID_TIMING,
                       RB_STRPROG_M_VID_M_N_GEN_EN | RB_STRPROG_M_VID_N_MUL,
                       rb_strprog_field(RB_STRPROG_M_VID_M_N_GEN_EN, 1u),
                       0u, 0u, "DP_VID_TIMING.M_N_GEN_EN 1, N_MUL 0");

        /* DIG_START 1 then 0 resets the FIFO (:519-525). */
        RB_STRPROG_ADD(RB_STRENC_DW_DIG_FE_CNTL, RB_STRPROG_M_DIG_START,
                       rb_strprog_field(RB_STRPROG_M_DIG_START, 1u),
                       0u, 1u, "DIG_FE_CNTL.DIG_START 1");
        RB_STRPROG_ADD(RB_STRENC_DW_DIG_FE_CNTL, RB_STRPROG_M_DIG_START,
                       0u, 0u, 0u, "DIG_FE_CNTL.DIG_START 0");

        /* The steer FIFO pulse (:529-534). THIS IS WHAT CLEARS DIG2'S HELD
         * RESET, by construction rather than as a special case: boots 472,
         * 473 and 474 all read DP_STEER_FIFO_RESET set on DIG2 and clear on
         * the other four, and boot 477 confirmed the pulse ends it at 0. */
        RB_STRPROG_ADD(RB_STRENC_DW_STEER_FIFO, RB_STRPROG_M_STEER_RESET,
                       rb_strprog_field(RB_STRPROG_M_STEER_RESET, 1u),
                       0u, 10u, "DP_STEER_FIFO_RESET 1");
        RB_STRPROG_ADD(RB_STRENC_DW_STEER_FIFO, RB_STRPROG_M_STEER_RESET,
                       0u, 0u, 100u, "DP_STEER_FIFO_RESET 0");

        /* Nothing here programs DP_VID_STREAM_DIS_DEFER: DIG2 reads 0 against
         * 2 everywhere else in three boots, but dcn20_stream_encoder.c:541
         * says the register "has no effect on enable transition" and upstream
         * stopped writing it, so it is retired and left alone. */
        RB_STRPROG_ADD(RB_STRENC_DW_VID_STREAM_CNTL, RB_STRPROG_M_STREAM_ENABLE,
                       rb_strprog_field(RB_STRPROG_M_STREAM_ENABLE, 1u),
                       0u, 0u, "DP_VID_STREAM_ENABLE 1 (then read STATUS)");
        plan->wait_on_idx = (int)(c - 1u);
    }

#undef RB_STRPROG_ADD

    plan->n = c;
    return RB_STRPROG_OK;
}

/* What the hardware answered. The whole point of the ENABLE/STATUS split:
 * `enabled` is what we wrote and `status` is what the block says, and a
 * driver that reads only the first cannot tell a stream from a request for
 * one. */
enum rb_strprog_verdict {
    RB_STRPROG_V_NOT_RUN     = 0,
    RB_STRPROG_V_RUNNING     = 1,  /* ENABLE 1 and STATUS 1 - a stream      */
    RB_STRPROG_V_NO_STATUS   = 2,  /* ENABLE 1, STATUS 0 - asked, refused   */
    RB_STRPROG_V_NOT_ENABLED = 3   /* the write did not take at all         */
};

static inline int rb_strprog_verdict(uint32_t cntl_after)
{
    const int en = rb_strenc_enabled(cntl_after) ? 1 : 0;
    const int st = rb_strenc_status(cntl_after) ? 1 : 0;
    if (!en) { return RB_STRPROG_V_NOT_ENABLED; }
    return st ? RB_STRPROG_V_RUNNING : RB_STRPROG_V_NO_STATUS;
}

#endif /* RB_STRPROG_H */
