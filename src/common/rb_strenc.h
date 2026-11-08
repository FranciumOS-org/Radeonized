/*
 * rb_strenc.h - the DIO STREAM encoder, read-only. The thing that was never
 * programmed.
 *
 * 302. Thirteen boots of this line put a clock on pipe 1, a raster on OTG1, a
 * front end on DIG2, a training pattern on the wire and a transmitter ENABLE
 * through the DMCUB, and the sink saw nothing. 301 retired the last candidate
 * ("the analog PHY is off" was read off the wrong register). What has never
 * been attempted, in any boot, is a STREAM: no stream encoder register on this
 * chip has ever been written or even read by this kext.
 *
 * `grep -rn DP_VID_STREAM src/` found nothing before this file.
 *
 * ------------------------------------------------------- WHICH FILES, per rule 46
 *
 * The method's version grouping, which here is THREE files for one operation -
 * 2FA's two-file split again, one worse:
 *
 *   dcn31_resource.c:1323  `dcn30_dio_stream_encoder_construct(...)` over
 *                          `stream_enc_regs[eng_id]`, a
 *                          `struct dcn10_stream_enc_registers` (:361). So the
 *                          construct is DCN30's and the register struct is
 *                          DCN10's.
 *   dcn30_dio_stream_encoder.c:832  `dcn30_str_enc_funcs`, and the two entries
 *                          that matter are NOT dcn30's:
 *                            .dp_set_stream_attribute = enc2_...  (DCN20)
 *                            .dp_unblank              = enc2_...  (DCN20)
 *                            .dig_connect_to_otg      = enc1_...  (DCN10)
 *   dcn20_stream_encoder.c:562  `enc2_stream_encoder_dp_set_stream_attribute`
 *                          delegates the whole body to
 *                          `enc1_stream_encoder_dp_set_stream_attribute` and
 *                          adds exactly ONE write of its own,
 *                          DP_SEC_FRAMING4.DP_SST_SDP_SPLITTING.
 *   dcn10_stream_encoder.c:245  the body: eight registers, listed below.
 *   dcn20_stream_encoder.c:490  `enc2_stream_encoder_dp_unblank`: the stream
 *                          enable, five more registers.
 *
 * AND A CORRECTION TO THE EARLIER HANDOFF, from the same read.
 * `setup_dio_stream_encoder` (link_hwss_dio.c:49) calls, in order,
 * `connect_dig_be_to_fe`, `enable_stream`, `map_stream_to_link`,
 * `set_input_mode` and `enable_fifo` - each guarded by `if (funcs->x)`. In
 * `dcn30_str_enc_funcs` **all four of the last ones are absent**, checked by
 * name against the table. So on THIS chip that whole function reduces to
 * `connect_dig_be_to_fe`, which 2EC already did. The stream does not come from
 * there. It comes from `setup_dio_stream_attribute` (:116) and from the
 * unblank, and neither has ever run here.
 *
 * ------------------------------------------- WHY THIS IS A READ AND NOT A WRITE
 *
 * **DIG0 IS A LIVE, WORKING DP STREAM** - the panel, lit, composited, measured
 * at 143.83 Hz native. That makes this the strongest in-boot positive control
 * this project has ever had for a write set: every field below has a known-good
 * value sitting in instance 0 right now, and DIG2's is whatever the firmware
 * left. A write designed against upstream alone would be a write designed
 * against a manual; a write designed against DIG0's live words is designed
 * against the machine (rule 25).
 *
 * It also discharges the address arithmetic the way 2EF did for the DTO base,
 * by a check the chip has to satisfy rather than by reasoning:
 * `DP_MSA_TIMING_PARAM1` on instance 0 must decode to the panel's **own**
 * H_TOTAL and V_TOTAL - 2106 and 1129, established on hardware and already in
 * the card. If it does not, the offsets are wrong and nothing else in this
 * file means anything. That check is `rb_strenc_base_verdict()` and it refuses
 * rather than reporting (rule 14).
 *
 * ------------------------------------------------------ AND THE VERDICT REGISTER
 *
 * `DP_VID_STREAM_CNTL` carries `DP_VID_STREAM_ENABLE` (bit 0, what the driver
 * writes) **and `DP_VID_STREAM_STATUS` (bit 16, what the hardware answers)**.
 * `enc2_stream_encoder_dp_unblank` writes the first and then
 * `REG_WAIT`s on the second, which is upstream telling us in its own code that
 * the second is a real status bit and not a readback of the first.
 *
 * **This is the verdict register the external-display line has been missing
 * since 2EB.** Every instrument so far has either answered our own writes
 * (DIG_BE_CNTL, TP_SEL, PIXEL_RATE_CNTL - a block agreeing with itself) or
 * been the wrong register (RDPCSTX2_PHY_CNTL6, retired in 301). This one is
 * a hardware-driven status with a live positive control on instance 0 and a
 * known-negative on the three instances never brought up.
 *
 * Read-only. There is no setter in this file. Nothing may add one before a
 * boot has established that instance 0's words decode as the panel's own
 * timing (rule 47 asked of a write), and the three never-brought-up instances
 * are the negative control for that same sample.
 */
#ifndef RB_STRENC_H
#define RB_STRENC_H

#include <stdint.h>

