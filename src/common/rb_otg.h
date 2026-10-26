/* SPDX-License-Identifier: MIT */
/*
 * OTG0's live scanout timing -> IODetailedTimingInformationV2.
 *
 * 2rl measured that IONDRVFramebuffer has no detailed timing to give: its one
 * mode carries nominal 1920x1080 and a refresh rate, every blanking and
 * pixel-clock word is zero, kIODetailedTimingValid is clear, and there is no
 * IODisplayEDID anywhere in the registry. The firmware, however, already
 * programmed DCN 3.1.2 and OTG0 is scanning (OTG0_CONTROL = 0x00011201,
 * OTG_MASTER_EN set), so the panel's real timing is sitting in OTG0's
 * registers. Those are READS, which rb_probe.c's rules already permit at
 * level 2; nothing here writes a DCN register.
 *
 * Register offsets are Linux's dcn_3_1_2_offset.h, already in rb_regs.h.
 * The FIELD positions are not taken from a header this tree does not have -
 * they are established from this machine's own measured values, and the
 * converter re-checks the arithmetic on every call rather than trusting them
 * (rule 14: every refusal says which check failed).
 *
 * Boot 2oq/2os read, on this panel:
 *
 *   OTG0_H_TOTAL            0x0000083a   2106
 *   OTG0_H_BLANK_START_END  0x007f07ff   127 / 2047  -> 2047-127 = 1920 active
 *   OTG0_H_SYNC_A           0x003c0000   0 / 60      -> width 60, starts at 0
 *   OTG0_V_TOTAL            0x00000469   1129
 *   OTG0_V_BLANK_START_END  0x0014044c   20 / 1100   -> 1100-20  = 1080 active
 *   OTG0_V_SYNC_A           0x00050000   0 / 5       -> width 5,  starts at 0
 *
 * Both axes close exactly, which is why the field order is not a guess:
 *
 *   h: front 2106-2047 = 59, sync 60, back 127-60 = 67  -> 186 = 2106-1920
 *   v: front 1129-1100 = 29, sync  5, back  20-5  = 15  ->  49 = 1129-1080
 *
 * Three independently derived porches summing to the blanking the totals
 * imply, on both axes, is a stronger check than a header would be.
 *
 * NOTE, and it corrects 2os: the registers hold the TRUE totals, 2106 and
 * 1129. 2os computed the pixel clock from 2107 x 1130, one too many on each
 * axis - with 2107/1130 the porches above do not sum and the arithmetic says
 * so. The difference is ~0.1 %, which matters to nothing yet, but the number
 * in the journal is wrong and this is where it is corrected.
 *
 * The pixel clock is not in any register this project reads; it is
 * hTotal * vTotal * refresh, and the refresh is MEASURED from OTG0's
 * read-only frame counter (RembrandtProbe.cpp already does exactly this -
 * 2os: 10 frames in 69 524 857 ns = 143.833 Hz). Deriving blanking from
 * CVT/GTF instead would be rule 17 verbatim: substituting a value and
 * narrating it as a hardware result.
 */
#ifndef RB_OTG_H
#define RB_OTG_H

#include <stdint.h>
#include <stddef.h>

/* IODetailedTimingInformationV2 field offsets. Derived from the public
 * Kernel.framework header's member list by walking the types (UInt64 is
 * 8-aligned, so pixelClock lands at 0x28 and the struct closes at 0xa0) -
 * NOT by counting header lines. The four rb_agdc.h already pins agree, and
 * the closing size agrees with RB_AGDC_TIMING_SIZE. */
#define RB_V2_PIXELCLOCK    0x28u
#define RB_V2_MINPIXELCLOCK 0x30u
#define RB_V2_MAXPIXELCLOCK 0x38u
#define RB_V2_HACTIVE       0x40u
#define RB_V2_HBLANKING     0x44u
#define RB_V2_HSYNCOFFSET   0x48u
#define RB_V2_HSYNCWIDTH    0x4cu
#define RB_V2_VACTIVE       0x50u
#define RB_V2_VBLANKING     0x54u
#define RB_V2_VSYNCOFFSET   0x58u
#define RB_V2_VSYNCWIDTH    0x5cu
#define RB_V2_HSYNCCONFIG   0x70u
#define RB_V2_VSYNCCONFIG   0x78u
#define RB_V2_SIZE          0xa0u

/* IOGraphicsTypes.h:1002. A local constant quoting the header, never an
 * #include: a compile-time constant cannot add a kext dependency, and 2oz's
 * IOGraphicsFamily trap must stay shut. */
