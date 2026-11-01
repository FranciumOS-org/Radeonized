/* rb_dmub.h - the DMCUB inbox-1 ring and the ABM commands that ride on it.
 *
 * 2zl established WHY this file exists: on DCN 3.1 the CPU does not write
 * BL_PWM_CNTL to change brightness. dcn31_panel_cntl.c has no
 * .set_backlight_level at all, dcn21_set_backlight_level takes the
 * register-writing dce110 path only `if (dc->dc->res_pool->dmcu)` - the old
 * DMCU this chip has not got - and otherwise ends at
 * dmub_abm_set_backlight(), which is a command pushed onto DMCUB's inbox 1.
 * Boot 271 then measured that ring FREE: WptrMoves 0 and RptrMoves 0 across
 * 125 samples with WPTR == RPTR == 2368 and InboxSize 8192, and 2368 is
 * 37 * 64 - thirty-seven dmub_rb_cmds at bring-up and then silence.
 *
 * Everything in here is a DECISION, which is why it is in src/common and not
 * in the kext: where the ring is, whether the registers that say so are
 * self-consistent, whether a ring slot is free, and the exact bytes of a
 * command. The kext does the two things only the kext can do - map the
 * memory and store to it. Rule 38: the hosttest owns every judgement this
 * file makes, so no number below is first exercised on hardware.
 *
 * Offsets and layouts: docs/linux-ref/dmub_cmd.h and dmub_srv.c at
 * cbbddb6b08cc9900a87cccaa9b22bbe06aa4f50a (see docs/linux-ref/UPSTREAM.md),
 * dcn_3_1_2_offset.h for the registers. NOTHING here is a bitfield: the
 * header is assembled as a u32 from measured bit positions, because a
 * compiler's bitfield layout is a belief and this struct crosses to firmware.
 */

#ifndef RB_DMUB_H
#define RB_DMUB_H

#include <stdint.h>

/* dmub_cmd.h:1867-1877. One command is 64 bytes; the inbox-1 ring is
 * DMUB_RB_CMD_SIZE * DMUB_RB_MAX_ENTRY = 8192, which is exactly the
 * DMCUB_INBOX1_SIZE boot 271 read. */
#define RB_DMUB_CMD_SIZE   64u
#define RB_DMUB_MAX_ENTRY  128u
#define RB_DMUB_RB_SIZE    (RB_DMUB_CMD_SIZE * RB_DMUB_MAX_ENTRY)

/* dmub_srv.c:881 - cw4.region.base = DMUB_CW4_BASE, and :891-892
 * inbox1.base = cw4.region.base, inbox1.top = base + DMUB_RB_SIZE. So the
 * value DMCUB_INBOX1_BASE_ADDRESS reads is a DMCUB-side window address, NOT
 * a system address, and boot 271 read exactly this. */
#define RB_DMUB_CW4_BASE   0x80000000u

/* dcn_3_1_2_offset.h, all base_idx 2 - the same segment and the same BAR5
 * mapping the BL_PWM block already uses. */
/* The four 2zl already reads from RBAMDFramebufferBacklight.cpp's own copies
 * of these names. Repeated here because this file is where the ring's
 * geometry is decided and a header that cannot name the pointers cannot
 * check them; the two sets are cross-checked by nothing, which is why the
 * values are quoted with their line numbers rather than copied. */
#define RB_DMUB_DW_INBOX1_BASE       0x01d4u
#define RB_DMUB_DW_INBOX1_SIZE       0x01d5u
#define RB_DMUB_DW_INBOX1_WPTR       0x01d6u
#define RB_DMUB_DW_INBOX1_RPTR       0x01d7u
#define RB_DMUB_DW_CW4_BASE_ADDRESS  0x01a9u
#define RB_DMUB_DW_CW4_TOP_ADDRESS   0x01b1u
#define RB_DMUB_DW_CW4_OFFSET        0x01bdu
#define RB_DMUB_DW_CW4_OFFSET_HIGH   0x01beu
#define RB_DMUB_DW_FB_LOCATION_BASE  0x0475u
#define RB_DMUB_DW_FB_OFFSET         0x0477u

