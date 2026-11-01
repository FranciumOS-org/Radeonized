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

/* Boot 272's OWN CW4 registers, measured, and boot 271's inbox pair.
 *
 * CW4 base 0x04000000, top 0x8400ffff (enable set, address 0x0400ffff) and
 * offset 0xf4_ffe40000 are what the machine printed - and they are why this
 * file was rewritten: CW4 is a 64 KB window somewhere else entirely, so the
 * inbox at DMCUB-side 0x80000000 is mapped by a DIFFERENT window, and the
 * offset is an MC address with no translation applied. WHICH window covers
 * it is still unknown, so the covering window below is CONSTRUCTED and
 * nothing about window 3 may be quoted as a hardware fact (rule 17). */
#define B272_CW4_BASE  0x04000000u
#define B272_CW4_TOP   0x8400ffffu
#define B272_CW4_OFFLO 0xffe40000u
#define B272_CW4_OFFHI 0xf4u

static void fill_measured(struct rb_dmub_loc *l)
{
    memset(l, 0, sizeof(*l));
    l->inbox_base  = 0x80000000u;    /* measured, boot 271 */
    l->inbox_size  = 8192u;          /* measured, boot 271 */
    l->fb_location = 0xf400u;        /* measured: MC base 0xF4_0000_0000 */
    l->fb_offset   = 0x780u;         /* measured: sysphys 0x7_8000_0000 */
    l->cw[4].base   = B272_CW4_BASE; /* measured, boot 272 */
    l->cw[4].top    = B272_CW4_TOP;
    l->cw[4].off_lo = B272_CW4_OFFLO;
    l->cw[4].off_hi = B272_CW4_OFFHI;
}

int dmub_tests(void);

