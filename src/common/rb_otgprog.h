/* rb_otgprog.h - programming a SECOND raster: optc1_program_timing plus
 * optc31_enable_crtc, for the pipe the external display's DP DTO already
 * clocks. Pure logic, no register access: it builds a write list and decides
 * every refusal, so the hosttest decides everything the kext decides.
 *
 * RULE 46 DISCHARGED BY SOURCE, not by reasoning. `dcn31_resource.c:1145`
 * builds every timing generator through `dcn31_timing_generator_init`, and
 * `dcn31_optc.c:248` is that file's `dcn31_tg_funcs`, whose
 * `.program_timing` is **`optc1_program_timing`** (line 250) - the DCN10
 * implementation, in `dcn10_optc.c`. `.enable_crtc` is `optc31_enable_crtc`
 * (line 255), DCN31's own. So the two halves of this file come from two
 * different files on purpose, and each is named where it is used.
 *
 * THE REGISTER OFFSETS ARE TABLES, NOT ARITHMETIC. The three blocks this
 * touches have THREE DIFFERENT STRIDES, read out of
 * `dcn_3_1_2_offset.h` per instance:
 *
 *     OTGn_*     stride 0x80   (OTG0_OTG_H_TOTAL 0x1b2a -> OTG1 0x1baa)
 *     VTGn_CONTROL stride 0x1  (0x052d, 0x052e, 0x052f, 0x0530)
 *     ODMn_*     stride 0x10   (0x1acb, 0x1adb, 0x1aeb, 0x1afb)
 *
 * Deriving one stride from another would have put the VTG and ODM writes
 * 0x80 dwords from where they belong - inside other blocks entirely. Every
 * offset below is quoted from the header for all four instances.
 *
 * ALL THREE BLOCKS ARE BASE_IDX 2, which is the base this kext has always
 * used (`rb_regs.h`), NOT the DP DTO's BASE_IDX 1. Checked per register in
 * `dcn_3_1_2_offset.h`: every `_BASE_IDX` beside the offsets below reads 2.
 *
 * WHAT THIS DOES NOT DO, said here so a null is not over-read (rule 14):
 *   - `program_global_sync` (OTG_VSTARTUP_PARAM, OTG_VUPDATE_PARAM,
 *     OTG_VREADY_PARAM) and `set_vtg_params` are NOT reproduced. They tell
 *     DCHUB when to fetch and when a flip may land - they matter to a
 *     SURFACE, and there is no surface on this pipe. A raster can scan
 *     without them; a flip cannot.
 *   - `OPTC_DATA_FORMAT_CONTROL` is left alone: the default 0 is RGB, which
 *     is what this sink is being driven as.
 *   - interlace is not programmed. This mode is progressive and
 *     `OTG_INTERLACE_ENABLE` already reads 0 on a dark pipe; writing 0 over
 *     0 would be rule 20's defect dressed as a step.
 *   - nothing is mapped from a stream encoder to this OTG. That is
 *     `enc31_map_stream_to_link` and it is a separate step that does not
 *     exist yet.
 *
 * So the HONEST verdict for this file is a RASTER, measured as
 * `OTG1_FRAME_COUNT` advancing - a MONOTONIC counter, which rule 31 requires
 * instead of two snapshots of a dialled value. A raster is not a picture.
 */
#ifndef RB_OTGPROG_H
#define RB_OTGPROG_H

#include <stdint.h>

#define RB_OTGPROG_COUNT        4u    /* dcn31_resource.c:872, num_timing_generator */

