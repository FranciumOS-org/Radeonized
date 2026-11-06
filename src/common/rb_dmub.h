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

/* dmub_srv.c:81-86. THESE ARE THE REAL VALUES and 2zn had one of them
 * wrong: DMUB_CW4_BASE is 0x64000000, not 0x80000000.
 *
 * HOW THAT HAPPENED, because the shape of the error matters more than the
 * number: boot 271 read DMCUB_INBOX1_BASE_ADDRESS as 0x80000000, 2zn wrote
 * 0x80000000 into this file as "DMUB_CW4_BASE", and the hosttest then
 * asserted that the constant equalled what the register read. That assertion
 * passed and proved nothing at all - it compared the reading with itself.
 * Rule 26 in its purest form: a search is an instrument, and so is a
 * CONSTANT. Ask of any value written down from a header: has anyone looked
 * at the header, or only at the thing it was supposed to agree with?
 *
 * AND THE ADDRESS FIELDS ARE 29 BITS. dcn_3_1_2_sh_mask.h:5881/5914 give
 * DMCUB_REGION3_CW4_BASE_ADDRESS and _TOP_ADDRESS a mask of 0x1FFFFFFF, with
 * the enable at bit 31 - so a window address is written TRUNCATED, and
 * 0x64000000 & 0x1FFFFFFF is 0x04000000, which is exactly what boot 272 read
 * in CW4_BASE. So CW4 IS the mailbox window on this firmware after all, with
 * a 64 KB window (0x04000000..0x0400ffff) rather than Linux's 2 * 8192.
 * DMCUB_INBOX1_BASE_ADDRESS is a full 32 bits (sh_mask.h:6154) and reads
 * 0x80000000, which is neither 0x64000000 nor its truncation, and nothing in
 * hand explains it. That is a FINDING, not a convention to pick. */
#define RB_DMUB_CW0_BASE   0x60000000u
#define RB_DMUB_CW1_BASE   0x61000000u
#define RB_DMUB_CW3_BASE   0x63000000u
#define RB_DMUB_CW4_BASE   0x64000000u
#define RB_DMUB_CW5_BASE   0x65000000u
#define RB_DMUB_CW6_BASE   0x66000000u

/* All seven region-3 windows, dcn_3_1_2_offset.h:921-979, base_idx 2. The
 * three groups are contiguous and the offset pair interleaves lo,hi - so the
 * register for window n is BASE + n, TOP + n, and OFFSET + 2n. */
#define RB_DMUB_CW_COUNT             7u
#define RB_DMUB_DW_CW_BASE_ADDRESS   0x01a5u
#define RB_DMUB_DW_CW_TOP_ADDRESS    0x01adu
#define RB_DMUB_DW_CW_OFFSET         0x01b5u

/* dcn_3_1_2_sh_mask.h:5881, 5913-5915. */
#define RB_DMUB_CW_ENABLE            0x80000000u
#define RB_DMUB_CW_ADDR_MASK         0x1fffffffu

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
#define RB_DMUB_BL_CONTROL_PWM       0u
#define RB_DMUB_BL_CONTROL_VESA_AUX  1u
#define RB_DMUB_BL_CONTROL_AMD_AUX   2u
#define RB_DMUB_BL_CONTROL_COUNT     3u

/* 2zt: THE CONTROL TYPE IS NOW A DIAL, AND IT IS THE LAST UNTRIED ONE.
 *
 * Boot 278 settled the other live hypothesis by exercising it: the three
 * parameter keys ("brightness" -> RBAMDBacklightParams.cpp, "brig" ->
 * IOFramebufferParameterHandler -> setAttributeForConnection, and
 * "linear-brightness") all land the SAME write, read back bit-identical
 * (RBFBBlLastCount == RBFBBlReadback == 16800, RBFBBlLastLevel 17203 !=
 * RBFBBlLevel0 22784), with RBFBBlLiveDiffs 0 over 2164 reads - and the
 * panel moved on none of them. So the boot-249 route is not a regression
 * and 2zl stands: BL_PWM_CNTL is real, writable, uncontested and not the
 * control surface.
 *
 * What has never been varied is the ONE byte the ABM command carries that
 * says which output stage the level is for. Every send so far has written
 * 0 = PWM, which boot 270 falsified independently - the register held our
 * duty cycle for 40 s with nothing else writing it and the panel unmoved.
 * 1 = VESA_AUX and 2 = AMD_AUX are the two the firmware also understands.
 *
 * 2zl read these as reachable only from edp_set_backlight_level_nits, which
 * is true of LINUX's call graph and is not a statement about the FIRMWARE's
 * command handler: dmub_cmd.h puts the field in dmub_cmd_abm_set_backlight_data
 * itself, so any SET_BACKLIGHT may carry any of the three. Linux's choice of
 * caller is not the firmware's validation.
 *
 * It is a settable property rather than a build-time constant because three
 * values of one byte would otherwise cost three boots, and 2zm already
 * established that every dial this project owned being a boot-arg is why
 * 2xn/2zb/2zc spent three boots on three points of one curve. */
static inline int rb_dmub_bl_type_ok(long t)
{ return t >= 0 && t < (long)RB_DMUB_BL_CONTROL_COUNT; }

