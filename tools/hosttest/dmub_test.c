/* 2zn: the DMCUB inbox decisions (src/common/rb_dmub.h).
 *
 * Every judgement the kext makes about the ring is here: where it is, whether
 * the eight registers that say so are self-consistent, whether a slot is
 * free, and the exact bytes of a command. The kext maps memory and stores to
 * it and decides nothing (rule 38), so if this file passes, the only thing
 * left that hardware can refute is the premise - and the premise has its own
 * instrument, the bring-up scan.
 *
 * THE REGISTER VALUES BELOW ARE CONSTRUCTED, NOT MEASURED, except the four
 * boot 271 read (inbox base 0x80000000, size 8192, wptr == rptr == 2368) and
 * FB_OFFSET 0x780, which is the carve-out base this project measured four
 * ways. The CW4 offset pair has never been read on this machine - reading it
 * is what the next boot is for - so it is a plausible value here and nothing
 * in this file may be quoted as a hardware fact (rule 17).
 */
#include <stdio.h>
#include <string.h>

#include "rb_dmub.h"

#define CHECK(cond, what) do { \
    printf((cond) ? "ok   %s\n" : "FAIL %s\n", (what)); \
    failures += !(cond); \
} while (0)

/* Boot 271's two measured inbox registers, boot-time FB_OFFSET, and a
 * CONSTRUCTED CW4 offset pair inside the carve-out. */
static void fill_plausible(struct rb_dmub_loc *l)
{
    memset(l, 0, sizeof(*l));
    l->inbox_base  = 0x80000000u;    /* measured, boot 271 */
    l->inbox_size  = 8192u;          /* measured, boot 271 */
    l->cw4_base    = 0x80000000u;
    l->cw4_top     = 0x80000000u + 16384u;  /* 2 * DMUB_RB_SIZE */
    l->cw4_off_lo  = 0x8abc0000u;    /* CONSTRUCTED */
    l->cw4_off_hi  = 0x7u;           /* CONSTRUCTED: pa 0x7_8abc_0000 */
    l->fb_location = 0xf400u;        /* measured: MC base 0xF4_0000_0000 */
    l->fb_offset   = 0x780u;         /* measured: sysphys 0x7_8000_0000 */
}

int dmub_tests(void);

