/* SPDX-License-Identifier: MIT */
#include "rb_backlight.h"
#include "rb_regs.h"
#include "rb_dcn_deny.h"
#include <stdio.h>
#include <string.h>

/* 2xs: the backlight arithmetic and its gate. Everything the kext decides is
 * decided by rb_backlight.h, so everything it decides is run here first -
 * the same contract rb_hubp.h has, for the same reason: this is a write.
 *
 * The anchor of the whole file is 2os's 22784. That number was derived by
 * hand, on paper, from the raw registers, three milestones before this code
 * existed. Here the SAME raw registers go through Linux's formula. Two
 * independent derivations agreeing on one value is the only reason to
 * believe either of them. */

/* Boot 133 / 2os, live, on this machine. */
#define BL_CNTL_MEASURED    0xc00056eau
#define BL_PERIOD_MEASURED  0x000c0fa0u
#define BL_LOCK_MEASURED    0x80000000u

static uint32_t reg_from_defs(const char *name)
{
    for (unsigned i = 0; i < RB_NREG_DEFS; i++) {
        if (!strcmp(rb_reg_defs[i].name, name)) { return rb_reg_defs[i].reg; }
    }
    return 0;
}

int backlight_tests(void)
{
    int bad = 0;
#define A(x) do { if (!(x)) { printf("  FAIL BL:%d: %s\n", __LINE__, #x); bad++; } } while (0)

    /* --- 3a: the offsets are the ones rb_regs.h carries, and none of the
     * four is denied. rb_regs.h was written from the Linux header by a
     * different hand at a different time, so this is a cross-check, not a
     * restatement - the same control hubp_tests() uses. */
    A(reg_from_defs("BL_PWM_CNTL")          == 0x2f19u);
    A(reg_from_defs("BL_PWM_CNTL2")         == 0x2f1au);
    A(reg_from_defs("BL_PWM_PERIOD_CNTL")   == 0x2f1bu);
    A(reg_from_defs("BL_PWM_GRP1_REG_LOCK") == 0x2f1cu);
    /* 2xp: the deny list carries 0x288b, which WAS this register in dcn_3_0_2
     * and is DCIO_BL_PWM_FRAME_START_DISP_SEL in dcn_3_1_2. So the entry
     * exists, it is a rename, and it does not cover our four. Both halves
     * asserted, because "not denied" alone would also pass if the deny list
     * were empty. */
    A(rb_dcn_is_denied(0x288bu));
    A(!rb_dcn_is_denied(0x2f19u));
    A(!rb_dcn_is_denied(0x2f1au));
    A(!rb_dcn_is_denied(0x2f1bu));
    A(!rb_dcn_is_denied(0x2f1cu));

    /* --- 3b: decoding the measured registers. Each field separately, so a
     * wrong mask fails on its own line rather than inside the arithmetic. */
    A(rb_bl_period(BL_PERIOD_MEASURED) == 4000u);     /* 0x0fa0 */
    A(rb_bl_bitcnt(BL_PERIOD_MEASURED) == 12u);       /* 0xc    */
    A((BL_CNTL_MEASURED & RB_BL_CNTL_ACTIVE_CNT_MASK) == 22250u);  /* 0x56ea */
    A((BL_CNTL_MEASURED & RB_BL_CNTL_PWM_EN) != 0u);
    A((BL_CNTL_MEASURED & RB_BL_CNTL_FRACTIONAL_EN) != 0u);
    /* The lock register read 0x80000000: IGNORE_MASTER_LOCK_EN set, and the
     * LOCK bit itself CLEAR - i.e. unlocked, with no update pending. 2os
     * called it "lock free" and this says which bits that was. */
    A((BL_LOCK_MEASURED & RB_BL_LOCK_IGNORE_MASTER) != 0u);
    A((BL_LOCK_MEASURED & RB_BL_LOCK_REG_LOCK) == 0u);
    A((BL_LOCK_MEASURED & RB_BL_LOCK_UPDATE_PENDING) == 0u);
    /* A BITCNT field of 0 means 16, not 0. If this ever reads 0 the mask
     * below would be (1<<0)-1 == 0 and every level would compute to zero. */
    A(rb_bl_bitcnt(0x00000fa0u) == 16u);

    /* --- 3c: THE CONTROL. 2os's hand arithmetic, reproduced by Linux's. */
    A(rb_bl_level_from_pwm(BL_CNTL_MEASURED, BL_PERIOD_MEASURED) == 22784u);
    /* and 22784/0x10000 is the 34.8 % 2os reported, to the digit it gave */
    A((22784u * 1000u) / RB_BL_LEVEL_FULL == 347u);

    /* --- 3d: the gate. 2or's falsification test is the first clause. */
    A(rb_bl_can_control(BL_CNTL_MEASURED, BL_PERIOD_MEASURED) == 1);
    /* period 0 -> "the panel is driven some other way and this route is
     * wrong". REFUSED, not patched with a substituted period. */
    A(rb_bl_can_control(BL_CNTL_MEASURED, 0x000c0000u) == 0);
    /* PWM disabled -> the duty cycle drives nothing. */
    A(rb_bl_can_control(BL_CNTL_MEASURED & ~RB_BL_CNTL_PWM_EN,
                        BL_PERIOD_MEASURED) == 0);
    /* Both wrong is still refused, and the all-zero register set - what a
     * failed read or an unmapped aperture looks like - fails safe. */
    A(rb_bl_can_control(0, 0) == 0);

    /* --- 3e: level -> duty cycle, and the round trip.
     * Full scale with this period must come out as period << (16-bitcnt),
     * i.e. an integer duty of exactly 4000 with no fraction: 4000<<4 =
     * 64000. That is a value predicted from the FIELD LAYOUT, not from
     * running the function, so it is a real check on it. */
    A(rb_bl_count_from_level(RB_BL_LEVEL_FULL, BL_PERIOD_MEASURED) == 64000u);
    A((64000u >> 4) == 4000u);
    /* Round trip at full scale returns full scale. */
    A(rb_bl_level_from_pwm(rb_bl_cntl_with_count(BL_CNTL_MEASURED, 64000u),
                           BL_PERIOD_MEASURED) == RB_BL_LEVEL_FULL);
    /* Round trip at the measured level returns the measured level: the two
     * directions are inverses on the value the hardware is actually at. */
    A(rb_bl_count_from_level(22784u, BL_PERIOD_MEASURED) == 22250u);
    A(rb_bl_level_from_pwm(BL_CNTL_MEASURED, BL_PERIOD_MEASURED) == 22784u);
    /* Zero in, zero out - the arithmetic itself does not have a floor; the
     * floor is rb_bl_clamp()'s job and is tested separately, so the two
     * concerns cannot be confused. */
    A(rb_bl_count_from_level(0, BL_PERIOD_MEASURED) == 0u);
    /* Monotonic across the range, which a wrong shift would break while
     * still passing the endpoints. */
    {
        uint32_t prev = 0;
        for (uint32_t l = 0; l <= RB_BL_LEVEL_FULL; l += 0x400u) {
            const uint32_t c = rb_bl_count_from_level(l, BL_PERIOD_MEASURED);
            A(c >= prev);
            prev = c;
        }
        A(prev == 64000u);
    }

    /* --- 3f: the read-modify-write touches ONE field. This is the property
     * that keeps the enable, the fractional enable and the status bits as the
     * firmware left them. */
    A(rb_bl_cntl_with_count(BL_CNTL_MEASURED, 0u) == 0xc0000000u);
    A(rb_bl_cntl_with_count(BL_CNTL_MEASURED, 64000u) == 0xc000fa00u);
    /* An over-wide count cannot bleed into FRACTIONAL_EN or PWM_EN. */
    A(rb_bl_cntl_with_count(BL_CNTL_MEASURED, 0xffffffffu) == 0xc000ffffu);
    A((rb_bl_cntl_with_count(BL_CNTL_MEASURED, 0x12345u) &
       ~RB_BL_CNTL_ACTIVE_CNT_MASK) ==
      (BL_CNTL_MEASURED & ~RB_BL_CNTL_ACTIVE_CNT_MASK));

    /* --- 3g: the clamp, i.e. the one state we refuse to enter. */
    A(rb_bl_clamp(0, RB_BL_LEVEL_FLOOR) == RB_BL_LEVEL_FLOOR);
    A(rb_bl_clamp(1, RB_BL_LEVEL_FLOOR) == RB_BL_LEVEL_FLOOR);
    A(rb_bl_clamp(RB_BL_LEVEL_FLOOR, RB_BL_LEVEL_FLOOR) == RB_BL_LEVEL_FLOOR);
    A(rb_bl_clamp(0x8000u, RB_BL_LEVEL_FLOOR) == 0x8000u);
    A(rb_bl_clamp(RB_BL_LEVEL_FULL, RB_BL_LEVEL_FLOOR) == RB_BL_LEVEL_FULL);
    /* Above full scale saturates rather than wrapping - a u16.16 of
     * 0x10001 would otherwise compute a duty cycle above the period. */
    A(rb_bl_clamp(RB_BL_LEVEL_FULL + 1u, RB_BL_LEVEL_FLOOR) == RB_BL_LEVEL_FULL);
    A(rb_bl_clamp(0xffffffffu, RB_BL_LEVEL_FLOOR) == RB_BL_LEVEL_FULL);
    /* And the clamped floor still produces a non-zero duty cycle, which is
     * the property that actually matters: a floor that rounded to a count of
     * 0 would be a dark panel with a reassuring-looking level. */
    A(rb_bl_count_from_level(RB_BL_LEVEL_FLOOR, BL_PERIOD_MEASURED) > 0u);
    A(RB_BL_LEVEL_FLOOR > 0u && RB_BL_LEVEL_FLOOR < RB_BL_LEVEL_FULL);

    /* --- 3h: the FourCC is DERIVED, not chosen. IOFramebufferParameter-
     * Handler::doIntegerSet (IOGraphicsFamily 0x14fce436) reads the first
     * four bytes of the parameter NAME and byte-swaps them into the attribute
     * it passes to setAttributeForConnection. So for kIODisplayBrightnessKey,
     * which is the string "brightness", the attribute can only ever be
     * 'brig'. Asserted here so the kext's constant is tied to the string. */
    {
        const char *name = "brightness";
        const uint32_t fourcc = ((uint32_t)(uint8_t)name[0] << 24) |
                                ((uint32_t)(uint8_t)name[1] << 16) |
                                ((uint32_t)(uint8_t)name[2] << 8) |
                                 (uint32_t)(uint8_t)name[3];
        A(fourcc == 0x62726967u);       /* 'brig' */
        /* and the constant the kext actually sends IS that derivation */
        A(RB_BL_ATTR_BRIGHTNESS == fourcc);
        /* The same rule on a parameter the live machine already shows, as
         * the control: AppleDisplay's IODisplayParameters carries "bgsc",
         * and kConnectionBlueGammaScale is 'bgsc'. */
        A((((uint32_t)'b' << 24) | ((uint32_t)'g' << 16) |
           ((uint32_t)'s' << 8) | (uint32_t)'c') == 0x62677363u);
    }

#undef A
    printf("backlight: 2os's 22784 reproduced by Linux's formula, gate, "
           "round trip, clamp and the derived 'brig'%s\n",
           bad ? " FAILED" : ": passed");
    return bad;
}
