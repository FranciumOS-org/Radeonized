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

/* BOOT 273's OWN WINDOW TABLE, all seven, measured - plus boot 271's inbox
 * pair and the carve-out registers. Nothing here is constructed.
 *
 * Six of the seven sit exactly where dmub_srv.c:81-86 puts them after the
 * 29-bit truncation, which is the identification; CW2 is disabled, which is
 * correct because dmub_srv.c:873 bases it inside CW0. The per-window reads
 * from the same boot are in the comments: CW5 scored 128 plausible slots of
 * 128 with no empties (it is DMUB_WINDOW_5_TRACEBUFF) and CW4 - the real
 * mailbox - read ALL ZEROS. */
static void fill_boot273(struct rb_dmub_loc *l)
{
    memset(l, 0, sizeof(*l));
    l->inbox_base  = 0x80000000u;    /* boot 271, and still unexplained */
    l->inbox_size  = 8192u;
    l->fb_location = 0xf400u;        /* MC base 0xF4_0000_0000 */
    l->fb_offset   = 0x780u;         /* sysphys 0x7_8000_0000 */

    /* base, top (enable at 31), off_lo, off_hi */
    l->cw[0].base = 0x00000000u; l->cw[0].top = 0x8002d08fu;
    l->cw[0].off_lo = 0x7ffb0000u; l->cw[0].off_hi = 0x8u;   /* run 0, bad 98 */
    l->cw[1].base = 0x01000000u; l->cw[1].top = 0x810d2f6fu;
    l->cw[1].off_lo = 0x7ffdd100u; l->cw[1].off_hi = 0x8u;   /* run 17, bad 111 */
    /* CW2 disabled, all zero */
    l->cw[3].base = 0x03000000u; l->cw[3].top = 0x8301ffffu;
    l->cw[3].off_lo = 0xffe20000u; l->cw[3].off_hi = 0xf4u;  /* run 1, bad 67 */
    l->cw[4].base = 0x04000000u; l->cw[4].top = 0x8400ffffu;
    l->cw[4].off_lo = 0xffe40000u; l->cw[4].off_hi = 0xf4u;  /* ALL ZEROS */
    l->cw[5].base = 0x05000000u; l->cw[5].top = 0x8500ffffu;
    l->cw[5].off_lo = 0xffe50000u; l->cw[5].off_hi = 0xf4u;  /* run 128, bad 0 */
    l->cw[6].base = 0x06000000u; l->cw[6].top = 0x8600ffffu;
    l->cw[6].off_lo = 0xffe60000u; l->cw[6].off_hi = 0xf4u;  /* run 0, bad 7 */
}

