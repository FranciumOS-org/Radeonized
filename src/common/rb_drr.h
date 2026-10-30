/* rb_drr.h - the decisions behind a refresh-rate change (2y5).
 *
 * The panel runs at clk/(H_TOTAL * V_TOTAL) and boot 253 measured the model
 * exact: 341995117/(2106*1129) = 143.836 Hz against a measured 143.836. The
 * active window is set by OTG_V_BLANK_START_END (start 1100, end 20, so 1080
 * lines) INDEPENDENTLY of V_TOTAL, so lengthening V_TOTAL stretches the
 * vertical blank and leaves the active raster, the sync widths, the pixel
 * clock and the DP link untouched. That is the whole mechanism.
 *
 * What is NOT the mechanism: writing OTG_V_TOTAL. That is what Linux does
 * during a modeset, with the pipe locked, and the pipe here is live with the
 * desktop on it. V_TOTAL_MIN/MAX with the MIN_SEL/MAX_SEL bits of
 * V_TOTAL_CONTROL are the variable-refresh path, designed to be written while
 * the raster scans; pinning MIN == MAX fixes the frame length.
 *
 * Boot 254 read the group as MIN 0, MAX 0, MID 0, CONTROL 0 - the firmware has
 * DRR switched off entirely, so there is nothing of its to preserve and the
 * restore is four zeros rather than something inferred. `ctl != 0` is
 * therefore a REFUSAL and not a case to handle: if a future boot (or another
 * machine) finds the firmware using DRR, the restore values are no longer
 * known and fighting it is the wrong move. Rule 37's question asked of a
 * register instead of an NVRAM key.
 *
 * Freestanding and pure: the hosttest decides everything the kext decides.
 */
#ifndef RB_DRR_H
#define RB_DRR_H

#include <stdint.h>

/* dcn_3_1_2_sh_mask.h:29356, 29359, 29362 - all three totals are 15 bits. */
#define RB_DRR_TOTAL_MASK    0x7fffu
/* dcn_3_1_2_sh_mask.h:29375, 29376 */
#define RB_DRR_CTL_MIN_SEL   0x1u
#define RB_DRR_CTL_MAX_SEL   0x2u

/* Why a refusal, so a boot can say which one rather than just "no". */
enum {
    RB_DRR_OK         = 0,
    RB_DRR_NO_RASTER  = 1,  /* H_TOTAL or V_TOTAL zero: not a live raster */
    RB_DRR_FW_DRR     = 2,  /* V_TOTAL_CONTROL non-zero: firmware owns DRR */
    RB_DRR_NOT_LONGER = 3,  /* asked for a frame no longer than the current */
    RB_DRR_TOO_LONG   = 4,  /* past the 15-bit field */
    RB_DRR_NO_CLK     = 5   /* no pixel clock to compute a refresh from */
};

/* Only ever LENGTHEN the frame. Shortening it would raise the refresh above
 * the rate the panel is running at, which is a different experiment with a
 * different failure mode, and nothing here has established what this panel
 * tolerates above 143.836 Hz. */
static inline int rb_drr_check(uint32_t htotal, uint32_t vtotal,
                               uint32_t ctl, uint32_t lines)
{
    if (htotal == 0u || vtotal == 0u) { return RB_DRR_NO_RASTER; }
    if (ctl != 0u)                    { return RB_DRR_FW_DRR; }
    if (lines <= vtotal)              { return RB_DRR_NOT_LONGER; }
    if (lines > RB_DRR_TOTAL_MASK)    { return RB_DRR_TOO_LONG; }
    return RB_DRR_OK;
}

/* Millihertz, to match rbgpuvblhz and `vbl_mhz`/`agdc_otg_mhz`. */
static inline uint32_t rb_drr_mhz(uint32_t clk_hz, uint32_t htotal, uint32_t lines)
{
    uint64_t den;
    if (clk_hz == 0u || htotal == 0u || lines == 0u) { return 0u; }
    den = (uint64_t)htotal * (uint64_t)lines;
    return (uint32_t)(((uint64_t)clk_hz * 1000u + den / 2u) / den);
}

