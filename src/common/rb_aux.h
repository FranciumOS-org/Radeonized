/* SPDX-License-Identifier: MIT */
#ifndef RB_AUX_H
#define RB_AUX_H

/* 2B8: the DP AUX engines, READ ONLY.
 *
 * WHY THIS IS A SURVEY AND NOT A TRANSACTION. 2B4 established the Type-C port
 * is HPD2 and is wired to this chip's display engine. The next question is
 * whether the AUX channel reaches it too - and the step before *that* is
 * whether the AUX ENGINE on that instance is configured at all. Those are
 * different facts: a transaction issued on an unconfigured engine times out,
 * which is indistinguishable from a sink that is not listening and from a
 * channel that is not wired (rule 14 - absence is not a result). So this
 * header reads the engines and decides nothing else.
 *
 * THE REFERENCE INSTANCE IS THE POINT. The firmware trained the eDP panel on
 * this chip, so one of these five engines is in a known-working configuration
 * RIGHT NOW, on this silicon, at this firmware revision. That is a far better
 * standard than any absolute judgement about what a configured engine should
 * look like - and it is 2zw's lesson applied before the fact: the 680M's
 * BL_PWM_CNTL looked perfect in isolation and was attached to nothing. So
 * rb_aux_matches_ref() compares an engine against the one the firmware uses,
 * and the kext publishes every engine's words so the comparison can be redone
 * by hand against a different reference if the first guess is wrong.
 *
 * OFFSETS, from docs/linux-ref/dcn_3_1_2_offset.h, instance 0 with a
 * 0x1c-dword stride (regDP_AUX2_AUX_CONTROL 0x1f88 = 0x1f50 + 2 * 0x1c):
 *     regDP_AUX0_AUX_CONTROL            0x1f50   BASE_IDX 2
 *     regDP_AUX0_AUX_SW_CONTROL         0x1f51
 *     regDP_AUX0_AUX_ARB_CONTROL        0x1f52
 *     regDP_AUX0_AUX_INTERRUPT_CONTROL  0x1f53
 *     regDP_AUX0_AUX_SW_STATUS          0x1f54
 *     regDP_AUX0_AUX_SW_DATA            0x1f56
 *     regDP_AUX0_AUX_DPHY_RX_CONTROL1   0x1f5b
 * Five instances, BASE_IDX 2 - the same base as HPD (0x1f14), OTG (0x1b41)
 * and HUBP, so this needs no new mapping, exactly as 2B3 did not.
 * AUX_EN is bit 0 of AUX_CONTROL (dcn_3_1_2_sh_mask.h).
 *
 * RULE 46 IS DISCHARGED, AND PROPERLY THIS TIME. The engine these registers
 * belong to is DCE's, not a DCN one - which is the exact shape of 2xs/2zl's
 * four-milestone argument - so the grouping was checked in the resource file
 * rather than assumed: dcn31_resource.c's dcn31_aux_engine_create() calls
 * dce110_aux_engine_construct() with aux_engine_regs[inst] built from
 * AUX_COMMON_REG_LIST0(id), five instances. **DCN 3.1 uses the DCE110 AUX
 * engine verbatim**, the same relationship 2B1 found for the HUBP
 * triple-buffer writer. dce110_aux_engine_construct() also touches NO
 * hardware - it is pure software state - so whatever configures these engines
 * is elsewhere, which is the other reason to go and look rather than assume.
 *
 * THERE IS NO WRITER IN THIS HEADER. An AUX transaction means writing
 * AUX_SW_DATA, AUX_SW_CONTROL and AUX_ARB_CONTROL on a shared resource the
 * DMCUB may also be arbitrating for, and taking the ARB lock wrongly on the
 * engine driving the live panel would be a way to lose the display. That is a
 * separate decision with its own evidence, and it is not taken here.
 */

#include <stdint.h>

#define RB_AUX_COUNT              5u
#define RB_AUX_DW_STRIDE          0x1cu

#define RB_AUX_DW_CONTROL         0x1f50u
#define RB_AUX_DW_SW_CONTROL      0x1f51u
#define RB_AUX_DW_ARB_CONTROL     0x1f52u
#define RB_AUX_DW_INT_CONTROL     0x1f53u
#define RB_AUX_DW_SW_STATUS       0x1f54u
#define RB_AUX_DW_SW_DATA         0x1f56u
#define RB_AUX_DW_DPHY_RX_CTL1    0x1f5bu

