/* vfct_test.c - 3vb-c. The VFCT walk, every refusal provoked, and this
 * machine's own ACPI table as the positive control.
 *
 * The control matters more than usual here: the table is the one input this
 * code will meet in the kernel, and the one thing a synthetic table cannot
 * check is whether the OFFSETS are right. So when firmware/VFCT.aml is in the
 * tree the test requires the walk to find 1002:1681 and to agree, byte for
 * byte, with firmware/680m-vbios.rom - which tools/vfct.py produced by an
 * independent implementation.
 */
#include <stdio.h>
#include <string.h>
#include "rb_vfct.h"

static int fails;
static void ok(int cond, const char *what)
{
    if (cond) { printf("ok   %s\n", what); }
    else      { printf("FAIL %s\n", what); fails++; }
}

static void put32(uint8_t *b, uint32_t o, uint32_t v)
{
    b[o] = (uint8_t)v; b[o+1] = (uint8_t)(v >> 8);
    b[o+2] = (uint8_t)(v >> 16); b[o+3] = (uint8_t)(v >> 24);
}
static void put16(uint8_t *b, uint32_t o, uint32_t v)
{
    b[o] = (uint8_t)v; b[o+1] = (uint8_t)(v >> 8);
}

/* one VFCT holding one image for vendor:device, ROM body `rom_len` bytes */
static uint32_t build(uint8_t *t, uint32_t cap, uint32_t vid, uint32_t did,
                      uint32_t rom_len, int make_atom)
{
    uint32_t first = 0x80, body = first + RB_VFCT_IHDR;
    uint32_t total = body + rom_len;
    memset(t, 0, cap);
    memcpy(t, "VFCT", 4);
    put32(t, RB_VFCT_O_FIRST, first);
    put16(t, first + 12, vid);
    put16(t, first + 14, did);
    put32(t, first + 24, rom_len);
    if (make_atom) {
        uint8_t *r = t + body;
        r[0] = 0x55; r[1] = 0xAA; r[RB_ATOMBIOS_O_LEN512] = 87;
        r[RB_ATOMBIOS_O_HDRPTR] = 0x90; r[RB_ATOMBIOS_O_HDRPTR + 1] = 0x01;
        memcpy(r + 0x190 + 4, "ATOM", 4);
        memcpy(r + RB_ATOMBIOS_O_SIG, RB_ATOMBIOS_SIG_STR, RB_ATOMBIOS_SIG_LEN);
    }
    put32(t, RB_VFCT_O_LENGTH, total);
    return total;
}

