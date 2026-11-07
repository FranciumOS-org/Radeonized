/* SPDX-License-Identifier: MIT */
#ifndef RB_DPDTO_H
#define RB_DPDTO_H

#include <stdint.h>

/* 2EF: THE DP PIXEL-CLOCK DTO, READ-ONLY - AND THE SEGMENT CHECK THAT HAS TO
 * COME BEFORE ANY WRITE TO IT.
 *
 * WHY THIS BLOCK AT ALL. Five consecutive entries (2BE, 2E6, 2E8, 2E9, 2ED)
 * named the remaining step in the external-display chain as "the pixel clock,
 * `DMUB_CMD__VBIOS` sub-type 2". 2EE read the caller and that was wrong: on
 * DCN 3.1 the DP clock source is `CLOCK_SOURCE_ID_DP_DTO`, whose
 * `program_pix_clk` is `dcn31_program_pix_clk` (dce_clock_source.c), and its
 * DP branch carries Linux's own comment -
 *
 *     // For these signal types Driver to program DP_DTO without calling
 *     // VBIOS Command table
 *
 * - before writing PHASE, MODULO and PIXEL_RATE_CNTL in registers and
 * returning. `set_pixel_clock` is reached only in the `else`, which is
 * HDMI/DVI. Our port is DP alt mode three independent ways (2BB's native-AUX
 * DPCD with no USB4 router, 2E7's `DP_CONFIG`, 2EB's granted alt mode), so
 * the pixel clock is OURS to write - rule 46, and 2zl's shape in a new place:
 * the command was real, its payload derivable and its DMUB route intact, and
 * this chip's `dc` would never call it for this signal.
 *
 * THE ARITHMETIC, WHICH IS THE WHOLE REASON THIS IS CHEAP TO CHECK.
 * `dcn31_program_pix_clk` writes
 *
 *     REG_WRITE(PHASE[inst],  pll_settings->actual_pix_clk_100hz * 100);
 *     REG_WRITE(MODULO[inst], dp_dto_ref_khz * 1000);
 *
 * so PHASE is the pixel clock IN HERTZ and MODULO is the DP reference clock
 * in hertz, and the DTO divides one by the other. That makes pipe 0's live
 * PHASE a number this project already knows two independent ways, and it
 * makes MODULO the `dprefclk` the SMU owns - a value this project has never
 * located and which is read here rather than assumed.
 *
 * ========================= THE SEGMENT IS THE HAZARD =====================
 * These four registers are **BASE_IDX 1**, and every register this kext has
 * ever touched is BASE_IDX 2 at the single DMU segment 0x34c0. Worse, the
 * offsets COLLIDE: at BASE_IDX 2, 0x0080..0x0083 are
 * regDOMAIN0_PG_CONFIG / regDOMAIN0_PG_STATUS / regDOMAIN1_PG_CONFIG /
 * regDOMAIN1_PG_STATUS - the display power-gating block rb_pg.h reads. A read
 * at the wrong base therefore answers 0x10000000-shaped words that are
 * perfectly plausible as a PIXEL_RATE_CNTL, and a WRITE at the wrong base
 * would hit the power gating of the pipe scanning the panel. That is rule 47
 * asked of a write: establish the live word decodes as the register the
 * conclusion assumes.
 *
 * WHERE 0xc0 COMES FROM, which is not a guess and not a new hardcode.
 * `tests/data/rembrandt-680m-tufa15.bin` is THIS MACHINE's own IP discovery
 * table, captured and committed, and the DMU entry in it reads
 *
 *     DMU  271  0  3.1.3.0   0x12 0xc0 0x34c0 0x9000 0x2403c00
 *
 * so base[1] is 0xc0 and **base[2] is 0x34c0 - the segment this kext already
 * uses and has confirmed on hardware for twenty-odd milestones.** That is the
 * built-in control: the same five-entry array that yields the new number
 * yields the known one in the next slot. The vendored `dcn_3_1_2_offset.h`
 * agrees independently, since Linux's own segment table for this IP carries
 * the same five values. The hosttest pins both against the captured binary,
 * which is the only reason this is allowed to be a constant at all - the same
 * licence `rb_gpuname.h`'s die geometry has (2CA).
 *
 * 2EE planned to plumb base[1] across the collection boundary over 2zd's
 * `callPlatformFunction` channel. That is not needed and would have been
 * WORSE: the plugin runs no discovery either (`g_disc` is a hand-built two-IP
 * stub for MP1), so the handover would have carried a constant from one file
 * to another and called the trip evidence.
 *
 * AND THE BASE IS STILL CHECKED ON THE CHIP, because a table on disk is not
 * an address that reads right (rule 41's shape). Two checks, in one sample:
 *   1. THE ARITHMETIC. Pipe 0 is the panel - OTG0_CONTROL 0x00011201 with
 *      OTG1-3 at 0x300 (2oq) - and eDP is a DP signal, so `dc`'s own branch
 *      says its clock comes from this DTO. PHASE[0] must then reproduce the
 *      panel's pixel clock, which the caller derives IN THE SAME BOOT from
 *      the live raster (H_TOTAL x V_TOTAL) times the measured refresh. No
 *      constant anywhere in the comparison. A wrong base cannot pass it.
 *   2. THE ALIAS CONTROL. The same four offsets are read at BASE_IDX 2 as
 *      well. If the two bases return bit-identical words, the segment
 *      arithmetic did nothing and the "DTO" being read is the DOMAIN block.
 *      A check that can only pass is not a check (rule 48), so this one is
 *      reported separately from the arithmetic and can fail on its own.
 *
 * THE TWO QUESTIONS ARE ANSWERED SEPARATELY, because they fail for different
 * reasons and one of them failing must not cost the other (rule 14). "Is the
 * base right?" and "is pipe 0 actually driven by this DTO?" are not the same
 * question: if the firmware brought the panel up on a PHYPLL instead, PHASE[0]
 * can be zero at a base that is perfectly correct. That outcome gets its own
 * code rather than being read as a bad address - and it is written down HERE,
 * before the boot, so that whichever of the three outcomes the hardware
 * returns, it was predicted (2EC's practice).
 *
 * NOTHING IN THIS HEADER WRITES. The three writes for the external display's
 * own pipe are a later milestone and must not be built until this read has
 * come back from hardware.
 * ===================================================================== */

