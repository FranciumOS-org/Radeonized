/* atombios_test.c - 3vb-c. Every refusal of rb_atombios_check() provoked, and
 * the accept path pinned against this machine's own ROM when it is present.
 *
 * The test's own subject is that the refusals are DISTINCT: "nothing is here"
 * and "something is here that we could not parse" are the two answers the
 * VBIOS probe exists to separate, so a test that only checked OK-vs-not would
 * be no coverage at all.
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "rb_atombios.h"

static int fails;
static void ok(int cond, const char *what)
{
    if (cond) { printf("ok   %s\n", what); }
    else      { printf("FAIL %s\n", what); fails++; }
}

/* A minimal well-formed ATOM option ROM, built rather than copied, so the
 * fields are the test's own and not a transcription of the expected output. */
static void build(uint8_t *b, uint32_t size, uint32_t hdr)
{
    memset(b, 0, size);
    b[0] = 0x55; b[1] = 0xAA;
    b[RB_ATOMBIOS_O_LEN512] = 87;                   /* 87 << 9 == 44544 */
    b[RB_ATOMBIOS_O_HDRPTR]     = (uint8_t)(hdr & 0xff);
    b[RB_ATOMBIOS_O_HDRPTR + 1] = (uint8_t)(hdr >> 8);
    memcpy(b + hdr + 4, "ATOM", 4);
    memcpy(b + RB_ATOMBIOS_O_SIG, RB_ATOMBIOS_SIG_STR, RB_ATOMBIOS_SIG_LEN);
}