/* OTGn_OTG_* dword offsets, BASE_IDX 2, quoted per instance. */
#define RB_OTGPROG_DW_H_TOTAL(i)     (0x1b2au + (i) * 0x80u)
#define RB_OTGPROG_DW_H_BLANK(i)     (0x1b2bu + (i) * 0x80u)
#define RB_OTGPROG_DW_H_SYNC_A(i)    (0x1b2cu + (i) * 0x80u)
#define RB_OTGPROG_DW_H_SYNC_CNTL(i) (0x1b2du + (i) * 0x80u)
#define RB_OTGPROG_DW_H_TIMING(i)    (0x1b2eu + (i) * 0x80u)
#define RB_OTGPROG_DW_V_TOTAL(i)     (0x1b2fu + (i) * 0x80u)
#define RB_OTGPROG_DW_V_TOTAL_MIN(i) (0x1b30u + (i) * 0x80u)
#define RB_OTGPROG_DW_V_TOTAL_MAX(i) (0x1b31u + (i) * 0x80u)
#define RB_OTGPROG_DW_V_BLANK(i)     (0x1b36u + (i) * 0x80u)
#define RB_OTGPROG_DW_V_SYNC_A(i)    (0x1b37u + (i) * 0x80u)
#define RB_OTGPROG_DW_V_SYNC_CNTL(i) (0x1b38u + (i) * 0x80u)
#define RB_OTGPROG_DW_CONTROL(i)     (0x1b41u + (i) * 0x80u)
#define RB_OTGPROG_DW_FRAME_COUNT(i) (0x1b4cu + (i) * 0x80u)
/* VTGn_CONTROL - stride ONE, not 0x80. 0x052d/0x052e/0x052f/0x0530. */
#define RB_OTGPROG_DW_VTG(i)         (0x052du + (i) * 0x1u)
/* ODMn_OPTC_DATA_SOURCE_SELECT - stride 0x10. 0x1acb/0x1adb/0x1aeb/0x1afb. */
#define RB_OTGPROG_DW_ODM_SRC(i)     (0x1acbu + (i) * 0x10u)

/* Field masks, from dcn_3_1_2_sh_mask.h. Each is <register>__<field>_MASK. */
#define RB_OTGPROG_M_H_TOTAL         0x00007fffu
#define RB_OTGPROG_M_H_SYNC_START    0x00007fffu
#define RB_OTGPROG_M_H_SYNC_END      0x7fff0000u
#define RB_OTGPROG_M_H_BLANK_START   0x00007fffu
#define RB_OTGPROG_M_H_BLANK_END     0x7fff0000u
#define RB_OTGPROG_M_H_SYNC_POL      0x00000001u
#define RB_OTGPROG_M_H_DIV_MODE      0x00000003u
#define RB_OTGPROG_M_V_TOTAL         0x00007fffu
#define RB_OTGPROG_M_V_TOTAL_MIN     0x00007fffu
#define RB_OTGPROG_M_V_TOTAL_MAX     0x00007fffu
#define RB_OTGPROG_M_V_SYNC_START    0x00007fffu
#define RB_OTGPROG_M_V_SYNC_END      0x7fff0000u
#define RB_OTGPROG_M_V_BLANK_START   0x00007fffu
#define RB_OTGPROG_M_V_BLANK_END     0x7fff0000u
#define RB_OTGPROG_M_V_SYNC_POL      0x00000001u
#define RB_OTGPROG_M_MASTER_EN       0x00000001u
#define RB_OTGPROG_M_DISABLE_POINT   0x00000300u
#define RB_OTGPROG_M_START_POINT     0x00001000u
#define RB_OTGPROG_M_FIELD_NUMBER    0x00002000u
#define RB_OTGPROG_M_VTG_ENABLE      0x80000000u
#define RB_OTGPROG_M_ODM_SEG0_SRC    0x000f0000u

/* The 15-bit raster fields. A value that does not fit is a REFUSAL, never a
 * truncation: OTG_V_TOTAL silently keeping the low 15 bits of 40000 would
 * program a raster nobody asked for on a pipe whose clock is live. */
#define RB_OTGPROG_FIELD_MAX     0x7fffu

/* optc1_program_timing's DP branch: start_point 1 for DISPLAY_PORT, MST and
 * EDP; field_num stays 0 on a progressive mode. */
#define RB_OTGPROG_START_POINT_DP   1u
/* optc31_enable_crtc writes OTG_DISABLE_POINT_CNTL 2 with OTG_MASTER_EN 1. */
#define RB_OTGPROG_DISABLE_POINT    2u
/* H_TIMING_DIV_MODE: H_TIMING_NO_DIV, one OPP per OTG. */
#define RB_OTGPROG_H_DIV_NONE       0u

/* apply_front_porch_workaround (dcn10_optc.c): a progressive mode's vertical
 * front porch is floored at 1. It is a WORKAROUND in Linux's own words and
 * it changes the blank start, so it is applied here and reported. */