/* BASE_IDX 1 of the DMU IP, and BASE_IDX 2 beside it as the control. Both
 * from `tests/data/rembrandt-680m-tufa15.bin`, pinned by the hosttest. */
#define RB_DPDTO_SEG1            0x00c0u
#define RB_DPDTO_SEG2_CONTROL    0x34c0u

/* The four pipes' register groups. From `dcn_3_1_2_offset.h`:
 *   regOTG0_PIXEL_RATE_CNTL  0x0080  BASE_IDX 1
 *   regDP_DTO0_PHASE         0x0081  BASE_IDX 1
 *   regDP_DTO0_MODULO        0x0082  BASE_IDX 1
 *   regOTG0_PHYPLL_PIXEL_RATE_CNTL 0x0083 BASE_IDX 1
 * and the group repeats every 4 dwords: OTG1 at 0x0084, OTG2 0x0088,
 * OTG3 0x008c, each verified in the header rather than extrapolated - the
 * DOMAIN block in rb_pg.h is the standing reminder that a stride which holds
 * for four entries can stop holding at the fifth. There is no fifth here:
 * DCN 3.1.2 has four pipes and the table ends at OTG3. */
#define RB_DPDTO_COUNT           4u
#define RB_DPDTO_STRIDE          4u
#define RB_DPDTO_DW_BASE         0x0080u

#define RB_DPDTO_REG_RATE_CNTL   0u
#define RB_DPDTO_REG_PHASE       1u
#define RB_DPDTO_REG_MODULO      2u
#define RB_DPDTO_REG_PHYPLL      3u
#define RB_DPDTO_REG_COUNT       4u

#define RB_DPDTO_DW_INVALID      0xffffffffu

/* The offset of one register of one pipe, WITHOUT a segment base. The caller
 * adds the base, because the caller is the one that owns an aperture. */
static inline uint32_t rb_dpdto_dw(uint32_t inst, uint32_t reg)
{
    if (inst >= RB_DPDTO_COUNT || reg >= RB_DPDTO_REG_COUNT) {
        return RB_DPDTO_DW_INVALID;
    }
    return RB_DPDTO_DW_BASE + inst * RB_DPDTO_STRIDE + reg;
}