#define B272_CW4_BASE  0x04000000u

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

    /* --- THE IDENTIFICATION. Boot 273's table, nothing constructed. --- */
    CHECK(RB_DMUB_CW4_BASE == 0x64000000u,
          "DMUB_CW4_BASE is 0x64000000 (dmub_srv.c:84) - 2zn had 0x80000000, "
          "copied from the register it was meant to be checked against, and "
          "asserted the two were equal, comparing a reading with itself");
    CHECK(RB_DMUB_CW_ADDR_MASK == 0x1fffffffu,
          "a window address field is 29 bits (sh_mask.h:5881), so an address "
          "is written TRUNCATED");

    fill_boot273(&l);
    CHECK(rb_dmub_layout_score(&l) == 6u,
          "boot 273 puts SIX of the seven windows exactly where dmub_srv.c "
          "does - that is the identification, and rule 18's answer to two "
          "numbers agreeing");
    CHECK(rb_dmub_mailbox_win(&l) == 4,
          "so the mailbox is CW4, by the layout rather than by a guess about "
          "its contents");
    CHECK(!rb_dmub_win_enabled(&l.cw[2]),
          "CW2 is disabled, which is correct - dmub_srv.c:873 bases it inside "
          "CW0 rather than at a constant of its own");

    /* Each of the six, individually, so a future firmware that moves one is
     * a named failure rather than a score that quietly drops to five. */
    {
        unsigned i;
        int all = 1;
        for (i = 0; i < RB_DMUB_CW_COUNT; i++) {
            if (i == 2u) { continue; }
            if ((l.cw[i].base & RB_DMUB_CW_ADDR_MASK) !=
                (rb_dmub_cw_base_expected(i) & RB_DMUB_CW_ADDR_MASK)) {
                all = 0;
            }
        }
        CHECK(all, "and every one of the six matches its own constant, not "
              "just six of them in aggregate");
    }

    /* --- THE 2zo REGRESSION: the trace buffer must never be chosen. --- */
    CHECK(rb_dmub_mailbox_win(&l) != 5,
          "CW5 is NOT chosen - it is DMUB_WINDOW_5_TRACEBUFF, and 2zo's byte "
          "signature picked it, which is how 64 bytes went into the trace "
          "buffer");
    CHECK(!rb_dmub_sig_ok(128u, 0u, 128u),
          "and boot 273's actual CW5 reading - 128 plausible slots of 128 "
          "with no empties - is REFUSED by the signature now: a ring written "
          "37 times has 91 empty slots after the run, dense data has none");
    CHECK(rb_dmub_sig_ok(37u, 0u, 128u),
          "while 37 of 128 with a clean tail is what a ring should look like");
    CHECK(!rb_dmub_sig_ok(0u, 0u, 128u),
          "boot 273's CW4 reading - all zeros - carries no signature either, "
          "which is why the signature is an observation and NOT the selector");
    CHECK(!rb_dmub_sig_ok(17u, 111u, 128u),
          "and CW1's 17-run with 111 broken slots is not a ring");

    /* Degrade the layout and nothing is chosen at all. */
    fill_boot273(&l);
    l.cw[5].base = 0x07000000u;
    CHECK(rb_dmub_layout_score(&l) == 5u && rb_dmub_mailbox_win(&l) == -1,
          "ONE window out of place and no mailbox is chosen - a partial match "
          "is the 2zn error again, so it is refused rather than scored");

    fill_boot273(&l);
    l.cw[4].top &= ~RB_DMUB_CW_ENABLE;
    CHECK(rb_dmub_mailbox_win(&l) == -1,
          "and a disabled CW4 is no mailbox, whatever the rest of the layout "
          "says");

    /* --- CW4 resolved, off boot 273's own registers --- */
    fill_boot273(&l);
    CHECK(rb_dmub_win_locate(&l, 4, &o) == RB_DMUB_LOC_OK,
          "boot 273's CW4 resolves to mappable memory");
    CHECK(o.addr_kind == RB_DMUB_ADDR_MC,
          "...classified as MC by RANGE - this machine's FIRMWARE wrote that "
          "register and did not apply dmub_dcn31.c's translation");
    CHECK(o.dmub_mem == 0xf4ffe40000ull && o.span == 8192u,
          "...at MC 0xf4_ffe40000, 8192 bytes of its 64 KB window");
    CHECK(o.ring_pa == 0x87fe40000ull,
          "...and mc2pa() gives system-physical 0x8_7FE4_0000");
    CHECK(rb_dmub_ring_fb_off(&l, &o) == 0xffe40000ull,
          "...0xffe40000 into the carve-out, which 2pr's BAR0-offset-0 "
          "finding makes its BAR0 offset too");

    /* The window 2zo actually wrote into, so the wrong answer is on record
     * as a value and not only as prose. */
    CHECK(rb_dmub_win_locate(&l, 5, &o) == RB_DMUB_LOC_OK &&
          o.ring_pa == 0x87fe50000ull,
          "CW5 resolves to 0x8_7FE5_0000 - the trace buffer 2zo put 64 bytes "
          "into, one 64 KB window above the real mailbox");

    CHECK(l.inbox_base != RB_DMUB_CW4_BASE &&
          l.inbox_base != (RB_DMUB_CW4_BASE & RB_DMUB_CW_ADDR_MASK),
          "DMCUB_INBOX1_BASE_ADDRESS reads 0x80000000, which is NEITHER "
          "0x64000000 nor its truncation - still unexplained, and nothing "
          "locates from it");

    /* --- the resolve's refusals, on the real CW4 --- */
    fill_boot273(&l);
    l.cw[4].off_lo = 0u; l.cw[4].off_hi = 0u;
    CHECK(rb_dmub_win_locate(&l, 4, &o) == RB_DMUB_LOC_NO_OFFSET,
          "an enabled window with a zero offset pair maps nowhere");

    fill_boot273(&l);
    l.cw[4].off_lo = 0x8abc0000u; l.cw[4].off_hi = 0x7u;
    CHECK(rb_dmub_win_locate(&l, 4, &o) == RB_DMUB_LOC_OK &&
          o.addr_kind == RB_DMUB_ADDR_PA && o.ring_pa == 0x78abc0000ull,
          "an offset already inside the system-physical range is taken as "
          "such and NOT translated twice - dmub_dcn31.c's own translation "
          "produces this, so a firmware update could start");

    fill_boot273(&l);
    l.cw[4].off_lo = 0u; l.cw[4].off_hi = 0x1u;
    CHECK(rb_dmub_win_locate(&l, 4, &o) == RB_DMUB_LOC_ADDR,
          "an address in neither range is refused - the two are disjoint "
          "precisely so this cannot be guessed at");

    fill_boot273(&l);
    l.cw[4].off_lo = 0xffe40020u;
    CHECK(rb_dmub_win_locate(&l, 4, &o) == RB_DMUB_LOC_ALIGN,
          "a ring that is not 64-byte aligned is not this ring");

    fill_boot273(&l);
    l.fb_offset = 0u;
    CHECK(rb_dmub_win_locate(&l, 4, &o) == RB_DMUB_LOC_FB,
          "a zero FB_OFFSET gives no range to classify against");
    fill_boot273(&l);
    l.fb_location = 0u;
    CHECK(rb_dmub_win_locate(&l, 4, &o) == RB_DMUB_LOC_FB,
          "and a zero FB_LOCATION_BASE likewise");

    fill_boot273(&l);
    CHECK(rb_dmub_win_locate(&l, RB_DMUB_CW_COUNT, &o) == RB_DMUB_LOC_NO_WINDOW,
          "an index past the last window is refused, not read");
    CHECK(rb_dmub_win_locate(&l, 2, &o) == RB_DMUB_LOC_NO_WINDOW,
          "and the disabled CW2 resolves to nothing");

    /* Spans, from the registers rather than from Linux's constant. */
    fill_boot273(&l);
    CHECK(rb_dmub_win_span(&l.cw[4]) == 8192u,
          "CW4's 64 KB window yields an 8192-byte ring - Linux puts the inbox "
          "in the first DMUB_RB_SIZE of the mailbox");
    CHECK(rb_dmub_win_span(&l.cw[0]) == RB_DMUB_RB_SIZE,
          "CW0's much larger window is clamped to the same 8192");
    l.cw[4].top = ((l.cw[4].base & RB_DMUB_CW_ADDR_MASK) + 0xfffu) |
                  RB_DMUB_CW_ENABLE;
    CHECK(rb_dmub_win_span(&l.cw[4]) == 4096u,
          "and a 4 KB window yields 4096 - the REGISTERS supply the size");
    l.cw[4].top = ((l.cw[4].base & RB_DMUB_CW_ADDR_MASK) + 0x6fu) |
                  RB_DMUB_CW_ENABLE;
    CHECK(rb_dmub_win_span(&l.cw[4]) == 64u,
          "a 112-byte window is rounded DOWN to one whole command, so a "
          "partial slot is never scanned or written");
    fill_boot273(&l);
    l.cw[4].top = (l.cw[4].base & RB_DMUB_CW_ADDR_MASK) | RB_DMUB_CW_ENABLE;
    CHECK(rb_dmub_win_span(&l.cw[4]) == 0u &&
          rb_dmub_win_locate(&l, 4, &o) == RB_DMUB_LOC_NO_WINDOW,
          "a window whose top equals its base spans nothing, and does not "
          "underflow into an enormous span");

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

    /* --- 2zq: QUERY_FEATURE_CAPS, the byte-level identification ---
     *
     * The command's bytes are asserted as LITERALS and not through this
     * file's own accessors: 2zp's hosttest asserted that a constant equalled
     * a register reading and thereby compared a reading with itself, and the
     * same shape here would be rb_dmub_hdr() checked against rb_dmub_hdr(). */
    CHECK(RB_DMUB_CMD_QUERY_FEATURE_CAPS == 6u,
          "DMUB_CMD__QUERY_FEATURE_CAPS is 6 in dmub_cmd_type");
    CHECK(RB_DMUB_CAPS_PAYLOAD == 14u,
          "struct dmub_feature_caps is 14 packed uint8_t fields: psr, "
          "fw_assisted_mclk_switch_ver, reserved[4], subvp_psr_support, "
          "gecc_enable, replay_supported, replay_reserved[3], "
          "abm_aux_backlight_support, lsdma_support_in_dmu");
    CHECK(RB_DMUB_HDR_RET_STATUS == 0x10000u,
          "ret_status is bit 16 - after type:8 and sub_type:8");
    CHECK(RB_DMUB_CAPS_OFF_ABM_AUX_BL == 16u,
          "abm_aux_backlight_support sits at payload byte 12, slot byte 16");
    CHECK(RB_DMUB_CAPS_OFF_LSDMA == 17u, "lsdma_support_in_dmu follows it");

    {
        uint8_t caps[RB_DMUB_CMD_SIZE];
        uint8_t got[RB_DMUB_CMD_SIZE];
        uint32_t i;
        int tail_zero = 1;

        rb_dmub_build_query_caps(caps);
        CHECK(caps[0] == 6u && caps[1] == 0u,
              "QUERY_FEATURE_CAPS's type byte is 6 and sub_type is 0");
        CHECK(caps[2] == 1u,
              "ret_status is SET on send - dc_dmub_srv_query_caps_cmd sets "
              "it to 1 and then accepts the reply only if it reads 0, so the "
              "firmware clears it and that bit is the identification");
        CHECK(caps[3] == 14u, "payload_bytes is 14");
        for (i = 4u; i < RB_DMUB_CMD_SIZE; i++) {
            if (caps[i] != 0u) { tail_zero = 0; }
        }
        CHECK(tail_zero,
              "the payload is left zero - it is the firmware's to fill in");

        /* ZERO: the slot reads empty. This is boot 273's measured failure
         * mode, not a hypothetical one. */
        for (i = 0; i < RB_DMUB_CMD_SIZE; i++) { got[i] = 0u; }
        CHECK(rb_dmub_reply_verdict(caps, got) == RB_DMUB_REPLY_ZERO,
              "an all-zero slot is ZERO - our bytes are not in the memory we "
              "read, which is exactly what boot 273 measured of the real ring");

        /* UNCHANGED: the write stuck and nobody replied. The expected answer
         * for an ABM command, and it must not read as success OR failure. */
        for (i = 0; i < RB_DMUB_CMD_SIZE; i++) { got[i] = caps[i]; }
        CHECK(rb_dmub_reply_verdict(caps, got) == RB_DMUB_REPLY_UNCHANGED,
              "a byte-identical slot is UNCHANGED - inconclusive, which is "
              "its own answer and not a verdict either way");

        /* CONFIRMED: ret_status cleared, type and sub_type intact. */
        got[2] = 0u;
        CHECK(rb_dmub_reply_verdict(caps, got) == RB_DMUB_REPLY_CONFIRMED,
              "ret_status 1 -> 0 with the type intact is CONFIRMED - only "
              "the DMCUB clears that bit, in memory we chose the address of");

        /* ...and the clear alone is not enough: the command must still be
         * recognisable, or we are reading something that merely happens to
         * have a zero there. */
        got[0] = 99u;
        CHECK(rb_dmub_reply_verdict(caps, got) == RB_DMUB_REPLY_CHANGED,
              "a cleared ret_status with a DIFFERENT type is only CHANGED - "
              "a zero bit in unrelated bytes is not a reply");

        /* A payload written with ret_status left set is CHANGED, not
         * CONFIRMED: the bit is the claim, not the data beside it. */
        for (i = 0; i < RB_DMUB_CMD_SIZE; i++) { got[i] = caps[i]; }
        got[RB_DMUB_CAPS_OFF_ABM_AUX_BL] = 1u;
        CHECK(rb_dmub_reply_verdict(caps, got) == RB_DMUB_REPLY_CHANGED,
              "payload bytes appearing with ret_status still set is CHANGED");

        /* An ABM command can never come back CONFIRMED, because it never
         * sets ret_status - so the verdict cannot flatter the thing we are
         * actually trying to make work. */
        {
            uint8_t bl[RB_DMUB_CMD_SIZE];
            rb_dmub_build_set_backlight(bl, 4096u, 0u, 0x01u);
            CHECK(rb_dmub_hdr_ret(rb_dmub_get32(bl, 0)) == 0u,
                  "SET_BACKLIGHT does not set ret_status - dmub_abm_lcd.c "
                  "does not, so no ABM send can be read as CONFIRMED");
            for (i = 0; i < RB_DMUB_CMD_SIZE; i++) { got[i] = bl[i]; }
            got[2] = 0u;
            CHECK(rb_dmub_reply_verdict(bl, got) == RB_DMUB_REPLY_UNCHANGED,
                  "clearing an already-clear ret_status changes nothing, so "
                  "it stays UNCHANGED");
        }

        CHECK(rb_dmub_hdr_ret(rb_dmub_get32(caps, 0)) == 1u,
              "the accessor reads back the bit the builder set");
        CHECK(rb_dmub_hdr_type(rb_dmub_get32(caps, 0)) == 6u &&
              rb_dmub_hdr_payload(rb_dmub_get32(caps, 0)) == 14u,
              "ret_status does not bleed into type or payload_bytes");
        CHECK(rb_dmub_hdr_plausible(rb_dmub_get32(caps, 0)),
              "the caps command passes the scan's own header test, so the "
              "scan would not reject a slot holding one");
    }

    /* --- 2zr: GPINT, the one test with no address in it ---
     *
     * Boot 275 pushed QUERY_FEATURE_CAPS into slot 0x940, RPTR followed, and
     * the slot still read `06 00 01 0e` - ret_status STILL SET. The ring
     * cannot say whether that is an unimplemented command, a wrong base, or
     * a DMCUB that is not processing commands, because every one of those
     * answers produces an advancing RPTR. GPINT has no base. */
    CHECK(RB_DMUB_GPINT_GET_FW_VERSION == 1u,
          "DMUB_GPINT__GET_FW_VERSION is 1, documented 'Queries the firmware "
          "version' - and the answer is checkable against DMCUB_SCRATCH0's "
          "4.0.15.47, read by a level-2 probe from a DIFFERENT register");
    CHECK(RB_DMUB_GPINT_STOP_FW == 2u, "DMUB_GPINT__STOP_FW is 2");
    CHECK(RB_DMUB_GPINT_STOP_RESPONSE == 0xDEADDEADu,
          "STOP_FW answers 0xDEADDEAD in SCRATCH7");

    /* THE GATE. The DMCUB is what lit this panel; halting it ends the boot. */
    CHECK(rb_dmub_gpint_allowed(RB_DMUB_GPINT_GET_FW_VERSION),
          "the version query is the one allowed code");
    CHECK(!rb_dmub_gpint_allowed(RB_DMUB_GPINT_STOP_FW),
          "STOP_FW IS REFUSED - it halts the firmware driving this panel, so "
          "this is a gate and not a comment");
    CHECK(!rb_dmub_gpint_allowed(0u), "INVALID_COMMAND is refused");
    {
        int any = 0;
        uint32_t c;
        for (c = 2u; c <= 144u; c++) {
            if (rb_dmub_gpint_allowed(c)) { any = 1; }
        }
        CHECK(!any,
              "no code from 2 to 144 is allowed - the allowlist is exactly "
              "one entry, checked exhaustively rather than by inspection");
    }

    {
        const uint32_t w = rb_dmub_gpint_word(RB_DMUB_GPINT_GET_FW_VERSION, 0u);

        /* param:16, command_code:12, status:4 - asserted as a literal word,
         * not through this file's own accessors. */
        CHECK(w == 0x10010000u,
              "the version query's word is 0x10010000: param 0, code 1 at "
              "bit 16, status 1 at bit 28");
        CHECK(rb_dmub_gpint_code(w) == 1u, "the accessor reads code 1 back");
        CHECK(rb_dmub_gpint_status(w) == 1u, "status is set on send");

        /* The ack: the firmware clears the status FIELD and leaves the rest. */
        CHECK(rb_dmub_gpint_acked(w, 0x00010000u),
              "status cleared with code and param intact is an ACK");
        CHECK(!rb_dmub_gpint_acked(w, w),
              "our own word read back unchanged is NOT an ack - the status "
              "field is still set, which is the pending state");
        CHECK(!rb_dmub_gpint_acked(w, 0u),
              "a zero readback is NOT an ack, though it has a clear status "
              "field - the code must survive, or a dead register reads as a "
              "reply (rule 14: absence is not a result)");
        CHECK(!rb_dmub_gpint_acked(w, 0xffffffffu),
              "all-ones is not an ack");
        CHECK(!rb_dmub_gpint_acked(w, 0x00020000u),
              "a cleared status under a DIFFERENT code is not our ack");

        /* A parameter rides in the low 16 bits and must not reach the code. */
        CHECK(rb_dmub_gpint_word(1u, 0xffffu) == 0x1001ffffu,
              "a full 16-bit parameter does not bleed into command_code");
        CHECK(rb_dmub_gpint_code(rb_dmub_gpint_word(1u, 0xffffu)) == 1u,
              "...and the code still reads 1 beneath it");
    }

    /* The registers, against docs/linux-ref/dcn_3_1_2_offset.h. */
    CHECK(RB_DMUB_DW_GPINT_DATAIN1 == 0x01f8u,
          "regDMCUB_GPINT_DATAIN1 is 0x01f8, base_idx 2");
    CHECK(RB_DMUB_DW_GPINT_DATAOUT == 0x01f9u,
          "regDMCUB_GPINT_DATAOUT is 0x01f9");
    CHECK(RB_DMUB_DW_SCRATCH7 == 0x01eau,
          "regDMCUB_SCRATCH7 is 0x01ea - where the answer lands");
    CHECK(RB_DMUB_DW_SCRATCH0 == 0x01e3u,
          "regDMCUB_SCRATCH0 is 0x01e3 - where the INDEPENDENT 4.0.15.47 "
          "reading came from, and it is not 0x01ea");
    CHECK(RB_DMUB_DW_SCRATCH0 != RB_DMUB_DW_SCRATCH7,
          "the control and the answer are different registers, or the "
          "positive control would be the instrument reading itself");

    return failures;
}