/* dmub_cmd.h:63-74. */
#define RB_DMUB_SET_ABM_PIPE_NORMAL             1u
#define RB_DMUB_SET_ABM_PIPE_IMMEDIATELY_DISABLE 255u

/* u16.16, the units dmub_abm_set_backlight takes (backlight_pwm_u16_16) and
 * the same units rb_backlight.h's levels are already in. */
#define RB_DMUB_LEVEL_FULL  0x10000u

/* dmub_cmd.h, enum dmub_cmd_type: DMUB_CMD__QUERY_FEATURE_CAPS = 6, doc
 * comment "Command type used to query FW feature caps".
 *
 * WHY A CAPS QUERY IS IN A BACKLIGHT FILE. Boot 274 measured four commands
 * pushed at CW4's WPTR and RPTR following every one of them inside the 1 ms
 * spin - and that is exactly what boot 273 measured while the command was
 * going into the TRACE BUFFER and the DMCUB was consuming a zeroed slot of
 * the real ring. An ack says the microcontroller advanced over the slot our
 * WPTR named; it says nothing about whose bytes were in it. Four milestones
 * of this surface have now rested on a layout agreement with no byte-level
 * confirmation, and the tool says so on every run.
 *
 * QUERY_FEATURE_CAPS closes that gap and costs nothing on the chip, because
 * it is a read. dc_dmub_srv.c's dc_dmub_srv_query_caps_cmd sets
 * header.ret_status = 1 BEFORE sending and then accepts the reply only
 * `if (... && cmd.query_feature_caps.header.ret_status == 0)` - so the
 * firmware CLEARS that bit. And the reply lands in our own slot:
 * dmub_srv_cmd_get_response ends at dmub_rb_get_return_data, whose address
 * is `base_address + rptr - DMUB_RB_CMD_SIZE`, i.e. the slot just consumed,
 * which is the slot we wrote. So after the ack, byte 2 bit 0 of our own 64
 * bytes going 1 -> 0 is a change only the DMCUB can have made, in memory we
 * chose the address of. That is the identification the layout could not
 * give, and it is one bit that cannot be confused with a reading of itself
 * (2zp's error, rule 26).
 *
 * The payload is one struct dmub_feature_caps, 14 bytes, all uint8_t:
 * psr, fw_assisted_mclk_switch_ver, reserved[4], subvp_psr_support,
 * gecc_enable, replay_supported, replay_reserved[3],
 * abm_aux_backlight_support, lsdma_support_in_dmu. Under
 * #pragma pack(push, 1), so the offsets below are the declaration order. */
#define RB_DMUB_CMD_QUERY_FEATURE_CAPS  6u
#define RB_DMUB_CAPS_PAYLOAD            14u

#define RB_DMUB_CAPS_OFF_PSR          4u
#define RB_DMUB_CAPS_OFF_MCLK_VER     5u
/* 6..9 reserved[4] */
#define RB_DMUB_CAPS_OFF_SUBVP_PSR    10u
#define RB_DMUB_CAPS_OFF_GECC         11u
#define RB_DMUB_CAPS_OFF_REPLAY       12u
/* 13..15 replay_reserved[3] */
#define RB_DMUB_CAPS_OFF_ABM_AUX_BL   16u
#define RB_DMUB_CAPS_OFF_LSDMA        17u

/* ret_status is bit 16 of the header word - dmub_cmd.h's dmub_cmd_header
 * puts it after type:8 and sub_type:8. */
#define RB_DMUB_HDR_RET_STATUS  (1u << 16)

/* ---- GPINT: the general-purpose interrupt, which needs no ring at all ----
 *
 * Boot 275 is why this exists. The caps command went into slot 0x940, WPTR
 * advanced to 0x980, RPTR followed - and the slot still read
 * `06 00 01 0e`, with ret_status STILL SET. The firmware stepped its read
 * pointer over our slot and did not clear the one bit
 * dc_dmub_srv_query_caps_cmd's whole contract depends on. Both routes are
 * now suspect and the ring cannot arbitrate between them:
 *
 *   (1) this firmware does not implement command 6 and discarded it;
 *   (2) the DMCUB reads a DIFFERENT base, so it consumed a zero slot
 *       (DMUB_CMD__NULL) from memory we have never looked at;
 *   (3) the DMCUB is not processing commands at all and RPTR advances for
 *       some other reason.
 *
 * GPINT separates (1) from (2) and (3) because **no address is involved**.
 * dmub_dcn31.c's set_gpint writes one register, DMCUB_GPINT_DATAIN1, and
 * is_gpint_acked reads that same register back and compares it against the
 * value sent with the status field CLEARED - so the firmware clears a bit we
 * set, in a register, with no memory, no window, no mc2pa and no mapping. If
 * that comes back acked, the DMCUB is alive and talking to us and the
 * problem is the ring or the command; if it does not, the ring was never the
 * question.
 *
 * And there is a POSITIVE CONTROL, from a different register than the answer
 * (rule 26: an instrument needs a known-present case). DMUB_GPINT__GET_FW_
 * VERSION = 1 returns the firmware version in DMCUB_SCRATCH7, and this
 * project read **4.0.15.47 running, 4.0.21.71 on ROM** off DMCUB_SCRATCH0 in
 * a level-2 probe long before any of this - 0x01e3 against 0x01ea, two
 * different registers. So the answer can be checked against something that
 * was not measured by the instrument producing it.
 *
 * union dmub_gpint_data_register: param:16, command_code:12, status:4.
 * Registers from dcn_3_1_2_offset.h, all base_idx 2 (DMU segment 2, 0x34c0).
 *
 * DMUB_GPINT__STOP_FW = 2 **HALTS THE FIRMWARE** - dmub_dcn31_reset sends it
 * and polls SCRATCH7 for 0xDEADDEAD. On this machine the DMCUB is what lit
 * the panel, so halting it is not a diagnostic, it is the end of the boot.
 * rb_dmub_gpint_allowed() therefore allowlists exactly ONE code and the
 * hosttest asserts 2 is refused. Not a comment - a gate. */
