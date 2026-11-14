/* 3fb: derive the framebuffer's geometry from the chip instead of from this
 * board.
 *
 * WHY. Issue #4 (FranciumOS-org/Radeonized, 2026-11-13): on a Legion 7
 * 16ARHA7 with a 2560x1600 eDP panel and a 680M at revision 0xc8, an armed
 * v0.1.0 boot never reaches the desktop - WindowServer fails to start and
 * watchdogd panics with "userspace watchdog timeout" about 139 s in, while
 * `-rbgpuoff` boots to 2560x1600 on the stock framebuffer. The reporter
 * located it precisely: RBAMDFramebuffer.cpp carried
 *
 *     kWidth = 1920, kHeight = 1080, kBytesPerRow = 7680,
 *     kApertureBytes = 8294400
 *
 * as constants, and their crashed boot's `rbgpu-diag-prev` shows the
 * accelerator built its framebuffer at OUR panel's size on THEIR machine
 * (`ax0_w=0x780 ax0_h=0x438`). Those four numbers were measured on this board
 * - the aperture one was read live out of `IOFBMemorySize` - and then frozen,
 * which is rule 41 in its plainest form: a constant is only a constant on the
 * machine it was measured on.
 *
 * WHERE THE NUMBERS COME FROM. Three registers, all already modelled in this
 * tree, all read read-only, and all with this board's live values recorded:
 *
 *   OTG0_H_BLANK_START_END  0x1b2b  rb_otg.h:23   0x007f07ff -> 2047-127 =1920
 *   OTG0_V_BLANK_START_END  0x1b36  rb_otg.h:26   0x0014044c -> 1100- 20 =1080
 *   HUBP0_SURFACE_PITCH     0x077f  rb_hubp.h:338 1919 + 1          =1920 px
 *
 * The active-size derivation is not invented here. `rb_strenc.h:516` already
 * states `h_active = BLANK_START - BLANK_END`, inverted out of
 * `optc1_program_timing` and validated in boot 473 against CEA-861 1080p60
 * exactly (HTOTAL 2200, VTOTAL 1125, HSTART 192, VSTART 41, 1920x1080). The
 * field split is START in [12:0] and END in [28:16], both 13-bit, which is
 * `RB_OTG_POS_MASK` in rb_otg.h.
 *
 * THE PITCH IS READ, NOT ASSUMED, AND THAT IS THE POINT. `width * 4` happens
 * to be right on this board because its rows are not padded - the comment
 * above the old constants says so, having checked `IOFBMemorySize` against
 * 1920*1080*4 exactly. There is no reason that holds on another panel, and a
 * stride wrong by a padding allowance renders a sheared image rather than
 * failing, which is the worst failure mode available. HUBP0_SURFACE_PITCH
 * carries what the firmware actually programmed, in a pitch-minus-one pixel
 * encoding (`RB_HUBP_EXPECT_PITCH 0x077f` = 1920 px), so the stride comes
 * from the pipe that is scanning.
 *
 * WHAT THIS REFUSES TO DO. Every path out is a numbered code, never a bool
 * (rule 14), and there is no fallback to the old constants. Falling back
 * would put 1920x1080 on a 2560x1600 panel, which is precisely the reported
 * failure; a framebuffer that cannot learn its own geometry must decline the
 * panel and leave the firmware's working one alone. The caller enforces that.
 *
 * The cross-checks are what make the reading mean something, since a single
 * plausible number proves nothing:
 *   - a blank word whose START is not above its END is not a raster;
 *   - an active size at or above its own total is arithmetically impossible;
 *   - a pitch below the active width cannot be the pitch of that surface;
 *   - and the whole thing is bounded, so a zeroed register block refuses
 *     rather than deriving a 0x0 mode.
 */
#ifndef RB_FBMODE_H
#define RB_FBMODE_H

#include <stdint.h>

#define RB_FBMODE_OK            0u
#define RB_FBMODE_E_HBLANK      1u  /* H blank word: START <= END          */
#define RB_FBMODE_E_VBLANK      2u  /* V blank word: START <= END          */
#define RB_FBMODE_E_WIDTH       3u  /* width outside [640, 7680]           */
#define RB_FBMODE_E_HEIGHT      4u  /* height outside [480, 4320]          */
#define RB_FBMODE_E_HTOTAL      5u  /* width >= h_total, or h_total zero   */
#define RB_FBMODE_E_VTOTAL      6u  /* height >= v_total, or v_total zero  */
#define RB_FBMODE_E_PITCH_LOW   7u  /* pitch < width - not this surface    */
#define RB_FBMODE_E_PITCH_HIGH  8u  /* pitch absurd (> 2x width + 4096)    */
#define RB_FBMODE_E_APERTURE    9u  /* row * height over the 256 MB cap    */

/* Bounds. The low end is below any panel macOS supports and the high end is
 * 8K, so these reject a dead register block rather than ranking panels. */