/* dmub_cmd.h:1905-1929 and :5463-5471. */
#define RB_DMUB_CMD_ABM              66u
#define RB_DMUB_CMD_ABM_SET_PIPE     1u
#define RB_DMUB_CMD_ABM_SET_BACKLIGHT 2u

/* dmub_cmd.h:216 - the "multi eDP support" version, the only one
 * dmub_abm_lcd.c ever sends. */
#define RB_DMUB_ABM_CONTROL_VERSION_1 0x1u

/* dmub_cmd.h:5767-5780. 0 is the PWM control this panel's BL_PWM block is
 * wired for; 1 and 2 are the AUX methods that 2zl showed are reachable only
 * from edp_set_backlight_level_nits. */
#define RB_DMUB_BL_CONTROL_PWM  0u

/* dmub_cmd.h:63-74. */
#define RB_DMUB_SET_ABM_PIPE_NORMAL             1u
#define RB_DMUB_SET_ABM_PIPE_IMMEDIATELY_DISABLE 255u

/* u16.16, the units dmub_abm_set_backlight takes (backlight_pwm_u16_16) and
 * the same units rb_backlight.h's levels are already in. */
#define RB_DMUB_LEVEL_FULL  0x10000u

/* Why a locate was refused. Published, never inferred: "we did not write"
 * and "we did not write BECAUSE the window registers disagreed" are
 * different findings (rule 14). */
enum {
    RB_DMUB_LOC_OK = 0,
    RB_DMUB_LOC_NO_MAP = 1,        /* no BAR5 registers to read at all */
    RB_DMUB_LOC_INBOX_BASE = 2,    /* INBOX1_BASE_ADDRESS is not CW4's base */
    RB_DMUB_LOC_INBOX_SIZE = 3,    /* INBOX1_SIZE is not DMUB_RB_SIZE */
    RB_DMUB_LOC_CW4_WINDOW = 4,    /* CW4 base/top cannot contain the ring */
    RB_DMUB_LOC_NO_OFFSET = 5,     /* CW4_OFFSET pair is zero - no mailbox */
    RB_DMUB_LOC_FB = 6,            /* the ring is not inside the carve-out */
    RB_DMUB_LOC_ALIGN = 7,         /* the ring is not 64-byte aligned */
    RB_DMUB_LOC_MAP_FAILED = 8     /* the kext could not map it */
};

/* The whole register-derived picture, so the decision below is a pure
 * function of what was read and the hosttest can hand it any boot's values. */
struct rb_dmub_loc {
    uint32_t inbox_base;    /* DMCUB_INBOX1_BASE_ADDRESS */
    uint32_t inbox_size;    /* DMCUB_INBOX1_SIZE */
    uint32_t cw4_base;      /* DMCUB_REGION3_CW4_BASE_ADDRESS */
    uint32_t cw4_top;       /* DMCUB_REGION3_CW4_TOP_ADDRESS */
    uint32_t cw4_off_lo;    /* DMCUB_REGION3_CW4_OFFSET */
    uint32_t cw4_off_hi;    /* DMCUB_REGION3_CW4_OFFSET_HIGH */
    uint32_t fb_location;   /* DCN_VM_FB_LOCATION_BASE, FB_BASE field */
    uint32_t fb_offset;     /* DCN_VM_FB_OFFSET, FB_OFFSET field */
};

/* dmub_dcn31.c:69-72 - both are the register field shifted left by 24. */
static inline uint64_t rb_dmub_fb_base(const struct rb_dmub_loc *l)
{
    return (uint64_t)l->fb_location << 24;
}

static inline uint64_t rb_dmub_fb_offset(const struct rb_dmub_loc *l)
{
    return (uint64_t)l->fb_offset << 24;
}