/* ---- regions 4/5/6: the windows this project never read (2zs) ----
 *
 * DMCUB_INBOX1_BASE_ADDRESS reads 0x80000000 and has been published as
 * "unexplained, unused" since boot 271. It is not unexplained: it is the
 * inbox base IN DMCUB ADDRESS SPACE. dmub_dcn31_setup_mailbox writes
 * `inbox1->base` with no translation, and on Linux dmub_srv.c sets
 * inbox1.base = cw4.region.base = DMUB_CW4_BASE = 0x64000000. **This
 * firmware wrote 0x80000000**, and no region-3 window covers it: CW0..CW6
 * are 0x60000000..0x66000000. So the mailbox is NOT in CW4 on this machine,
 * and six exact layout matches never contradicted that - they confirmed the
 * CODE windows sit where Linux puts them and said nothing about where this
 * firmware put its ring.
 *
 * Regions 4, 5 and 6 are the other apertures. Their DMCUB-space bases are
 * fixed in hardware (nothing writes them), so only OFFSET - the framebuffer
 * address - and TOP - the size - are programmable, which is why
 * dmub_dcn31_setup_windows programs REGION5_OFFSET/_TOP_ADDRESS and never a
 * REGION5 base. One of them is the aperture at 0x80000000.
 *
 * So: read all three, resolve each offset by the same MC-vs-physical range
 * test the CW windows use, and let the CAPS QUERY say which one is right.
 * That instrument now works - boot 276 proved the DMCUB answers us over
 * GPINT with no address in the path, and the caps query's ret_status bit is
 * a byte-level test that only needs the correct address to be conclusive.
 * One boot tries every candidate. */
#define RB_DMUB_DW_REGION4_OFFSET      0x0196u
#define RB_DMUB_DW_REGION4_OFFSET_HI   0x0197u
#define RB_DMUB_DW_REGION5_OFFSET      0x0198u
#define RB_DMUB_DW_REGION5_OFFSET_HI   0x0199u
#define RB_DMUB_DW_REGION6_OFFSET      0x019au
#define RB_DMUB_DW_REGION6_OFFSET_HI   0x019bu
#define RB_DMUB_DW_REGION4_TOP         0x01a1u
#define RB_DMUB_DW_REGION5_TOP         0x01a2u
#define RB_DMUB_DW_REGION6_TOP         0x01a3u

/* The candidate windows a push may target. 0..6 are region-3 CW0..CW6,
 * 7/8/9 are regions 4/5/6. */
#define RB_DMUB_WINSEL_CW0     0u
#define RB_DMUB_WINSEL_REGION4 7u
#define RB_DMUB_WINSEL_REGION5 8u
#define RB_DMUB_WINSEL_REGION6 9u
#define RB_DMUB_WINSEL_COUNT   10u

static inline int rb_dmub_winsel_ok(long v)
{
    return v >= 0 && v < (long)RB_DMUB_WINSEL_COUNT;
}

/* Does this DMCUB-space address fall inside the region-3 window set? If not,
 * the mailbox is behind one of the fixed-base apertures and CW4 cannot be
 * it, whatever the layout agrees about. */
static inline int rb_dmub_inbox_in_region3(uint32_t inbox_base)
{
    return inbox_base >= RB_DMUB_CW0_BASE &&
           inbox_base <= RB_DMUB_CW6_BASE + 0x00ffffffu;
}

#define RB_DMUB_DW_SCRATCH0        0x01e3u
#define RB_DMUB_DW_SCRATCH7        0x01eau
#define RB_DMUB_DW_GPINT_DATAIN0   0x01f7u
#define RB_DMUB_DW_GPINT_DATAIN1   0x01f8u
#define RB_DMUB_DW_GPINT_DATAOUT   0x01f9u

#define RB_DMUB_GPINT_GET_FW_VERSION  1u
#define RB_DMUB_GPINT_STOP_FW         2u    /* NEVER SEND. See above. */
#define RB_DMUB_GPINT_STOP_RESPONSE   0xDEADDEADu