#define RB_OTGPROG_V_FRONT_PORCH_MIN 1u

enum rb_otgprog_err {
    RB_OTGPROG_OK          = 0,
    RB_OTGPROG_E_ARGS      = 1,  /* NULL, or no room for the writes      */
    RB_OTGPROG_E_INST      = 2,  /* instance >= RB_OTGPROG_COUNT         */
    RB_OTGPROG_E_PANEL     = 3,  /* instance 0 - the panel's, by name    */
    RB_OTGPROG_E_HACTIVE   = 4,  /* h_addressable 0                      */
    RB_OTGPROG_E_VACTIVE   = 5,
    RB_OTGPROG_E_HTOTAL    = 6,  /* h_total does not exceed the active   */
    RB_OTGPROG_E_VTOTAL    = 7,
    RB_OTGPROG_E_HSYNC     = 8,  /* sync width 0, or wider than blanking */
    RB_OTGPROG_E_VSYNC     = 9,
    RB_OTGPROG_E_HBLANK    = 10, /* front porch exceeds the blanking     */
    RB_OTGPROG_E_VBLANK    = 11,
    RB_OTGPROG_E_FIELD     = 12  /* a computed value exceeds 15 bits     */
};

/* A VESA-shaped mode, the same decomposition dc_crtc_timing uses. Borders
 * are not carried: this sink's EDID detailed timing has none, and a border
 * silently treated as 0 would be worse than one that cannot be expressed. */
struct rb_otgprog_mode {
    uint32_t h_addressable;
    uint32_t h_front_porch;
    uint32_t h_sync_width;
    uint32_t h_total;
    uint32_t v_addressable;
    uint32_t v_front_porch;
    uint32_t v_sync_width;
    uint32_t v_total;
    uint32_t h_sync_positive;   /* 1 = positive, which programs POL 0 */
    uint32_t v_sync_positive;
};

/* One write. `whole` distinguishes Linux's REG_SET (write the register,
 * zeroing every other field) from REG_UPDATE (read, modify, write back) -
 * optc1_program_timing uses REG_SET for H_TOTAL and V_TOTAL and REG_UPDATE
 * for the rest, and the difference is not cosmetic on a register whose other
 * fields hold live state. */
struct rb_otgprog_write {
    uint32_t dw;
    uint32_t mask;
    uint32_t value;   /* already shifted into place */
    uint32_t whole;
};

#define RB_OTGPROG_MAX_WRITES 16u

static inline uint32_t rb_otgprog_shift(uint32_t mask)
{
    uint32_t s = 0;
    if (!mask) return 0;
    while (!(mask & 1u)) { mask >>= 1; s++; }
    return s;
}

static inline uint32_t rb_otgprog_field(uint32_t mask, uint32_t v)
{
    return (v << rb_otgprog_shift(mask)) & mask;
}

/* The vertical front porch after Linux's workaround. Exposed so the caller
 * can publish what was actually programmed rather than what was asked. */
static inline uint32_t rb_otgprog_v_front_porch(const struct rb_otgprog_mode *m)
{
    if (!m) return 0;
    return m->v_front_porch < RB_OTGPROG_V_FRONT_PORCH_MIN
           ? RB_OTGPROG_V_FRONT_PORCH_MIN : m->v_front_porch;
}

/* The pixel clock a mode implies, in hertz. Not programmed by this file - the
 * DP DTO carries the clock - but it is the arithmetic that says a mode and a
 * clock belong together, and the caller refuses a mismatch. */
static inline uint32_t rb_otgprog_clock_hz(const struct rb_otgprog_mode *m,
                                           uint32_t refresh_hz)
{
    uint64_t p;
    if (!m || !m->h_total || !m->v_total || !refresh_hz) return 0;
    p = (uint64_t)m->h_total * (uint64_t)m->v_total * (uint64_t)refresh_hz;
    if (p > 0xffffffffull) return 0;
    return (uint32_t)p;
}

/* Build the write list. Returns RB_OTGPROG_OK and sets *n, or a refusal code
 * with *n = 0. The order is optc1_program_timing's, then
 * optc31_enable_crtc's, because the VTG enable is written 0 by the first and
 * 1 by the second and swapping them leaves the generator off. */
