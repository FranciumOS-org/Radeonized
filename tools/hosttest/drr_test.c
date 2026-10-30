/* The refresh-rate change (2y5). This is a WRITE to a live raster, so every
 * decision behind it is decided by rb_drr.h and every one of them is run here
 * first - the same contract rb_hubp.h and rb_backlight.h have, for the same
 * reason.
 *
 * The anchor is the live raster of boot 253/254: clk 341995117, H_TOTAL 2106,
 * V_TOTAL 1129, and a refresh MEASURED on an OTG0_FRAME_COUNT edge as
 * 143836 millihertz. If rb_drr_mhz() does not reproduce that from the
 * registers alone, nothing it computes for 60 Hz is worth writing to a chip. */
#include <stdio.h>
#include <string.h>

#include "rb_drr.h"

#define CHECK(cond, what) do { \
    printf((cond) ? "ok   %s\n" : "FAIL %s\n", (what)); \
    failures += !(cond); \
} while (0)

/* Live, boot 254, read by the framebuffer off its own BAR5 map. */
#define DRR_CLK       341995117u
#define DRR_HTOTAL    2106u
#define DRR_VTOTAL    1129u
#define DRR_MEAS_MHZ  143836u
#define DRR_CTL_FW    0u          /* firmware has DRR off: MIN/MAX/MID/CTL all 0 */

int drr_tests(void);

int drr_tests(void)
{
    int failures = 0;

    printf("\n-- refresh-rate / DRR (2y5) --\n");

    /* --- the model, against the measurement -------------------------- */

    CHECK(rb_drr_mhz(DRR_CLK, DRR_HTOTAL, DRR_VTOTAL) == DRR_MEAS_MHZ,
          "the model reproduces the MEASURED 143836 mHz from the registers alone");
    /* The inverse has to come back to the same place, or one of the two is
     * wrong and both are used: lines -> mHz to report, mHz -> lines to write. */
    CHECK(rb_drr_lines_for_mhz(DRR_CLK, DRR_HTOTAL, DRR_MEAS_MHZ) == DRR_VTOTAL,
          "and inverts back to V_TOTAL 1129");
    CHECK(rb_drr_lines_for_mhz(DRR_CLK, DRR_HTOTAL, 60000u) == 2707u,
          "60 Hz is 2707 lines");
    CHECK(rb_drr_mhz(DRR_CLK, DRR_HTOTAL, 2707u) == 59989u,
          "...which lands at 59.989 Hz, and the shortfall is stated not hidden");
    CHECK(rb_drr_lines_for_mhz(DRR_CLK, DRR_HTOTAL, 120000u) == 1353u &&
          rb_drr_lines_for_mhz(DRR_CLK, DRR_HTOTAL, 90000u) == 1804u,
          "120 Hz is 1353 lines and 90 Hz is 1804");

    /* Nothing is invented when an input is missing (rule 17). */
    CHECK(rb_drr_mhz(0u, DRR_HTOTAL, DRR_VTOTAL) == 0u &&
          rb_drr_mhz(DRR_CLK, 0u, DRR_VTOTAL) == 0u &&
          rb_drr_mhz(DRR_CLK, DRR_HTOTAL, 0u) == 0u,
          "a missing clock, H_TOTAL or line count computes 0, not a guess");
    CHECK(rb_drr_lines_for_mhz(DRR_CLK, DRR_HTOTAL, 1u) == 0u,
          "a refresh needing more lines than the field holds returns 0");

    /* --- the gate ----------------------------------------------------- */

    CHECK(rb_drr_check(DRR_HTOTAL, DRR_VTOTAL, DRR_CTL_FW, 2707u) == RB_DRR_OK,
          "boot 254's raster accepts the 60 Hz request");
    CHECK(rb_drr_check(0u, DRR_VTOTAL, 0u, 2707u) == RB_DRR_NO_RASTER &&
          rb_drr_check(DRR_HTOTAL, 0u, 0u, 2707u) == RB_DRR_NO_RASTER,
          "a zero H_TOTAL or V_TOTAL is not a live raster and is refused");
    /* The one that matters most: if the firmware is using DRR, the restore
     * values are no longer four zeros and this must not proceed. */
    CHECK(rb_drr_check(DRR_HTOTAL, DRR_VTOTAL, RB_DRR_CTL_MIN_SEL, 2707u)
              == RB_DRR_FW_DRR &&
          rb_drr_check(DRR_HTOTAL, DRR_VTOTAL, 0x10000u, 2707u) == RB_DRR_FW_DRR,
          "any non-zero V_TOTAL_CONTROL refuses - the restore state is unknown");
    CHECK(rb_drr_check(DRR_HTOTAL, DRR_VTOTAL, 0u, DRR_VTOTAL) == RB_DRR_NOT_LONGER &&
          rb_drr_check(DRR_HTOTAL, DRR_VTOTAL, 0u, 1000u) == RB_DRR_NOT_LONGER,
          "only LENGTHENING the frame is allowed - raising the refresh is untested");
    CHECK(rb_drr_check(DRR_HTOTAL, DRR_VTOTAL, 0u, RB_DRR_TOTAL_MASK) == RB_DRR_OK &&
          rb_drr_check(DRR_HTOTAL, DRR_VTOTAL, 0u, RB_DRR_TOTAL_MASK + 1u)
              == RB_DRR_TOO_LONG,
          "the 15-bit field is the ceiling, and the boundary is exact");

    /* --- the control register ----------------------------------------- */

    CHECK(rb_drr_ctl_on(0u) == (RB_DRR_CTL_MIN_SEL | RB_DRR_CTL_MAX_SEL),
          "arming sets both SEL bits");
    /* Read-modify-write: one field changed and nothing else, so a bit the
     * firmware set for its own reasons survives us. */
    CHECK(rb_drr_ctl_on(0x00010010u) == 0x00010013u,
          "and leaves every other bit of V_TOTAL_CONTROL alone");
    CHECK(rb_drr_ctl_off(rb_drr_ctl_on(0x00010010u)) == 0x00010010u,
          "disarming returns the register exactly to where it started");
    CHECK(rb_drr_ctl_off(0u) == 0u,
          "disarming an already-clear register is a no-op");

    return failures;
}