#define RB_DMUB_GPINT_STATUS_SHIFT  28u
#define RB_DMUB_GPINT_STATUS_MASK   (0xfu << RB_DMUB_GPINT_STATUS_SHIFT)
#define RB_DMUB_GPINT_CODE_SHIFT    16u
#define RB_DMUB_GPINT_CODE_MASK     0xfffu

/* The only code this file will build a word for. */
static inline int rb_dmub_gpint_allowed(uint32_t code)
{
    return code == RB_DMUB_GPINT_GET_FW_VERSION;
}

static inline uint32_t rb_dmub_gpint_word(uint32_t code, uint32_t param)
{
    return (param & 0xffffu) |
           ((code & RB_DMUB_GPINT_CODE_MASK) << RB_DMUB_GPINT_CODE_SHIFT) |
           (1u << RB_DMUB_GPINT_STATUS_SHIFT);
}

/* dmub_dcn31_is_gpint_acked: clear status in the local copy and compare the
 * whole word against the readback. An ack is the firmware having cleared
 * that field and left everything else alone - so a readback of 0, or of
 * garbage, or of our own word unchanged, are all NOT acks, and each is a
 * different reading. */
static inline int rb_dmub_gpint_acked(uint32_t sent, uint32_t readback)
{
    return readback == (sent & ~RB_DMUB_GPINT_STATUS_MASK);
}

static inline uint32_t rb_dmub_gpint_status(uint32_t w)
{
    return (w & RB_DMUB_GPINT_STATUS_MASK) >> RB_DMUB_GPINT_STATUS_SHIFT;
}
static inline uint32_t rb_dmub_gpint_code(uint32_t w)
{
    return (w >> RB_DMUB_GPINT_CODE_SHIFT) & RB_DMUB_GPINT_CODE_MASK;
}

/* Why a locate was refused. Published, never inferred: "we did not write"
 * and "we did not write BECAUSE no enabled window covers the inbox" are
 * different findings (rule 14).
 *
 * RENUMBERED after boot 272. The old codes 2/3/4 asserted Linux's window
 * assignment and 6 asserted that the offset pair was already
 * system-physical; both were wrong, so the codes do not mean what the 2zn
 * journal entry says they meant. Boot 272's printed "locate 2" was the old
 * INBOX_BASE check firing on `inbox_base != cw4_base` - it is the reading
 * that refuted the design, and it is not comparable to a 2 from this table. */
enum {
    RB_DMUB_LOC_OK = 0,
    RB_DMUB_LOC_NO_MAP = 1,      /* no BAR5 registers to read at all */
    RB_DMUB_LOC_INBOX = 2,       /* INBOX1_BASE/SIZE cannot be a ring */
    RB_DMUB_LOC_NO_WINDOW = 3,   /* no ENABLED CWn contains the inbox */
    RB_DMUB_LOC_NO_OFFSET = 4,   /* that window's offset pair is zero */
    RB_DMUB_LOC_FB = 5,          /* FB_LOCATION/FB_OFFSET give no ranges */
    RB_DMUB_LOC_ADDR = 6,        /* derived address is neither MC nor sysphys */
    RB_DMUB_LOC_ALIGN = 7,       /* the ring is not 64-byte aligned */
    RB_DMUB_LOC_MAP_FAILED = 8   /* the kext could not map it */
};

/* What kind of address the covering window's offset pair turned out to hold.
 * Boot 272 settled this and it was NOT what 2zn predicted, so it is a
 * measured output of the locate rather than a constant of the design. */
enum {
    RB_DMUB_ADDR_NONE = 0,
    RB_DMUB_ADDR_MC = 1,        /* an MC address; mc2pa() applies */
    RB_DMUB_ADDR_PA = 2         /* already system-physical */
};

/* One region-3 window, as its three registers read. */
struct rb_dmub_win {
    uint32_t base;      /* DMCUB_REGION3_CWn_BASE_ADDRESS */
    uint32_t top;       /* DMCUB_REGION3_CWn_TOP_ADDRESS, enable at bit 31 */
    uint32_t off_lo;    /* DMCUB_REGION3_CWn_OFFSET */
    uint32_t off_hi;    /* DMCUB_REGION3_CWn_OFFSET_HIGH */
};

/* The whole register-derived picture, so the decision below is a pure
 * function of what was read and the hosttest can hand it any boot's values. */
struct rb_dmub_loc {
    uint32_t inbox_base;    /* DMCUB_INBOX1_BASE_ADDRESS, a DMCUB address */
    uint32_t inbox_size;    /* DMCUB_INBOX1_SIZE, the ring's capacity */
    struct rb_dmub_win cw[RB_DMUB_CW_COUNT];
    uint32_t fb_location;   /* DCN_VM_FB_LOCATION_BASE, FB_BASE field */
    uint32_t fb_offset;     /* DCN_VM_FB_OFFSET, FB_OFFSET field */
};

/* Everything the locate worked out, so a refusal can be diagnosed from the
 * same structure a success is read from (rule 14: a zero that was never
 * written must not look like a zero that was). */