/* Five DIG/DP instances, as everywhere else in this tree. DP0/DIGA is the
 * PANEL'S and is refused by name in every half of every experiment on this
 * line; here it is read, because reading it is the entire control. */
#define RB_STRENC_COUNT        5u
#define RB_STRENC_PANEL_INST   0u
#define RB_STRENC_TYPEC_INST   2u   /* HPD2 = AUX2 = phyid 2 = DIG2 */

/* dcn_3_1_2_offset.h, all BASE_IDX 2, instance stride 0x100 - but the stride
 * is NOT used to derive them. Every offset is pinned per register below and
 * per instance in the hosttest, because this chip has already been measured
 * holding three different strides inside one function (2FA: OTG 0x80, VTG 1,
 * ODM 0x10), and a formula that happens to be right is still a formula checked
 * against itself. The stride constant exists only so the hosttest can assert
 * the pinned values are consistent with it - never the other way round. */
#define RB_STRENC_INST_STRIDE  0x100u

/* The eight writes of enc1_stream_encoder_dp_set_stream_attribute
 * (dcn10_stream_encoder.c:245), in that function's own order. */
#define RB_STRENC_DW_PIXEL_FORMAT   0x2109u  /* DP0_DP_PIXEL_FORMAT      */
#define RB_STRENC_DW_MSA_COLORIM    0x210au  /* DP0_DP_MSA_COLORIMETRY   */
#define RB_STRENC_DW_MSA_MISC       0x210eu  /* DP0_DP_MSA_MISC          */
#define RB_STRENC_DW_MSA_PARAM1     0x214cu  /* HTOTAL / VTOTAL          */
#define RB_STRENC_DW_MSA_PARAM2     0x214du  /* HSTART / VSTART          */
#define RB_STRENC_DW_MSA_PARAM3     0x214eu  /* sync width / polarity    */
#define RB_STRENC_DW_MSA_PARAM4     0x214fu  /* HWIDTH / VHEIGHT         */
/* enc2's one addition (dcn20_stream_encoder.c:577). */
#define RB_STRENC_DW_SEC_FRAMING4   0x2130u
/* The five of enc2_stream_encoder_dp_unblank (dcn20_stream_encoder.c:490). */
#define RB_STRENC_DW_VID_STREAM_CNTL 0x210cu /* ENABLE + the STATUS bit  */
#define RB_STRENC_DW_STEER_FIFO      0x210du
#define RB_STRENC_DW_VID_TIMING      0x2110u
#define RB_STRENC_DW_VID_N           0x2111u
#define RB_STRENC_DW_VID_M           0x2112u
/* DIG_FE_CNTL: not a DP register, but it carries DIG_SOURCE_SELECT (the OTG
 * this front end takes its pixels from - enc1_dig_connect_to_otg's only write,
 * dcn10_stream_encoder.c:1497) and DIG_START, which dp_unblank toggles. The
 * stream cannot be read without it. */
#define RB_STRENC_DW_DIG_FE_CNTL     0x208bu

#define RB_STRENC_DW_INVALID         0u

/* Register slots, in the order published. Adding one means adding its offset,
 * its hosttest case and its break-it check - the enum's LAST value is the
 * count, so a slot cannot be added without the array growing with it. */
enum rb_strenc_reg {
    RB_STRENC_R_PIXEL_FORMAT = 0,
    RB_STRENC_R_MSA_COLORIM,
    RB_STRENC_R_MSA_MISC,
    RB_STRENC_R_MSA_PARAM1,
    RB_STRENC_R_MSA_PARAM2,
    RB_STRENC_R_MSA_PARAM3,
    RB_STRENC_R_MSA_PARAM4,
    RB_STRENC_R_SEC_FRAMING4,
    RB_STRENC_R_VID_STREAM_CNTL,
    RB_STRENC_R_STEER_FIFO,
    RB_STRENC_R_VID_TIMING,
    RB_STRENC_R_VID_N,
    RB_STRENC_R_VID_M,
    RB_STRENC_R_DIG_FE_CNTL,
    RB_STRENC_REG_COUNT
};

static inline uint32_t rb_strenc_reg_base(enum rb_strenc_reg r)
{
    switch (r) {
    case RB_STRENC_R_PIXEL_FORMAT:    return RB_STRENC_DW_PIXEL_FORMAT;
    case RB_STRENC_R_MSA_COLORIM:     return RB_STRENC_DW_MSA_COLORIM;
    case RB_STRENC_R_MSA_MISC:        return RB_STRENC_DW_MSA_MISC;
    case RB_STRENC_R_MSA_PARAM1:      return RB_STRENC_DW_MSA_PARAM1;
    case RB_STRENC_R_MSA_PARAM2:      return RB_STRENC_DW_MSA_PARAM2;
    case RB_STRENC_R_MSA_PARAM3:      return RB_STRENC_DW_MSA_PARAM3;
    case RB_STRENC_R_MSA_PARAM4:      return RB_STRENC_DW_MSA_PARAM4;
    case RB_STRENC_R_SEC_FRAMING4:    return RB_STRENC_DW_SEC_FRAMING4;
    case RB_STRENC_R_VID_STREAM_CNTL: return RB_STRENC_DW_VID_STREAM_CNTL;
    case RB_STRENC_R_STEER_FIFO:      return RB_STRENC_DW_STEER_FIFO;
    case RB_STRENC_R_VID_TIMING:      return RB_STRENC_DW_VID_TIMING;
    case RB_STRENC_R_VID_N:           return RB_STRENC_DW_VID_N;
    case RB_STRENC_R_VID_M:           return RB_STRENC_DW_VID_M;
    case RB_STRENC_R_DIG_FE_CNTL:     return RB_STRENC_DW_DIG_FE_CNTL;
    default:                           return RB_STRENC_DW_INVALID;
    }
}