#define RB_FBMODE_W_MIN       640u
#define RB_FBMODE_W_MAX      7680u
#define RB_FBMODE_H_MIN       480u
#define RB_FBMODE_H_MAX      4320u
#define RB_FBMODE_BPP           4u
#define RB_FBMODE_APER_CAP  (256u * 1024u * 1024u)

/* 13-bit position fields, START low and END high. rb_otg.h's RB_OTG_POS_MASK
 * is the same constant; it is restated rather than included so this header
 * stays usable from the hosttest on its own. */
#define RB_FBMODE_POS_MASK  0x1fffu
#define RB_FBMODE_END_SHIFT     16u

struct rb_fbmode_in {
    uint32_t h_total;             /* OTG0_H_TOTAL,            raw */
    uint32_t v_total;             /* OTG0_V_TOTAL,            raw */
    uint32_t h_blank_start_end;   /* OTG0_H_BLANK_START_END,  raw */
    uint32_t v_blank_start_end;   /* OTG0_V_BLANK_START_END,  raw */
    uint32_t surface_pitch;       /* HUBP0_SURFACE_PITCH,     raw (pitch-1) */
};

struct rb_fbmode_out {
    uint32_t width;
    uint32_t height;
    uint32_t pitch_px;            /* pixels, pitch-1 decoded */
    uint32_t bytes_per_row;
    uint32_t aperture_bytes;
    uint32_t padded;              /* 1 if pitch_px > width - rows ARE padded */
};

static inline uint32_t rb_fbmode_active(uint32_t blank_word, uint32_t *start,
                                        uint32_t *end)
{
    const uint32_t s = blank_word & RB_FBMODE_POS_MASK;
    const uint32_t e = (blank_word >> RB_FBMODE_END_SHIFT) & RB_FBMODE_POS_MASK;
    if (start) { *start = s; }
    if (end)   { *end   = e; }
    return (s > e) ? (s - e) : 0u;
}

/* Writes *out only on RB_FBMODE_OK, so a refused derivation cannot leave a
 * half-filled geometry behind for a caller that ignored the code. */
static inline uint32_t rb_fbmode_derive(const struct rb_fbmode_in *in,
                                        struct rb_fbmode_out *out)
{
    uint32_t hs = 0, he = 0, vs = 0, ve = 0;
    uint32_t w, h, pitch, row;
    uint64_t aper;

    if (!in || !out) { return RB_FBMODE_E_HBLANK; }

    w = rb_fbmode_active(in->h_blank_start_end, &hs, &he);
    if (w == 0u || hs <= he) { return RB_FBMODE_E_HBLANK; }
    h = rb_fbmode_active(in->v_blank_start_end, &vs, &ve);
    if (h == 0u || vs <= ve) { return RB_FBMODE_E_VBLANK; }

    if (w < RB_FBMODE_W_MIN || w > RB_FBMODE_W_MAX) { return RB_FBMODE_E_WIDTH; }
    if (h < RB_FBMODE_H_MIN || h > RB_FBMODE_H_MAX) { return RB_FBMODE_E_HEIGHT; }

    /* The TOTAL registers hold the real total, not total-minus-one. That is
     * measured, not assumed: boot 567 publishes `RBFBOtgHTotal` 2106 and
     * `RBFBOtgVTotal` 1129 from a raw read (RBAMDFramebufferVBL.cpp:697), and
     * rb_drr.h and drr_test.c both use 2106/1129 directly as the totals to
     * reproduce the refresh MEASURED on an OTG0_FRAME_COUNT edge. An earlier
     * draft of this file asserted total-minus-one and compensated with a +1,
     * and its hosttest fed it 2105/1128 so that it passed for the wrong
     * reason - rule 48's exact shape, caught by reading the live keys off the
     * running machine before installing. The active raster must sit strictly
     * inside the total. */
    if (in->h_total == 0u || w >= in->h_total) {
        return RB_FBMODE_E_HTOTAL;
    }
    if (in->v_total == 0u || h >= in->v_total) {
        return RB_FBMODE_E_VTOTAL;
    }

    pitch = (in->surface_pitch & 0xffffu) + 1u;
    if (pitch < w)                     { return RB_FBMODE_E_PITCH_LOW;  }
    if (pitch > (2u * w) + 4096u)      { return RB_FBMODE_E_PITCH_HIGH; }

    row  = pitch * RB_FBMODE_BPP;
    aper = (uint64_t)row * (uint64_t)h;
    if (aper == 0u || aper > (uint64_t)RB_FBMODE_APER_CAP) {
        return RB_FBMODE_E_APERTURE;
    }

    out->width          = w;
    out->height         = h;
    out->pitch_px       = pitch;
    out->bytes_per_row  = row;
    out->aperture_bytes = (uint32_t)aper;
    out->padded         = (pitch > w) ? 1u : 0u;
    return RB_FBMODE_OK;
}

#endif /* RB_FBMODE_H */