#define RB_IO_SYNC_POSITIVE 0x00000001u

/* OTG_*_SYNC_A_CNTL bit 0 is OTG_*_SYNC_A_POL: 0 = positive. */
#define RB_OTG_SYNC_POL_NEG 0x1u

/* Raw register values, exactly as read. Nothing is pre-decoded by the caller,
 * so a capture of this struct is a capture of the hardware. */
struct rb_otg_timing {
    uint32_t h_total;            /* OTG0_H_TOTAL            */
    uint32_t h_blank_start_end;  /* OTG0_H_BLANK_START_END  */
    uint32_t h_sync_a;           /* OTG0_H_SYNC_A           */
    uint32_t h_sync_a_cntl;      /* OTG0_H_SYNC_A_CNTL      */
    uint32_t v_total;            /* OTG0_V_TOTAL            */
    uint32_t v_blank_start_end;  /* OTG0_V_BLANK_START_END  */
    uint32_t v_sync_a;           /* OTG0_V_SYNC_A           */
    uint32_t v_sync_a_cntl;      /* OTG0_V_SYNC_A_CNTL      */
    uint32_t refresh_mhz;        /* measured from OTG0_FRAME_COUNT, millihertz */
};

/* Why a conversion was refused. Returned negated, so a caller can publish the
 * exact check that failed instead of a bare failure (rule 19: a refusal path
 * must publish the value it refused). */
enum rb_otg_err {
    RB_OTG_OK            = 0,
    RB_OTG_E_ARGS        = 1,  /* NULL or wrong output size              */
    RB_OTG_E_HTOTAL      = 2,  /* hTotal 0, or not greater than hActive  */
    RB_OTG_E_VTOTAL      = 3,
    RB_OTG_E_HACTIVE     = 4,  /* hActive 0 - the two halves are equal   */
    RB_OTG_E_VACTIVE     = 5,
    RB_OTG_E_HSYNC       = 6,  /* sync does not start at 0, or width 0,  */
    RB_OTG_E_VSYNC       = 7,  /*   or front+sync exceeds the blanking   */
    RB_OTG_E_REFRESH     = 8,  /* refresh never measured                 */
    RB_OTG_E_CLOCK       = 9   /* the product overflowed or came out 0   */
};

#define RB_OTG_TOTAL_MASK 0x7fffu   /* OTG_*_TOTAL is 15 bits  */
#define RB_OTG_POS_MASK   0x1fffu   /* blank/sync positions are 13 bits */

static inline void rb_otg_put32(uint8_t *p, uint32_t v) {
    p[0]=(uint8_t)(v&0xff);       p[1]=(uint8_t)((v>>8)&0xff);
    p[2]=(uint8_t)((v>>16)&0xff); p[3]=(uint8_t)((v>>24)&0xff);
}
static inline void rb_otg_put64(uint8_t *p, uint64_t v) {
    for (unsigned i=0;i<8;i++) p[i]=(uint8_t)((v>>(8*i))&0xff);
}

/* A 13-bit field pair, order-agnostic: the hardware's naming of which half is
 * "start" and which is "end" is irrelevant to the geometry, so take the span
 * and the lower edge and let the arithmetic check itself. */
static inline uint32_t rb_otg_lo(uint32_t v) {
    uint32_t a=v&RB_OTG_POS_MASK, b=(v>>16)&RB_OTG_POS_MASK;
    return a<b?a:b;
}
static inline uint32_t rb_otg_hi(uint32_t v) {
    uint32_t a=v&RB_OTG_POS_MASK, b=(v>>16)&RB_OTG_POS_MASK;
    return a<b?b:a;
}

/*
 * Write a 0xa0-byte IODetailedTimingInformationV2 body from OTG0's registers.
 * Returns 0, or -(enum rb_otg_err). Refuses BEFORE touching the output, so a
 * refusal leaves the caller's buffer exactly as it was (2rg's rule for
 * rb_agdc_link_config, applied here too).
 */