static inline uint32_t rb_strenc_dw(unsigned inst, enum rb_strenc_reg r)
{
    const uint32_t base = rb_strenc_reg_base(r);

    if (inst >= RB_STRENC_COUNT) { return RB_STRENC_DW_INVALID; }
    if (base == RB_STRENC_DW_INVALID) { return RB_STRENC_DW_INVALID; }
    return base + (uint32_t)inst * RB_STRENC_INST_STRIDE;
}

/* 0x254f: DP4_DP_MSA_TIMING_PARAM4, the highest dword this file touches.
 *
 * THE ENUM ORDER IS NOT THE ADDRESS ORDER, and this bound was WRONG in its
 * first draft - the hosttest caught it before any boot. DIG_FE_CNTL (0x208b)
 * sorts last in the enum and is the LOWEST address; DP_SEC_FRAMING4 (0x2130)
 * reads like the top of the block and is not, because the four
 * DP_MSA_TIMING_PARAM registers sit at 0x214c..0x214f, ABOVE it. A bound taken
 * from either the final enum slot or from SEC_FRAMING4 is short - by 0xc4
 * dwords in the first case and 0x1f in the second - and a short aperture bound
 * is a dereference outside the mapping (rule 19). So the hosttest derives the
 * true maximum over every (instance, register) pair and compares it with this
 * function rather than trusting either to be right. */
static inline uint32_t rb_strenc_dw_max(void)
{
    return RB_STRENC_DW_MSA_PARAM4 +
           (RB_STRENC_COUNT - 1u) * RB_STRENC_INST_STRIDE;
}

/* ---------------------------------------------------------------- the fields */

/* dcn_3_1_2_sh_mask.h. DP_VID_STREAM_ENABLE is what the driver writes;
 * DP_VID_STREAM_STATUS is what the hardware answers, and upstream REG_WAITs on
 * it after writing the other, which is what makes it a witness and not a
 * readback. */
#define RB_STRENC_VID_STREAM_ENABLE   0x00000001u  /* bit 0  */
#define RB_STRENC_VID_STREAM_STATUS   0x00010000u  /* bit 16 */
#define RB_STRENC_PIXEL_ENCODING_MASK 0x00000007u
#define RB_STRENC_COMPONENT_DEPTH_MASK 0x07000000u
#define RB_STRENC_COMPONENT_DEPTH_SHIFT 24u
#define RB_STRENC_MSA_HTOTAL_MASK     0xffff0000u
#define RB_STRENC_MSA_HTOTAL_SHIFT    16u
#define RB_STRENC_MSA_VTOTAL_MASK     0x0000ffffu
#define RB_STRENC_VID_M_N_GEN_EN      0x00000100u  /* bit 8  */
#define RB_STRENC_VID_N_MASK          0x00ffffffu
#define RB_STRENC_VID_M_MASK          0x00ffffffu
#define RB_STRENC_DIG_SOURCE_SEL_MASK 0x00000007u
#define RB_STRENC_DIG_START           0x00000400u  /* bit 10 */
#define RB_STRENC_STEER_FIFO_RESET    0x00000001u  /* bit 0  */

static inline int rb_strenc_enabled(uint32_t cntl)
{
    return (cntl & RB_STRENC_VID_STREAM_ENABLE) ? 1 : 0;
}

/* The witness. Separate accessor from the one above, deliberately: publishing
 * `applied` without `changed` is 2E6's defect, and publishing the bit we wrote
 * as though it were the bit the hardware answered would be the same error in a
 * new register. */
static inline int rb_strenc_status(uint32_t cntl)
{
    return (cntl & RB_STRENC_VID_STREAM_STATUS) ? 1 : 0;
}

static inline uint32_t rb_strenc_htotal(uint32_t param1)
{
    return (param1 & RB_STRENC_MSA_HTOTAL_MASK) >> RB_STRENC_MSA_HTOTAL_SHIFT;
}

static inline uint32_t rb_strenc_vtotal(uint32_t param1)
{
    return param1 & RB_STRENC_MSA_VTOTAL_MASK;
}

static inline uint32_t rb_strenc_source_otg(uint32_t fe_cntl)
{
    return fe_cntl & RB_STRENC_DIG_SOURCE_SEL_MASK;
}

static inline int rb_strenc_dig_started(uint32_t fe_cntl)
{
    return (fe_cntl & RB_STRENC_DIG_START) ? 1 : 0;
}

/* ------------------------------------------------ the per-instance verdict */

