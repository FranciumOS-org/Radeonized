/* otgprog_test.c - rb_otgprog.h against this chip's own numbers.
 *
 * What is pinned here and WHY each pin is the one that would catch a real
 * mistake:
 *
 *  - the three STRIDES, every instance, against dcn_3_1_2_offset.h. OTG is
 *    0x80, VTG is 1 and ODM is 0x10, and deriving any from another puts a
 *    write inside a different block. This is the pin that matters most.
 *  - OTG1's offsets against rb_regs.h BY NAME, so the two copies of
 *    OTG1_H_TOTAL / OTG1_V_TOTAL / OTG1_CONTROL in this tree cannot drift.
 *  - the 1080p60 mode against its own clock by arithmetic: 2200 x 1125 x 60
 *    is EXACTLY 148 500 000, the clock boot 461 programmed into the DTO and
 *    the figure 2BD read out of this sink's EDID. A mode that does not
 *    multiply out to the clock is the error this catches.
 *  - the sixteen writes' order, offsets, masks and values, computed by hand
 *    from optc1_program_timing.
 *  - the VTG write appearing TWICE with 0 then 1, in that order.
 *  - every refusal, including the panel by name.
 */
#include <stdio.h>
#include <string.h>
#include "rb_otgprog.h"
#include "rb_regs.h"

static int fails;

static void ck(int cond, const char *what)
{
    if (!cond) { printf("FAIL otgprog: %s\n", what); fails++; }
}

static void ck_u(uint32_t got, uint32_t want, const char *what)
{
    if (got != want) {
        printf("FAIL otgprog: %s - got 0x%x want 0x%x\n", what, got, want);
        fails++;
    }
}

/* rb_regs.h by name, so a pin is against the other copy and not against a
 * number retyped here (rule 26: the search is an instrument). */
static int reg_dw(const char *name, uint32_t *dw, uint32_t *base_idx)
{
    size_t i;
    for (i = 0; i < RB_NREG_DEFS; i++) {
        if (strcmp(rb_reg_defs[i].name, name) == 0) {
            *dw = rb_reg_defs[i].reg;
            *base_idx = rb_reg_defs[i].base_idx;
            return 1;
        }
    }
    return 0;
}

int otgprog_tests(void);