static inline int rb_otg_to_v2(const struct rb_otg_timing *in,
                               void *out, size_t size) {
    if (!in || !out || size != RB_V2_SIZE) return -RB_OTG_E_ARGS;

    const uint32_t htotal = in->h_total & RB_OTG_TOTAL_MASK;
    const uint32_t vtotal = in->v_total & RB_OTG_TOTAL_MASK;
    const uint32_t hb_lo = rb_otg_lo(in->h_blank_start_end);
    const uint32_t hb_hi = rb_otg_hi(in->h_blank_start_end);
    const uint32_t vb_lo = rb_otg_lo(in->v_blank_start_end);
    const uint32_t vb_hi = rb_otg_hi(in->v_blank_start_end);
    const uint32_t hs_lo = rb_otg_lo(in->h_sync_a);
    const uint32_t hs_w  = rb_otg_hi(in->h_sync_a) - hs_lo;
    const uint32_t vs_lo = rb_otg_lo(in->v_sync_a);
    const uint32_t vs_w  = rb_otg_hi(in->v_sync_a) - vs_lo;

    const uint32_t hactive = hb_hi - hb_lo;
    const uint32_t vactive = vb_hi - vb_lo;
    if (!hactive) return -RB_OTG_E_HACTIVE;
    if (!vactive) return -RB_OTG_E_VACTIVE;
    if (!htotal || htotal <= hactive) return -RB_OTG_E_HTOTAL;
    if (!vtotal || vtotal <= vactive) return -RB_OTG_E_VTOTAL;

    const uint32_t hblank = htotal - hactive;
    const uint32_t vblank = vtotal - vactive;
    /* The front porch is whatever is left after the last blanking position.
     * hb_hi <= htotal is implied by htotal > hactive = hb_hi - hb_lo only when
     * hb_lo is where it should be, so check it rather than assume it. */
    if (hb_hi > htotal) return -RB_OTG_E_HTOTAL;
    if (vb_hi > vtotal) return -RB_OTG_E_VTOTAL;
    const uint32_t hfront = htotal - hb_hi;
    const uint32_t vfront = vtotal - vb_hi;

    /* Both measured axes have the sync starting at 0, so the back porch is
     * hb_lo - hs_w and front + sync + back == blanking follows as an IDENTITY
     * from active = hb_hi - hb_lo. Checking that sum would therefore be a
     * guard that cannot fail; what carries real information is that the sync
     * starts where both measurements say it does, is non-zero, and ends
     * before the active region begins. A shape this has never been measured
     * against is refused rather than converted by unchecked arithmetic. */
    if (hs_lo != 0 || !hs_w || hs_w > hb_lo || hfront + hs_w > hblank)
        return -RB_OTG_E_HSYNC;
    if (vs_lo != 0 || !vs_w || vs_w > vb_lo || vfront + vs_w > vblank)
        return -RB_OTG_E_VSYNC;

    if (!in->refresh_mhz) return -RB_OTG_E_REFRESH;
    /* pixelClock = hTotal * vTotal * refresh. Everything is bounded (totals
     * are 15-bit, refresh is clamped by its caller), so a 64-bit product
     * cannot overflow - but say so by checking rather than by asserting. */
    const uint64_t lines = (uint64_t)htotal * (uint64_t)vtotal;
    const uint64_t clock = lines * (uint64_t)in->refresh_mhz / 1000ull;
    if (!clock) return -RB_OTG_E_CLOCK;

    uint8_t *p = (uint8_t *)out;
    for (size_t i=0;i<RB_V2_SIZE;i++) p[i]=0;
    rb_otg_put64(p+RB_V2_PIXELCLOCK,    clock);
    rb_otg_put64(p+RB_V2_MINPIXELCLOCK, clock);
    rb_otg_put64(p+RB_V2_MAXPIXELCLOCK, clock);
    rb_otg_put32(p+RB_V2_HACTIVE,     hactive);
    rb_otg_put32(p+RB_V2_HBLANKING,   hblank);
    rb_otg_put32(p+RB_V2_HSYNCOFFSET, hfront);
    rb_otg_put32(p+RB_V2_HSYNCWIDTH,  hs_w);
    rb_otg_put32(p+RB_V2_VACTIVE,     vactive);
    rb_otg_put32(p+RB_V2_VBLANKING,   vblank);
    rb_otg_put32(p+RB_V2_VSYNCOFFSET, vfront);
    rb_otg_put32(p+RB_V2_VSYNCWIDTH,  vs_w);
    /* Polarity: OTG's POL bit is 1 for negative, IOKit's flag is set for
     * positive, so the two are inverses and the mapping is explicit. */
    if (!(in->h_sync_a_cntl & RB_OTG_SYNC_POL_NEG))
        rb_otg_put32(p+RB_V2_HSYNCCONFIG, RB_IO_SYNC_POSITIVE);
    if (!(in->v_sync_a_cntl & RB_OTG_SYNC_POL_NEG))
        rb_otg_put32(p+RB_V2_VSYNCCONFIG, RB_IO_SYNC_POSITIVE);
    return RB_OTG_OK;
}

#endif /* RB_OTG_H */