int dmub_tests(void)
{
    int failures = 0;
    struct rb_dmub_loc l;
    uint8_t cmd[RB_DMUB_CMD_SIZE];

    printf("-- dmub (2zn) --\n");

    /* The sizes are the ABI, not a choice. */
    CHECK(RB_DMUB_CMD_SIZE == 64u, "a dmub_rb_cmd is 64 bytes");
    CHECK(RB_DMUB_RB_SIZE == 8192u,
          "the inbox ring is 8192 bytes - exactly the DMCUB_INBOX1_SIZE "
          "boot 271 read");
    CHECK(RB_DMUB_CW4_BASE == 0x80000000u,
          "DMUB_CW4_BASE is 0x80000000 - exactly the DMCUB_INBOX1_BASE_ADDRESS "
          "boot 271 read");
    CHECK(2368u == 37u * RB_DMUB_CMD_SIZE,
          "boot 271's WPTR of 2368 is 37 whole commands, which is why the "
          "ring is believed to hold 37 of the firmware's own");

    /* --- locate --- */
    fill_plausible(&l);
    CHECK(rb_dmub_locate(&l) == RB_DMUB_LOC_OK,
          "a self-consistent window set locates");
    CHECK(rb_dmub_ring_pa(&l) == 0x78abc0000ull,
          "the ring's system-physical address is the CW4 offset pair");
    CHECK(rb_dmub_fb_offset(&l) == 0x780000000ull,
          "FB_OFFSET 0x780 is the carve-out at system-physical 0x7_8000_0000");
    CHECK(rb_dmub_fb_base(&l) == 0xf400000000ull,
          "FB_LOCATION_BASE 0xf400 is the MC base 0xF4_0000_0000");
    CHECK(rb_dmub_ring_fb_off(&l) == 0xabc0000ull,
          "the ring's offset inside the carve-out is pa - FB_OFFSET, which "
          "2pr's BAR0-offset-0 finding makes its BAR0 offset too");

    fill_plausible(&l);
    l.cw4_base = 0x81000000u;
    CHECK(rb_dmub_locate(&l) == RB_DMUB_LOC_INBOX_BASE,
          "an inbox base that is not CW4's base is refused - dmub_srv.c "
          "makes them equal, so a mismatch means a different layout");

    fill_plausible(&l);
    l.inbox_base = 0x90000000u;
    l.cw4_base = 0x90000000u;
    CHECK(rb_dmub_locate(&l) == RB_DMUB_LOC_INBOX_BASE,
          "a consistent pair that is not DMUB_CW4_BASE is still refused");

    fill_plausible(&l);
    l.inbox_size = 4096u;
    CHECK(rb_dmub_locate(&l) == RB_DMUB_LOC_INBOX_SIZE,
          "an inbox of 4096 is refused - the ring geometry is the ABI");

    fill_plausible(&l);
    l.cw4_top = 0x80000000u + 4096u;
    CHECK(rb_dmub_locate(&l) == RB_DMUB_LOC_CW4_WINDOW,
          "a CW4 window too small to hold the ring is refused");

    fill_plausible(&l);
    l.cw4_top = 0x80000000u;
    CHECK(rb_dmub_locate(&l) == RB_DMUB_LOC_CW4_WINDOW,
          "an empty CW4 window is refused rather than underflowing");

    fill_plausible(&l);
    l.cw4_off_lo = 0u; l.cw4_off_hi = 0u;
    CHECK(rb_dmub_locate(&l) == RB_DMUB_LOC_NO_OFFSET,
          "a zero CW4 offset pair means no mailbox was ever placed");

    /* The two wrong-address cases that matter, because they are the two
     * values actually lying around in the neighbouring registers: the
     * DMCUB-side window address, and the MC address. If the derivation is
     * ever rewritten to use the wrong one, this is what catches it. */
    fill_plausible(&l);
    l.cw4_off_lo = 0x80000000u; l.cw4_off_hi = 0u;
    CHECK(rb_dmub_locate(&l) == RB_DMUB_LOC_FB,
          "a DMCUB-side window address (0x80000000) is below the carve-out "
          "and refused");

    fill_plausible(&l);
    l.cw4_off_lo = 0x00000000u; l.cw4_off_hi = 0xf4u;
    CHECK(rb_dmub_locate(&l) == RB_DMUB_LOC_FB,
          "an MC address (0xF4_0000_0000) is above the carve-out and refused");

    fill_plausible(&l);
    l.fb_offset = 0u;
    CHECK(rb_dmub_locate(&l) == RB_DMUB_LOC_FB,
          "a zero FB_OFFSET gives no carve-out to check against, so the "
          "locate refuses instead of accepting everything");

    fill_plausible(&l);
    l.cw4_off_lo = 0x8abc0020u;
    CHECK(rb_dmub_locate(&l) == RB_DMUB_LOC_ALIGN,
          "a ring that is not 64-byte aligned is not this ring");

    /* --- ring arithmetic --- */
    CHECK(rb_dmub_next_wptr(2368u, 8192u) == 2432u,
          "the next WPTR after boot 271's is one command on");
    CHECK(rb_dmub_next_wptr(8128u, 8192u) == 0u,
          "the last slot wraps to zero");
    CHECK(rb_dmub_next_wptr(100u, 0u) == 100u,
          "a zero capacity cannot advance, and does not divide by zero");

    CHECK(!rb_dmub_rb_full(2368u, 2368u, 8192u),
          "boot 271's empty ring is not full");
    CHECK(rb_dmub_rb_full(8128u, 0u, 8192u),
          "127 outstanding commands is full - one slot is always unusable");
    CHECK(!rb_dmub_rb_full(8064u, 0u, 8192u),
          "126 outstanding commands is not full");
    CHECK(rb_dmub_rb_full(64u, 128u, 8192u),
          "full is recognised across the wrap too");
    CHECK(rb_dmub_rb_full(0u, 0u, 0u),
          "a zero capacity reads as full, so nothing is ever pushed into it");

    CHECK(rb_dmub_ptr_ok(2368u, 8192u),
          "boot 271's 2368 is a legal ring position");
    CHECK(rb_dmub_ptr_ok(0u, 8192u), "zero is a legal ring position");
    CHECK(!rb_dmub_ptr_ok(8192u, 8192u),
          "the capacity itself is one past the end");
    CHECK(!rb_dmub_ptr_ok(2360u, 8192u),
          "a position that is not a multiple of 64 is refused - the pointers "
          "are the only evidence the ring is where we think it is");
    CHECK(!rb_dmub_ptr_ok(0u, 0u), "no position is legal in a zero ring");

    /* --- the header word --- */
    CHECK(rb_dmub_hdr(66u, 2u, 32u) == 0x20000242u,
          "ABM/SET_BACKLIGHT/32 assembles to 0x20000242 - type at bits 0-7, "
          "sub_type at 8-15, payload_bytes at 24-29");
    CHECK(rb_dmub_hdr_type(0x20000242u) == 66u, "the type reads back");
    CHECK(rb_dmub_hdr_sub(0x20000242u) == 2u, "the sub_type reads back");
    CHECK(rb_dmub_hdr_payload(0x20000242u) == 32u,
          "the payload length reads back");
    CHECK(rb_dmub_hdr_payload(0xffffffffu) == 63u,
          "payload_bytes is six bits wide and does not bleed into reserved1");

    CHECK(rb_dmub_hdr_plausible(0x20000242u),
          "a real ABM command header is plausible");
    CHECK(!rb_dmub_hdr_plausible(0u),
          "an all-zero word is not - DMUB_CMD__NULL is 0 and so is unwritten "
          "memory, which is the whole point of the control");
    CHECK(!rb_dmub_hdr_plausible(0x3d000042u),
          "a payload of 61 bytes cannot fit beside a 4-byte header");
    CHECK(rb_dmub_hdr_plausible(0x3c000042u),
          "a payload of 60 bytes is the inclusive maximum");
    CHECK(rb_dmub_hdr_plausible(0x007a004au),
          "a command type this file has never heard of is still plausible - "
          "the control must not reject the firmware's own traffic");

    /* --- the bytes --- */
    memset(cmd, 0xaa, sizeof(cmd));
    rb_dmub_build_set_backlight(cmd, 0x8000u, 0u, 0x01u);
    /* Asserted as literal bytes rather than through rb_dmub_get32 and
     * rb_dmub_hdr, which would only prove the builder agrees with itself
     * (the 2u9 tautology). */
    CHECK(cmd[0] == 66u && cmd[1] == 2u && cmd[2] == 0u && cmd[3] == 32u,
          "SET_BACKLIGHT's header bytes are 66 / 2 / 0 / 32");
    CHECK(cmd[4] == 0u && cmd[5] == 0u && cmd[6] == 0u && cmd[7] == 0u,
          "frame_ramp 0 - an immediate change, as Linux passes");
    CHECK(cmd[8] == 0x00u && cmd[9] == 0x80u && cmd[10] == 0u && cmd[11] == 0u,
          "backlight_user_level is little-endian u16.16 at offset 8");
    CHECK(cmd[12] == 1u, "version is DMUB_CMD_ABM_CONTROL_VERSION_1");
    CHECK(cmd[13] == 1u, "panel_mask bit 0 is panel control instance 0");
    CHECK(cmd[14] == 0u && cmd[15] == 0u, "aux_inst and its pad are zero");
    CHECK(cmd[16] == 0u && cmd[17] == 0u && cmd[18] == 0u && cmd[19] == 0u,
          "backlight_control_type is PWM (0), not either AUX method");
    {
        int tail_zero = 1;
        uint32_t i;
        for (i = 20u; i < RB_DMUB_CMD_SIZE; i++) {
            if (cmd[i] != 0u) { tail_zero = 0; }
        }
        CHECK(tail_zero,
              "the four luminance/pwm bounds and all padding are zero, which "
              "is what dmub_abm_lcd.c leaves them for a PWM panel - a value "
              "invented for a field whose consumer we have not read is the "
              "recurring error");
    }
    CHECK(RB_DMUB_SET_BL_PAYLOAD == 32u,
          "the SET_BACKLIGHT payload is 32 bytes: 4+4+1+1+1+1+4+4+4+4+4");

    memset(cmd, 0xaa, sizeof(cmd));
    rb_dmub_build_set_pipe(cmd, 0u, 0u, 0u, 1u);
    CHECK(cmd[0] == 66u && cmd[1] == 1u && cmd[2] == 0u && cmd[3] == 8u,
          "SET_PIPE's header bytes are 66 / 1 / 0 / 8");
    CHECK(cmd[4] == 0u && cmd[5] == 0u, "otg_inst and panel_inst are 0");
    CHECK(cmd[6] == 1u, "set_pipe_option is SET_ABM_PIPE_NORMAL");
    CHECK(cmd[7] == 0u, "ramping_boundary is unused upstream and left zero");
    CHECK(cmd[8] == 0u, "pwrseq_inst is 0 - PWRSEQ0 is the block every "
          "milestone since 2xs has read and written");
    {
        int tail_zero = 1;
        uint32_t i;
        for (i = 9u; i < RB_DMUB_CMD_SIZE; i++) {
            if (cmd[i] != 0u) { tail_zero = 0; }
        }
        CHECK(tail_zero, "SET_PIPE's padding is zero to the end of the slot");
    }

    /* --- the trigger's bound --- */
    CHECK(rb_dmub_level_ok(0),
          "0 is a legal request - it is the one end of the range a human can "
          "actually see the panel take");
    CHECK(rb_dmub_level_ok(65536), "0x10000 is full scale and legal");
    CHECK(!rb_dmub_level_ok(65537), "above full scale is refused");
    CHECK(!rb_dmub_level_ok(-1),
          "a negative request is refused, so a signed read cannot wrap into "
          "an enormous level");
    CHECK(RB_DMUB_LEVEL_FULL == 0x10000u,
          "full scale is u16.16, the same units rb_backlight.h already uses");

    return failures;
}