/* The system-physical address of the ring.
 *
 * dmub_dcn31.c:80 writes CW4_OFFSET as `gpu_addr - fb_base + fb_offset`, and
 * this project's own mc2pa() is `mc - vram_start + vram_base_offset` with
 * vram_start the MC base (0xF4_0000_0000) and vram_base_offset the
 * system-physical base (0x7_8000_0000, FB_OFFSET 0x780). They are the SAME
 * transform, so CW4_OFFSET already holds a system-physical address and there
 * is nothing to invert. That is a claim with a consequence, so it is checked
 * rather than assumed: rb_dmub_locate() requires the derived address to lie
 * inside [fb_offset, fb_offset + 16 GB), which is false for a DMCUB-side or
 * an MC-side value. */
static inline uint64_t rb_dmub_ring_pa(const struct rb_dmub_loc *l)
{
    return ((uint64_t)l->cw4_off_hi << 32) | (uint64_t)l->cw4_off_lo;
}

/* 16 GB: larger than any carve-out this machine can have (4 GB measured) and
 * small enough that an MC address (0xF4_..) or a DMCUB window address
 * (0x8000_0000, below fb_offset) falls outside. */
#define RB_DMUB_FB_SPAN  (16ull * 1024ull * 1024ull * 1024ull)

static inline int rb_dmub_locate(const struct rb_dmub_loc *l)
{
    uint64_t pa, fb;

    /* dmub_srv.c:891 - the ring IS cw4's base, not somewhere inside it. A
     * mismatch means this firmware laid the mailbox out differently and
     * every offset below would be wrong. */
    if (l->inbox_base != l->cw4_base) { return RB_DMUB_LOC_INBOX_BASE; }
    if (l->inbox_base != RB_DMUB_CW4_BASE) { return RB_DMUB_LOC_INBOX_BASE; }
    if (l->inbox_size != RB_DMUB_RB_SIZE) { return RB_DMUB_LOC_INBOX_SIZE; }

    /* dmub_srv.c:882 - cw4.top = base + mail_fb->size, and the mailbox is
     * 2 * DMUB_RB_SIZE, so the inbox has to fit with room to spare. */
    if (l->cw4_top <= l->cw4_base) { return RB_DMUB_LOC_CW4_WINDOW; }
    if ((l->cw4_top - l->cw4_base) < RB_DMUB_RB_SIZE) {
        return RB_DMUB_LOC_CW4_WINDOW;
    }

    if (l->cw4_off_lo == 0u && l->cw4_off_hi == 0u) {
        return RB_DMUB_LOC_NO_OFFSET;
    }

    pa = rb_dmub_ring_pa(l);
    fb = rb_dmub_fb_offset(l);
    if (fb == 0ull) { return RB_DMUB_LOC_FB; }
    if (pa < fb || pa >= fb + RB_DMUB_FB_SPAN) { return RB_DMUB_LOC_FB; }
    if (pa + RB_DMUB_RB_SIZE > fb + RB_DMUB_FB_SPAN) { return RB_DMUB_LOC_FB; }

    /* Every command is 64 bytes and the firmware's own WPTR is a multiple of
     * 64, so a ring that is not 64-byte aligned is not this ring. */
    if ((pa & (uint64_t)(RB_DMUB_CMD_SIZE - 1u)) != 0ull) {
        return RB_DMUB_LOC_ALIGN;
    }

    return RB_DMUB_LOC_OK;
}

/* The offset of the ring inside the carve-out, which is also its offset
 * inside BAR0 - 2pr measured the GOP framebuffer at BAR0 offset 0, i.e. BAR0
 * is the carve-out seen through the aperture. Only meaningful once
 * rb_dmub_locate() has returned OK. */
static inline uint64_t rb_dmub_ring_fb_off(const struct rb_dmub_loc *l)
{
    return rb_dmub_ring_pa(l) - rb_dmub_fb_offset(l);
}

/* dmub_cmd.h:8155-8165, with the capacity passed in rather than assumed:
 * one entry is always left unusable so full and empty stay distinguishable. */