/* The inverse, rounded to the nearest whole line - what 60 Hz costs in lines.
 * Returns 0 rather than a guess when it cannot be computed. */
static inline uint32_t rb_drr_lines_for_mhz(uint32_t clk_hz, uint32_t htotal,
                                            uint32_t mhz)
{
    uint64_t den, lines;
    if (clk_hz == 0u || htotal == 0u || mhz == 0u) { return 0u; }
    den = (uint64_t)htotal * (uint64_t)mhz;
    lines = ((uint64_t)clk_hz * 1000u + den / 2u) / den;
    if (lines == 0u || lines > RB_DRR_TOTAL_MASK) { return 0u; }
    return (uint32_t)lines;
}

/* The two SEL bits set, every other bit of V_TOTAL_CONTROL left as it was -
 * one field changed and nothing else, the same read-modify-write discipline
 * the backlight path uses. */
static inline uint32_t rb_drr_ctl_on(uint32_t ctl0)
{
    return ctl0 | RB_DRR_CTL_MIN_SEL | RB_DRR_CTL_MAX_SEL;
}

static inline uint32_t rb_drr_ctl_off(uint32_t ctl0)
{
    return ctl0 & ~(uint32_t)(RB_DRR_CTL_MIN_SEL | RB_DRR_CTL_MAX_SEL);
}

/* ---- 2y6: from a one-shot experiment to a display mode ------------------
 *
 * Boot 255 measured the write WORKING: 2707 lines gave 60025 mHz against a
 * predicted 59989, with the revert putting 143821 back. So the remaining work
 * is not "does the panel accept it" but "how does macOS get told", and that
 * needs three numbers per mode rather than one: the line count to write, the
 * 16.16 refreshRate IOFramebuffer advertises, and the nanosecond period our
 * VBL thread_call ticks at. All three have to come from the same place or the
 * compositor is paced against a rate the raster is not running at - which is
 * exactly what boot 255 did for eight seconds, deliberately.
 *
 * THE PIXEL CLOCK COSTS NOTHING TO GET. It is not in the OTG registers (it
 * comes from the DP PLL) and this project has never read it. It does not need
 * to: clk = mHz * H_TOTAL * V_TOTAL, and boot 255 measured all three on the
 * live raster. 143843 mHz * 2106 * 1129 / 1000 = 342011761 Hz, which
 * round-trips back to 1129 lines through rb_drr_lines_for_mhz and gives 2707
 * for 60 Hz - the same answer the hosttest already pinned against the
 * hand-computed value. A derived clock that reproduces its own input is worth
 * more than a register nobody has located, and rule 18's objection does not
 * apply because this is one measurement used self-consistently, not a table
 * built from an n=1 row.
 */

/* clk = mHz * H_TOTAL * V_TOTAL / 1000, from a refresh measured on the live
 * raster. Returns 0 rather than a guess on a missing input. */
static inline uint32_t rb_drr_clk_from_mhz(uint32_t mhz, uint32_t htotal,
                                           uint32_t vtotal)
{
    uint64_t hz;
    if (mhz == 0u || htotal == 0u || vtotal == 0u) { return 0u; }
    hz = ((uint64_t)mhz * (uint64_t)htotal * (uint64_t)vtotal + 500u) / 1000u;
    if (hz > 0xffffffffull) { return 0u; }
    return (uint32_t)hz;
}

/* IODisplayModeInformation::refreshRate is 16.16 fixed point. */
static inline uint32_t rb_drr_rate1616(uint32_t mhz)
{
    if (mhz == 0u) { return 0u; }
    return (uint32_t)(((uint64_t)mhz * 65536u + 500u) / 1000u);
}

/* The VBL period, derived from the SAME 16.16 value IOFramebuffer was handed,
 * not from the mHz independently. Two expressions of one rate drift; one
 * expression cannot. For the 143.83 Hz mode this reproduces the constant the
 * kext has ticked at since 2xe bit for bit, which is the continuity check. */
static inline uint64_t rb_drr_period_ns(uint32_t rate1616)
{
    if (rate1616 == 0u) { return 0u; }
    return 1000000000ull * 65536ull / (uint64_t)rate1616;
}

#endif /* RB_DRR_H */