/* The highest dword this header can name, for one aperture-bound comparison
 * in the caller (2B3's pattern). Relative to a segment base, as above. */
static inline uint32_t rb_dpdto_dw_max(void)
{
    return RB_DPDTO_DW_BASE +
           (RB_DPDTO_COUNT - 1u) * RB_DPDTO_STRIDE +
           (RB_DPDTO_REG_COUNT - 1u);
}

/* OTG*_PIXEL_RATE_CNTL fields, from `dcn_3_1_2_sh_mask.h`. Only the ones a
 * reading of this block needs are named; a field nothing reads would be a
 * fourth place for the same number to live. */
#define RB_DPDTO_RATE_SOURCE_MASK      0x00000003u
#define RB_DPDTO_DTBCLK_ENABLE         0x00000008u
#define RB_DPDTO_DP_DTO_ENABLE         0x00000010u
#define RB_DPDTO_DTBCLK_STATUS         0x00000040u
#define RB_DPDTO_DP_DTO_STATUS         0x00000080u
#define RB_DPDTO_SRC_SEL               0x00001000u

static inline uint32_t rb_dpdto_rate_source(uint32_t rate_cntl)
{
    return rate_cntl & RB_DPDTO_RATE_SOURCE_MASK;
}

static inline int rb_dpdto_enabled(uint32_t rate_cntl)
{
    return (rate_cntl & RB_DPDTO_DP_DTO_ENABLE) ? 1 : 0;
}

/* `DPDTO0_ENABLE_STATUS` is the hardware's own answer rather than the bit the
 * driver asked for, which is what makes it worth reading beside the enable:
 * the two disagreeing is a reading, and a reading nobody can get from the
 * enable alone (rule 20's shape - publish what happened beside what was
 * asked for). */
static inline int rb_dpdto_enable_status(uint32_t rate_cntl)
{
    return (rate_cntl & RB_DPDTO_DP_DTO_STATUS) ? 1 : 0;
}

static inline int rb_dpdto_src_sel(uint32_t rate_cntl)
{
    return (rate_cntl & RB_DPDTO_SRC_SEL) ? 1 : 0;
}

/* ---------------------------------------------------------------- base check
 *
 * Verdict codes. Negative is "not sampled" so that a boot which never ran
 * this cannot be read as one that ran it and found nothing (rule 14). */
#define RB_DPDTO_BASE_NOT_SAMPLED   (-1)
#define RB_DPDTO_BASE_CONFIRMED       0  /* PHASE[0] reproduces the raster   */
#define RB_DPDTO_BASE_PHASE_ZERO      1  /* PHASE[0] is 0 - says nothing     */
#define RB_DPDTO_BASE_PHASE_WRONG     2  /* PHASE[0] is some OTHER number    */
#define RB_DPDTO_BASE_MODULO_BAD      3  /* MODULO[0] is not a ref clock     */
#define RB_DPDTO_BASE_ALIASED         4  /* seg1 == seg2 word for word       */
#define RB_DPDTO_BASE_NO_EXPECTED     5  /* the caller could not derive one  */

/* The DP reference clock's plausible range. Deliberately WIDE: the point of
 * reading MODULO is that this project does not know `dprefclk` on this chip,
 * so the bound only has to exclude words that cannot be a reference clock at
 * all (a zero, a status register, a power-gating word). Narrowing it to a
 * value recalled from another ASIC would be rule 17 in a bound. */
#define RB_DPDTO_REF_HZ_MIN      100000000u    /* 100 MHz */
#define RB_DPDTO_REF_HZ_MAX     2000000000u    /*   2 GHz */

/* How close PHASE[0] has to be to the raster-derived pixel clock. The two
 * independent derivations this project already has - ~341 987 984 Hz measured
 * and 342 011 761 Hz from boot 255's own raster - differ by 24 kHz, which is
 * 0.007 %, so the measurement's own spread is three orders of magnitude
 * inside this window. It is 0.5 % rather than 0.01 % because the quantity
 * being excluded is a DIFFERENT REGISTER, not a slightly different clock: the
 * nearest plausible impostor at the colliding base is a power-gating word
 * (0x10000000 = 268 MHz, 21 % away). A tolerance tight enough to reject a
 * rounding difference would buy nothing and could refuse a correct base. */
#define RB_DPDTO_PHASE_TOL_PPT   5u            /* 5 parts per thousand */