struct rb_dmub_loc_out {
    int      refuse;
    int      win;          /* the covering window, -1 if none */
    uint64_t dmub_mem;     /* the address as the window register holds it */
    uint64_t ring_pa;      /* ...translated to system-physical */
    int      addr_kind;    /* RB_DMUB_ADDR_* */
    uint32_t span;         /* how many bytes of it are a candidate ring */
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

static inline uint64_t rb_dmub_win_off(const struct rb_dmub_win *w)
{
    return ((uint64_t)w->off_hi << 32) | (uint64_t)w->off_lo;
}

static inline int rb_dmub_win_enabled(const struct rb_dmub_win *w)
{
    return (w->top & RB_DMUB_CW_ENABLE) != 0u;
}

static inline uint32_t rb_dmub_win_top(const struct rb_dmub_win *w)
{
    return w->top & RB_DMUB_CW_ADDR_MASK;
}

/* How much of this window we are willing to treat as a ring. The mailbox
 * window on boot 272 is 64 KB; Linux's inbox is the first DMUB_RB_SIZE of
 * it, so a candidate ring is the window's first 8192 bytes or the whole
 * window if it is smaller. The TOP convention is not assumed: dmub_srv.c:882
 * computes `top = base + size` while boot 272 read base 0x04000000 against
 * top 0x0400ffff, which is base + size - 1. Treating top as the LAST byte is
 * correct under that reading and one byte conservative under the other, and
 * being one byte short cannot produce a wrong address while guessing can. */
static inline uint32_t rb_dmub_win_span(const struct rb_dmub_win *w)
{
    uint32_t base = w->base & RB_DMUB_CW_ADDR_MASK;
    uint32_t top = rb_dmub_win_top(w);
    uint32_t span;

    if (!rb_dmub_win_enabled(w)) { return 0u; }
    if (top <= base) { return 0u; }
    span = top - base + 1u;
    if (span > RB_DMUB_RB_SIZE) { span = RB_DMUB_RB_SIZE; }
    span &= ~(RB_DMUB_CMD_SIZE - 1u);      /* whole commands only */
    return span;
}

/* 16 GB: larger than any carve-out this machine can have (4 GB measured) and
 * small enough that the MC range (0xF4_..) and the system-physical range
 * (0x7_8000_0000) stay disjoint, so an address can be CLASSIFIED rather than
 * guessed at. */
#define RB_DMUB_FB_SPAN  (16ull * 1024ull * 1024ull * 1024ull)

/* Linux's window layout, which is the identification.
 *
 * BOOT 273 READ ALL SEVEN AND SIX OF THEM MATCH, after the 29-bit
 * truncation: CW0 0x00000000, CW1 0x01000000, CW3 0x03000000, CW4
 * 0x04000000, CW5 0x05000000, CW6 0x06000000, against dmub_srv.c:81-86's
 * 0x60000000 / 0x61000000 / 0x63000000 / 0x64000000 / 0x65000000 /
 * 0x66000000. CW2 is disabled, which is also correct - dmub_srv.c:873 bases
 * it inside CW0 rather than at a constant of its own.
 *
 * SIX AGREEMENTS IS THE IDENTIFICATION, and it is what 2zn should have had.
 * Rule 18 says two numbers agreeing to one figure is not an identification;
 * this is six exact matches of a published layout, read off the machine
 * rather than assumed onto it. So CW4 is the mailbox - dmub_srv.c was right
 * all along, and what was wrong in 2zn was the CONSTANT (0x80000000 for
 * 0x64000000) and in 2zo the idea that a byte signature could do better.
 *
 * 2zo's signature chose CW5, which is DMUB_WINDOW_5_TRACEBUFF. A trace
 * buffer is a dense table of records and therefore the ONE structure
 * guaranteed to pass a weak `looks like a header` test - boot 273 scored it
 * 128 of 128 slots with zero empties, where a command ring written 37 times
 * must have 91 empty slots after the run. The lesson is not "tighten the
 * test": it is that a weak test plus a permissive selector will find the
 * densest data in the search space, so the selector must be the thing that
 * is actually known. The layout is known. */
#define RB_DMUB_LAYOUT_CHECKED  6u

static inline uint32_t rb_dmub_cw_base_expected(unsigned idx)
{
    switch (idx) {
    case 0: return RB_DMUB_CW0_BASE;
    case 1: return RB_DMUB_CW1_BASE;
    case 3: return RB_DMUB_CW3_BASE;
    case 4: return RB_DMUB_CW4_BASE;
    case 5: return RB_DMUB_CW5_BASE;
    case 6: return RB_DMUB_CW6_BASE;
    default: return 0u;        /* CW2 has no constant of its own */
    }
}

/* How many of the six windows with a published base sit where Linux puts
 * them. Enabled and matching counts; anything else does not. */
static inline uint32_t rb_dmub_layout_score(const struct rb_dmub_loc *l)
{
    uint32_t n = 0;
    unsigned i;

    for (i = 0; i < RB_DMUB_CW_COUNT; i++) {
        const uint32_t want = rb_dmub_cw_base_expected(i);
        if (i == 2u) { continue; }      /* no constant of its own */
        if (!rb_dmub_win_enabled(&l->cw[i])) { continue; }
        if ((l->cw[i].base & RB_DMUB_CW_ADDR_MASK) ==
            (want & RB_DMUB_CW_ADDR_MASK)) {
            n++;
        }
    }
    return n;
}

/* The mailbox window index, or -1.
 *
 * Nothing is chosen unless the WHOLE layout corroborates. One window
 * matching a constant is the 2zn error again; six matching is a layout this
 * firmware demonstrably follows, and then CW4 is the mailbox by that layout
 * and not by a guess about its contents. */
static inline int rb_dmub_mailbox_win(const struct rb_dmub_loc *l)
{
    if (rb_dmub_layout_score(l) < RB_DMUB_LAYOUT_CHECKED) { return -1; }
    if (!rb_dmub_win_enabled(&l->cw[4])) { return -1; }
    if ((l->cw[4].base & RB_DMUB_CW_ADDR_MASK) !=
        (RB_DMUB_CW4_BASE & RB_DMUB_CW_ADDR_MASK)) { return -1; }
    return 4;
}

/* What a command ring's memory should look like, kept as an OBSERVATION and
 * no longer as the selector.
 *
 * It is still worth computing and publishing, because it is the only thing
 * that could ever confirm the address at byte level - but boot 273 showed
 * CW4's 8192 bytes read ALL ZEROS while the registers said WPTR == RPTR ==
 * 2368, so the "37 commands sit at bring-up" belief was an inference from
 * the POINTER and never from memory. There are no bytes there to recognise,
 * which is exactly why the selector cannot depend on them.
 *
 * A ring that does hold commands has a leading run followed by empty slots,
 * so `run < slots` is required: a window with no empty tail at all is dense
 * data, which is what the trace buffer was. */
static inline int rb_dmub_sig_ok(uint32_t run, uint32_t bad, uint32_t slots)
{
    if (run < 2u || bad != 0u) { return 0; }
    if (slots == 0u || run >= slots) { return 0; }
    return 1;
}

/* One window, resolved to memory we could map.
 *
 * WHY PER-WINDOW AND NOT "the mailbox is CW<n>". 2zn located the ring by
 * naming a window, got the name from dmub_srv.c, and was refuted on the
 * first boot. The replacement does not name one: every enabled window is
 * resolved, the kext maps each and looks for the firmware's own 37 commands,
 * and the window that SHOWS them is the mailbox. That is a search with its
 * control built in, and it needs no convention at all - which matters
 * because the one register that was supposed to settle it,
 * DMCUB_INBOX1_BASE_ADDRESS, reads 0x80000000 and nothing in hand explains
 * that.
 *
 * The address KIND is measured, not assumed. dmub_dcn31.c:80 writes a window
 * offset as `gpu_addr - fb_base + fb_offset`, which is this project's own
 * mc2pa(), so 2zn concluded the register already held a system-physical
 * address. Boot 272 read 0xf4_ffe40000 in CW4 - an MC address, untranslated,
 * because this machine's FIRMWARE wrote that register and not Linux. Both
 * conventions are therefore accepted and told apart by range. */
static inline int rb_dmub_win_locate(const struct rb_dmub_loc *l,
                                     unsigned idx,
                                     struct rb_dmub_loc_out *o)
{
    uint64_t fb_base, fb_off, mem, pa;
    uint32_t span;