/* 0 is not a valid offset for any of these blocks, so it is the refusal
 * value - a caller that ignores the bound reads nothing meaningful rather
 * than reading some other block's register (rule 19, as rb_hpd.h). */
#define RB_AUX_DW_INVALID         0u

#define RB_AUX_EN                 0x00000001u
#define RB_AUX_LS_READ_EN         0x00000100u

enum rb_aux_reg {
    RB_AUX_REG_CONTROL = 0,
    RB_AUX_REG_SW_CONTROL,
    RB_AUX_REG_ARB_CONTROL,
    RB_AUX_REG_INT_CONTROL,
    RB_AUX_REG_SW_STATUS,
    RB_AUX_REG_SW_DATA,
    RB_AUX_REG_DPHY_RX_CTL1,
    RB_AUX_REG_COUNT
};

static inline uint32_t rb_aux_reg_base(enum rb_aux_reg which)
{
    switch (which) {
    case RB_AUX_REG_CONTROL:       return RB_AUX_DW_CONTROL;
    case RB_AUX_REG_SW_CONTROL:    return RB_AUX_DW_SW_CONTROL;
    case RB_AUX_REG_ARB_CONTROL:   return RB_AUX_DW_ARB_CONTROL;
    case RB_AUX_REG_INT_CONTROL:   return RB_AUX_DW_INT_CONTROL;
    case RB_AUX_REG_SW_STATUS:     return RB_AUX_DW_SW_STATUS;
    case RB_AUX_REG_SW_DATA:       return RB_AUX_DW_SW_DATA;
    case RB_AUX_REG_DPHY_RX_CTL1:  return RB_AUX_DW_DPHY_RX_CTL1;
    default:                       return RB_AUX_DW_INVALID;
    }
}

static inline uint32_t rb_aux_dw(unsigned inst, enum rb_aux_reg which)
{
    const uint32_t base = rb_aux_reg_base(which);
    if (inst >= RB_AUX_COUNT || base == RB_AUX_DW_INVALID) {
        return RB_AUX_DW_INVALID;
    }
    return base + inst * RB_AUX_DW_STRIDE;
}

/* The highest dword any AUX read touches, for the aperture bound check -
 * one comparison against the maximum rather than a check per register that a
 * later addition can forget (2B3's shape). */
static inline uint32_t rb_aux_dw_max(void)
{
    return RB_AUX_DW_DPHY_RX_CTL1 + (RB_AUX_COUNT - 1u) * RB_AUX_DW_STRIDE;
}

static inline int rb_aux_enabled(uint32_t control)
{ return (control & RB_AUX_EN) != 0u; }

static inline uint32_t rb_aux_enabled_mask(const uint32_t *control,
                                           unsigned count)
{
    uint32_t mask = 0;
    if (!control) { return 0; }
    if (count > RB_AUX_COUNT) { count = RB_AUX_COUNT; }
    for (unsigned i = 0; i < count; i++) {
        if (rb_aux_enabled(control[i])) { mask |= (1u << i); }
    }
    return mask;
}

/* Does this engine look like the one the firmware is actually driving?
 *
 * Deliberately NOT a judgement about absolute values. Both engines are on the
 * same die at the same firmware revision, so the reference answers "what does
 * configured look like HERE" - and the DPHY word carries the receive timing,
 * which is what an engine that was never brought up would not have.
 *
 * Returns 0 when the reference itself is not enabled, because then there is no
 * reference and "matches" would be a statement about nothing (rule 14 - the
 * same reason rb_hpd_verdict refuses on a disabled block).
 */
static inline int rb_aux_matches_ref(uint32_t control, uint32_t dphy,
                                     uint32_t ref_control, uint32_t ref_dphy)
{
    if (!rb_aux_enabled(ref_control)) { return 0; }
    if (!rb_aux_enabled(control)) { return 0; }
    return (dphy == ref_dphy) ? 1 : 0;
}

#endif /* RB_AUX_H */
