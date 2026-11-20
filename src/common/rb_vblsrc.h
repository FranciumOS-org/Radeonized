/* SPDX-License-Identifier: MIT */
/* rb_vblsrc.h - where the frame clock's initial period comes from (issue #8).
 *
 * THE DEFECT. RBAMDFramebuffer's VBL thread_call takes its period from
 * `vblPeriodNs`, and `vblRetune()` - the only thing that ever changes it - is
 * called from exactly one place, `modeApply()`. So until the user switches
 * display mode, the period is whatever `start()` put there, and that was
 * `kVBLPeriodNsFast`: a constant derived from this chassis' measured 143.83 Hz
 * panel. On #8's 2880x1800 90 Hz OLED the frame clock therefore ticked at
 * 144 Hz from boot, on a 90 Hz raster, with no boot-arg able to correct it on
 * the framebuffer side. The reporter worked around it with a local patch plus
 * `rbgpuvblhz=89871` for the plugin's half.
 *
 * This is the same defect as issues #4 and #5 - a number measured on the
 * reference board standing in for one that has to be derived per machine - and
 * it gets the same answer.
 *
 * WHY IT IS OPT-IN. The derivation needs a refresh measurement, and the only
 * one available at `start()` is `measureMhz()`: 16 frames latched on a
 * FRAME_COUNT edge, ~111 ms at 144 Hz, ~178 ms at 90 Hz, bounded at 1.2 s of
 * `IODelay` - a busy-wait in `start()`. On a machine whose panel this constant
 * already describes, that spin buys nothing, so `-rbgpufbnative` is the user's
 * statement that it does not describe theirs. When `-rbgpufbmodes` is armed
 * the mode list's native entry is already a period derived from the same
 * raster, and reusing it costs no second spin.
 *
 * WHAT THIS FILE IS FOR. One conversion, so the two routes into `vblPeriodNs`
 * cannot disagree about the arithmetic, and the hosttest decides it rather
 * than a boot. The SOURCE is published alongside, because a period that fell
 * back to the constant and a period that was derived are indistinguishable
 * from the number alone - and "it is derived now" is exactly the claim a
 * reader of #8 would otherwise have to take on trust (rule 14).
 */
#ifndef RB_VBLSRC_H
#define RB_VBLSRC_H

#include <stdint.h>

/* Which route produced the period in force. Ordered so that 0 is the
 * pre-existing behaviour: a boot that published 0 is a boot where nothing
 * about this changed. */
#define RB_VBLSRC_CONST        0u  /* the constant - not armed, or fell back */
#define RB_VBLSRC_MODELIST     1u  /* reused the mode list's native entry    */
#define RB_VBLSRC_MEASURED     2u  /* measured here, at start()              */
#define RB_VBLSRC_MEAS_FAILED  3u  /* measureMhz returned 0; constant stands */
#define RB_VBLSRC_NO_MAP       4u  /* no BAR5 map, so nothing to measure     */

/* The bound `measureMhz` itself refuses outside, restated here because this
 * function is also reached with values from a boot-arg and from a mode list,
 * neither of which went through that check. 20 Hz to 400 Hz, the same window
 * the plugin's `rbgpuvblhz` parse has used since 2ou - one bound, two
 * consumers, rather than two bounds that can drift apart. */
#define RB_VBLSRC_MHZ_MIN   20000u
#define RB_VBLSRC_MHZ_MAX  400000u

/* millihertz -> nanoseconds per frame. Returns 0 - never a fallback value -
 * when the input is outside the bound, so the caller has to decide what to do
 * about it and cannot mistake a refusal for a measurement (rule 14).
 *
 * Rounded to nearest rather than truncated: at 143.833 Hz the exact period is
 * 6 952 992 ns and truncation loses 0.6 ns per frame, which is nothing per
 * frame and ~0.3 ms per hour of accumulated phase against a clock whose
 * cadence 2xe established is load-bearing. The +half-divisor form is exact in
 * 64-bit for every input in range (the largest numerator is ~1e18, well under
 * 2^63). */
static inline uint64_t rb_vblsrc_period_ns(uint32_t mhz) {
    if (mhz < RB_VBLSRC_MHZ_MIN || mhz > RB_VBLSRC_MHZ_MAX) { return 0u; }
    return (1000000000000ull + (uint64_t)mhz / 2u) / (uint64_t)mhz;
}

/* The inverse, for reading a published period back as a refresh. Same bound,
 * applied to the OUTPUT, so a round trip that lands outside the window is a 0
 * rather than a number that looks plausible. */
static inline uint32_t rb_vblsrc_mhz_from_ns(uint64_t ns) {
    uint64_t mhz;
    if (ns == 0u) { return 0u; }
    mhz = (1000000000000ull + ns / 2u) / ns;
    if (mhz < RB_VBLSRC_MHZ_MIN || mhz > RB_VBLSRC_MHZ_MAX) { return 0u; }
    return (uint32_t)mhz;
}

/* Did this boot actually derive the period, as opposed to publishing a source
 * code and then using the constant anyway? Both terms are needed: a source of
 * MEASURED with a zero period is the failure 309's break-it sweep would call a
 * conjunction with a term carrying no information, and it has a code of its
 * own precisely so it cannot read as a success. */
static inline int rb_vblsrc_derived(uint32_t src, uint64_t periodNs) {
    return periodNs != 0u &&
           (src == RB_VBLSRC_MODELIST || src == RB_VBLSRC_MEASURED);
}

#endif /* RB_VBLSRC_H */
