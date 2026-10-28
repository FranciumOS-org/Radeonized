/* SPDX-License-Identifier: MIT */
#ifndef RB_HUBP_H
#define RB_HUBP_H

/* 2wh: the primary surface address of DCN 3.1.2's display pipe, and the
 * guards that decide whether writing it is safe.
 *
 * This is the first write this project makes to the display engine. Every
 * other DCN access has been a read (2os, 2rm), on the stated grounds that a
 * wrong read reports a wrong number while a wrong write blanks the panel -
 * and rule 12 says a black panel is indistinguishable from a hang. So the
 * whole decision procedure lives here, as pure functions the hosttest proves
 * on the host, and the kext does nothing the hosttest has not already run.
 *
 * WHAT THE WRITE DOES ON THE CHIP. HUBP is DCN's display-data fetch block.
 * DCSURF_PRIMARY_SURFACE_ADDRESS(_HIGH) is the address it reads scanout
 * pixels from, double-buffered and latched at the next vblank. Changing it
 * changes which bytes in VRAM reach the panel, and nothing else: no timing,
 * no clocks, no pipe topology, no power state. OTG keeps scanning at the same
 * 143.833 Hz either way. A wrong value shows garbage or black until the next
 * correct write, and one boot-arg reverses the whole thing.
 *
 * WHY ONLY THE ADDRESS. Boot 133 (2os) measured the live pipe:
 *     HUBP0_SURFACE_PITCH    0x077f   = 1920 px
 *     HUBP0_SURFACE_CONFIG   0x0008   = ARGB8888
 *     HUBP0_PRIMARY_ADDR_HI  0x00f4
 *     HUBP0_PRIMARY_ADDR     0x0000_0000   -> MC 0xf4_0000_0000
 * and boot 194 (2wg) measured the surface the compositor renders into:
 *     1920 px pitch, 4 bytes/pixel, MC 0xf4_1002_a000.
 * The pitch and the format ALREADY MATCH, and the HIGH dword is already
 * 0xf4, so exactly one dword has to change. That is what makes this the
 * narrowest possible first write.
 *
 * AND THE REGISTER TAKES AN MC ADDRESS, MEASURED NOT DERIVED. The pair reads
 * 0xf4:0x00000000 today, i.e. the carve-out base, which is an MC address.
 * 2os also noted that mc2pa(0xf400000000) is 0x7_8000_0000 system-physical -
 * that is the equivalent, NOT what the register holds. Do not apply mc2pa
 * here; the hardware is already telling us which space it wants.
 *
 * WHAT IS STILL UNMEASURED, AND WHY THIS BUILD READS IT. Pitch and format
 * match, but the SWIZZLE does not have to. The firmware's GOP framebuffer is
 * linear; a surface the GPU rendered into may be tiled (the _X family, which
 * is the whole of this project's swizzle defect). HUBP fetches through
 * DCSURF_ADDR_CONFIG / DCSURF_TILING_CONFIG, and if the surface's layout is
 * not the one those describe, the address is right and the picture is
 * scrambled. Nothing in 2os read them, so this build publishes both: a
 * legible image means they already agree, and a scrambled one means the next
 * step is a tiling write, not an address hunt. Reading them costs nothing and
 * is the difference between a boot that explains itself and one that does not.
 *
 * A COINCIDENCE THAT MUST NOT BE READ AS A RELATIONSHIP: 1920 == 0x780 and
 * the FB_OFFSET field is also 0x780. Same hex digits, unrelated quantities.
 */

#include <stdint.h>

#define RB_HUBP_COUNT  4u

/* Dword offsets inside DMU segment 2, from
 * docs/linux-ref/dcn_3_1_2_offset.h. These are the same values rb_regs.h
 * already lists under HUBPn_PRIMARY_ADDR / _HI; the hosttest cross-checks
 * the two tables against each other so a typo cannot survive in one of them.
 * The stride is uniform at 0xdc, but the TABLE is authoritative because it
 * came out of the header - the stride is only asserted, never used. */
#define RB_HUBP_STRIDE 0xdcu

enum rb_hubp_reg {
    RB_HUBP_REG_ADDR   = 0,
    RB_HUBP_REG_ADDR_HI,
    RB_HUBP_REG_PITCH,
    RB_HUBP_REG_CONFIG,
    RB_HUBP_REG_ADDR_CFG,   /* DCSURF_ADDR_CONFIG   - swizzle / pipe interleave */
    RB_HUBP_REG_TILING,     /* DCSURF_TILING_CONFIG - the swizzle mode itself */
    RB_HUBP_REG_COUNT
};