enum rb_strenc_verdict {
    RB_STRENC_NOT_SAMPLED   = 0,
    /* ENABLE set and STATUS set: a stream is running on this instance. On
     * instance 0 this is the POSITIVE CONTROL and must be what comes back, or
     * nothing else in the sample is a reading. */
    RB_STRENC_RUNNING       = 1,
    /* ENABLE set, STATUS clear. The state upstream's REG_WAIT exists for, and
     * it is reported as its own code rather than folded into either
     * neighbour: "asked for and not granted" is the single most interesting
     * thing this register can say about DIG2 after a write. */
    RB_STRENC_ASKED_NOT_RUNNING = 2,
    /* ENABLE clear, STATUS set. Should not occur; reported rather than
     * normalised, because it would mean the word is not this register
     * (rule 14 - a refusal is a code). */
    RB_STRENC_STATUS_WITHOUT_ENABLE = 3,
    /* Both clear: no stream. The expected reading on DIG2 before any write,
     * and on the three instances never brought up. */
    RB_STRENC_OFF           = 4
};

static inline enum rb_strenc_verdict rb_strenc_verdict(uint32_t cntl)
{
    const int en = rb_strenc_enabled(cntl);
    const int st = rb_strenc_status(cntl);

    if (en && st)   { return RB_STRENC_RUNNING; }
    if (en && !st)  { return RB_STRENC_ASKED_NOT_RUNNING; }
    if (!en && st)  { return RB_STRENC_STATUS_WITHOUT_ENABLE; }
    return RB_STRENC_OFF;
}

/* --------------------------------- THE BASE CHECK, which gates everything else
 *
 * The one thing the chip itself has to satisfy. Instance 0 is the panel, so its
 * DP_MSA_TIMING_PARAM1 must decode as the panel's live raster; if it does not,
 * the offsets are wrong - and 2FA's three-strides-in-one-function is exactly
 * how that happens at an address that still reads something plausible.
 *
 * The totals are PASSED IN rather than hardcoded, because the DRR ladder moves
 * V_TOTAL (that is the whole point of `V_BLANK_START_END` 1311820) and 2FB
 * already misconverted a measurement by treating 143.83 Hz as true at the
 * moment of a reading when the panel was at 61.40. So the caller supplies this
 * boot's own live OTG0 values and the comparison is against the machine, not
 * against the card.
 *
 * H_TOTAL is the strict half: it does not move with DRR. V_TOTAL is allowed a
 * window for exactly that reason, and the window is NAMED rather than silent.
 *
 * ------------------------------------------- THE OFF-BY-ONE, PAID FOR IN BOOT 472
 *
 * **THE TWO REGISTERS DO NOT HOLD THE SAME NUMBER.** This check's first draft
 * compared them raw and boot 472 refused with `BASE_HTOTAL_BAD` - instance 0
 * read H_TOTAL 2107 / V_TOTAL 1130 against a live OTG0 of 2106 / 1129. Exactly
 * +1 on both, which is a convention and not an address fault:
 *
 *   dcn10_optc.c:191-193   `/` `* CRTC_H_TOTAL = vesa.h_total - 1 *` `/`
 *                          REG_SET(OTG_H_TOTAL, 0, OTG_H_TOTAL,
 *                                  patched_crtc_timing.h_total - 1);
 *   dcn10_stream_encoder.c:419-421
 *                          REG_SET_2(DP_MSA_TIMING_PARAM1, 0,
 *                                    DP_MSA_HTOTAL, hw_crtc_timing.h_total,
 *                                    DP_MSA_VTOTAL, hw_crtc_timing.v_total);
 *
 * The OTG register holds `total - 1`; MSA holds the true total. So
 * MSA == OTG + 1, on both axes, and the +1 below is upstream's arithmetic
 * rather than a fudge to make a reading fit. The relation is asserted in the
 * hosttest against boot 472's own measured word.
 *
 * It is worth saying what this near-miss was: the gate REFUSED, printed that
 * nothing below was a reading, and exited - which is why one register's
 * convention was found in minutes instead of becoming a wrong conclusion about
 * five instances. An off-by-one that passes is 2FB's shape.
 *
 * AND THE CORROBORATION THAT COSTS NOTHING. `DP_MSA_TIMING_PARAM4` holds
 * HWIDTH and VHEIGHT - the ACTIVE raster (dcn10_stream_encoder.c:458-462),
 * which does NOT move with DRR at all, unlike either total. Boot 472 read
 * 0x07800438 on instance 0 = 1920 x 1080, the panel exactly. That is a second,
 * independent confirmation of the same addresses from the same sample, and
 * `rb_strenc_active_agrees()` exists so a caller can publish it beside the
 * base verdict rather than a human noticing it by eye.
 */
#define RB_STRENC_VTOTAL_SLACK  512u   /* the DRR ladder's range, generously */