static inline int rb_otgprog_build(uint32_t inst,
                                   const struct rb_otgprog_mode *m,
                                   struct rb_otgprog_write *w,
                                   uint32_t max,
                                   uint32_t *n)
{
    uint32_t h_blank_start, h_blank_end, v_blank_start, v_blank_end;
    uint32_t v_fp, h_tot_m1, v_tot_m1, c = 0;

    if (n) *n = 0;
    if (!m || !w || !n || max < RB_OTGPROG_MAX_WRITES) return RB_OTGPROG_E_ARGS;
    /* Instance 0 is the panel's. OTG0_CONTROL reads 0x00011201 with
     * OTG_MASTER_EN set and 2F0 measured its DP DTO driving the panel; a
     * raster rewrite there takes the desktop down. Refused by name, and the
     * tool refuses it again on the way in. */
    if (inst == 0) return RB_OTGPROG_E_PANEL;
    if (inst >= RB_OTGPROG_COUNT) return RB_OTGPROG_E_INST;

    if (!m->h_addressable) return RB_OTGPROG_E_HACTIVE;
    if (!m->v_addressable) return RB_OTGPROG_E_VACTIVE;
    if (m->h_total <= m->h_addressable) return RB_OTGPROG_E_HTOTAL;
    if (m->v_total <= m->v_addressable) return RB_OTGPROG_E_VTOTAL;

    v_fp = rb_otgprog_v_front_porch(m);

    /* The blanking has to hold the front porch AND the sync. Linux computes
     * these without checking, because `dc` validated the mode upstream; here
     * the mode comes off a tool argument or an EDID this project parsed, so
     * the check is ours to make. */
    if (m->h_front_porch >= m->h_total - m->h_addressable)
        return RB_OTGPROG_E_HBLANK;
    if (v_fp >= m->v_total - m->v_addressable)
        return RB_OTGPROG_E_VBLANK;
    if (!m->h_sync_width
        || m->h_front_porch + m->h_sync_width > m->h_total - m->h_addressable)
        return RB_OTGPROG_E_HSYNC;
    if (!m->v_sync_width
        || v_fp + m->v_sync_width > m->v_total - m->v_addressable)
        return RB_OTGPROG_E_VSYNC;

    h_tot_m1      = m->h_total - 1u;
    v_tot_m1      = m->v_total - 1u;
    h_blank_start = m->h_total - m->h_front_porch;
    h_blank_end   = h_blank_start - m->h_addressable;
    v_blank_start = m->v_total - v_fp;
    v_blank_end   = v_blank_start - m->v_addressable;

    if (h_tot_m1 > RB_OTGPROG_FIELD_MAX || v_tot_m1 > RB_OTGPROG_FIELD_MAX
        || h_blank_start > RB_OTGPROG_FIELD_MAX
        || v_blank_start > RB_OTGPROG_FIELD_MAX
        || m->h_sync_width > RB_OTGPROG_FIELD_MAX
        || m->v_sync_width > RB_OTGPROG_FIELD_MAX)
        return RB_OTGPROG_E_FIELD;

    /* --- optc1_program_timing, in its own order --- */

    /* REG_SET: OTG_H_TOTAL = h_total - 1, every other field zeroed. */
    w[c].dw = RB_OTGPROG_DW_H_TOTAL(inst);
    w[c].mask = RB_OTGPROG_M_H_TOTAL;
    w[c].value = rb_otgprog_field(RB_OTGPROG_M_H_TOTAL, h_tot_m1);
    w[c].whole = 1u; c++;

    /* H_SYNC_A: start 0, end = h_sync_width. Both fields in one write, so
     * the start is explicitly zeroed rather than left as it was. */
    w[c].dw = RB_OTGPROG_DW_H_SYNC_A(inst);
    w[c].mask = RB_OTGPROG_M_H_SYNC_START | RB_OTGPROG_M_H_SYNC_END;
    w[c].value = rb_otgprog_field(RB_OTGPROG_M_H_SYNC_START, 0u)
               | rb_otgprog_field(RB_OTGPROG_M_H_SYNC_END, m->h_sync_width);
    w[c].whole = 0u; c++;

    w[c].dw = RB_OTGPROG_DW_H_BLANK(inst);
    w[c].mask = RB_OTGPROG_M_H_BLANK_START | RB_OTGPROG_M_H_BLANK_END;
    w[c].value = rb_otgprog_field(RB_OTGPROG_M_H_BLANK_START, h_blank_start)
               | rb_otgprog_field(RB_OTGPROG_M_H_BLANK_END, h_blank_end);
    w[c].whole = 0u; c++;

    /* POL is 0 for a POSITIVE sync - the polarity is inverted against the
     * flag's name, which is why this is a named conversion and not a copy. */
    w[c].dw = RB_OTGPROG_DW_H_SYNC_CNTL(inst);
    w[c].mask = RB_OTGPROG_M_H_SYNC_POL;
    w[c].value = rb_otgprog_field(RB_OTGPROG_M_H_SYNC_POL,
                                  m->h_sync_positive ? 0u : 1u);
    w[c].whole = 0u; c++;

    w[c].dw = RB_OTGPROG_DW_V_TOTAL(inst);
    w[c].mask = RB_OTGPROG_M_V_TOTAL;
    w[c].value = rb_otgprog_field(RB_OTGPROG_M_V_TOTAL, v_tot_m1);
    w[c].whole = 1u; c++;

    /* set_vtotal_min_max(v_total, v_total) with the SAME v_total - 1 Linux
     * passes. 2F0 measured MIN/MAX/MID all 0 on the panel, so DRR is off
     * here and equal bounds is what keeps it off. */
    w[c].dw = RB_OTGPROG_DW_V_TOTAL_MIN(inst);
    w[c].mask = RB_OTGPROG_M_V_TOTAL_MIN;
    w[c].value = rb_otgprog_field(RB_OTGPROG_M_V_TOTAL_MIN, v_tot_m1);
    w[c].whole = 0u; c++;

    w[c].dw = RB_OTGPROG_DW_V_TOTAL_MAX(inst);
    w[c].mask = RB_OTGPROG_M_V_TOTAL_MAX;
    w[c].value = rb_otgprog_field(RB_OTGPROG_M_V_TOTAL_MAX, v_tot_m1);
    w[c].whole = 0u; c++;

    w[c].dw = RB_OTGPROG_DW_V_SYNC_A(inst);
    w[c].mask = RB_OTGPROG_M_V_SYNC_START | RB_OTGPROG_M_V_SYNC_END;
    w[c].value = rb_otgprog_field(RB_OTGPROG_M_V_SYNC_START, 0u)
               | rb_otgprog_field(RB_OTGPROG_M_V_SYNC_END, m->v_sync_width);
    w[c].whole = 0u; c++;

    w[c].dw = RB_OTGPROG_DW_V_BLANK(inst);
    w[c].mask = RB_OTGPROG_M_V_BLANK_START | RB_OTGPROG_M_V_BLANK_END;
    w[c].value = rb_otgprog_field(RB_OTGPROG_M_V_BLANK_START, v_blank_start)
               | rb_otgprog_field(RB_OTGPROG_M_V_BLANK_END, v_blank_end);
    w[c].whole = 0u; c++;

    w[c].dw = RB_OTGPROG_DW_V_SYNC_CNTL(inst);
    w[c].mask = RB_OTGPROG_M_V_SYNC_POL;
    w[c].value = rb_otgprog_field(RB_OTGPROG_M_V_SYNC_POL,
                                  m->v_sync_positive ? 0u : 1u);
    w[c].whole = 0u; c++;

    /* "VTG enable set to 0 first VInit" - Linux's own comment. This is the
     * write that makes the ORDER of this list load-bearing. */
    w[c].dw = RB_OTGPROG_DW_VTG(inst);
    w[c].mask = RB_OTGPROG_M_VTG_ENABLE;
    w[c].value = rb_otgprog_field(RB_OTGPROG_M_VTG_ENABLE, 0u);
    w[c].whole = 0u; c++;

    w[c].dw = RB_OTGPROG_DW_CONTROL(inst);
    w[c].mask = RB_OTGPROG_M_START_POINT | RB_OTGPROG_M_FIELD_NUMBER;
    w[c].value = rb_otgprog_field(RB_OTGPROG_M_START_POINT,
                                  RB_OTGPROG_START_POINT_DP)
               | rb_otgprog_field(RB_OTGPROG_M_FIELD_NUMBER, 0u);
    w[c].whole = 0u; c++;

    w[c].dw = RB_OTGPROG_DW_H_TIMING(inst);
    w[c].mask = RB_OTGPROG_M_H_DIV_MODE;
    w[c].value = rb_otgprog_field(RB_OTGPROG_M_H_DIV_MODE,
                                  RB_OTGPROG_H_DIV_NONE);
    w[c].whole = 0u; c++;

    /* --- optc31_enable_crtc --- */

    /* "opp instance for OTG, 1 to 1 mapping and odm will adjust" */
    w[c].dw = RB_OTGPROG_DW_ODM_SRC(inst);
    w[c].mask = RB_OTGPROG_M_ODM_SEG0_SRC;
    w[c].value = rb_otgprog_field(RB_OTGPROG_M_ODM_SEG0_SRC, inst);
    w[c].whole = 0u; c++;

    /* "VTG enable first is for HW workaround" */
    w[c].dw = RB_OTGPROG_DW_VTG(inst);
    w[c].mask = RB_OTGPROG_M_VTG_ENABLE;
    w[c].value = rb_otgprog_field(RB_OTGPROG_M_VTG_ENABLE, 1u);
    w[c].whole = 0u; c++;

    /* The one that starts it. */
    w[c].dw = RB_OTGPROG_DW_CONTROL(inst);
    w[c].mask = RB_OTGPROG_M_DISABLE_POINT | RB_OTGPROG_M_MASTER_EN;
    w[c].value = rb_otgprog_field(RB_OTGPROG_M_DISABLE_POINT,
                                  RB_OTGPROG_DISABLE_POINT)
               | rb_otgprog_field(RB_OTGPROG_M_MASTER_EN, 1u);
    w[c].whole = 0u; c++;

    *n = c;
    return RB_OTGPROG_OK;
}