static const uint32_t rb_hubp_regs[RB_HUBP_COUNT][RB_HUBP_REG_COUNT] = {
    /*            ADDR     ADDR_HI   PITCH    CONFIG   ADDR_CFG  TILING */
    /* HUBP0 */ { 0x060a,  0x060b,   0x0607,  0x05e5,  0x05e6,   0x05e7 },
    /* HUBP1 */ { 0x06e6,  0x06e7,   0x06e3,  0x06c1,  0x06c2,   0x06c3 },
    /* HUBP2 */ { 0x07c2,  0x07c3,   0x07bf,  0x079d,  0x079e,   0x079f },
    /* HUBP3 */ { 0x089e,  0x089f,   0x089b,  0x0879,  0x087a,   0x087b },
};

/* 2os, boot 133, this machine. These are not defaults to fall back on - they
 * are the three values that have to still be true for us to believe we are
 * addressing HUBP0 of a firmware-lit ARGB8888 1920-pitch pipe. If any has
 * moved, we are either misaddressed or the pipe is not what was measured, and
 * the right answer is to refuse rather than to write. */
#define RB_HUBP_EXPECT_PITCH    0x0000077fu   /* 1920 px, pitch-1 encoding */
#define RB_HUBP_EXPECT_CONFIG   0x00000008u   /* ARGB8888 */
#define RB_HUBP_EXPECT_ADDR_HI  0x000000f4u   /* the carve-out's high dword */

/* The carve-out, agreed four independent ways (CLAUDE.md chip facts). */
#define RB_MC_CARVEOUT_BASE  0xf400000000ULL
#define RB_MC_CARVEOUT_SPAN  0x0100000000ULL  /* 4 GB, the published VRAM */

/* DCN latches the primary surface address in 256-byte units; the low 8 bits
 * of the LOW dword are not address bits. A misaligned value is not a
 * near-miss, it is a guaranteed garbage fetch. */
#define RB_HUBP_ADDR_ALIGN  0x100ULL

enum rb_hubp_verdict {
    RB_HUBP_OK = 0,
    RB_HUBP_REFUSE_INDEX,      /* HUBP index out of range */
    RB_HUBP_REFUSE_ZERO,       /* address is zero */
    RB_HUBP_REFUSE_RANGE,      /* outside the measured carve-out */
    RB_HUBP_REFUSE_ALIGN,      /* not 256-byte aligned */
    RB_HUBP_REFUSE_HI_MOVED,   /* the HIGH dword would change */
    RB_HUBP_REFUSE_PITCH,      /* pipe pitch is not what 2os measured */
    RB_HUBP_REFUSE_CONFIG,     /* pipe format is not what 2os measured */
    RB_HUBP_REFUSE_STATE_HI,   /* pipe is not scanning the carve-out */
    RB_HUBP_REFUSE_FRAME       /* the frame would run past the carve-out */
};

static inline uint32_t rb_hubp_dw(unsigned hubp, enum rb_hubp_reg which)
{
    if (hubp >= RB_HUBP_COUNT || (unsigned)which >= RB_HUBP_REG_COUNT) { return 0; }
    return rb_hubp_regs[hubp][which];
}

/* Is the pipe the one 2os measured? Called once, before anything is written. */
static inline int rb_hubp_check_state(uint32_t pitch, uint32_t config, uint32_t hi)
{
    if (pitch  != RB_HUBP_EXPECT_PITCH)   { return RB_HUBP_REFUSE_PITCH; }
    if (config != RB_HUBP_EXPECT_CONFIG)  { return RB_HUBP_REFUSE_CONFIG; }
    if (hi     != RB_HUBP_EXPECT_ADDR_HI) { return RB_HUBP_REFUSE_STATE_HI; }
    return RB_HUBP_OK;
}

/* May this MC address be written, given the HIGH dword the pipe holds now and
 * the size of one frame? `frame_bytes` of 0 skips the frame-fit check. */
static inline int rb_hubp_check_addr(uint64_t mc, uint32_t orig_hi, uint64_t frame_bytes)
{
    if (mc == 0)                                     { return RB_HUBP_REFUSE_ZERO; }
    if (mc <  RB_MC_CARVEOUT_BASE)                   { return RB_HUBP_REFUSE_RANGE; }
    if (mc >= RB_MC_CARVEOUT_BASE + RB_MC_CARVEOUT_SPAN) { return RB_HUBP_REFUSE_RANGE; }
    if (mc % RB_HUBP_ADDR_ALIGN)                     { return RB_HUBP_REFUSE_ALIGN; }
    if ((uint32_t)(mc >> 32) != orig_hi)             { return RB_HUBP_REFUSE_HI_MOVED; }
    if (frame_bytes &&
        mc + frame_bytes > RB_MC_CARVEOUT_BASE + RB_MC_CARVEOUT_SPAN) {
        return RB_HUBP_REFUSE_FRAME;
    }
    return RB_HUBP_OK;
}

#endif /* RB_HUBP_H */