enum rb_strenc_base {
    RB_STRENC_BASE_NOT_SAMPLED = 0,
    /* Instance 0 decodes as the live panel raster. The addresses are right. */
    RB_STRENC_BASE_CONFIRMED   = 1,
    /* H_TOTAL disagrees. The offsets are wrong, or this is not the panel's
     * instance. Nothing else in the sample is a reading. */
    RB_STRENC_BASE_HTOTAL_BAD  = 2,
    /* H_TOTAL agrees and V_TOTAL is outside the DRR window. Weaker than
     * HTOTAL_BAD and reported separately, because it is what a DRR transition
     * mid-sample looks like and that is not an address fault. */
    RB_STRENC_BASE_VTOTAL_ODD  = 3,
    /* Instance 0's word is zero. Distinct from a disagreement: it is what a
     * dead aperture, a gated block and a wrong segment all read, and it must
     * never pass as a confirmation (2zw's shape). */
    RB_STRENC_BASE_ZERO        = 4,
    /* No live raster to compare against - modeSetup never measured one. Its
     * own code, not a failure: an absent reference is not a wrong address
     * (rule 14, and 2EF's first draft got this wrong and would have cost a
     * boot). */
    RB_STRENC_BASE_NO_EXPECTED = 5
};

static inline enum rb_strenc_base rb_strenc_base_verdict(uint32_t param1_inst0,
                                                           uint32_t live_htotal,
                                                           uint32_t live_vtotal)
{
    uint32_t h, v, dv;

    if (live_htotal == 0u || live_vtotal == 0u) {
        return RB_STRENC_BASE_NO_EXPECTED;
    }
    if (param1_inst0 == 0u) { return RB_STRENC_BASE_ZERO; }

    h = rb_strenc_htotal(param1_inst0);
    v = rb_strenc_vtotal(param1_inst0);

    /* +1: the OTG register holds `total - 1` and MSA holds the true total.
     * See the header - this is upstream's arithmetic, cited, not a fit. */
    if (h != live_htotal + 1u) { return RB_STRENC_BASE_HTOTAL_BAD; }

    live_vtotal += 1u;
    dv = (v > live_vtotal) ? (v - live_vtotal) : (live_vtotal - v);
    if (dv > RB_STRENC_VTOTAL_SLACK) { return RB_STRENC_BASE_VTOTAL_ODD; }

    return RB_STRENC_BASE_CONFIRMED;
}

/* The live active raster, derived from OTG0's blank registers rather than from
 * a constant - rule 25, read the machine. `OTG_H_BLANK_START_END` holds START
 * in the low 15 bits and END in the high, and the blank WRAPS: on this panel
 * the word is 0x007f07ff, which is START 2047 and END 127, and the active
 * width is START - END = 1920 (rb_otg.h:23 records the same decode).
 *
 * So the subtraction is START minus END and not the other way round, which is
 * the one thing about this register that looks wrong until the wrap is
 * noticed. Returns 0 if the fields do not make a sane window, so the caller's
 * "no reference" path takes over rather than a nonsense number being compared
 * (rule 14). */
/* dcn_3_1_2_offset.h: regOTG0_OTG_H_BLANK_START_END 0x1b2b,
 * regOTG0_OTG_V_BLANK_START_END 0x1b36, both BASE_IDX 2 - the same segment and
 * the same block rb_dpdto.h takes OTG0's totals from. */
#define RB_STRENC_DW_OTG0_H_BLANK   0x1b2bu
#define RB_STRENC_DW_OTG0_V_BLANK   0x1b36u

#define RB_STRENC_BLANK_START_MASK  0x00007fffu
#define RB_STRENC_BLANK_END_MASK    0x7fff0000u
#define RB_STRENC_BLANK_END_SHIFT   16u

static inline uint32_t rb_strenc_active_from_blank(uint32_t blank)
{
    const uint32_t start = blank & RB_STRENC_BLANK_START_MASK;
    const uint32_t end   = (blank & RB_STRENC_BLANK_END_MASK) >>
                           RB_STRENC_BLANK_END_SHIFT;

    if (start == 0u || end >= start) { return 0; }
    return start - end;
}

/* THE SECOND CHECK, and the one that cannot drift. DP_MSA_TIMING_PARAM4 holds
 * the ACTIVE raster (HWIDTH, VHEIGHT - dcn10_stream_encoder.c:458-462), which
 * the DRR ladder does not touch: V_BLANK_START_END is what moves, and the
 * whole point of that register is that V_TOTAL changes without the active
 * window changing. So this comparison needs no window and no slack at all,
 * which makes it a better address check than either total - it just cannot be
 * made against a number the kext measures per boot, so the panel's active size
 * is the caller's to supply.
 *
 * Returns 1 on agreement, 0 on disagreement, and -1 for "nothing to compare",
 * because a missing reference is not a failure (rule 14). */
static inline int rb_strenc_active_agrees(uint32_t param4_inst0,
                                          uint32_t active_w,
                                          uint32_t active_h)
{
    if (active_w == 0u || active_h == 0u) { return -1; }
    if (param4_inst0 == 0u) { return 0; }
    return (((param4_inst0 & RB_STRENC_MSA_HTOTAL_MASK) >>
             RB_STRENC_MSA_HTOTAL_SHIFT) == active_w &&
            (param4_inst0 & RB_STRENC_MSA_VTOTAL_MASK) == active_h) ? 1 : 0;
}

/* ------------------------------------------------------------- the sweep */