int otgprog_tests(void)
{
    struct rb_otgprog_write w[RB_OTGPROG_MAX_WRITES];
    struct rb_otgprog_mode m;
    uint32_t n = 0, dw = 0, bi = 0, i;
    int rc;

    fails = 0;

    /* --- the offsets, every instance, against the header --- */
    {
        static const uint32_t h_total[4]  = {0x1b2a, 0x1baa, 0x1c2a, 0x1caa};
        static const uint32_t control[4]  = {0x1b41, 0x1bc1, 0x1c41, 0x1cc1};
        static const uint32_t frames[4]   = {0x1b4c, 0x1bcc, 0x1c4c, 0x1ccc};
        static const uint32_t vtg[4]      = {0x052d, 0x052e, 0x052f, 0x0530};
        static const uint32_t odm[4]      = {0x1acb, 0x1adb, 0x1aeb, 0x1afb};
        static const uint32_t v_blank[4]  = {0x1b36, 0x1bb6, 0x1c36, 0x1cb6};
        static const uint32_t h_timing[4] = {0x1b2e, 0x1bae, 0x1c2e, 0x1cae};
        for (i = 0; i < RB_OTGPROG_COUNT; i++) {
            ck_u(RB_OTGPROG_DW_H_TOTAL(i),     h_total[i],  "H_TOTAL offset");
            ck_u(RB_OTGPROG_DW_CONTROL(i),     control[i],  "CONTROL offset");
            ck_u(RB_OTGPROG_DW_FRAME_COUNT(i), frames[i],   "FRAME_COUNT offset");
            ck_u(RB_OTGPROG_DW_VTG(i),         vtg[i],      "VTG offset");
            ck_u(RB_OTGPROG_DW_ODM_SRC(i),     odm[i],      "ODM offset");
            ck_u(RB_OTGPROG_DW_V_BLANK(i),     v_blank[i],  "V_BLANK offset");
            ck_u(RB_OTGPROG_DW_H_TIMING(i),    h_timing[i], "H_TIMING offset");
        }
        /* The strides are DIFFERENT, and that is the point. If any two of
         * these ever compare equal, something derived one from another. */
        ck(RB_OTGPROG_DW_H_TOTAL(1) - RB_OTGPROG_DW_H_TOTAL(0) == 0x80,
           "OTG stride is 0x80");
        ck(RB_OTGPROG_DW_VTG(1) - RB_OTGPROG_DW_VTG(0) == 0x1,
           "VTG stride is 1, NOT 0x80");
        ck(RB_OTGPROG_DW_ODM_SRC(1) - RB_OTGPROG_DW_ODM_SRC(0) == 0x10,
           "ODM stride is 0x10, NOT 0x80");
    }

    /* --- against rb_regs.h's own copies, by name --- */
    ck(reg_dw("OTG1_H_TOTAL", &dw, &bi), "rb_regs.h has OTG1_H_TOTAL");
    ck_u(dw, RB_OTGPROG_DW_H_TOTAL(1), "OTG1_H_TOTAL agrees with rb_regs.h");
    ck_u(bi, 2u, "OTG1_H_TOTAL is BASE_IDX 2");
    ck(reg_dw("OTG1_V_TOTAL", &dw, &bi), "rb_regs.h has OTG1_V_TOTAL");
    ck_u(dw, RB_OTGPROG_DW_V_TOTAL(1), "OTG1_V_TOTAL agrees with rb_regs.h");
    ck(reg_dw("OTG1_CONTROL", &dw, &bi), "rb_regs.h has OTG1_CONTROL");
    ck_u(dw, RB_OTGPROG_DW_CONTROL(1), "OTG1_CONTROL agrees with rb_regs.h");
    ck_u(bi, 2u, "OTG1_CONTROL is BASE_IDX 2");

    /* --- the mode multiplies out to the clock that is already running --- */
    memset(&m, 0, sizeof(m));
    m.h_addressable = 1920; m.h_front_porch = 88;
    m.h_sync_width  = 44;   m.h_total       = 2200;
    m.v_addressable = 1080; m.v_front_porch = 4;
    m.v_sync_width  = 5;    m.v_total       = 1125;
    m.h_sync_positive = 1;  m.v_sync_positive = 1;
    ck_u(rb_otgprog_clock_hz(&m, 60u), 148500000u,
         "1080p60 is exactly 148.5 MHz, the clock boot 461 programmed");
    /* The back porches follow, and are what the register values encode. */
    ck_u(m.h_total - m.h_addressable - m.h_front_porch - m.h_sync_width, 148u,
         "h back porch 148");
    ck_u(m.v_total - m.v_addressable - m.v_front_porch - m.v_sync_width, 36u,
         "v back porch 36");

    /* --- the build, write by write --- */
    rc = rb_otgprog_build(1u, &m, w, RB_OTGPROG_MAX_WRITES, &n);
    ck_u((uint32_t)rc, RB_OTGPROG_OK, "build accepts 1080p60 on pipe 1");
    ck_u(n, 16u, "sixteen writes");

    ck_u(w[0].dw, 0x1baa, "w0 is OTG1_H_TOTAL");
    ck_u(w[0].value, 2199u, "H_TOTAL is h_total - 1");
    ck_u(w[0].whole, 1u, "H_TOTAL is a REG_SET, not a REG_UPDATE");
    ck_u(w[1].dw, 0x1bac, "w1 is OTG1_H_SYNC_A");
    ck_u(w[1].value, 44u << 16, "H_SYNC_A end 44, start 0");
    ck_u(w[1].mask, 0x7fff7fffu, "H_SYNC_A writes BOTH fields");
    ck_u(w[2].dw, 0x1bab, "w2 is OTG1_H_BLANK_START_END");
    ck_u(w[2].value, (192u << 16) | 2112u, "H_BLANK start 2112, end 192");
    ck_u(w[3].dw, 0x1bad, "w3 is OTG1_H_SYNC_A_CNTL");
    ck_u(w[3].value, 0u, "a POSITIVE hsync programs POL 0");
    ck_u(w[4].dw, 0x1baf, "w4 is OTG1_V_TOTAL");
    ck_u(w[4].value, 1124u, "V_TOTAL is v_total - 1");
    ck_u(w[4].whole, 1u, "V_TOTAL is a REG_SET");
    ck_u(w[5].dw, 0x1bb0, "w5 is OTG1_V_TOTAL_MIN");
    ck_u(w[5].value, 1124u, "V_TOTAL_MIN equals V_TOTAL");
    ck_u(w[6].dw, 0x1bb1, "w6 is OTG1_V_TOTAL_MAX");
    ck_u(w[6].value, 1124u, "V_TOTAL_MAX equals V_TOTAL");
    ck_u(w[7].dw, 0x1bb7, "w7 is OTG1_V_SYNC_A");
    ck_u(w[7].value, 5u << 16, "V_SYNC_A end 5, start 0");
    ck_u(w[8].dw, 0x1bb6, "w8 is OTG1_V_BLANK_START_END");
    ck_u(w[8].value, (41u << 16) | 1121u, "V_BLANK start 1121, end 41");
    ck_u(w[9].dw, 0x1bb8, "w9 is OTG1_V_SYNC_A_CNTL");
    ck_u(w[9].value, 0u, "a POSITIVE vsync programs POL 0");
    ck_u(w[10].dw, 0x052e, "w10 is VTG1_CONTROL");
    ck_u(w[10].value, 0u, "the FIRST VTG write is ZERO - Linux's VInit");
    ck_u(w[10].mask, 0x80000000u, "VTG0_ENABLE is bit 31");
    ck_u(w[11].dw, 0x1bc1, "w11 is OTG1_CONTROL");
    ck_u(w[11].value, 1u << 12, "START_POINT_CNTL 1 for DP, FIELD_NUMBER 0");
    ck_u(w[12].dw, 0x1bae, "w12 is OTG1_H_TIMING_CNTL");
    ck_u(w[12].value, 0u, "H_TIMING_DIV_MODE 0, one OPP per OTG");
    ck_u(w[13].dw, 0x1adb, "w13 is ODM1_OPTC_DATA_SOURCE_SELECT");
    ck_u(w[13].value, 1u << 16, "SEG0_SRC_SEL is the instance, 1 to 1");
    ck_u(w[14].dw, 0x052e, "w14 is VTG1_CONTROL again");
    ck_u(w[14].value, 0x80000000u, "the SECOND VTG write is ONE");
    ck_u(w[15].dw, 0x1bc1, "w15 is OTG1_CONTROL, the one that starts it");
    ck_u(w[15].value, (2u << 8) | 1u, "DISABLE_POINT 2 with MASTER_EN 1");
    ck_u(w[15].mask, 0x301u, "and it touches ONLY those two fields");
    /* The order of the two VTG writes is load-bearing: swapped, the
     * generator comes out disabled. Asserted as an ordering, not as two
     * independent values. */
    ck(w[10].dw == w[14].dw && w[10].value == 0u
       && w[14].value == 0x80000000u, "VTG goes 0 then 1, in that order");

    /* --- no write lands outside the three blocks --- */
    for (i = 0; i < n; i++) {
        int ok = (w[i].dw >= 0x1baa && w[i].dw <= 0x1bcc)   /* OTG1   */
              || (w[i].dw == 0x052e)                        /* VTG1   */
              || (w[i].dw == 0x1adb);                       /* ODM1   */
        ck(ok, "every write is inside OTG1, VTG1 or ODM1");
        ck(w[i].value == (w[i].value & w[i].mask),
           "no write's value escapes its own mask");
    }

    /* --- refusals --- */
    {
        struct rb_otgprog_mode b;
        ck_u((uint32_t)rb_otgprog_build(0u, &m, w, RB_OTGPROG_MAX_WRITES, &n),
             RB_OTGPROG_E_PANEL, "pipe 0 is the panel's and is refused");
        ck_u(n, 0u, "a refusal writes nothing");
        ck_u((uint32_t)rb_otgprog_build(4u, &m, w, RB_OTGPROG_MAX_WRITES, &n),
             RB_OTGPROG_E_INST, "instance 4 does not exist");
        ck_u((uint32_t)rb_otgprog_build(1u, NULL, w, RB_OTGPROG_MAX_WRITES, &n),
             RB_OTGPROG_E_ARGS, "a NULL mode is refused");
        ck_u((uint32_t)rb_otgprog_build(1u, &m, w, 15u, &n),
             RB_OTGPROG_E_ARGS, "room for 15 writes is not enough for 16");

        b = m; b.h_addressable = 0;
        ck_u((uint32_t)rb_otgprog_build(1u, &b, w, RB_OTGPROG_MAX_WRITES, &n),
             RB_OTGPROG_E_HACTIVE, "h_addressable 0");
        b = m; b.v_addressable = 0;
        ck_u((uint32_t)rb_otgprog_build(1u, &b, w, RB_OTGPROG_MAX_WRITES, &n),
             RB_OTGPROG_E_VACTIVE, "v_addressable 0");
        b = m; b.h_total = 1920;
        ck_u((uint32_t)rb_otgprog_build(1u, &b, w, RB_OTGPROG_MAX_WRITES, &n),
             RB_OTGPROG_E_HTOTAL, "h_total equal to the active area");
        b = m; b.v_total = 1080;
        ck_u((uint32_t)rb_otgprog_build(1u, &b, w, RB_OTGPROG_MAX_WRITES, &n),
             RB_OTGPROG_E_VTOTAL, "v_total equal to the active area");
        b = m; b.h_front_porch = 280;
        ck_u((uint32_t)rb_otgprog_build(1u, &b, w, RB_OTGPROG_MAX_WRITES, &n),
             RB_OTGPROG_E_HBLANK, "h front porch filling the whole blanking");
        b = m; b.v_front_porch = 45;
        ck_u((uint32_t)rb_otgprog_build(1u, &b, w, RB_OTGPROG_MAX_WRITES, &n),
             RB_OTGPROG_E_VBLANK, "v front porch filling the whole blanking");
        b = m; b.h_sync_width = 0;
        ck_u((uint32_t)rb_otgprog_build(1u, &b, w, RB_OTGPROG_MAX_WRITES, &n),
             RB_OTGPROG_E_HSYNC, "an hsync of zero width");
        b = m; b.h_sync_width = 200;
        ck_u((uint32_t)rb_otgprog_build(1u, &b, w, RB_OTGPROG_MAX_WRITES, &n),
             RB_OTGPROG_E_HSYNC, "front porch plus hsync past the blanking");
        b = m; b.v_sync_width = 0;
        ck_u((uint32_t)rb_otgprog_build(1u, &b, w, RB_OTGPROG_MAX_WRITES, &n),
             RB_OTGPROG_E_VSYNC, "a vsync of zero width");
        b = m; b.v_sync_width = 44;
        ck_u((uint32_t)rb_otgprog_build(1u, &b, w, RB_OTGPROG_MAX_WRITES, &n),
             RB_OTGPROG_E_VSYNC, "front porch plus vsync past the blanking");
        /* 15 bits, and a truncation here would program a raster nobody
         * asked for on a pipe whose clock is live. */
        b = m; b.h_total = 40000; b.h_addressable = 39000;
        b.h_front_porch = 100; b.h_sync_width = 100;
        ck_u((uint32_t)rb_otgprog_build(1u, &b, w, RB_OTGPROG_MAX_WRITES, &n),
             RB_OTGPROG_E_FIELD, "an h_total past 15 bits is refused, not cut");
        /* The case above does NOT exercise the h_tot_m1 term: its
         * h_blank_start is 39900, so the OR chain fires on that instead, and
         * a break-it run with the h_tot_m1 term removed was SILENT. Rule 48 -
         * a conjunction can be correct while one term carries no
         * information. This mode fires h_tot_m1 ALONE: h_total - 1 is 32768,
         * one past the field, while h_blank_start is exactly 32767 and fits. */
        b = m; b.h_total = 32769; b.h_addressable = 32000;
        b.h_front_porch = 2; b.h_sync_width = 100;
        ck_u(b.h_total - b.h_front_porch, 32767u,
             "the discriminating mode's blank start fits in 15 bits");
        ck_u((uint32_t)rb_otgprog_build(1u, &b, w, RB_OTGPROG_MAX_WRITES, &n),
             RB_OTGPROG_E_FIELD, "h_total - 1 past 15 bits, on its own");
    }

    /* --- the front porch workaround, which CHANGES the blank start --- */
    {
        struct mode_zero { int unused; } keep;
        struct rb_otgprog_mode z = m;
        (void)keep;
        z.v_front_porch = 0;
        ck_u(rb_otgprog_v_front_porch(&z), 1u,
             "a zero v front porch is floored at 1 (Linux's workaround)");
        ck_u((uint32_t)rb_otgprog_build(1u, &z, w, RB_OTGPROG_MAX_WRITES, &n),
             RB_OTGPROG_OK, "and the mode still builds");
        ck_u(w[8].value, (44u << 16) | 1124u,
             "with the floor applied, V_BLANK start 1124 end 44");
        ck_u(rb_otgprog_v_front_porch(&m), 4u,
             "a front porch above the floor is left alone");
    }

    /* --- the clock arithmetic's own guards --- */
    {
        struct rb_otgprog_mode z = m;
        ck_u(rb_otgprog_clock_hz(NULL, 60u), 0u, "NULL mode gives no clock");
        ck_u(rb_otgprog_clock_hz(&m, 0u), 0u, "no refresh gives no clock");
        z.h_total = 0;
        ck_u(rb_otgprog_clock_hz(&z, 60u), 0u, "no h_total gives no clock");
        z = m; z.v_total = 0;
        ck_u(rb_otgprog_clock_hz(&z, 60u), 0u, "no v_total gives no clock");
        /* The overflow guard: 0x7fff x 0x7fff x 60 is far past 32 bits, and
         * a wrapped product would be a plausible-looking clock. */
        z = m; z.h_total = 0x7fff; z.v_total = 0x7fff;
        ck_u(rb_otgprog_clock_hz(&z, 60u), 0u, "an overflowing product is 0");
    }

    /* --- the field helpers --- */
    ck_u(rb_otgprog_shift(0x7fff0000u), 16u, "shift of a high field");
    ck_u(rb_otgprog_shift(0x00000001u), 0u,  "shift of bit 0");
    ck_u(rb_otgprog_shift(0x80000000u), 31u, "shift of bit 31");
    ck_u(rb_otgprog_shift(0u), 0u, "shift of an empty mask is 0, not a hang");
    ck_u(rb_otgprog_field(0x00000300u, 2u), 0x200u, "field 2 into bits 9:8");
    ck_u(rb_otgprog_field(0x00007fffu, 0x1ffffu), 0x7fffu,
         "a value wider than its field is cut by the mask, not wrapped");

    /* --- the verdict, including the wrap --- */
    ck_u((uint32_t)rb_otgprog_verdict(0x0u, 10u, 11u),
         (uint32_t)RB_OTGPROG_V_NOT_ENABLED,
         "no MASTER_EN outranks a moving counter");
    ck_u((uint32_t)rb_otgprog_verdict(0x1u, 10u, 11u),
         (uint32_t)RB_OTGPROG_V_SCANNING, "enabled and advancing");
    ck_u((uint32_t)rb_otgprog_verdict(0x1u, 10u, 10u),
         (uint32_t)RB_OTGPROG_V_STILL, "enabled and still");
    ck_u((uint32_t)rb_otgprog_verdict(0x301u, 0xffffffffu, 0u),
         (uint32_t)RB_OTGPROG_V_SCANNING,
         "a counter wrapping past its width still reads as advanced");
    /* The case above does NOT discriminate signed from unsigned: signed,
     * 0 - (-1) is 1, which is also "advanced". A break-it run that made the
     * comparison signed was SILENT on it. This pair differs - unsigned the
     * difference is 0x80000000 and non-zero, signed it is negative - so it
     * is the case that holds the unsigned subtraction in place. */
    ck_u((uint32_t)rb_otgprog_verdict(0x301u, 0u, 0x80000000u),
         (uint32_t)RB_OTGPROG_V_SCANNING,
         "an advance across the sign bit is still an advance");

    /* --- the write gate --------------------------------------------- */
    {
        const uint32_t DTO = 148500000u;   /* what boot 461 programmed */
        ck_u(rb_otgprog_write_refuse(1u, &m, 60u, 0x300u, DTO, 1, 1),
             RB_OTGPROG_W_OK, "pipe 1, 1080p60, dark, clock matching");
        ck_u(rb_otgprog_write_refuse(1u, &m, 60u, 0x300u, DTO, 0, 1),
             RB_OTGPROG_W_UNARMED, "unarmed outranks everything");
        ck_u(rb_otgprog_write_refuse(1u, &m, 60u, 0x300u, DTO, 1, 0),
             RB_OTGPROG_W_NOREGS, "no register window");
        ck_u(rb_otgprog_write_refuse(1u, NULL, 60u, 0x300u, DTO, 1, 1),
             RB_OTGPROG_W_NOREGS, "a NULL mode is NOREGS, not a crash");
        ck_u(rb_otgprog_write_refuse(0u, &m, 60u, 0x300u, DTO, 1, 1),
             RB_OTGPROG_W_PANEL, "pipe 0 refused by name at the gate too");
        ck_u(rb_otgprog_write_refuse(4u, &m, 60u, 0x300u, DTO, 1, 1),
             RB_OTGPROG_W_INST, "instance 4");
        /* 0x00011201 is the panel's own live OTG0_CONTROL, measured in 2oq.
         * Used here as the "already scanning" word so the test uses a real
         * one rather than a bit set by hand. */
        ck_u(rb_otgprog_write_refuse(1u, &m, 60u, 0x00011201u, DTO, 1, 1),
             RB_OTGPROG_W_RUNNING, "a pipe already scanning is refused");
        ck_u(rb_otgprog_write_refuse(1u, &m, 60u, 0x300u, 0u, 1, 1),
             RB_OTGPROG_W_NOCLOCK, "no DTO clock on that pipe");
        {
            struct rb_otgprog_mode bad = m;
            bad.h_sync_width = 0;
            ck_u(rb_otgprog_write_refuse(1u, &bad, 60u, 0x300u, DTO, 1, 1),
                 RB_OTGPROG_W_MODE, "a mode the builder refuses");
        }
        /* The clock agreement. 2BD's other mode, 1920x1080 at ~100 Hz, needs
         * 228 MHz - against a DTO holding 148.5 MHz that is a 54 % miss, and
         * it is the realistic version of this mistake: the right monitor,
         * the right sink, the wrong one of its two timings. */
        ck_u(rb_otgprog_write_refuse(1u, &m, 100u, 0x300u, DTO, 1, 1),
             RB_OTGPROG_W_CLOCKMISS, "1080p at 100 Hz against a 148.5 MHz DTO");
        ck(!rb_otgprog_clock_agrees(0u, DTO), "a zero mode clock never agrees");
        ck(!rb_otgprog_clock_agrees(DTO, 0u), "a zero DTO phase never agrees");
        ck(rb_otgprog_clock_agrees(DTO, DTO), "exact agreement");
        /* The window's own edges, both sides, at 0.5 % of 148.5 MHz =
         * 742 500 Hz. One hertz past it must fail, or the tolerance is not
         * being applied at all. */
        ck(rb_otgprog_clock_agrees(DTO + 742500u, DTO), "at the window, above");
        ck(rb_otgprog_clock_agrees(DTO - 742500u, DTO), "at the window, below");
        ck(!rb_otgprog_clock_agrees(DTO + 742501u, DTO), "one hertz past, above");
        ck(!rb_otgprog_clock_agrees(DTO - 742501u, DTO), "one hertz past, below");
        /* And the window is WIDE ENOUGH for the one real pair this project
         * has measured: 2F0's panel PHASE sat 0.208 % from its own raster's
         * product and the base was CONFIRMED there. A 0.01 % window would
         * have rejected a correct pair, which is the mistake 2F0 recorded. */
        ck(rb_otgprog_clock_agrees(343000000u, 342287571u),
           "2F0's own 0.208 % pair is INSIDE the window");
    }

    if (!fails) printf("otgprog: all passed (%u writes pinned)\n", n);
    return fails;
}