    o->refuse = RB_DMUB_LOC_OK;
    o->win = -1;
    o->dmub_mem = 0;
    o->ring_pa = 0;
    o->addr_kind = RB_DMUB_ADDR_NONE;
    o->span = 0;

    if (idx >= RB_DMUB_CW_COUNT) { return o->refuse = RB_DMUB_LOC_NO_WINDOW; }

    span = rb_dmub_win_span(&l->cw[idx]);
    if (span < RB_DMUB_CMD_SIZE) { return o->refuse = RB_DMUB_LOC_NO_WINDOW; }

    mem = rb_dmub_win_off(&l->cw[idx]);
    if (mem == 0ull) { return o->refuse = RB_DMUB_LOC_NO_OFFSET; }

    fb_base = rb_dmub_fb_base(l);
    fb_off = rb_dmub_fb_offset(l);
    if (fb_base == 0ull || fb_off == 0ull) {
        return o->refuse = RB_DMUB_LOC_FB;
    }

    if (mem >= fb_base && mem + span <= fb_base + RB_DMUB_FB_SPAN) {
        o->addr_kind = RB_DMUB_ADDR_MC;
        pa = mem - fb_base + fb_off;
    } else if (mem >= fb_off && mem + span <= fb_off + RB_DMUB_FB_SPAN) {
        o->addr_kind = RB_DMUB_ADDR_PA;
        pa = mem;
    } else {
        o->addr_kind = RB_DMUB_ADDR_NONE;
        o->dmub_mem = mem;
        return o->refuse = RB_DMUB_LOC_ADDR;
    }

    if ((pa & (uint64_t)(RB_DMUB_CMD_SIZE - 1u)) != 0ull) {
        o->dmub_mem = mem;
        return o->refuse = RB_DMUB_LOC_ALIGN;
    }