static inline int rb_dmub_rb_full(uint32_t wptr, uint32_t rptr, uint32_t cap)
{
    uint32_t used;

    if (cap == 0u) { return 1; }
    used = (wptr >= rptr) ? (wptr - rptr) : (cap - (rptr - wptr));
    return used == (cap - RB_DMUB_CMD_SIZE);
}

/* dmub_cmd.h:8190-8194. */
static inline uint32_t rb_dmub_next_wptr(uint32_t wptr, uint32_t cap)
{
    uint32_t n = wptr + RB_DMUB_CMD_SIZE;
    if (cap == 0u) { return wptr; }
    if (n >= cap) { n %= cap; }
    return n;
}

/* Is a pointer a legal ring position at all? The firmware's own WPTR is the
 * only evidence we will have that the ring is where we think it is, so a
 * value that cannot be a position is a refusal and not a warning. */
static inline int rb_dmub_ptr_ok(uint32_t p, uint32_t cap)
{
    if (cap == 0u) { return 0; }
    if (p >= cap) { return 0; }
    return (p & (RB_DMUB_CMD_SIZE - 1u)) == 0u;
}

/* The header word, assembled from dmub_cmd.h:2125-2134's bit positions:
 * type:8 sub_type:8 ret_status:1 multi_cmd_pending:1 is_reg_based:1
 * reserved0:5 payload_bytes:6 reserved1:2. */
static inline uint32_t rb_dmub_hdr(uint32_t type, uint32_t sub_type,
                                   uint32_t payload_bytes)
{
    return (type & 0xffu) |
           ((sub_type & 0xffu) << 8) |
           ((payload_bytes & 0x3fu) << 24);
}

static inline uint32_t rb_dmub_hdr_type(uint32_t h)    { return h & 0xffu; }
static inline uint32_t rb_dmub_hdr_sub(uint32_t h)     { return (h >> 8) & 0xffu; }
static inline uint32_t rb_dmub_hdr_payload(uint32_t h) { return (h >> 24) & 0x3fu; }

/* Could this word be a dmub_cmd_header a producer wrote?
 *
 * This is the CONTROL for the whole address derivation (rule 26: a search is
 * an instrument). If the ring is where rb_dmub_locate() says, then reading
 * the 37 slots between RPTR and WPTR must yield 37 plausible headers at a
 * 64-byte stride; random memory does not do that, and neither does a wrong
 * address. The test is deliberately weak in the only direction that matters:
 * it must not reject a real firmware command whose type this file has never
 * heard of, so it checks the two fields whose RANGE is fixed by the ABI -
 * payload_bytes <= 60 because the payload excludes the 4-byte header, and a
 * non-zero type, DMUB_CMD__NULL being 0. The reserved bits are NOT checked:
 * a future firmware may use them and this would then reject a real command. */
static inline int rb_dmub_hdr_plausible(uint32_t h)
{
    if (rb_dmub_hdr_type(h) == 0u) { return 0; }
    if (rb_dmub_hdr_payload(h) > (RB_DMUB_CMD_SIZE - 4u)) { return 0; }
    return 1;
}

/* A 64-byte command, built little-endian byte by byte so no struct layout is
 * trusted across the boundary to firmware. */
static inline void rb_dmub_put32(uint8_t *buf, uint32_t off, uint32_t v)
{
    buf[off + 0] = (uint8_t)(v & 0xffu);
    buf[off + 1] = (uint8_t)((v >> 8) & 0xffu);
    buf[off + 2] = (uint8_t)((v >> 16) & 0xffu);
    buf[off + 3] = (uint8_t)((v >> 24) & 0xffu);
}

static inline uint32_t rb_dmub_get32(const uint8_t *buf, uint32_t off)
{
    return (uint32_t)buf[off + 0] |
           ((uint32_t)buf[off + 1] << 8) |
           ((uint32_t)buf[off + 2] << 16) |
           ((uint32_t)buf[off + 3] << 24);
}