enum rb_strenc_sweep {
    RB_STRENC_SWEEP_NOT_SAMPLED = 0,
    /* At least two instances differ. The control: it proves these are five
     * register sets at five addresses and not one block read five times, which
     * is the failure the 0x100 stride makes easy to miss. */
    RB_STRENC_SWEEP_DIFFERS     = 1,
    /* All five identical. Here, unlike rb_dio.h's uniform case, this would be
     * a POSITIVE DEFECT and not merely uninterpretable: instance 0 is a
     * running stream and the others are not, so five identical words mean the
     * instance arithmetic did nothing. */
    RB_STRENC_SWEEP_UNIFORM     = 2,
    RB_STRENC_SWEEP_BAD_ARGS    = 3
};

static inline enum rb_strenc_sweep rb_strenc_sweep_verdict(
    const uint32_t *cntl, unsigned count)
{
    unsigned i;

    if (!cntl || count == 0u || count > RB_STRENC_COUNT) {
        return RB_STRENC_SWEEP_BAD_ARGS;
    }
    for (i = 1; i < count; i++) {
        if (cntl[i] != cntl[0]) { return RB_STRENC_SWEEP_DIFFERS; }
    }
    return RB_STRENC_SWEEP_UNIFORM;
}

/* Mask of instances with a running stream. Expected: bit 0 only, before
 * anything of ours has written. Published as a mask and NOT as a count or an
 * index, because `RBFBAuxEn` and `RBFBHpdSink` were masks read as indexes and
 * that cost two misreads (2FB, rule 33). */
static inline uint32_t rb_strenc_running_mask(const uint32_t *cntl,
                                               unsigned count)
{
    uint32_t m = 0;
    unsigned i;

    if (!cntl) { return 0; }
    if (count > RB_STRENC_COUNT) { count = RB_STRENC_COUNT; }
    for (i = 0; i < count; i++) {
        if (rb_strenc_verdict(cntl[i]) == RB_STRENC_RUNNING) {
            m |= (uint32_t)1u << i;
        }
    }
    return m;
}

/* ===================================================================== *
 *  304 - the MSA derivation, and the panel as its control.
 *
 *  Every field DP_MSA_TIMING_PARAM1..4 wants is recoverable from six OTG
 *  registers of the pipe that will feed the stream, by inverting
 *  optc1_program_timing (dcn10_optc.c:189-250) into
 *  enc1_stream_encoder_dp_set_stream_attribute (dcn10_stream_encoder.c:422-460).
 *
 *  optc writes, per axis:
 *      H_TOTAL        = h_total - 1                       (:191)
 *      H_SYNC_A       START = 0, END = h_sync_width       (:195)
 *      H_BLANK START  = h_total - h_front_porch           (:201)
 *      H_BLANK END    = BLANK_START - h_active            (:205)
 *      H_SYNC_A_CNTL  POL = !HSYNC_POSITIVE_POLARITY      (:217)
 *  and the MSA write wants:
 *      HTOTAL      = h_total
 *      HSTART      = h_sync_width + h_back_porch
 *      HSYNCWIDTH  = h_sync_width
 *      HSYNCPOLARITY = !HSYNC_POSITIVE_POLARITY
 *      HWIDTH      = h_active (borders folded in)
 *
 *  Substituting h_blank, h_front_porch and h_back_porch out collapses the
 *  whole of HSTART to a single register field:
 *
 *      h_front_porch = h_total      - BLANK_START
 *      h_active      = BLANK_START  - BLANK_END
 *      h_blank       = h_total      - h_active
 *      h_back_porch  = h_blank - h_front_porch - h_sync_width
 *                    = BLANK_END - SYNC_END
 *      HSTART        = h_sync_width + h_back_porch
 *                    = SYNC_END + (BLANK_END - SYNC_END)
 *                    = BLANK_END                     <-- just the field
 *
 *  So MSA_HSTART is OTG_H_BLANK_END and MSA_VSTART is OTG_V_BLANK_END, and
 *  the polarity bit transfers unchanged because optc and MSA both store the
 *  NEGATION of the same flag.
 *
 *  Verified against the live panel before any of this was written (boot 473):
 *      HSTART  127  == OTG0 H_BLANK_END  127
 *      VSTART   20  == OTG0 V_BLANK_END   20
 *      PARAM4  1920x1080 == blank START - END on both axes
 *
 *  THIS IS WHY THERE IS A CONTROL AND NOT JUST A DERIVATION. DIG0 is a live,
 *  working DP stream fed by OTG0, so deriving OTG0 -> MSA and requiring the
 *  result to equal DIG0's four live words BIT FOR BIT tests the arithmetic
 *  on hardware, in the same sample, before it is applied to DIG2. A
 *  derivation that cannot reproduce the panel does not get to program the
 *  external port (rule 47 asked of a write, rule 45's "its own instrument").
 * ===================================================================== */

/* Per-pipe OTG registers, stride 0x80, BASE_IDX 2. Instance 0 is the
 * panel's and is the control; instance 1 is the Type-C pipe. */
#define RB_STRENC_OTG_STRIDE        0x80u
#define RB_STRENC_DW_OTG_H_TOTAL      0x1b2au
#define RB_STRENC_DW_OTG_H_BLANK      0x1b2bu
#define RB_STRENC_DW_OTG_H_SYNC       0x1b2cu
#define RB_STRENC_DW_OTG_H_SYNC_CNTL  0x1b2du
#define RB_STRENC_DW_OTG_V_TOTAL      0x1b2fu
#define RB_STRENC_DW_OTG_V_BLANK      0x1b36u
#define RB_STRENC_DW_OTG_V_SYNC       0x1b37u
#define RB_STRENC_DW_OTG_V_SYNC_CNTL  0x1b38u

