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
    /* 2xj: DCSURF_FLIP_CONTROL / _CONTROL2. READ-ONLY, and the reason they
     * exist is the tearing the user reported once 2xi made the panel actually
     * flip. 2wh wrote the prediction down before any of this ran: it programs
     * the surface address and "deliberately omits" the
     * DCSURF_FLIP_CONTROL / SURFACE_UPDATE_LOCK dance, and said in the same
     * paragraph that "tearing is expected and acceptable". At boot 213's 0.16
     * flips/s a mid-scanout latch was one chance every six seconds; at 2xi's
     * 112 flips/s it is 112 chances a second. So the tear was always in the
     * design and the fix made it VISIBLE - the same shape as 2wi, where the
     * 0.09 Hz rate was always there and 2wh only revealed it.
     * These are NOT written and NOT gates. The field layout of this register
     * is in dcn_3_1_2_sh_mask.h, which this repo does NOT carry, so the bit
     * that selects an immediate flip over a vsync'd one is UNKNOWN here and
     * deriving it from memory into a display-engine write is exactly rule 17.
     * Read it first; decide afterwards. */
    RB_HUBP_REG_FLIP_CTL,   /* DCSURF_FLIP_CONTROL  - flip type / update lock */
    RB_HUBP_REG_FLIP_CTL2,  /* DCSURF_FLIP_CONTROL2 */
    RB_HUBP_REG_COUNT
};

static const uint32_t rb_hubp_regs[RB_HUBP_COUNT][RB_HUBP_REG_COUNT] = {
    /*            ADDR     ADDR_HI   PITCH    CONFIG   ADDR_CFG  TILING   FLIP_CTL FLIP_CTL2 */
    /* HUBP0 */ { 0x060a,  0x060b,   0x0607,  0x05e5,  0x05e6,   0x05e7,  0x061b,  0x061c },
    /* HUBP1 */ { 0x06e6,  0x06e7,   0x06e3,  0x06c1,  0x06c2,   0x06c3,  0x06f7,  0x06f8 },
    /* HUBP2 */ { 0x07c2,  0x07c3,   0x07bf,  0x079d,  0x079e,   0x079f,  0x07d3,  0x07d4 },
    /* HUBP3 */ { 0x089e,  0x089f,   0x089b,  0x0879,  0x087a,   0x087b,  0x08af,  0x08b0 },
};

/* 2os, boot 133, this machine. These are not defaults to fall back on - they
 * are the three values that have to still be true for us to believe we are
 * addressing HUBP0 of a firmware-lit ARGB8888 1920-pitch pipe. If any has
 * moved, we are either misaddressed or the pipe is not what was measured, and
 * the right answer is to refuse rather than to write. */
/* 2xk: DCSURF_FLIP_CONTROL field masks, taken from
 * drivers/gpu/drm/amd/include/asic_reg/dcn/dcn_3_1_2_sh_mask.h at
 * torvalds/linux 4dd16e31135ea8437a720222df34e8745f385d6d. That header is
 * 6.5 MB and is NOT copied into docs/linux-ref (the same decision UPSTREAM.md
 * records for the offset headers); the fields used are listed here with their
 * source, which is the convention that file states.
 *
 * Boot 216 read the live register as 0x04100000 and it decodes with NO
 * leftover bits - SURFACE_FLIP_PENDING_DELAY = 65 and every other field zero -
 * which is the control that this map is the right one.
 *
 * SURFACE_FLIP_TYPE is the field 2xj went looking for, and it reads **0**.
 * Linux's own hubp1_program_surface_flip_and_addr writes
 * `REG_UPDATE(DCSURF_FLIP_CONTROL, SURFACE_FLIP_TYPE, flip_immediate)`
 * (dcn10_hubp.c:361), so 1 is immediate and 0 is vsync'd: the firmware
 * already latches at VUPDATE and **the tearing is not the flip type**. That
 * candidate is eliminated by a read, with nothing written.
 *
 * SURFACE_FLIP_PENDING is the hardware's "armed but not yet latched" status,
 * and with SURFACE_TRIPLE_BUFFER_ENABLE = 0 (read live, FLIP_CONTROL2 = 0x40
 * is entirely PENDING_MIN_TIME = 64) there is exactly ONE pending slot. We
 * have never read it before programming the next address. */
#define RB_HUBP_FLIP_UPDATE_LOCK   0x00000001u
#define RB_HUBP_FLIP_TYPE          0x00000002u   /* 1 = immediate, 0 = vsync'd */
#define RB_HUBP_FLIP_PENDING       0x00000100u
#define RB_HUBP_FLIP_PENDING_DELAY 0x3ff00000u

/* True iff DCN still has a flip armed that has not latched. A pure predicate
 * so the hosttest decides it before the kext does, exactly as every other
 * HUBP decision in this file. */
static inline int rb_hubp_flip_pending(uint32_t flip_ctl)
{
    return (flip_ctl & RB_HUBP_FLIP_PENDING) != 0u;
}

/* DCSURF_FLIP_CONTROL2, from dcn_3_1_2_sh_mask.h (see UPSTREAM.md, 2xk). */
#define RB_HUBP_FLIP2_PENDING_MIN_TIME 0x000000ffu
#define RB_HUBP_FLIP2_GSL_ENABLE       0x00000100u
#define RB_HUBP_FLIP2_TRIPLE_BUFFER    0x00000400u