int vfct_tests(void)
{
    static uint8_t t[0x4000];
    struct rb_vfct_found f;
    uint32_t n;

    printf("\n-- ACPI VFCT walk (3vb-c) --\n");

    n = build(t, sizeof(t), 0x1002, 0x1681, 0x800, 1);
    memset(&f, 0, sizeof(f));
    ok(rb_vfct_find(t, n, 0x1002, 0x1681, &f) == RB_VFCT_OK,
       "a one-image VFCT yields its image");
    ok(f.off == 0x80 + RB_VFCT_IHDR, "the image body is 28 bytes past its header");
    ok(f.len == 0x800, "the length comes from the image header");
    ok(f.images == 1, "one image was walked");
    ok(f.info.hdr_start == 0x190, "and the ROM inside it parsed as ATOM");

    ok(rb_vfct_find(t, n, 0x1002, 0x164d, &f) == RB_VFCT_E_NO_MATCH,
       "a device id that is not present refuses E_NO_MATCH");
    ok(f.images == 1, "...and still reports how many images it walked");

    ok(rb_vfct_find(NULL, n, 0x1002, 0x1681, &f) == RB_VFCT_E_NULL,
       "a NULL table refuses E_NULL");
    ok(rb_vfct_find(t, n, 0x1002, 0x1681, NULL) == RB_VFCT_E_NO_OUT,
       "no output refuses E_NO_OUT");
    ok(rb_vfct_find(t, RB_VFCT_MIN - 1, 0x1002, 0x1681, &f) == RB_VFCT_E_SHORT,
       "under 0x4c bytes refuses E_SHORT");

    n = build(t, sizeof(t), 0x1002, 0x1681, 0x800, 1);
    t[1] = 'X';
    ok(rb_vfct_find(t, n, 0x1002, 0x1681, &f) == RB_VFCT_E_SIG,
       "a table that does not say VFCT refuses E_SIG");

    n = build(t, sizeof(t), 0x1002, 0x1681, 0x800, 1);
    put32(t, RB_VFCT_O_LENGTH, n + 1);
    ok(rb_vfct_find(t, n, 0x1002, 0x1681, &f) == RB_VFCT_E_LEN,
       "a header length past the buffer refuses E_LEN");
    put32(t, RB_VFCT_O_LENGTH, RB_VFCT_MIN - 1);
    ok(rb_vfct_find(t, n, 0x1002, 0x1681, &f) == RB_VFCT_E_LEN,
       "...and so does one below the minimum table size");

    n = build(t, sizeof(t), 0x1002, 0x1681, 0x800, 1);
    put32(t, RB_VFCT_O_FIRST, n);
    ok(rb_vfct_find(t, n, 0x1002, 0x1681, &f) == RB_VFCT_E_OFF,
       "a first-image offset at the end refuses E_OFF");
    put32(t, RB_VFCT_O_FIRST, 4);
    ok(rb_vfct_find(t, n, 0x1002, 0x1681, &f) == RB_VFCT_E_OFF,
       "...and so does one inside the table header itself");

    n = build(t, sizeof(t), 0x1002, 0x1681, 0x800, 1);
    put32(t, RB_VFCT_O_LENGTH, 0x80 + RB_VFCT_IHDR - 1);
    ok(rb_vfct_find(t, n, 0x1002, 0x1681, &f) == RB_VFCT_E_HDR_TRUNC,
       "an image header running off the end refuses E_HDR_TRUNC");

    n = build(t, sizeof(t), 0x1002, 0x1681, 0x800, 1);
    put32(t, 0x80 + 24, 0x4000);
    ok(rb_vfct_find(t, n, 0x1002, 0x1681, &f) == RB_VFCT_E_IMG_TRUNC,
       "an image body running off the end refuses E_IMG_TRUNC");

    n = build(t, sizeof(t), 0x1002, 0x1681, 0x800, 1);
    put32(t, 0x80 + 24, 0);
    ok(rb_vfct_find(t, n, 0x1002, 0x1681, &f) == RB_VFCT_E_ZERO_LEN,
       "a zero-length image refuses E_ZERO_LEN rather than looping");

    n = build(t, sizeof(t), 0x1002, 0x1681, 0x800, 0);
    ok(rb_vfct_find(t, n, 0x1002, 0x1681, &f) == RB_VFCT_E_NOT_ATOM,
       "a MATCHING image that is not an ATOM ROM refuses E_NOT_ATOM");
    ok(f.atom_rc == RB_ATOMBIOS_E_NOT_ROM,
       "...and carries the atombios code that refused it, not just a bool");
    ok(f.off == 0x80 + RB_VFCT_IHDR && f.len == 0x800,
       "...and says WHERE the thing it refused was");

    /* An offset that would wrap if the bounds were written as additions. */
    n = build(t, sizeof(t), 0x1002, 0x1681, 0x800, 1);
    put32(t, 0x80 + 24, 0xfffffff0u);
    ok(rb_vfct_find(t, n, 0x1002, 0x1681, &f) == RB_VFCT_E_IMG_TRUNC,
       "a near-UINT32_MAX image length cannot wrap the bounds check");

    /* This machine's own table: the only check of whether the OFFSETS are
     * right, and it is cross-checked against an independent implementation. */
    {
        FILE *a = fopen(RB_VFCT_AML, "rb");
        if (!a) {
            printf("ok   (no %s in this tree - real-table check skipped)\n",
                   RB_VFCT_AML);
        } else {
            static uint8_t aml[512 * 1024];
            size_t an = fread(aml, 1, sizeof(aml), a);
            fclose(a);
            memset(&f, 0, sizeof(f));
            int rc = rb_vfct_find(aml, (uint32_t)an, 0x1002, 0x1681, &f);
            ok(rc == RB_VFCT_OK, "this machine's own VFCT yields 1002:1681");
            if (rc == RB_VFCT_OK) {
                FILE *r = fopen(RB_ATOMBIOS_ROM, "rb");
                if (!r) {
                    printf("ok   (no %s - byte compare skipped)\n", RB_ATOMBIOS_ROM);
                } else {
                    static uint8_t rom[256 * 1024];
                    size_t rn = fread(rom, 1, sizeof(rom), r);
                    fclose(r);
                    ok(f.len == (uint32_t)rn,
                       "...the image length equals what tools/vfct.py extracted");
                    ok(f.len == (uint32_t)rn &&
                       memcmp(aml + f.off, rom, rn) == 0,
                       "...and the bytes are IDENTICAL to that independent parse");
                    ok(f.info.sig_ok == 1, "...and it carries the AMD signature");
                }
            }
        }
    }

    return fails;
}