    o->win = (int)idx;
    o->dmub_mem = mem;
    o->ring_pa = pa;
    o->span = span;
    return RB_DMUB_LOC_OK;
}

/* The ring's offset inside the carve-out, which is also its offset inside
 * BAR0 - 2pr measured the GOP framebuffer at BAR0 offset 0, i.e. BAR0 is the
 * carve-out seen through the aperture. Only meaningful once a window has
 * located OK. */
static inline uint64_t rb_dmub_ring_fb_off(const struct rb_dmub_loc *l,
                                           const struct rb_dmub_loc_out *o)
{
    return o->ring_pa - rb_dmub_fb_offset(l);
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
static inline uint32_t rb_dmub_hdr_ret(uint32_t h)     { return (h >> 16) & 0x1u; }

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
                                               uint8_t panel_mask,
                                               uint32_t ctrl_type,
                                               uint8_t aux_inst)
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
    /* aux_inst is documented in dmub_cmd.h only as "AUX HW Instance", so for
     * a PWM send it stays the zero dmub_abm_lcd.c leaves it (the recurring
     * error: no value invented for a field whose consumer is unread). For an
     * AUX send it is the caller's, because then it is load-bearing and the
     * only instance this machine has an eDP on is 0. */
    buf[14] = aux_inst;
    buf[15] = 0u;                               /* pad[1] */
    rb_dmub_put32(buf, 16, ctrl_type);
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

/* Resolve a fixed-base aperture (region 4/5/6). These have no BASE register -
 * their DMCUB-space base is fixed in hardware - so there is nothing to match
 * a layout against and the only question is where the offset points and how
 * big TOP says it is. Same MC-vs-physical range test as the CW windows, so
 * the address convention is measured in one place. */
static inline int rb_dmub_reg_locate(const struct rb_dmub_loc *l,
                                      uint32_t off_lo, uint32_t off_hi,
                                      uint32_t top,
                                      struct rb_dmub_loc_out *o)
{
    uint64_t fb_base, fb_off, mem, pa;
    uint32_t span;

    o->refuse = RB_DMUB_LOC_OK; o->win = -1; o->dmub_mem = 0;
    o->ring_pa = 0; o->addr_kind = RB_DMUB_ADDR_NONE; o->span = 0;

    if ((top & RB_DMUB_CW_ENABLE) == 0u) { return o->refuse = RB_DMUB_LOC_NO_WINDOW; }
    span = (top & RB_DMUB_CW_ADDR_MASK) + 1u;
    if (span > RB_DMUB_RB_SIZE) { span = RB_DMUB_RB_SIZE; }
    span &= ~(RB_DMUB_CMD_SIZE - 1u);
    if (span < RB_DMUB_CMD_SIZE) { return o->refuse = RB_DMUB_LOC_NO_WINDOW; }

    mem = ((uint64_t)off_hi << 32) | (uint64_t)off_lo;
    if (mem == 0ull) { return o->refuse = RB_DMUB_LOC_NO_OFFSET; }
    fb_base = rb_dmub_fb_base(l); fb_off = rb_dmub_fb_offset(l);
    if (fb_base == 0ull || fb_off == 0ull) { return o->refuse = RB_DMUB_LOC_FB; }

