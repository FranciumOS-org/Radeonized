/* rb_atombios.h - is this buffer an ATOM video BIOS? (3vb-c)
 *
 * Linux's own `check_atom_bios()` (drivers/gpu/drm/amd/amdgpu/amdgpu_bios.c),
 * transcribed field for field, plus the legacy option-ROM header the same file
 * reads in `amdgpu_read_bios_from_rom()`:
 *
 *     AMD_IS_VALID_VBIOS(p)   p[0] == 0x55 && p[1] == 0xAA
 *     AMD_VBIOS_LENGTH(p)     p[2] << 9
 *     AMD_VBIOS_SIGNATURE     " 761295520"  at offset 0x30
 *     bios_header_start       p[0x48] | (p[0x49] << 8)
 *     "ATOM" or "MOTA"        at bios_header_start + 4
 *
 * Freestanding, read-only, no allocation. The point of the file is that every
 * way of REFUSING is a distinct code (rule 14), because "no VBIOS here" and
 * "a VBIOS we could not parse" are the two answers this probe exists to tell
 * apart, and a bool collapses them.
 *
 * Verified against this machine's own firmware/680m-vbios.rom: 44544 bytes,
 * p[2] = 87 so AMD_VBIOS_LENGTH is 44544 exactly, header start 0x0190 with
 * "ATOM" at 0x0194, signature at 0x30.
 */
#ifndef RB_ATOMBIOS_H
#define RB_ATOMBIOS_H

#include <stdint.h>

#define RB_ATOMBIOS_OK            0
#define RB_ATOMBIOS_E_NULL        1   /* no buffer                           */
#define RB_ATOMBIOS_E_SHORT       2   /* under 0x49 bytes - Linux's own floor */
#define RB_ATOMBIOS_E_NOT_ROM     3   /* no 0x55 0xAA - not an option ROM     */
#define RB_ATOMBIOS_E_NO_HEADER   4   /* header pointer reads 0               */
#define RB_ATOMBIOS_E_HDR_PAST    5   /* header pointer past the buffer       */
#define RB_ATOMBIOS_E_NOT_ATOM    6   /* an option ROM, but not ATOM/MOTA     */
#define RB_ATOMBIOS_E_NO_OUT      7   /* caller passed no output              */

/* offsets, named rather than spelled inline */
#define RB_ATOMBIOS_O_LEN512      0x02
#define RB_ATOMBIOS_O_SIG         0x30
#define RB_ATOMBIOS_O_HDRPTR      0x48
#define RB_ATOMBIOS_MIN           0x49
#define RB_ATOMBIOS_SIG_STR       " 761295520"
#define RB_ATOMBIOS_SIG_LEN       10

/* What the probe publishes about a buffer it accepted. */
struct rb_atombios_info {
    uint32_t hdr_start;    /* p[0x48] | p[0x49]<<8                           */
    uint32_t rom_len;      /* AMD_VBIOS_LENGTH: p[2] << 9                    */
    uint32_t sig_ok;       /* 1 if " 761295520" sits at 0x30                 */
    uint32_t mota;         /* 1 if the magic was byte-swapped ("MOTA")       */
};

static inline int rb_atombios_check(const uint8_t *b, uint32_t size,
                                    struct rb_atombios_info *out)
{
    uint32_t hdr, i;
    if (!out) { return RB_ATOMBIOS_E_NO_OUT; }
    if (!b)   { return RB_ATOMBIOS_E_NULL; }
    if (size < RB_ATOMBIOS_MIN) { return RB_ATOMBIOS_E_SHORT; }
    if (b[0] != 0x55u || b[1] != 0xAAu) { return RB_ATOMBIOS_E_NOT_ROM; }

    hdr = (uint32_t)b[RB_ATOMBIOS_O_HDRPTR]
        | ((uint32_t)b[RB_ATOMBIOS_O_HDRPTR + 1] << 8);
    if (hdr == 0) { return RB_ATOMBIOS_E_NO_HEADER; }
    /* Linux checks `size < bios_header_start + 4`; we need all four magic
     * bytes readable, so the bound is hdr + 8 here.
     *
     * It is written subtractively, but the break-it sweep established that
     * the wrap-safe FORM carries no information here: `hdr` is built from two
     * bytes, so it cannot exceed 0xffff and `hdr + 8` cannot wrap a uint32
     * whatever the input. The additive form passes every test. Kept as it is
     * because it costs nothing and the same shape IS load-bearing in
     * rb_vfct.h, where the length comes from a dword - but it is not a guard,
     * and a test claiming to cover it would pass for the wrong reason
     * (rule 48). The hdr + 8 BOUND itself is load-bearing: breaking it to
     * hdr + 4 fails the suite. */
    if (hdr > size || size - hdr < 8u) { return RB_ATOMBIOS_E_HDR_PAST; }

    {
        const uint8_t *m = b + hdr + 4;
        int atom = (m[0] == 'A' && m[1] == 'T' && m[2] == 'O' && m[3] == 'M');
        int mota = (m[0] == 'M' && m[1] == 'O' && m[2] == 'T' && m[3] == 'A');
        if (!atom && !mota) { return RB_ATOMBIOS_E_NOT_ATOM; }
        out->mota = mota ? 1u : 0u;
    }

    out->hdr_start = hdr;
    out->rom_len   = (uint32_t)b[RB_ATOMBIOS_O_LEN512] << 9;

    /* The bounds test below is INERT and the sweep proved it: breaking it to
     * `if (1)` fires nothing, because the E_SHORT floor is Linux's 0x49 and
     * the signature ends at 0x30 + 10 = 0x3a, so passing the floor already
     * guarantees the room. Kept because the floor is Linux's constant and not
     * ours to rely on, but it is not a guard and has no test (rule 48 - a
     * break that fires nothing is a result, not a gap to paper over). */
    out->sig_ok = 0u;
    if (size >= RB_ATOMBIOS_O_SIG + RB_ATOMBIOS_SIG_LEN) {
        const char *s = RB_ATOMBIOS_SIG_STR;
        out->sig_ok = 1u;
        for (i = 0; i < RB_ATOMBIOS_SIG_LEN; i++) {
            if (b[RB_ATOMBIOS_O_SIG + i] != (uint8_t)s[i]) { out->sig_ok = 0u; break; }
        }
    }
    return RB_ATOMBIOS_OK;
}

#endif /* RB_ATOMBIOS_H */
