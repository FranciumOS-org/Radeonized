/* SPDX-License-Identifier: MIT */
#ifndef RB_HPD_H
#define RB_HPD_H

/* 2B3: can this chip SEE a display on the Type-C port?
 *
 * WHY THIS EXISTS. The chassis has a USB4 Type-C port the user reports
 * carries DP alt mode from the iGPU. Plugged in it is black - which is the
 * EXPECTED reading either way, because RBAMDFramebuffer drives OTG0 and HUBP0
 * and nothing else, so a second display would be black even on a port wired
 * straight to this silicon. Rule 47 is the question: before concluding
 * anything about an output, establish it is WIRED to this chip right now.
 * DC_HPD_SENSE is the hardware's own answer, and it is a read.
 *
 * WHAT IT CANNOT SAY. HPD sense means a sink is electrically present on that
 * connector. It does NOT mean the link is trained, that EDID was read, or
 * that any pipe is driving it. It also says nothing about WHICH physical
 * connector an instance maps to - DCN numbers HPD blocks, the board wires
 * them, and that mapping is not in any header. So the usable reading is a
 * DIFFERENCE: sample with the cable out, sample with it in, and the instance
 * whose sense bit moved is the port. One sample on its own identifies
 * nothing (rule 18, and rule 33 - say when the reading was taken).
 *
 * THE CONTROL THAT MAKES IT AN INSTRUMENT. DC_HPD_EN (bit 28 of
 * DC_HPD_CONTROL) gates the block. With EN clear, SENSE reads 0 whatever is
 * plugged in - so a bare "sense 0" is indistinguishable from "the instrument
 * is off", which is rule 14 exactly: absence is not a result. Every reading
 * publishes EN beside SENSE and rb_hpd_verdict() refuses rather than
 * reporting "nothing connected" when the block is disabled.
 *
 * OFFSETS. docs/linux-ref/dcn_3_1_2_offset.h:
 *     regHPD0_DC_HPD_INT_STATUS  0x1f14  BASE_IDX 2
 *     regHPD0_DC_HPD_CONTROL     0x1f16  BASE_IDX 2
 *     regHPD1_DC_HPD_INT_STATUS  0x1f1c           -> stride 8 dwords
 * and five instances, HPD0..HPD4. BASE_IDX 2 is the same base the OTG and
 * HUBP registers this project already reads come from
 * (regOTG0_OTG_CONTROL 0x1b41 BASE_IDX 2, read at kRbDmuSeg2 + 0x1b41 and
 * returning 0x00011201 exactly as 2oq measured), so these are inside the
 * aperture already mapped and need no new mapping and no new write surface.
 * Fields from dcn_3_1_2_sh_mask.h:
 *     DC_HPD_INT_STATUS   bit 0
 *     DC_HPD_SENSE        bit 1
 *     DC_HPD_SENSE_DELAYED bit 4   (debounced - the one Linux believes)
 *     DC_HPD_EN           bit 28 of DC_HPD_CONTROL
 *
 * THIS HEADER IS READ-ONLY BY CONSTRUCTION. It computes dword offsets and
 * decodes words. There is no setter, because enabling an HPD block is a write
 * to the display engine on behalf of a port we have not established is ours.
 */

#include <stdint.h>

#define RB_HPD_COUNT            5u
#define RB_HPD_DW_INT_STATUS    0x1f14u
#define RB_HPD_DW_CONTROL       0x1f16u
#define RB_HPD_DW_STRIDE        0x8u

/* 0 is never a valid DMU dword offset for these blocks, so it is the refusal
 * value: a caller that ignores the bound reads nothing meaningful rather than
 * reading some other block's register. */
#define RB_HPD_DW_INVALID       0u

#define RB_HPD_INT_STATUS       0x00000001u
#define RB_HPD_SENSE            0x00000002u
#define RB_HPD_SENSE_DELAYED    0x00000010u
#define RB_HPD_EN               0x10000000u

static inline uint32_t rb_hpd_dw_int_status(unsigned inst)
{
    if (inst >= RB_HPD_COUNT) { return RB_HPD_DW_INVALID; }
    return RB_HPD_DW_INT_STATUS + inst * RB_HPD_DW_STRIDE;
}

static inline uint32_t rb_hpd_dw_control(unsigned inst)
{
    if (inst >= RB_HPD_COUNT) { return RB_HPD_DW_INVALID; }
    return RB_HPD_DW_CONTROL + inst * RB_HPD_DW_STRIDE;
}

/* The highest dword any HPD read touches, for the aperture bound check. */
static inline uint32_t rb_hpd_dw_max(void)
{
    return RB_HPD_DW_CONTROL + (RB_HPD_COUNT - 1u) * RB_HPD_DW_STRIDE;
}

static inline int rb_hpd_sense(uint32_t int_status)
{ return (int_status & RB_HPD_SENSE) != 0u; }

static inline int rb_hpd_sense_delayed(uint32_t int_status)
{ return (int_status & RB_HPD_SENSE_DELAYED) != 0u; }

static inline int rb_hpd_enabled(uint32_t control)
{ return (control & RB_HPD_EN) != 0u; }

enum rb_hpd_verdict {
    RB_HPD_DISABLED  = 0,  /* EN clear: SENSE says NOTHING, not "no sink" */
    RB_HPD_NO_SINK   = 1,  /* EN set, debounced sense clear */
    RB_HPD_SINK      = 2,  /* EN set, debounced sense set */
    RB_HPD_SETTLING  = 3   /* EN set, raw sense set but not yet debounced */
};

/* The whole reason this is a function and not two bit tests at the call site:
 * "no sink" and "the block is off" are different facts that look identical in
 * a sense bit, and 2zw cost six milestones to the same shape of mistake one
 * level out (a register that worked perfectly, attached to nothing). */
static inline enum rb_hpd_verdict rb_hpd_verdict(uint32_t int_status,
                                                 uint32_t control)
{
    if (!rb_hpd_enabled(control)) { return RB_HPD_DISABLED; }
    if (rb_hpd_sense_delayed(int_status)) { return RB_HPD_SINK; }
    if (rb_hpd_sense(int_status)) { return RB_HPD_SETTLING; }
    return RB_HPD_NO_SINK;
}

/* A bitmask over instances, so one published scalar carries all five and a
 * before/after difference is one XOR. Bit i is instance i. */
static inline uint32_t rb_hpd_sink_mask(const uint32_t *int_status,
                                        const uint32_t *control,
                                        unsigned count)
{
    uint32_t mask = 0;
    if (!int_status || !control) { return 0; }
    if (count > RB_HPD_COUNT) { count = RB_HPD_COUNT; }
    for (unsigned i = 0; i < count; i++) {
        if (rb_hpd_verdict(int_status[i], control[i]) == RB_HPD_SINK) {
            mask |= (1u << i);
        }
    }
    return mask;
}

static inline uint32_t rb_hpd_enabled_mask(const uint32_t *control,
                                           unsigned count)
{
    uint32_t mask = 0;
    if (!control) { return 0; }
    if (count > RB_HPD_COUNT) { count = RB_HPD_COUNT; }
    for (unsigned i = 0; i < count; i++) {
        if (rb_hpd_enabled(control[i])) { mask |= (1u << i); }
    }
    return mask;
}

#endif /* RB_HPD_H */