#define RB_STRENC_SYNC_START_MASK   0x00007fffu
#define RB_STRENC_SYNC_END_MASK     0x7fff0000u
#define RB_STRENC_SYNC_POL          0x00000001u
#define RB_STRENC_TOTAL_MASK        0x00007fffu

/* MSA field masks (dcn_3_1_2_sh_mask.h, DP0_DP_MSA_TIMING_PARAM*). */
#define RB_STRENC_MSA_VTOTAL_MASK   0x0000ffffu
#define RB_STRENC_MSA_HTOTAL_MASK   0xffff0000u
#define RB_STRENC_MSA_VSTART_MASK   0x0000ffffu
#define RB_STRENC_MSA_HSTART_MASK   0xffff0000u
#define RB_STRENC_MSA_VSYNCW_MASK   0x00007fffu
#define RB_STRENC_MSA_VSYNCPOL      0x00008000u
#define RB_STRENC_MSA_HSYNCW_MASK   0x7fff0000u
#define RB_STRENC_MSA_HSYNCPOL      0x80000000u
#define RB_STRENC_MSA_VHEIGHT_MASK  0x0000ffffu
#define RB_STRENC_MSA_HWIDTH_MASK   0xffff0000u

/* One pipe's raster, as eight raw words read in one sample. */
struct rb_strenc_raster {
    uint32_t h_total;      /* OTG_H_TOTAL          - holds h_total - 1 */
    uint32_t h_blank;      /* OTG_H_BLANK_START_END                    */
    uint32_t h_sync;       /* OTG_H_SYNC_A                             */
    uint32_t h_sync_cntl;  /* OTG_H_SYNC_A_CNTL                        */
    uint32_t v_total;      /* OTG_V_TOTAL          - holds v_total - 1 */
    uint32_t v_blank;      /* OTG_V_BLANK_START_END                    */
    uint32_t v_sync;       /* OTG_V_SYNC_A                             */
    uint32_t v_sync_cntl;  /* OTG_V_SYNC_A_CNTL                        */
};

struct rb_strenc_msa {
    uint32_t param1;
    uint32_t param2;
    uint32_t param3;
    uint32_t param4;
};

/* Every way the derivation can decline. A refusal is a CODE and every path
 * out says something (rule 14); nothing here returns a bool. */
enum rb_strenc_derive {
    RB_STRENC_DERIVE_OK             = 0,
    RB_STRENC_DERIVE_NO_ARGS        = 1,
    RB_STRENC_DERIVE_TOTAL_ZERO     = 2,  /* pipe not programmed at all     */
    RB_STRENC_DERIVE_BLANK_ZERO     = 3,
    RB_STRENC_DERIVE_BLANK_ORDER    = 4,  /* END >= START: not a raster     */
    RB_STRENC_DERIVE_ACTIVE_ZERO    = 5,
    RB_STRENC_DERIVE_ACTIVE_OVER    = 6,  /* active >= total                */
    RB_STRENC_DERIVE_SYNC_ZERO      = 7,
    RB_STRENC_DERIVE_SYNC_START_SET = 8,  /* optc programs START = 0 (:195) */
    RB_STRENC_DERIVE_FIELD_OVERFLOW = 9   /* would not fit its MSA field    */
};

static inline uint32_t rb_strenc_sync_start(uint32_t w)
{ return w & RB_STRENC_SYNC_START_MASK; }

static inline uint32_t rb_strenc_sync_end(uint32_t w)
{ return (w & RB_STRENC_SYNC_END_MASK) >> 16u; }

static inline uint32_t rb_strenc_blank_start(uint32_t w)
{ return w & RB_STRENC_BLANK_START_MASK; }

static inline uint32_t rb_strenc_blank_end(uint32_t w)
{ return (w & RB_STRENC_BLANK_END_MASK) >> 16u; }

/* Derive the four MSA words from one pipe's eight OTG words. Writes *out
 * ONLY on RB_STRENC_DERIVE_OK, so a refused derivation cannot be half
 * applied. */