/* --- the write gate ---------------------------------------------------
 *
 * A separate code space from the build errors above, so a refusal published
 * as a number says which of the two decided (rule 14: refusals are codes).
 * The numbers start at 40 to keep them distinct in a published field from
 * rb_dpdto.h's, which a reader of the same ioreg dump sees beside them.
 */
#define RB_OTGPROG_W_OK           0u
#define RB_OTGPROG_W_UNARMED     40u  /* -rbgpufbraster absent             */
#define RB_OTGPROG_W_NOREGS      41u  /* no register window, or no sample   */
#define RB_OTGPROG_W_PANEL       42u  /* instance 0, by name                */
#define RB_OTGPROG_W_INST        43u  /* instance out of range              */
#define RB_OTGPROG_W_RUNNING     44u  /* OTG_MASTER_EN already set          */
#define RB_OTGPROG_W_NOCLOCK     45u  /* that pipe's DP DTO is not running  */
#define RB_OTGPROG_W_MODE        46u  /* the mode itself was refused        */
#define RB_OTGPROG_W_CLOCKMISS   47u  /* the mode does not match the clock  */

/* How far the mode's implied clock may sit from the clock already programmed
 * into that pipe's DP DTO, in parts per thousand. 2F0 measured the panel's
 * PHASE 0.208 % away from its own raster's product and CONFIRMED the base at
 * that distance, so a window tighter than that would reject a correct pair.
 * 5 parts per thousand is 0.5 %, the same window rb_dpdto.h argued for
 * before it had data. */
