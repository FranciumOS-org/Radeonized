/* SPDX-License-Identifier: MIT */
#ifndef RB_BACKLIGHT_H
#define RB_BACKLIGHT_H

/* 2xs: the panel's backlight, and the arithmetic that decides what to write.
 *
 * Same shape as rb_hubp.h and for the same reason: this is a WRITE to the
 * display engine, so every decision lives here as a pure function the
 * hosttest proves on the host, and the kext does nothing the hosttest has
 * not already run. A wrong value here does not scramble the image - it
 * changes how bright the panel is, and a value of zero makes the screen look
 * dead, which rule 12 says is indistinguishable from a hang. That is the
 * whole risk, and it is why rb_bl_clamp() has a non-zero floor.
 *
 * WHAT THE WRITE DOES ON THE CHIP. PWRSEQ0's BL_PWM block generates the
 * pulse-width-modulated signal the panel's backlight driver takes as its
 * brightness input. BL_PWM_PERIOD_CNTL sets the period and BL_PWM_CNTL's
 * BL_ACTIVE_INT_FRAC_CNT sets the active (on) part of it. We change only the
 * duty cycle: not the period, not the enable, not the reference divider, not
 * PWRSEQ's panel power sequencing. Nothing about scanout, timing, clocks or
 * power state is touched, so this cannot blank the image or wedge DCN - it
 * can only make the panel dim or dark.
 *
 * THE REGISTERS, measured on this machine in 2os and spelled for DCN 3.1.2
 * in 2xp. dcn_3_1_2_offset.h calls them regPWRSEQ0_BL_PWM_*, base_idx 2:
 *     BL_PWM_CNTL           0x2f19   read 0xc00056ea
 *     BL_PWM_CNTL2          0x2f1a
 *     BL_PWM_PERIOD_CNTL    0x2f1b   read 0x000c0fa0
 *     BL_PWM_GRP1_REG_LOCK  0x2f1c   read 0x80000000
 * None is on rb_dcn_deny.h's list. The list DOES carry 0x288b, which was
 * BL_PWM_GRP1_REG_LOCK in dcn_3_0_2 and is DCIO_BL_PWM_FRAME_START_DISP_SEL
 * in dcn_3_1_2 - the list's own comment records that rename, so it is not a
 * conflict with these four (2xp, checked again by the hosttest).
 *
 * THE ARITHMETIC IS NOT OURS. Both directions are transcribed from
 * docs/linux-ref/dce_panel_cntl.c (v6.12):
 *     dce_get_16_bit_backlight_from_pwm()  -> rb_bl_level_from_pwm()
 *     dce_driver_set_backlight()  step 1   -> rb_bl_count_from_level()
 * and every field mask below is copied from
 * docs/linux-ref/dcn_3_1_2_sh_mask.h, not from the bit positions 2os
 * inferred. The unit is Linux's own: a u16.16 fraction where 0x10000 is full
 * scale, so no unit is invented here either.
 *
 * AND THE TWO DERIVATIONS AGREE, which is this file's one real control.
 * 2os computed 34.8 % by hand from the raw registers. Feeding the SAME raw
 * registers to the Linux formula gives 22784, and 22784 / 0x10000 = 34.77 %.
 * The hosttest asserts the 22784 exactly: if a future edit breaks the
 * arithmetic, it stops agreeing with a number that was derived a different
 * way, on paper, three milestones earlier.
 */

#include <stdint.h>

/* --- field masks, verbatim from dcn_3_1_2_sh_mask.h ------------------- */

/* PWRSEQ0_BL_PWM_CNTL */
#define RB_BL_CNTL_ACTIVE_CNT_MASK   0x0000ffffu  /* BL_ACTIVE_INT_FRAC_CNT */
#define RB_BL_CNTL_ACTIVE_CNT_SHIFT  0u
#define RB_BL_CNTL_FRACTIONAL_EN     0x40000000u  /* BL_PWM_FRACTIONAL_EN  */
#define RB_BL_CNTL_PWM_EN            0x80000000u  /* BL_PWM_EN             */