    if (mem >= fb_base && mem + span <= fb_base + RB_DMUB_FB_SPAN) {
        o->addr_kind = RB_DMUB_ADDR_MC;
        pa = mem - fb_base + fb_off;
    } else if (mem >= fb_off && mem + span <= fb_off + RB_DMUB_FB_SPAN) {
        o->addr_kind = RB_DMUB_ADDR_PA;
        pa = mem;
    } else {
        o->dmub_mem = mem;
        return o->refuse = RB_DMUB_LOC_ADDR;
    }
    if ((pa & (uint64_t)(RB_DMUB_CMD_SIZE - 1u)) != 0ull) {
        o->dmub_mem = mem; return o->refuse = RB_DMUB_LOC_ALIGN;
    }
    o->dmub_mem = mem; o->ring_pa = pa; o->span = span;
    return RB_DMUB_LOC_OK;
}

/* QUERY_FEATURE_CAPS, with ret_status pre-set to 1 so the firmware has a bit
 * to clear. Everything else is zero: the payload is the firmware's to fill
 * in, and dc_dmub_srv_query_caps_cmd memsets the command before sending. */
static inline void rb_dmub_build_query_caps(uint8_t *buf)
{
    uint32_t i;

    for (i = 0; i < RB_DMUB_CMD_SIZE; i++) { buf[i] = 0u; }

    rb_dmub_put32(buf, 0, rb_dmub_hdr(RB_DMUB_CMD_QUERY_FEATURE_CAPS, 0u,
                                      RB_DMUB_CAPS_PAYLOAD) |
                          RB_DMUB_HDR_RET_STATUS);
}

/* DMUB_CMD__VBIOS / DOMAIN_CONTROL - 2DC.
 *
 * dmub_cmd.h: `DMUB_CMD__VBIOS` = 128 and `DMUB_CMD__VBIOS_DOMAIN_CONTROL` =
 * 28. The payload is `struct dmub_rb_cmd_domain_control_data`, which that
 * header defines completely:
 *
 *     uint8_t inst : 6;        DOMAIN instance to control
 *     uint8_t power_gate : 1;  1 = power gate, 0 = power up
 *     uint8_t reserved[3];
 *
 * so the payload is FOUR bytes and both fields live in the first one. That
 * completeness is the reason this sub-type was chosen over the transmitter
 * (rb_pg.h says why at length): no atom enum, no ROM-revision-dependent
 * struct, nothing recalled.
 *
 * NO ret_status. It is a command, not a query - 2D9's whole point is that
 * five of the seven VBIOS sub-types have no reply and their product is a
 * register change, so setting the bit would ask for an answer the ABI never
 * promised and then read its absence as a failure (rule 14).
 *
 * Built byte by byte like every other command here, so no struct layout is
 * trusted across the boundary to firmware, and the bitfield packing is
 * written out explicitly rather than left to a compiler's choice. */
#define RB_DMUB_CMD_VBIOS                 128u
#define RB_DMUB_CMD_VBIOS_DOMAIN_CONTROL   28u
#define RB_DMUB_DOMAIN_PAYLOAD              4u
#define RB_DMUB_DOMAIN_INST_BITS         0x3fu   /* inst is 6 bits */
#define RB_DMUB_DOMAIN_GATE_BIT          0x40u   /* power_gate is bit 6 */

static inline int rb_dmub_domain_inst_valid(uint32_t inst)
{
    return inst <= RB_DMUB_DOMAIN_INST_BITS ? 1 : 0;
}

static inline void rb_dmub_build_domain_control(uint8_t *buf, uint32_t inst,
                                                uint32_t gate)
{
    uint32_t i;

    for (i = 0; i < RB_DMUB_CMD_SIZE; i++) { buf[i] = 0u; }

    rb_dmub_put32(buf, 0, rb_dmub_hdr(RB_DMUB_CMD_VBIOS,
                                      RB_DMUB_CMD_VBIOS_DOMAIN_CONTROL,
                                      RB_DMUB_DOMAIN_PAYLOAD));

    buf[4] = (uint8_t)((inst & RB_DMUB_DOMAIN_INST_BITS) |
                       (gate ? RB_DMUB_DOMAIN_GATE_BIT : 0u));
    /* buf[5..7] are reserved[3] and stay zero. */
}

/* What the slot read back after the ack MEANS.
 *
 * The four answers are deliberately separate, because they point at four
 * different next steps and three of them have already happened on this
 * surface in one form or another:
 *
 *  ZERO       the 64 bytes read all zero. Our store did not land in the
 *             memory we read, or the mapping is not coherent with it. Boot
 *             273 read the REAL ring as all zeros while its registers said
 *             37 commands sat in it, so this is a measured failure mode and
 *             not a hypothetical one.
 *  UNCHANGED  byte-identical to what we sent. The write landed and stuck,
 *             and the firmware did not reply - which is the EXPECTED answer
 *             for an ABM command, since those do not set ret_status. It is
 *             inconclusive about the address, so it must not read as either
 *             success or failure.
 *  CONFIRMED  same type and sub_type, ret_status 1 -> 0. Only the DMCUB
 *             clears that bit, and it cleared it here. The mailbox address
 *             is identified at the byte level.
 *  CHANGED    the bytes differ but not that way. Something writes this
 *             memory and it is not doing what dc_dmub_srv.c describes -
 *             worth reading the bytes rather than guessing, so the caller
 *             publishes both buffers and this verdict says only "look".
 *
 * `sent` must be the 64 bytes handed to the push, not a rebuild of them. */
enum {
    RB_DMUB_REPLY_NONE      = 0,
    RB_DMUB_REPLY_ZERO      = 1,
    RB_DMUB_REPLY_UNCHANGED = 2,
    RB_DMUB_REPLY_CONFIRMED = 3,
    RB_DMUB_REPLY_CHANGED   = 4
};

static inline int rb_dmub_reply_verdict(const uint8_t *sent,
                                        const uint8_t *got)
{
    uint32_t i;
    int all_zero = 1, same = 1;
    uint32_t hs, hg;

    for (i = 0; i < RB_DMUB_CMD_SIZE; i++) {
        if (got[i] != 0u)      { all_zero = 0; }
        if (got[i] != sent[i]) { same = 0; }
    }
    if (all_zero) { return RB_DMUB_REPLY_ZERO; }
    if (same)     { return RB_DMUB_REPLY_UNCHANGED; }

    hs = rb_dmub_get32(sent, 0);
    hg = rb_dmub_get32(got, 0);
    if (rb_dmub_hdr_type(hg) == rb_dmub_hdr_type(hs) &&
        rb_dmub_hdr_sub(hg)  == rb_dmub_hdr_sub(hs)  &&
        rb_dmub_hdr_ret(hs) == 1u && rb_dmub_hdr_ret(hg) == 0u) {
        return RB_DMUB_REPLY_CONFIRMED;
    }
    return RB_DMUB_REPLY_CHANGED;
}

/* The level a userspace trigger asked for, bounded. 0 is a legal request -
 * it is the one end of the range the panel can actually be seen to take -
 * so the only refusal is above full scale. */
static inline int rb_dmub_level_ok(long v)
{
    return v >= 0 && v <= (long)RB_DMUB_LEVEL_FULL;
}

#endif /* RB_DMUB_H */