#define RB_OTGPROG_CLOCK_TOL_PPT  5u

/* Why the clock has to match at all, said once: the DTO carries the pixel
 * clock and the OTG carries the raster, and the refresh rate is the QUOTIENT
 * of the two. A raster that does not multiply out to the clock already
 * running produces a refresh nobody asked for, on a link the sink was told
 * to expect something else on. Nothing downstream would report that as an
 * error - it would just not lock - so it is checked here.
 *
 * `dto_phase_hz` is the DTO's PHASE register, which 2F0 established is the
 * clock the firmware or we ASKED for, not a readback of what the hardware
 * achieved. That is exactly the right thing to compare a mode against: both
 * sides are requests. */
static inline int rb_otgprog_clock_agrees(uint32_t mode_hz, uint32_t dto_phase_hz)
{
    uint64_t diff, tol;
    if (!mode_hz || !dto_phase_hz) return 0;
    diff = mode_hz > dto_phase_hz ? (uint64_t)(mode_hz - dto_phase_hz)
                                  : (uint64_t)(dto_phase_hz - mode_hz);
    tol = (uint64_t)dto_phase_hz * RB_OTGPROG_CLOCK_TOL_PPT / 1000ull;
    return diff <= tol;
}

/* The whole gate, in one place, so the kext cannot hold half of it.
 * `live_control` is OTGn_CONTROL as read THIS sample; `dto_phase_hz` is that
 * pipe's DP_DTO PHASE; `refresh_hz` is the refresh the caller is asking for,
 * which with the mode gives the clock to compare. */