/* PWRSEQ0_BL_PWM_PERIOD_CNTL */
#define RB_BL_PERIOD_MASK            0x0000ffffu  /* BL_PWM_PERIOD         */
#define RB_BL_PERIOD_SHIFT           0u
#define RB_BL_BITCNT_MASK            0x000f0000u  /* BL_PWM_PERIOD_BITCNT  */
#define RB_BL_BITCNT_SHIFT           16u

/* PWRSEQ0_BL_PWM_GRP1_REG_LOCK */
#define RB_BL_LOCK_REG_LOCK          0x00000001u  /* BL_PWM_GRP1_REG_LOCK  */
#define RB_BL_LOCK_UPDATE_PENDING    0x00000100u  /* ..._REG_UPDATE_PENDING */
#define RB_BL_LOCK_IGNORE_MASTER     0x80000000u  /* ..._IGNORE_MASTER_LOCK_EN */

/* The connection attribute a brightness set arrives as. NOT chosen:
 * IOFramebufferParameterHandler::doIntegerSet (IOGraphicsFamily 0x14fce436)
 * byte-swaps the first four bytes of the parameter NAME into the attribute it
 * passes to setAttributeForConnection, and kIODisplayBrightnessKey is the
 * string "brightness". The hosttest derives this value from that string
 * rather than restating the macro (2v1). */
#define RB_BL_ATTR_BRIGHTNESS        0x62726967u  /* 'brig' */

/* A sanity bound on the base framebuffer's own parameter count, used only to
 * refuse appending into an index we have no reason to believe. The base
 * writes at most 'rgsc','ggsc','bgsc','gsc ','vblm','oscn' = 6. */
#define RB_BL_PARM_MAX               32u

/* Full scale, in Linux's u16.16 units. 0x10000 == 100 %. */
#define RB_BL_LEVEL_FULL             0x10000u

/* The floor rb_bl_clamp() will not go below, in the same units. This is a
 * POLICY choice, not a measurement, and it is here rather than in the kext so
 * it is visible and tested: a backlight set to zero is a screen that looks
 * dead, and rule 12 says that is indistinguishable from a hang, so the one
 * state we refuse to enter is the one we could not diagnose from. 8 % is low
 * enough to be useful and high enough to read the screen in a dark room. */
#define RB_BL_LEVEL_FLOOR            0x1400u      /* 5120 == 7.8 % */

/* --- decoding ---------------------------------------------------------- */

static inline uint32_t rb_bl_period(uint32_t period_cntl)
{
    return (period_cntl & RB_BL_PERIOD_MASK) >> RB_BL_PERIOD_SHIFT;
}

static inline uint32_t rb_bl_bitcnt(uint32_t period_cntl)
{
    /* Linux: "if (bl_int_count == 0) bl_int_count = 16;" - a BITCNT of 0
     * means the full 16 bits, it does not mean a zero-width period. */
    const uint32_t raw = (period_cntl & RB_BL_BITCNT_MASK) >> RB_BL_BITCNT_SHIFT;
    return raw == 0u ? 16u : raw;
}

/* --- the gate ---------------------------------------------------------- */

/* Would writing a duty cycle mean anything on this chip right now?
 *
 * 2or stated the falsification test for this whole route before any of it was
 * built: "BL_PWM_PERIOD == 0 would mean the panel is driven some other way
 * and this route is wrong." So a zero period REFUSES rather than being
 * patched up - Linux's own `if (bl_period == 0) bl_period = 0xFFFF;` is a
 * read-side fallback for a BIOS bug, and substituting a period we invented
 * and then writing a duty cycle computed from it is exactly rule 17.
 *
 * BL_PWM_EN clear means the PWM output is off, so a duty cycle would change
 * a signal nobody is driving the panel with - also a refusal, and also a sign
 * the panel is lit some other way. */
static inline int rb_bl_can_control(uint32_t cntl, uint32_t period_cntl)
{
    if ((cntl & RB_BL_CNTL_PWM_EN) == 0u) {
        return 0;               /* the PWM is not driving anything */
    }
    if (rb_bl_period(period_cntl) == 0u) {
        return 0;               /* 2or's falsification test, failing */
    }
    return 1;
}