int atombios_tests(void)
{
    struct rb_atombios_info info;
    static uint8_t buf[0x1000];
    int rc;

    printf("\n-- ATOM VBIOS header (3vb-c) --\n");

    build(buf, sizeof(buf), 0x190);
    memset(&info, 0xAA, sizeof(info));
    rc = rb_atombios_check(buf, sizeof(buf), &info);
    ok(rc == RB_ATOMBIOS_OK, "a well-formed ATOM option ROM is accepted");
    ok(info.hdr_start == 0x190, "the header pointer is read from 0x48/0x49");
    ok(info.rom_len == 44544, "AMD_VBIOS_LENGTH is p[2] << 9, not p[2] << 8");
    ok(info.sig_ok == 1, "the \" 761295520\" signature at 0x30 is found");
    ok(info.mota == 0, "ATOM is not reported as byte-swapped");

    /* MOTA: Linux accepts it, so we must too, and must say which it was. */
    build(buf, sizeof(buf), 0x190);
    memcpy(buf + 0x190 + 4, "MOTA", 4);
    rc = rb_atombios_check(buf, sizeof(buf), &info);
    ok(rc == RB_ATOMBIOS_OK, "MOTA is accepted as well as ATOM");
    ok(info.mota == 1, "...and is reported as byte-swapped, not silently equal");

    /* Each refusal, and each one DISTINCT. */
    ok(rb_atombios_check(NULL, 0x1000, &info) == RB_ATOMBIOS_E_NULL,
       "a NULL buffer refuses E_NULL");
    build(buf, sizeof(buf), 0x190);
    ok(rb_atombios_check(buf, 0x1000, NULL) == RB_ATOMBIOS_E_NO_OUT,
       "no output pointer refuses E_NO_OUT");
    ok(rb_atombios_check(buf, RB_ATOMBIOS_MIN - 1, &info) == RB_ATOMBIOS_E_SHORT,
       "a buffer under Linux's own 0x49 floor refuses E_SHORT");
    ok(rb_atombios_check(buf, RB_ATOMBIOS_MIN, &info) != RB_ATOMBIOS_E_SHORT,
       "...and exactly 0x49 is not short - the boundary is where Linux puts it");

    build(buf, sizeof(buf), 0x190);
    buf[1] = 0xAB;
    ok(rb_atombios_check(buf, sizeof(buf), &info) == RB_ATOMBIOS_E_NOT_ROM,
       "a buffer with no 0x55 0xAA refuses E_NOT_ROM");
    /* An unbacked physical read returns all ones - boot 547 measured exactly
     * that at 0x87f800000 - so it must refuse, and refuse as NOT_ROM. */
    memset(buf, 0xff, sizeof(buf));
    ok(rb_atombios_check(buf, sizeof(buf), &info) == RB_ATOMBIOS_E_NOT_ROM,
       "an all-ones page (an unbacked physical read) refuses E_NOT_ROM");
    memset(buf, 0x00, sizeof(buf));
    ok(rb_atombios_check(buf, sizeof(buf), &info) == RB_ATOMBIOS_E_NOT_ROM,
       "an all-zero page refuses E_NOT_ROM too");

    build(buf, sizeof(buf), 0x190);
    buf[RB_ATOMBIOS_O_HDRPTR] = 0; buf[RB_ATOMBIOS_O_HDRPTR + 1] = 0;
    ok(rb_atombios_check(buf, sizeof(buf), &info) == RB_ATOMBIOS_E_NO_HEADER,
       "a zero header pointer refuses E_NO_HEADER, not E_HDR_PAST");

    build(buf, sizeof(buf), 0x190);
    ok(rb_atombios_check(buf, 0x190, &info) == RB_ATOMBIOS_E_HDR_PAST,
       "a header pointer at the very end refuses E_HDR_PAST");
    ok(rb_atombios_check(buf, 0x190 + 7, &info) == RB_ATOMBIOS_E_HDR_PAST,
       "...and so does one whose four magic bytes are only partly readable");
    ok(rb_atombios_check(buf, 0x190 + 8, &info) == RB_ATOMBIOS_OK,
       "...while hdr + 8 readable is enough - the bound is exact");

    /* NOT a wrap test: the sweep showed hdr is 16-bit so hdr + 8 cannot wrap,
     * and a test claiming otherwise would pass for the wrong reason. What this
     * does check is that a header pointer far past a small buffer refuses. */
    build(buf, sizeof(buf), 0xfffe);
    ok(rb_atombios_check(buf, 0x1000, &info) == RB_ATOMBIOS_E_HDR_PAST,
       "a header pointer far past the buffer refuses E_HDR_PAST");

    build(buf, sizeof(buf), 0x190);
    memcpy(buf + 0x190 + 4, "ATMO", 4);
    ok(rb_atombios_check(buf, sizeof(buf), &info) == RB_ATOMBIOS_E_NOT_ATOM,
       "an option ROM that is not ATOM refuses E_NOT_ATOM, a DIFFERENT code");
    /* Found by the break-it sweep: shortening the MOTA compare to its first
     * byte fired NOTHING, because every negative case above starts with 'A'.
     * So the MOTA branch had no coverage at all. */
    build(buf, sizeof(buf), 0x190);
    memcpy(buf + 0x190 + 4, "MOXX", 4);
    ok(rb_atombios_check(buf, sizeof(buf), &info) == RB_ATOMBIOS_E_NOT_ATOM,
       "magic starting 'M' but not MOTA refuses too - the MOTA compare is whole");

    /* sig_ok is a reading, not a gate: a ROM with no signature is still
     * accepted, because Linux's check_atom_bios() does not look at it. */
    build(buf, sizeof(buf), 0x190);
    memset(buf + RB_ATOMBIOS_O_SIG, 0, RB_ATOMBIOS_SIG_LEN);
    rc = rb_atombios_check(buf, sizeof(buf), &info);
    ok(rc == RB_ATOMBIOS_OK && info.sig_ok == 0,
       "a missing signature is REPORTED, not refused - Linux does not gate on it");

    /* No output may be written on any refusal (rule 14's other half). */
    {
        struct rb_atombios_info z;
        memset(&z, 0x5a, sizeof(z));
        build(buf, sizeof(buf), 0x190);
        buf[0] = 0;
        (void)rb_atombios_check(buf, sizeof(buf), &z);
        ok(z.hdr_start == 0x5a5a5a5a && z.rom_len == 0x5a5a5a5a,
           "a refusal writes NOTHING into the caller's struct");
    }

    /* This machine's own ROM, when the tree has it: the positive control that
     * is not of the test's own making. Absent is not a failure - a release
     * clone has no firmware/ - but a PRESENT file that fails is. */
    {
        FILE *f = fopen(RB_ATOMBIOS_ROM, "rb");
        if (!f) {
            printf("ok   (no %s in this tree - real-ROM check skipped)\n",
                   RB_ATOMBIOS_ROM);
        } else {
            static uint8_t rom[256 * 1024];
            size_t n = fread(rom, 1, sizeof(rom), f);
            fclose(f);
            rc = rb_atombios_check(rom, (uint32_t)n, &info);
            ok(rc == RB_ATOMBIOS_OK, "this machine's own VBIOS is accepted");
            ok(info.hdr_start == 0x190, "...header start 0x190, as read by hand");
            ok(info.rom_len == (uint32_t)n,
               "...AMD_VBIOS_LENGTH equals the file's own size exactly");
            ok(info.sig_ok == 1, "...and it carries the AMD signature");
        }
    }

    return fails;
}