/* dmub_cmd.h:5785-5845. frame_ramp, backlight_user_level, then
 * version/panel_mask/aux_inst/pad, then backlight_control_type and the four
 * luminance/pwm bounds. 32 payload bytes, which is what
 * dmub_abm_lcd.c:295 puts in payload_bytes.
 *
 * The four bounds and aux_inst are left ZERO, which is what dmub_abm_lcd.c
 * leaves them for a PWM panel: it memsets the whole command and sets only
 * frame_ramp, backlight_user_level, version and panel_mask. Writing anything
 * else there would be inventing a value for a field whose consumer we have
 * not read (the recurring error, stated once in docs/RULES.md). */
#define RB_DMUB_SET_BL_PAYLOAD  32u

static inline void rb_dmub_build_set_backlight(uint8_t *buf,
                                               uint32_t level_u16_16,
                                               uint32_t frame_ramp,
                                               uint8_t panel_mask)
{
    uint32_t i;

    for (i = 0; i < RB_DMUB_CMD_SIZE; i++) { buf[i] = 0u; }

    rb_dmub_put32(buf, 0, rb_dmub_hdr(RB_DMUB_CMD_ABM,
                                      RB_DMUB_CMD_ABM_SET_BACKLIGHT,
                                      RB_DMUB_SET_BL_PAYLOAD));
    rb_dmub_put32(buf, 4, frame_ramp);
    rb_dmub_put32(buf, 8, level_u16_16);
    buf[12] = (uint8_t)RB_DMUB_ABM_CONTROL_VERSION_1;
    buf[13] = panel_mask;
    buf[14] = 0u;                               /* aux_inst */
    buf[15] = 0u;                               /* pad[1] */
    rb_dmub_put32(buf, 16, RB_DMUB_BL_CONTROL_PWM);
    /* 20..35: min/max_luminance, min/max_backlight_pwm - all zero. */
}

/* dmub_cmd.h:5716-5747. otg_inst, panel_inst, set_pipe_option,
 * ramping_boundary, pwrseq_inst, pad[3] - 8 payload bytes.
 *
 * WHY THIS EXISTS AS A SEPARATE COMMAND. dcn21_set_backlight_level sends
 * ABM_SET_PIPE with SET_ABM_PIPE_NORMAL *before* the backlight level, so a
 * SET_BACKLIGHT on a pipe ABM has not been bound to may be ignored. Whether
 * this chip needs it is unknown: the firmware's own 37 bring-up commands may
 * already have bound it. So it is a second, separately triggered command
 * rather than something sent unasked - try the backlight alone first, and if
 * the panel does not move, bind the pipe and try again, both inside one
 * boot. A SET_PIPE is the more invasive of the two and should not be spent
 * on a boot that did not need it. */
#define RB_DMUB_SET_PIPE_PAYLOAD  8u

static inline void rb_dmub_build_set_pipe(uint8_t *buf,
                                          uint8_t otg_inst,
                                          uint8_t panel_inst,
                                          uint8_t pwrseq_inst,
                                          uint8_t option)
{
    uint32_t i;

    for (i = 0; i < RB_DMUB_CMD_SIZE; i++) { buf[i] = 0u; }

    rb_dmub_put32(buf, 0, rb_dmub_hdr(RB_DMUB_CMD_ABM,
                                      RB_DMUB_CMD_ABM_SET_PIPE,
                                      RB_DMUB_SET_PIPE_PAYLOAD));
    buf[4] = otg_inst;
    buf[5] = panel_inst;
    buf[6] = option;
    buf[7] = 0u;                /* ramping_boundary, "Unused" upstream */
    buf[8] = pwrseq_inst;
    /* 9..11: pad[3] - zero. */
}

/* The level a userspace trigger asked for, bounded. 0 is a legal request -
 * it is the one end of the range the panel can actually be seen to take -
 * so the only refusal is above full scale. */
static inline int rb_dmub_level_ok(long v)
{
    return v >= 0 && v <= (long)RB_DMUB_LEVEL_FULL;
}

#endif /* RB_DMUB_H */
