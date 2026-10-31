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

/* Boot 255, the boot where the write worked (2y5 DONE). These are the anchor
 * for the mode table: a measured before, a measured 60 Hz, and a measured
 * after, all from one run. */
#define B255_BEFORE      143843u
#define B255_DURING       60025u
#define B255_AFTER       143821u
#define B255_CLK      342011761u   /* 143843 * 2106 * 1129 / 1000 */
#define B255_MODE60_MHZ   59992u   /* what 2707 lines actually gives */

/* The 16.16 refreshRate in `.Display_boot`'s own live IOFBConfig, which is
 * what the kext has advertised and ticked at since 2xe. Not re-derived: it is
 * the value already in the registry, and the mode table must not change it. */
#define DRR_RATE1616_LIVE 0x008fd53fu

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

    /* --- 2y6: the mode table -------------------------------------------
     *
     * Anchored on boot 255, which is a different and stronger anchor than the
     * rest of this file: these are the numbers from the boot where the write
     * WORKED. Measured 143843 mHz before, 60025 during at 2707 lines, 143821
     * after. The before/after pair bounds this instrument's SHORT-TERM noise
     * at 22 mHz, and that is the tolerance the comparison below is allowed.
     *
     * NOT run-to-run noise, and boot 256 is the correction: it measured the
     * same unchanged raster at 143765, which is 78 mHz - 3.5x that band - off
     * boot 255. Two measurements 8 s apart bound jitter within one run and say
     * nothing about variation across boots; 0.054 % is the shape of
     * DisplayPort spread-spectrum clocking, which modulates the link clock by
     * design. So every constant below is correct FOR BOOT 255'S RASTER and is
     * not a property of the panel, which is exactly why the kext derives the
     * line count at runtime from its own measurement instead of using 2707:
     * boot 256 computed 2705 and landed at 60.004 Hz, where a hardcoded 2707
     * would have given 59.96. The derivation is self-correcting; the number
     * is not. Rule 41, asked of a measurement rather than an address.
     */
    CHECK(rb_drr_clk_from_mhz(B255_BEFORE, DRR_HTOTAL, DRR_VTOTAL) == B255_CLK,
          "the pixel clock comes from the measurement, not from a register");
    CHECK(rb_drr_clk_from_mhz(0u, DRR_HTOTAL, DRR_VTOTAL) == 0u &&
          rb_drr_clk_from_mhz(B255_BEFORE, 0u, DRR_VTOTAL) == 0u &&
          rb_drr_clk_from_mhz(B255_BEFORE, DRR_HTOTAL, 0u) == 0u,
          "and is 0 rather than a guess when an input is missing");

    /* The self-consistency check, and the only reason a derived clock is
     * trustworthy at all: feed it back and it must return the raster it came
     * from. A clock derived wrongly would still produce a plausible 60 Hz line
     * count, so the 60 Hz answer alone proves nothing. */
    CHECK(rb_drr_lines_for_mhz(B255_CLK, DRR_HTOTAL, B255_BEFORE) == DRR_VTOTAL,
          "the derived clock round-trips to the V_TOTAL it was derived from");
    CHECK(rb_drr_lines_for_mhz(B255_CLK, DRR_HTOTAL, 60000u) == 2707u,
          "and independently reproduces 2707 lines for 60 Hz");

    /* What the chip will actually do at 2707 lines, which is what gets
     * advertised - not the 60000 that was asked for. The difference is 8 mHz
     * and publishing the request instead of the achievable value is how a
     * mode list comes to lie. */
    CHECK(rb_drr_mhz(B255_CLK, DRR_HTOTAL, 2707u) == B255_MODE60_MHZ,
          "the advertised rate is what 2707 lines gives, not what was asked");
    {
        const uint32_t d = (B255_DURING > B255_MODE60_MHZ)
                               ? (B255_DURING - B255_MODE60_MHZ)
                               : (B255_MODE60_MHZ - B255_DURING);
        const uint32_t noise = (B255_BEFORE > B255_AFTER)
                                   ? (B255_BEFORE - B255_AFTER)
                                   : (B255_AFTER - B255_BEFORE);
        CHECK(noise == 22u, "the before/after pair bounds the noise at 22 mHz");
        CHECK(d <= 2u * noise,
              "and the computed 60 Hz rate matches boot 255's MEASURED 60025 "
              "inside that noise");
    }

    /* 16.16, the format IODisplayModeInformation::refreshRate is in. */
    CHECK(rb_drr_rate1616(B255_MODE60_MHZ) == 0x003bfdf4u,
          "60 Hz in 16.16 fixed point");
    CHECK(rb_drr_rate1616(0u) == 0u, "and 0 mHz is 0, not a division fault");
    /* An exact one, so a sign or shift error cannot hide behind rounding. */
    CHECK(rb_drr_rate1616(1000u) == 65536u, "1 Hz is exactly 1.0 in 16.16");

    /* THE CONTINUITY CHECK. The 143.83 Hz mode must keep ticking at the period
     * the kext has used since 2xe, because that constant is what three
     * milestones of frame-rate work was measured against. If this changes, the
     * mode table has silently retimed the compositor. */
    CHECK(rb_drr_period_ns(DRR_RATE1616_LIVE) == 6952507ull,
          "mode 100's VBL period is bit-identical to the live 2xe constant");
    CHECK(rb_drr_period_ns(rb_drr_rate1616(B255_MODE60_MHZ)) == 16668887ull,
          "and 60 Hz ticks at 16.669 ms");
    CHECK(rb_drr_period_ns(0u) == 0ull, "a zero rate has no period");
    /* The two periods must differ by the ratio of the two rates, within
     * rounding - the cheapest guard against one mode being handed the other's
     * clock, which is the bug this whole table exists to prevent. */
    CHECK(rb_drr_period_ns(rb_drr_rate1616(B255_MODE60_MHZ)) >
              2u * rb_drr_period_ns(DRR_RATE1616_LIVE),
          "60 Hz is more than twice the period of 143.83 Hz");

    /* --- 2z0: the mode list, built from the panel ----------------------- */
    {
        struct rb_drr_mode m[RB_DRR_MAX_MODES];
        unsigned n = rb_drr_build_modes(B255_CLK, DRR_HTOTAL, DRR_VTOTAL,
                                        DRR_RATE1616_LIVE, m,
                                        RB_DRR_MAX_MODES);
        CHECK(n == 8u, "this 143.8 Hz panel yields 8 modes (native + 7)");
        CHECK(m[0].native == 1u && m[0].lines == DRR_VTOTAL,
              "index 0 is NATIVE and carries the firmware's own V_TOTAL");
        /* Strictly descending: a mode list out of order is a mode list the
         * Displays panel renders in a confusing order, and a duplicate is a
         * mode that cannot be selected twice. */
        {
            int ordered = 1, grows = 1;
            unsigned i;
            for (i = 1; i < n; i++) {
                if (m[i].mhz >= m[i - 1].mhz)     { ordered = 0; }
                if (m[i].lines <= m[i - 1].lines) { grows = 0; }
                if (m[i].native != 0u)            { ordered = 0; }
            }
            CHECK(ordered, "every later mode is strictly slower, and non-native");
            CHECK(grows, "and strictly longer in lines - the two must agree");
        }
        /* Ladder order is 120/100/90/75/60/50/48, so index 4 is 75 Hz and 5
         * is 60 Hz. The line counts are those of BOOT 255'S clock: boot 256
         * measured 341826303 and got 2164/2705 for the same two modes. That
         * difference is the whole reason the list is derived per boot rather
         * than tabulated (2y6), so it is asserted here rather than smoothed
         * over. */
        CHECK(m[5].lines == 2707u && m[5].mhz == B255_MODE60_MHZ,
              "60 Hz is 2707 lines on boot 255's clock - 2y6's own answer");
        CHECK(m[4].lines == 2165u && m[4].mhz > 74900u && m[4].mhz < 75100u,
              "75 Hz is 2165 lines, the mode just under the ~76/s ceiling");
        CHECK(rb_drr_lines_for_mhz(341826303u, DRR_HTOTAL, 60000u) == 2705u &&
              rb_drr_lines_for_mhz(341826303u, DRR_HTOTAL, 75000u) == 2164u,
              "and boot 256's clock gives 2705/2164 for the same two modes");
        /* The native mode must advertise the REGISTRY's rate, not a derived
         * one - it moves between boots and every boot since 2sl was read
         * against the constant. */
        CHECK(m[0].rate1616 == DRR_RATE1616_LIVE &&
              m[0].period_ns == 6952507ull,
              "native advertises the registry rate and the 2xe period");
        {
            struct rb_drr_mode d[RB_DRR_MAX_MODES];
            (void)rb_drr_build_modes(B255_CLK, DRR_HTOTAL, DRR_VTOTAL, 0u, d,
                                     RB_DRR_MAX_MODES);
            CHECK(d[0].rate1616 == rb_drr_rate1616(B255_BEFORE) &&
                  d[0].rate1616 != DRR_RATE1616_LIVE,
                  "and passing 0 derives it instead, which is a DIFFERENT "
                  "number - the reason the caller supplies the constant");
        }

        /* A 75 Hz panel must NOT be offered 120 or 144. This is the whole
         * reason the list is derived: 2y6's hardcoded table would have
         * claimed 143.8 Hz on hardware that has never run it. */
        n = rb_drr_build_modes(166587000u, 2200u, 1010u, 0u, m,
                               RB_DRR_MAX_MODES);
        CHECK(n == 4u, "a 75 Hz panel yields exactly 4 modes");
        CHECK(m[0].native == 1u && m[0].mhz > 74000u && m[0].mhz < 76000u,
              "whose native mode is its own 75 Hz");
        {
            int none_faster = 1;
            unsigned i;
            for (i = 0; i < n; i++) {
                if (m[i].mhz > m[0].mhz) { none_faster = 0; }
            }
            CHECK(none_faster,
                  "and nothing faster than native is ever offered");
        }

        /* The bound is honoured even when the ladder could fill more. */
        n = rb_drr_build_modes(B255_CLK, DRR_HTOTAL, DRR_VTOTAL, 0u, m, 3u);
        CHECK(n == 3u, "max is respected rather than overrun");

        /* No raster, no modes - not one fabricated native entry. */
        CHECK(rb_drr_build_modes(0u, DRR_HTOTAL, DRR_VTOTAL, 0u, m,
                                 RB_DRR_MAX_MODES) == 0u &&
              rb_drr_build_modes(B255_CLK, 0u, DRR_VTOTAL, 0u, m,
                                 RB_DRR_MAX_MODES) == 0u &&
              rb_drr_build_modes(B255_CLK, DRR_HTOTAL, 0u, 0u, m,
                                 RB_DRR_MAX_MODES) == 0u &&
              rb_drr_build_modes(B255_CLK, DRR_HTOTAL, DRR_VTOTAL, 0u, m,
                                 0u) == 0u,
              "a missing input yields NO modes rather than a guessed one");

        /* A panel already at 48 Hz has nothing below it on the ladder, so the
         * list is native ALONE - which must still be a valid list. */
        n = rb_drr_build_modes(B255_CLK, DRR_HTOTAL, 3381u, 0u, m,
                               RB_DRR_MAX_MODES);
        CHECK(n == 1u && m[0].native == 1u,
              "a panel at the bottom of the ladder gets native only");
    }

    /* 2z1: mode -> mode. Boot 258 reached 60 Hz and then refused every other
     * mode with RB_DRR_FW_DRR, because the live V_TOTAL_CONTROL it checked
     * carried the SEL bits OUR OWN arming had just set. The door locked
     * behind us and 60 Hz was the last mode the boot could reach. */
    {
        const uint32_t armed = rb_drr_ctl_on(0u);   /* what we leave behind */

        CHECK(armed == 3u, "arming a zero CONTROL sets exactly MIN_SEL|MAX_SEL");
        CHECK(rb_drr_ctl_is_ours(armed, 0u) &&
              !rb_drr_ctl_is_ours(5u, 0u) &&
              !rb_drr_ctl_is_ours(0u, 0u),
              "ours is the armed value; neither a foreign CONTROL nor the "
              "unarmed snapshot is");

        /* The regression itself, stated as the old call and the new one on the
         * identical inputs. The old one is what shipped in 2z0. */
        CHECK(rb_drr_check(DRR_HTOTAL, DRR_VTOTAL, armed, 2707u) == RB_DRR_FW_DRR,
              "the 2z0 check refuses a mode change from a mode WE armed");
        CHECK(rb_drr_check_owned(DRR_HTOTAL, DRR_VTOTAL, armed, 0u, 2707u)
                  == RB_DRR_OK,
              "mode -> mode is allowed when the live CONTROL is our own");

        /* Native -> mode, the path that already worked, must not change. */
        CHECK(rb_drr_check_owned(DRR_HTOTAL, DRR_VTOTAL, 0u, 0u, 2707u)
                  == RB_DRR_OK,
              "native -> mode still passes");

        /* And the refusal this check EXISTS for survives: a CONTROL that is
         * neither the snapshot nor our arming of it belongs to someone else,
         * and the restore values really are unknown. */
        CHECK(rb_drr_check_owned(DRR_HTOTAL, DRR_VTOTAL, 5u, 0u, 2707u)
                  == RB_DRR_FW_DRR,
              "a third party owning CONTROL still refuses");
        CHECK(rb_drr_check_owned(DRR_HTOTAL, DRR_VTOTAL, 2u, 0u, 2707u)
                  == RB_DRR_FW_DRR,
              "a PARTIAL arming is not ours - MAX_SEL alone refuses");

        /* A boot whose firmware had DRR on refuses whatever the live value is,
         * because ctl0 itself is non-zero: 2y5 read MIN/MAX/MID/CONTROL all 0
         * on this panel, and that reading is what makes the restore knowable. */
        CHECK(rb_drr_check_owned(DRR_HTOTAL, DRR_VTOTAL, 1u, 1u, 2707u)
                  == RB_DRR_FW_DRR &&
              rb_drr_check_owned(DRR_HTOTAL, DRR_VTOTAL, 3u, 1u, 2707u)
                  == RB_DRR_FW_DRR,
              "a firmware that owns DRR refuses from either state");

        /* The non-CONTROL refusals must still come through unchanged, or the
         * new wrapper would be a hole rather than a door. */
        CHECK(rb_drr_check_owned(DRR_HTOTAL, DRR_VTOTAL, armed, 0u, DRR_VTOTAL)
                  == RB_DRR_NOT_LONGER &&
              rb_drr_check_owned(DRR_HTOTAL, DRR_VTOTAL, armed, 0u, 0x8000u)
                  == RB_DRR_TOO_LONG &&
              rb_drr_check_owned(0u, DRR_VTOTAL, armed, 0u, 2707u)
                  == RB_DRR_NO_RASTER,
              "every other refusal still fires through the owned check");

        /* Boot 258's own three modes, pairwise: from 60 Hz every other mode on
         * the ladder must now be reachable without passing through native. */
        {
            struct rb_drr_mode mm[RB_DRR_MAX_MODES];
            const unsigned k = rb_drr_build_modes(342056937u, DRR_HTOTAL,
                                                  DRR_VTOTAL, 0u, mm,
                                                  RB_DRR_MAX_MODES);
            unsigned i, ok = 0;
            CHECK(k == 8u, "boot 258's clock gives the same eight modes");
            for (i = 1; i < k; i++) {
                if (rb_drr_check_owned(DRR_HTOTAL, DRR_VTOTAL, armed, 0u,
                                       mm[i].lines) == RB_DRR_OK) { ok++; }
            }
            CHECK(ok == k - 1u,
                  "from an armed mode, all seven non-native modes are reachable");
        }
    }

    return failures;
}
