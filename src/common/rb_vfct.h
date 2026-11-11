/* rb_vfct.h - find this board's VBIOS inside the ACPI VFCT table (3vb-c)
 *
 * The same parse tools/vfct.py already does, and the same one Linux's
 * amdgpu_acpi_vfct_bios() does: a VFCT table header whose dword at 0x34 is the
 * offset of the first VFCT_IMAGE_HEADER, then a chain of 28-byte image headers
 * each followed by its own option ROM. We take the image whose vendor/device
 * matches, and verify it with rb_atombios_check() rather than trusting the
 * header's own length.
 *
 *   VFCT header:  "VFCT" at 0, table length dword at 4, first image offset
 *                 dword at 0x34, minimum table size 0x4c.
 *   image header: bus/dev/fn dwords, then vendor, device, ssvid, ssid as
 *                 uint16, then revision and image length as dwords. 28 bytes.
 *
 * Freestanding, read-only, no allocation. Every refusal is its own code
 * (rule 14): "no VFCT", "a VFCT with no image for this device" and "an image
 * that is not an ATOM ROM" are three different findings and a bool loses all
 * three. The last two DO write their diagnostic fields - `images`, and for a
 * non-ATOM match also `off`/`len`/`atom_rc` - because a refusal that cannot
 * say what it refused is the thing rule 19 is about; `info` is written only on
 * OK. Every other refusal writes nothing.
 */
#ifndef RB_VFCT_H
#define RB_VFCT_H

#include <stdint.h>
#include "rb_atombios.h"

#define RB_VFCT_OK            0
#define RB_VFCT_E_NULL        1   /* no table                                */
#define RB_VFCT_E_SHORT       2   /* under 0x4c bytes                        */
#define RB_VFCT_E_SIG         3   /* first four bytes are not "VFCT"         */
#define RB_VFCT_E_LEN         4   /* header length past the buffer, or < 0x4c */
#define RB_VFCT_E_OFF         5   /* first-image offset outside the table    */
#define RB_VFCT_E_HDR_TRUNC   6   /* an image header runs off the end        */
#define RB_VFCT_E_IMG_TRUNC   7   /* an image body runs off the end          */
#define RB_VFCT_E_NO_MATCH    8   /* walked it all; no image for this device */
#define RB_VFCT_E_ZERO_LEN    9   /* chain ended on a zero-length image      */
#define RB_VFCT_E_NO_OUT     10   /* caller passed no output                 */
#define RB_VFCT_E_NOT_ATOM   11   /* matched, but rb_atombios_check refused  */

#define RB_VFCT_O_LENGTH      0x04
#define RB_VFCT_O_FIRST       0x34
#define RB_VFCT_MIN           0x4c
#define RB_VFCT_IHDR          28

struct rb_vfct_found {
    uint32_t off;                 /* offset of the image INSIDE the table    */
    uint32_t len;                 /* its length, from its own header         */
    uint32_t images;              /* how many images the chain held          */
    uint32_t atom_rc;             /* rb_atombios_check()'s code on it        */
    struct rb_atombios_info info; /* and what that check read                */
};

static inline uint32_t rb_vfct_u32(const uint8_t *b, uint32_t o)
{
    return (uint32_t)b[o] | ((uint32_t)b[o + 1] << 8)
         | ((uint32_t)b[o + 2] << 16) | ((uint32_t)b[o + 3] << 24);
}

static inline uint32_t rb_vfct_u16(const uint8_t *b, uint32_t o)
{
    return (uint32_t)b[o] | ((uint32_t)b[o + 1] << 8);
}

/* Walks the chain for vendor:device. See the note above on which refusals
 * write diagnostic fields into `out`. */
static inline int rb_vfct_find(const uint8_t *t, uint32_t size,
                               uint32_t vendor, uint32_t device,
                               struct rb_vfct_found *out)
{
    uint32_t length, off, n = 0;

    if (!out) { return RB_VFCT_E_NO_OUT; }
    if (!t)   { return RB_VFCT_E_NULL; }
    if (size < RB_VFCT_MIN) { return RB_VFCT_E_SHORT; }
    if (t[0] != 'V' || t[1] != 'F' || t[2] != 'C' || t[3] != 'T') {
        return RB_VFCT_E_SIG;
    }
    length = rb_vfct_u32(t, RB_VFCT_O_LENGTH);
    if (length > size || length < RB_VFCT_MIN) { return RB_VFCT_E_LEN; }

    off = rb_vfct_u32(t, RB_VFCT_O_FIRST);
    if (off < RB_VFCT_MIN || off >= length) { return RB_VFCT_E_OFF; }

    while (off < length) {
        uint32_t vid, did, ilen, body;
        /* written so no addition can wrap a 32-bit offset */
        if (length - off < RB_VFCT_IHDR) { return RB_VFCT_E_HDR_TRUNC; }
        vid  = rb_vfct_u16(t, off + 12);
        did  = rb_vfct_u16(t, off + 14);
        ilen = rb_vfct_u32(t, off + 24);
        body = off + RB_VFCT_IHDR;
        if (ilen == 0) { return RB_VFCT_E_ZERO_LEN; }
        if (length - body < ilen) { return RB_VFCT_E_IMG_TRUNC; }
        n++;
        if (vid == vendor && did == device) {
            struct rb_atombios_info info;
            int rc = rb_atombios_check(t + body, ilen, &info);
            if (rc != RB_ATOMBIOS_OK) {
                /* A match that is not an ATOM ROM is a DIFFERENT finding from
                 * no match at all, and the caller needs to be able to say
                 * which - so this does not fall through and keep walking. */
                out->off = body; out->len = ilen; out->images = n;
                out->atom_rc = (uint32_t)rc;
                return RB_VFCT_E_NOT_ATOM;
            }
            out->off = body; out->len = ilen; out->images = n;
            out->atom_rc = RB_ATOMBIOS_OK; out->info = info;
            return RB_VFCT_OK;
        }
        off = body + ilen;
    }
    out->images = n;
    return RB_VFCT_E_NO_MATCH;
}

#endif /* RB_VFCT_H */