/* --- read: duty cycle -> level ---------------------------------------- */

/* dce_get_16_bit_backlight_from_pwm(), docs/linux-ref/dce_panel_cntl.c:49.
 * Transcribed, with Linux's own comments' logic kept in the same order. The
 * one deliberate difference: Linux's `if (bl_period == 0) bl_period = 0xFFFF`
 * is kept, because here it is only a DIVISOR guard on a read - rb_bl_can_
 * control() has already refused that case for any write. */
static inline uint32_t rb_bl_level_from_pwm(uint32_t cntl, uint32_t period_cntl)
{
    const uint32_t bl_int_count = rb_bl_bitcnt(period_cntl);
    const uint32_t bl_period_mask = (uint32_t)((1u << bl_int_count) - 1u);
    uint32_t bl_period = rb_bl_period(period_cntl) & bl_period_mask;
    const uint32_t frac_en = (cntl & RB_BL_CNTL_FRACTIONAL_EN) != 0u;
    uint32_t bl_pwm = (cntl & RB_BL_CNTL_ACTIVE_CNT_MASK) >> RB_BL_CNTL_ACTIVE_CNT_SHIFT;
    uint64_t current_backlight;

    if (!frac_en) {
        const uint32_t bl_pwm_mask = bl_period_mask << (16u - bl_int_count);
        bl_pwm &= bl_pwm_mask;
    } else {
        bl_pwm &= 0xffffu;
    }

    current_backlight = (uint64_t)bl_pwm << (1u + bl_int_count);

    if (bl_period == 0u) {
        bl_period = 0xffffu;
    }

    current_backlight /= bl_period;
    current_backlight = (current_backlight + 1u) >> 1;

    return (uint32_t)current_backlight;
}

/* --- write: level -> duty cycle --------------------------------------- */

/* dce_driver_set_backlight() step 1, docs/linux-ref/dce_panel_cntl.c:188.
 * Returns the BL_ACTIVE_INT_FRAC_CNT field value to program; the caller does
 * the lock / write / unlock sequence, which is mechanism, not decision. */
static inline uint32_t rb_bl_count_from_level(uint32_t level, uint32_t period_cntl)
{
    const uint32_t bit_count = rb_bl_bitcnt(period_cntl);
    const uint32_t masked_pwm_period =
        rb_bl_period(period_cntl) & (uint32_t)((1u << bit_count) - 1u);
    const uint64_t active_duty_cycle = (uint64_t)level * (uint64_t)masked_pwm_period;
    uint32_t backlight_16bit;

    backlight_16bit = (uint32_t)(active_duty_cycle >> bit_count);
    backlight_16bit &= 0xffffu;
    backlight_16bit += (uint32_t)((active_duty_cycle >> (bit_count - 1u)) & 0x1u);

    return backlight_16bit & RB_BL_CNTL_ACTIVE_CNT_MASK;
}

/* Fold a new duty cycle into BL_PWM_CNTL, leaving every other bit - the
 * enable, the fractional-enable, the status bits - exactly as the firmware
 * left it. The whole point of a read-modify-write here is that we change one
 * field and nothing else. */
static inline uint32_t rb_bl_cntl_with_count(uint32_t cntl, uint32_t count)
{
    return (cntl & ~RB_BL_CNTL_ACTIVE_CNT_MASK) |
           ((count << RB_BL_CNTL_ACTIVE_CNT_SHIFT) & RB_BL_CNTL_ACTIVE_CNT_MASK);
}

/* Clamp a requested level into [floor, full]. See RB_BL_LEVEL_FLOOR. */
static inline uint32_t rb_bl_clamp(uint32_t level, uint32_t floor)
{
    if (level > RB_BL_LEVEL_FULL) { return RB_BL_LEVEL_FULL; }
    if (level < floor)            { return floor; }
    return level;
}

#endif /* RB_BACKLIGHT_H */