static inline uint32_t rb_dpdto_abs_diff(uint32_t a, uint32_t b)
{
    return a > b ? a - b : b - a;
}

/* Does PHASE[0] reproduce `expected_hz`?
 *
 * `expected_hz` is derived by the caller IN THE SAME BOOT from the live
 * raster and the measured refresh, and 0 means it could not be - which is its
 * own code, because "the instrument had no reference" and "the address is
 * wrong" are not the same answer. */
static inline int rb_dpdto_phase_matches(uint32_t phase, uint32_t expected_hz)
{
    if (!phase || !expected_hz) { return 0; }
    {
        const uint64_t tol =
            ((uint64_t)expected_hz * RB_DPDTO_PHASE_TOL_PPT) / 1000ull;
        /* A tolerance that rounds to zero would turn this into an equality
         * test by accident, so it has a floor. */
        const uint64_t window = tol ? tol : 1ull;
        return (uint64_t)rb_dpdto_abs_diff(phase, expected_hz) <= window;
    }
}

/* The whole base verdict, in one place so the hosttest owns it.
 *
 * `seg1` is the four-register group read at BASE_IDX 1 for pipe 0, `seg2` the
 * same four offsets read at BASE_IDX 2 - the alias control. Both are read
 * unconditionally by the caller and published whichever way this falls,
 * because a refusal must publish what it refused on. */
static inline int rb_dpdto_base_verdict(const uint32_t *seg1,
                                        const uint32_t *seg2,
                                        uint32_t expected_hz)
{
    uint32_t i;
    int all_same;

    if (!seg1 || !seg2) { return RB_DPDTO_BASE_NOT_SAMPLED; }

    /* The alias control comes FIRST, because it is the one failure that makes
     * every other field meaningless: if the two bases read the same words,
     * nothing below is about the DTO. */
    all_same = 1;
    for (i = 0; i < RB_DPDTO_REG_COUNT; i++) {
        if (seg1[i] != seg2[i]) { all_same = 0; break; }
    }
    if (all_same) { return RB_DPDTO_BASE_ALIASED; }

    if (!expected_hz) { return RB_DPDTO_BASE_NO_EXPECTED; }

    if (!seg1[RB_DPDTO_REG_PHASE]) { return RB_DPDTO_BASE_PHASE_ZERO; }

    if (!rb_dpdto_phase_matches(seg1[RB_DPDTO_REG_PHASE], expected_hz)) {
        return RB_DPDTO_BASE_PHASE_WRONG;
    }

    /* MODULO is checked LAST and on its own code. PHASE already matching the
     * raster to 0.5 % is strong evidence the address is right, so a MODULO
     * outside the bound at that point is a finding about the reference clock
     * rather than about the segment - and conflating the two would hide it. */
    {
        const uint32_t modulo = seg1[RB_DPDTO_REG_MODULO];
        if (modulo < RB_DPDTO_REF_HZ_MIN || modulo > RB_DPDTO_REF_HZ_MAX) {
            return RB_DPDTO_BASE_MODULO_BAD;
        }
    }

    return RB_DPDTO_BASE_CONFIRMED;
}

/* ------------------------------------------------------- is the DTO driving?
 *
 * The second, independent question. Asked only of pipe 0 here, because pipe 0
 * is the one whose answer is KNOWN from outside this block - the panel is lit
 * - and an unknown pipe's bits would be a row in a table built from n=1
 * (rule 18). */
#define RB_DPDTO_PIPE_NOT_SAMPLED  (-1)
#define RB_DPDTO_PIPE_DTO            0  /* enable set and hardware agrees    */
#define RB_DPDTO_PIPE_DTO_ASKED      1  /* enable set, status bit disagrees  */
#define RB_DPDTO_PIPE_OTHER_SOURCE   2  /* not on the DP DTO at all          */

static inline int rb_dpdto_pipe_verdict(uint32_t rate_cntl)
{
    if (!rb_dpdto_enabled(rate_cntl)) { return RB_DPDTO_PIPE_OTHER_SOURCE; }
    if (!rb_dpdto_enable_status(rate_cntl)) { return RB_DPDTO_PIPE_DTO_ASKED; }
    return RB_DPDTO_PIPE_DTO;
}