/* 2xm: would programming the primary surface address RIGHT NOW throw away a
 * flip the hardware has already armed and not yet latched?
 *
 * 2xk measured SURFACE_FLIP_TYPE == 0, so DCN latches our address pair at
 * VUPDATE rather than immediately - which means there is a window between the
 * write and the latch during which the register holds an address the panel is
 * not yet reading. Writing again inside that window replaces the armed
 * address, so the frame we armed first never reaches the screen and the one
 * that does reach it changes partway down: the tear 2wh predicted in writing.
 *
 * The triple-buffer guard is the reason this is a function and not a mask
 * test. SURFACE_FLIP_PENDING describes ONE pending slot; with
 * SURFACE_TRIPLE_BUFFER_ENABLE set there is more than one, so a pending flip
 * no longer implies that a second write discards anything, and deferring
 * would drop frames for nothing. Boot 216 and boot 217 both read
 * FLIP_CONTROL2 == 0x40, i.e. PENDING_MIN_TIME = 64 with triple buffering
 * OFF, so the guard is inert on this machine today - it exists so that a
 * machine (or a future compositor) where it is on does not inherit a
 * conclusion measured where it was off.
 *
 * Returns 1 to defer, 0 to write.
 */
static inline int rb_hubp_flip_would_discard(uint32_t flip_ctl,
                                             uint32_t flip_ctl2)
{
    if ((flip_ctl2 & RB_HUBP_FLIP2_TRIPLE_BUFFER) != 0u) {
        return 0;               /* more than one slot: nothing is discarded */
    }
    return rb_hubp_flip_pending(flip_ctl);
}

/* 2xn: should getAttribute('amtr') answer kIOReturnNotReady right now?
 *
 * Apple's own AMDFramebuffer::getAttribute('amtr') (0x2a6faa, disassembled in
 * 2wc and recorded in the journal) is:
 *
 *     if (!(ctrl->0x31332 >> 9 & 1)) return kIOReturnUnsupported;
 *     if (getOutstandingDisplayPipeTransactions()) return kIOReturnNotReady;
 *     return kIOReturnSuccess;
 *
 * Ours has answered kIOReturnSuccess UNCONDITIONALLY since 2wc, so the
 * compositor is told every transaction completed the instant it was handed
 * over - which frees it to draw into a surface that is still the live
 * scanout. That corrupts the WHOLE frame, which is the shape the user
 * reports, where a flip programmed over an unlatched one (2xm) can only
 * corrupt the part of the frame below the scanline.
 * Our stand-in for getOutstandingDisplayPipeTransactions() is DCN's own
 * SURFACE_FLIP_PENDING: our flip is outstanding exactly while the hardware
 * has it armed and unlatched. The triple-buffer guard of
 * rb_hubp_flip_would_discard() does NOT apply here - with more than one slot
 * a pending flip still means OUR address has not been latched - so this is
 * the bare pending bit by design, not by omission.
 *
 * THE DEADLINE IS THE SAFETY PROPERTY, not a refinement. 2x6 recorded that
 * isTransactionComplete (0xbe4a794) decides the current transaction by
 * `getAttribute('amtr', NULL) == 0`, so answering NotReady makes Apple's
 * accelerator WAIT. Answering 0 always, as we do today, is the optimistic
 * answer and cannot stall anything; this one can. If the pending bit never
 * clears - a mode set, a disabled pipe, a register we have misread - an
 * unbounded hold would wedge the display for good. So the hold is bounded and
 * the caller passes the deadline: past it we answer ready whatever the
 * hardware says, and the kext counts how often that happened.
 *
 * Returns 1 to answer kIOReturnNotReady, 0 to answer success.
 */
static inline int rb_hubp_hold_notready(uint32_t flip_ctl, uint64_t now,
                                        uint64_t deadline)
{
    if (!rb_hubp_flip_pending(flip_ctl)) {
        return 0;               /* latched already: nothing is outstanding */
    }
    return now < deadline ? 1 : 0;
}

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

/* 2wm: WHICH transactions get a write.
 *
 * Bit 0x1 of the transaction's flag word is "the plane resource changed",
 * and 2wf read AMDFramebuffer's own consumer making exactly this test before
 * it dereferences txn->0x10. So 2wh gated the write on it: without it, a
 * transaction that only changed the colour matrix would re-write a stale
 * address from a stale pointer.
 *
 * But boot 196 showed the panel updating smoothly while the user TYPED and
 * not otherwise, and adding characters counting while deleting them did not.
 * Adding a character grows a text layer and so forces a NEW surface; deleting
 * reuses the one already allocated. If a content update into an unchanged
 * surface arrives as a transaction with bit 0x1 clear, we are being handed
 * frames and refusing to draw them, and that asymmetry is exactly what it
 * would look like.
 *
 * `all` is -rbgpufbflipall and drops the gate. It is NOT obviously safe to
 * leave on: writing on a bit-clear transaction means trusting txn->0x10 to
 * still point at a valid _AMD_PIPE_FLIP, which is the thing bit 0x1 is the
 * consumer's own evidence for. The caller still range-checks the pointer and
 * every address still passes rb_hubp_check_addr, so the failure mode is a
 * refused write, not a wild one. It is an experiment with its own boot-arg,
 * and `RBFBAmtrNoFlip` says in the same boot whether it had anything to do. */
#define RB_HUBP_FLAG_PLANE 0x1ull

static inline int rb_hubp_should_write(uint64_t flags, int all)
{
    if (all) { return 1; }
    return (flags & RB_HUBP_FLAG_PLANE) ? 1 : 0;
}

#endif /* RB_HUBP_H */