static inline enum rb_strenc_derive rb_strenc_derive_msa(
        const struct rb_strenc_raster *r, struct rb_strenc_msa *out)
{
    uint32_t ht, vt, hb_s, hb_e, vb_s, vb_e;
    uint32_t h_act, v_act, hsw, vsw, hpol, vpol;

    if (!r || !out) { return RB_STRENC_DERIVE_NO_ARGS; }

    /* The +1 that refused boot 472: the OTG register holds `total - 1` and
     * MSA holds the true total (dcn10_optc.c:191 vs
     * dcn10_stream_encoder.c:419). */
    ht = (r->h_total & RB_STRENC_TOTAL_MASK);
    vt = (r->v_total & RB_STRENC_TOTAL_MASK);
    if (ht == 0u || vt == 0u) { return RB_STRENC_DERIVE_TOTAL_ZERO; }
    ht += 1u;
    vt += 1u;

    hb_s = rb_strenc_blank_start(r->h_blank);
    hb_e = rb_strenc_blank_end(r->h_blank);
    vb_s = rb_strenc_blank_start(r->v_blank);
    vb_e = rb_strenc_blank_end(r->v_blank);
    if (hb_s == 0u || vb_s == 0u) { return RB_STRENC_DERIVE_BLANK_ZERO; }
    if (hb_e >= hb_s || vb_e >= vb_s) { return RB_STRENC_DERIVE_BLANK_ORDER; }

    h_act = hb_s - hb_e;
    v_act = vb_s - vb_e;
    if (h_act == 0u || v_act == 0u) { return RB_STRENC_DERIVE_ACTIVE_ZERO; }
    if (h_act >= ht || v_act >= vt) { return RB_STRENC_DERIVE_ACTIVE_OVER; }

    /* optc writes START = 0 on both sync registers (dcn10_optc.c:195, :233).
     * A non-zero start means this is not a raster optc programmed, so the
     * inversion below does not apply to it. */
    if (rb_strenc_sync_start(r->h_sync) != 0u ||
        rb_strenc_sync_start(r->v_sync) != 0u) {
        return RB_STRENC_DERIVE_SYNC_START_SET;
    }
    hsw = rb_strenc_sync_end(r->h_sync);
    vsw = rb_strenc_sync_end(r->v_sync);
    if (hsw == 0u || vsw == 0u) { return RB_STRENC_DERIVE_SYNC_ZERO; }

    /* Both sides store the negation of the same flag, so the bit transfers
     * unchanged (dcn10_optc.c:217 vs dcn10_stream_encoder.c:448). */
    hpol = r->h_sync_cntl & RB_STRENC_SYNC_POL;
    vpol = r->v_sync_cntl & RB_STRENC_SYNC_POL;

    if (ht > 0xffffu || vt > 0xffffu ||
        hb_e > 0xffffu || vb_e > 0xffffu ||
        h_act > 0xffffu || v_act > 0xffffu ||
        hsw > 0x7fffu || vsw > 0x7fffu) {
        return RB_STRENC_DERIVE_FIELD_OVERFLOW;
    }

    out->param1 = ((ht << 16u) & RB_STRENC_MSA_HTOTAL_MASK) |
                  (vt & RB_STRENC_MSA_VTOTAL_MASK);
    /* HSTART = H_BLANK_END, VSTART = V_BLANK_END - the collapse above. */
    out->param2 = ((hb_e << 16u) & RB_STRENC_MSA_HSTART_MASK) |
                  (vb_e & RB_STRENC_MSA_VSTART_MASK);
    out->param3 = ((hsw << 16u) & RB_STRENC_MSA_HSYNCW_MASK) |
                  (hpol ? RB_STRENC_MSA_HSYNCPOL : 0u) |
                  (vsw & RB_STRENC_MSA_VSYNCW_MASK) |
                  (vpol ? RB_STRENC_MSA_VSYNCPOL : 0u);
    out->param4 = ((h_act << 16u) & RB_STRENC_MSA_HWIDTH_MASK) |
                  (v_act & RB_STRENC_MSA_VHEIGHT_MASK);
    return RB_STRENC_DERIVE_OK;
}

/* The control. Returns a bitmask of which PARAMs disagree with the live
 * words, bit N for param N+1; 0 means all four agree bit for bit. A
 * disagreement is reported per word rather than as a bool so the journal
 * records WHICH field the arithmetic got wrong. */
#define RB_STRENC_MSA_MISMATCH_ARGS  0x10u
static inline uint32_t rb_strenc_msa_mismatch(const struct rb_strenc_msa *d,
                                               const uint32_t *live4)
{
    uint32_t m = 0;

    if (!d || !live4) { return RB_STRENC_MSA_MISMATCH_ARGS; }
    if (d->param1 != live4[0]) { m |= 1u; }
    if (d->param2 != live4[1]) { m |= 2u; }
    if (d->param3 != live4[2]) { m |= 4u; }
    if (d->param4 != live4[3]) { m |= 8u; }
    return m;
}

/* ---- the unblank's M/N (dcn20_stream_encoder.c:476-512) ---- */

#define RB_STRENC_N_VID             0x8000u
#define RB_STRENC_LINK_REF_KHZ      27000u
#define RB_STRENC_LINK_RATE_HBR     10u   /* 2.70 Gbps */
#define RB_STRENC_LINK_RATE_HBR2    20u   /* 5.40 Gbps */

/* m_vid = N * pixel_khz / (link_rate * 27000), in 64-bit because the
 * numerator passes 2^32 at any real pixel clock. Returns 0 on bad input,
 * which the caller must treat as a refusal: 0 is never a valid M. */
static inline uint32_t rb_strenc_m_vid(uint32_t pix_khz, uint32_t link_rate)
{
    uint64_t num;
    uint32_t den;

    if (pix_khz == 0u || link_rate == 0u) { return 0u; }
    den = link_rate * RB_STRENC_LINK_REF_KHZ;
    if (den == 0u) { return 0u; }
    num = (uint64_t)RB_STRENC_N_VID * (uint64_t)pix_khz;
    num /= (uint64_t)den;
    if (num == 0u || num > 0x00ffffffu) { return 0u; }  /* DP_VID_M is 24 bits */
    return (uint32_t)num;
}

#endif /* RB_STRENC_H */