/* The reference clock, named as what it is once the base is confirmed. Zero
 * when there is nothing to report, so a caller cannot publish a MODULO it was
 * never entitled to read as a `dprefclk`. */
static inline uint32_t rb_dpdto_ref_hz(const uint32_t *seg1, int base_verdict)
{
    if (!seg1 || base_verdict != RB_DPDTO_BASE_CONFIRMED) { return 0; }
    return seg1[RB_DPDTO_REG_MODULO];
}

/* ---------------------------------------- the raster the comparison needs
 *
 * OTG0's own H_TOTAL and V_TOTAL, so the sampler does not depend on another
 * setup path having run. Dword offsets at DMU segment 2, from `rb_regs.h`'s
 * own table (`OTG0_H_TOTAL` 0x1b2a, `OTG0_V_TOTAL` 0x1b2f) - and the hosttest
 * asserts these two against that table where both headers are visible, which
 * is the only reason a fourth copy of them is tolerable (2C4's pattern, the
 * licence `rb_gpuname.h`'s RCC_CONFIG_MEMSIZE alias has).
 *
 * THERE ARE THREE OTHER COPIES of these two numbers in src/fbkext - the
 * backlight, DRR and modes files each carry their own - and none of them is
 * checked against anything. That is pre-existing debt, recorded here rather
 * than quietly added to: this copy is the only one with a pin. */
#define RB_DPDTO_DW_OTG0_H_TOTAL   0x1b2au
#define RB_DPDTO_DW_OTG0_V_TOTAL   0x1b2fu
#define RB_DPDTO_TOTAL_MASK        0x7fffu    /* == RB_DRR_TOTAL_MASK */

/* Where the refresh used for the comparison came from. Published, because a
 * reading taken against a measured rate and one taken against this panel's
 * registry constant are not the same reading and must not look alike
 * (rule 17 - never substitute a value and then narrate it as a result). */
#define RB_DPDTO_REF_SRC_NONE      0u  /* nothing to compare against        */
#define RB_DPDTO_REF_SRC_MEASURED  1u  /* modeClkHz: measured on a          */
                                       /* FRAME_COUNT edge at mode setup    */
#define RB_DPDTO_REF_SRC_RASTER    2u  /* live raster x the rate IOKit is   */
                                       /* actually running the clock at     */

/* A vblank period in nanoseconds as a refresh in millihertz. The framebuffer
 * holds the period (`vblPeriodNs`), not the rate, so the conversion is here
 * where the hosttest can pin it rather than inline at the call site. */
static inline uint32_t rb_dpdto_mhz_from_period_ns(uint64_t period_ns)
{
    if (!period_ns) { return 0; }
    /* 1e12 ns-per-second-in-millihertz / period. A period of 6 952 653 ns -
     * this panel's 143.83 Hz - gives 143 830. */
    return (uint32_t)(1000000000000ull / period_ns);
}

/* AND THERE IS NO `rb_dpdto_expected_hz()` HERE, WHICH IS ALSO THE POINT.
 *
 * The first draft of this header grew one: h_total * v_total * refresh_mhz /
 * 1000, with its own zero and overflow guards. `rb_drr.h` has formed exactly
 * that product since 2y6 - `rb_drr_clk_from_mhz()`, which the framebuffer
 * already calls at mode setup and whose answer it already keeps as
 * `modeClkHz`, the number handed to IOKit as the mode's `pixelClock`. A
 * second copy would have been a second place for the panel's pixel clock to
 * be computed, and the two would have differed on day one: rb_drr.h rounds
 * (+500 before the divide) and the copy truncated.
 *
 * So the caller passes `modeClkHz` in as `expected_hz` and this header only
 * COMPARES. The hosttest pins rb_drr_clk_from_mhz against the panel's raster
 * by hand so the number the comparison rests on is checked where it is
 * defined, once.
 *
 * (Two break-it runs went into the deleted version's guards before it was
 * deleted, and what they found is recorded in 2EF: both of its zero checks
 * fired nothing, because a zero factor makes the product zero and returning
 * a zero product is returning zero. The finding outlived the function - the
 * same inert pair is in `rb_drr_clk_from_mhz`'s own early return, where it
 * is load-bearing for a DIFFERENT reason: that function's callers
 * distinguish its 0 from a refusal code.) */

#endif /* RB_DPDTO_H */