int dmub_tests(void)
{
    int failures = 0;
    struct rb_dmub_loc l;
    struct rb_dmub_loc_out o;
    uint8_t cmd[RB_DMUB_CMD_SIZE];

    printf("-- dmub (2zn) --\n");

    /* The sizes are the ABI, not a choice. */
    CHECK(RB_DMUB_CMD_SIZE == 64u, "a dmub_rb_cmd is 64 bytes");
    CHECK(RB_DMUB_RB_SIZE == 8192u,
          "Linux's ring is 8192 bytes - the same DMCUB_INBOX1_SIZE boot 271 "
          "read, but the REGISTER is the authority and supplies the capacity");
    CHECK(2368u == 37u * RB_DMUB_CMD_SIZE,
          "boot 271's WPTR of 2368 is 37 whole commands, which is why the "
          "ring is believed to hold 37 of the firmware's own");

    /* --- THE REGRESSION TESTS. Boot 272's registers, nothing constructed.
     *
     * Both errors this milestone made are nailed down here: the constant
     * that was never checked against its header, and the convention that
     * named a window instead of looking for one. --- */
    CHECK(RB_DMUB_CW4_BASE == 0x64000000u,
          "DMUB_CW4_BASE is 0x64000000 (dmub_srv.c:84) - 2zn had 0x80000000, "
          "copied from the register it was meant to be checked against, and "
          "the old hosttest asserted the two were equal, which compared a "
          "reading with itself");
    CHECK(RB_DMUB_CW_ADDR_MASK == 0x1fffffffu,
          "a window address field is 29 bits (sh_mask.h:5881), so an address "
          "is written TRUNCATED");
    CHECK((RB_DMUB_CW4_BASE & RB_DMUB_CW_ADDR_MASK) == B272_CW4_BASE,
          "and 0x64000000 truncated IS boot 272's CW4_BASE of 0x04000000 - "
          "so CW4 really is the mailbox window, 64 KB of it");

    fill_measured(&l);
    CHECK(rb_dmub_win_enabled(&l.cw[4]),
          "boot 272's CW4 is enabled");
    CHECK(rb_dmub_win_top(&l.cw[4]) == 0x0400ffffu,
          "its top address is 0x0400ffff once the enable bit at 31 is masked "
          "off with the 29-bit field mask");
    CHECK(rb_dmub_win_span(&l.cw[4]) == 8192u,
          "its 64 KB window yields an 8192-byte candidate ring - Linux puts "
          "the inbox in the first DMUB_RB_SIZE of the mailbox");
    CHECK(rb_dmub_win_off(&l.cw[4]) == 0xf4ffe40000ull,
          "and its offset is 0xf4_ffe40000, an MC address - so this "
          "machine's FIRMWARE did not apply dmub_dcn31.c's translation");

    CHECK(rb_dmub_win_locate(&l, 4, &o) == RB_DMUB_LOC_OK,
          "boot 272's own CW4 resolves to mappable memory");
    CHECK(o.win == 4 && o.addr_kind == RB_DMUB_ADDR_MC,
          "...classified as MC by RANGE, not assumed either way");
    CHECK(o.dmub_mem == 0xf4ffe40000ull && o.span == 8192u,
          "...carrying the address the register holds and the span to scan");
    CHECK(o.ring_pa == 0x87fe40000ull,
          "...and mc2pa() gives system-physical 0x8_7FE4_0000: "
          "mem - 0xF4_0000_0000 + 0x7_8000_0000");
    CHECK(rb_dmub_ring_fb_off(&l, &o) == 0xffe40000ull,
          "...at 0xffe40000 into the carve-out, which 2pr's BAR0-offset-0 "
          "finding makes its BAR0 offset too");

    CHECK(l.inbox_base != RB_DMUB_CW4_BASE &&
          l.inbox_base != (RB_DMUB_CW4_BASE & RB_DMUB_CW_ADDR_MASK),
          "DMCUB_INBOX1_BASE_ADDRESS reads 0x80000000, which is NEITHER "
          "0x64000000 nor its truncation - an open finding, and the reason "
          "nothing locates from that register any more");

    /* Every other window on boot 272 was zero, so none of them resolves -
     * and a search that found a candidate in unprogrammed registers would be
     * the error this whole rewrite exists to prevent. */
    {
        int others = 0;
        unsigned i;
        for (i = 0; i < RB_DMUB_CW_COUNT; i++) {
            if (i == 4u) { continue; }
            if (rb_dmub_win_locate(&l, i, &o) == RB_DMUB_LOC_OK) { others++; }
        }
        CHECK(others == 0,
              "and no OTHER window on boot 272 resolves at all - zeroed "
              "registers are not candidates");
    }

    /* --- the signature, which is now the whole decision --- */
    CHECK(rb_dmub_sig_ok(37u, 0u),
          "boot 271's 37 commands with nothing after them is a mailbox");
    CHECK(rb_dmub_sig_ok(2u, 0u),
          "two consecutive headers is the floor - one is ~1 in 5 by accident "
          "at this test's strength, two at a 64-byte stride is ~1 in 25");
    CHECK(!rb_dmub_sig_ok(1u, 0u),
          "one header alone is not enough to call a window a ring");
    CHECK(!rb_dmub_sig_ok(0u, 0u),
          "an all-empty window is not a ring, and must not read as one just "
          "because nothing contradicted it");
    CHECK(!rb_dmub_sig_ok(37u, 1u),
          "a single non-header, non-empty slot after the run disqualifies "
          "it - a ring written once and never wrapped has none");

    /* --- the resolve's refusals --- */
    fill_measured(&l);
    l.cw[4].top &= ~RB_DMUB_CW_ENABLE;
    CHECK(rb_dmub_win_locate(&l, 4, &o) == RB_DMUB_LOC_NO_WINDOW,
          "a window with the enable bit CLEAR is not mapped, whatever its "
          "address says");

    fill_measured(&l);
    l.cw[4].off_lo = 0u; l.cw[4].off_hi = 0u;
    CHECK(rb_dmub_win_locate(&l, 4, &o) == RB_DMUB_LOC_NO_OFFSET,
          "an enabled window with a zero offset pair maps nowhere");

    fill_measured(&l);
    l.cw[4].off_lo = 0x8abc0000u; l.cw[4].off_hi = 0x7u;
    CHECK(rb_dmub_win_locate(&l, 4, &o) == RB_DMUB_LOC_OK &&
          o.addr_kind == RB_DMUB_ADDR_PA && o.ring_pa == 0x78abc0000ull,
          "an offset already inside the system-physical range is taken as "
          "such and NOT translated a second time - dmub_dcn31.c's own "
          "translation produces this, so a firmware update could start");

    fill_measured(&l);
    l.cw[4].off_lo = 0u; l.cw[4].off_hi = 0x1u;      /* 0x1_0000_0000 */
    CHECK(rb_dmub_win_locate(&l, 4, &o) == RB_DMUB_LOC_ADDR,
          "an address in neither range is refused - the two are disjoint "
          "precisely so this cannot be guessed at");

    fill_measured(&l);
    l.cw[4].off_lo = 0xffe40020u;
    CHECK(rb_dmub_win_locate(&l, 4, &o) == RB_DMUB_LOC_ALIGN,
          "a ring that is not 64-byte aligned is not this ring");

    fill_measured(&l);
    l.fb_offset = 0u;
    CHECK(rb_dmub_win_locate(&l, 4, &o) == RB_DMUB_LOC_FB,
          "a zero FB_OFFSET gives no range to classify against, so the "
          "resolve refuses instead of accepting everything");
    fill_measured(&l);
    l.fb_location = 0u;
    CHECK(rb_dmub_win_locate(&l, 4, &o) == RB_DMUB_LOC_FB,
          "and a zero FB_LOCATION_BASE likewise");

    fill_measured(&l);
    CHECK(rb_dmub_win_locate(&l, RB_DMUB_CW_COUNT, &o) == RB_DMUB_LOC_NO_WINDOW,
          "an index past the last window is refused, not read");

    fill_measured(&l);
    l.cw[4].top = (l.cw[4].base & RB_DMUB_CW_ADDR_MASK) | RB_DMUB_CW_ENABLE;
    CHECK(rb_dmub_win_span(&l.cw[4]) == 0u &&
          rb_dmub_win_locate(&l, 4, &o) == RB_DMUB_LOC_NO_WINDOW,
          "a window whose top equals its base spans nothing, and does not "
          "underflow into an enormous span");

    /* A window larger than Linux's ring is clamped rather than scanned
     * whole; a smaller one is taken at its own size. */
    fill_measured(&l);
    l.cw[4].top = ((l.cw[4].base & RB_DMUB_CW_ADDR_MASK) + 0xfffffu) |
                  RB_DMUB_CW_ENABLE;
    CHECK(rb_dmub_win_span(&l.cw[4]) == RB_DMUB_RB_SIZE,
          "a 1 MB window still yields an 8192-byte candidate ring");
    fill_measured(&l);
    l.cw[4].top = ((l.cw[4].base & RB_DMUB_CW_ADDR_MASK) + 0xfffu) |
                  RB_DMUB_CW_ENABLE;
    CHECK(rb_dmub_win_span(&l.cw[4]) == 4096u,
          "and a 4 KB window yields 4096 - the REGISTERS supply the size, "
          "not a constant copied out of Linux");
    fill_measured(&l);
    l.cw[4].top = ((l.cw[4].base & RB_DMUB_CW_ADDR_MASK) + 0x6fu) |
                  RB_DMUB_CW_ENABLE;
    CHECK(rb_dmub_win_span(&l.cw[4]) == 64u,
          "a 112-byte window is rounded DOWN to one whole command, so a "
          "partial slot is never scanned or written");

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