static inline uint32_t rb_otgprog_write_refuse(uint32_t inst,
                                               const struct rb_otgprog_mode *m,
                                               uint32_t refresh_hz,
                                               uint32_t live_control,
                                               uint32_t dto_phase_hz,
                                               int armed,
                                               int have_regs)
{
    struct rb_otgprog_write w[RB_OTGPROG_MAX_WRITES];
    uint32_t n = 0, mode_hz;
    int rc;

    if (!armed) return RB_OTGPROG_W_UNARMED;
    if (!have_regs || !m) return RB_OTGPROG_W_NOREGS;
    if (inst == 0) return RB_OTGPROG_W_PANEL;
    if (inst >= RB_OTGPROG_COUNT) return RB_OTGPROG_W_INST;

    /* Nothing in this project turns a running OTG off, and rewriting the
     * raster of a generator that is already scanning is the one way this
     * code could take a display down. The panel is refused by name above;
     * this refuses any pipe that is live, including one we started
     * ourselves - which is why a second call needs a reboot, exactly as the
     * DP DTO's own guard does. */
    if (live_control & RB_OTGPROG_M_MASTER_EN) return RB_OTGPROG_W_RUNNING;

    /* A raster with no pixel clock is a raster that cannot scan. The DTO
     * must already be programmed on this pipe, which is a step the operator
     * takes first and which has its own verdict. */
    if (!dto_phase_hz) return RB_OTGPROG_W_NOCLOCK;

    rc = rb_otgprog_build(inst, m, w, RB_OTGPROG_MAX_WRITES, &n);
    if (rc != RB_OTGPROG_OK) return RB_OTGPROG_W_MODE;

    mode_hz = rb_otgprog_clock_hz(m, refresh_hz);
    if (!rb_otgprog_clock_agrees(mode_hz, dto_phase_hz))
        return RB_OTGPROG_W_CLOCKMISS;

    return RB_OTGPROG_W_OK;
}

/* The verdict codes. The verdict is OTG1_FRAME_COUNT ADVANCING, which is a
 * monotonic hardware counter and not a readback of anything written (rule
 * 31, rule 20). MASTER_EN reading back 1 is NOT the verdict: it is our own
 * bit coming back. */
#define RB_OTGPROG_V_NOT_RUN      (-1)
#define RB_OTGPROG_V_SCANNING       0   /* frame count advanced  */
#define RB_OTGPROG_V_STILL          1   /* master en set, count did not move */
#define RB_OTGPROG_V_NOT_ENABLED    2   /* MASTER_EN did not read back       */
#define RB_OTGPROG_V_REFUSED        3

static inline int rb_otgprog_verdict(uint32_t control_after,
                                     uint32_t frames_before,
                                     uint32_t frames_after)
{
    if (!(control_after & RB_OTGPROG_M_MASTER_EN))
        return RB_OTGPROG_V_NOT_ENABLED;
    /* Unsigned difference, so a counter wrapping past its width still reads
     * as advanced rather than as a huge negative. */
    if ((uint32_t)(frames_after - frames_before) != 0u)
        return RB_OTGPROG_V_SCANNING;
    return RB_OTGPROG_V_STILL;
}

#endif /* RB_OTGPROG_H */
