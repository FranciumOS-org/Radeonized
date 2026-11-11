/* rb_fwhdr.h - slice an amdgpu firmware container at runtime.
 *
 * 3fxe. Until now the plugin carried each image's PAYLOAD, transcribed into
 * C arrays by a generator since deleted (tools/fw_extract_rlc.py): the bytes at
 * ucode_array_offset_bytes for ucode_size_bytes, with the container header
 * dropped and every size hardcoded as hex at the use site. AMD's licence
 * (third_party/LICENSE.amdgpu) grants distribution of "copies, in binary form
 * only", and sets no condition about the files being unmodified - but it
 * grants no right to excerpt either, and "all rights reserved" means a narrow
 * reading is the safe one. A payload is a partial reproduction, and whether
 * that is a "copy" is a judgment call nobody needs to make.
 *
 * So the binary now embeds each yellow_carp_*.bin WHOLE and byte-exact and
 * slices it here, which is also what Linux does
 * (amdgpu_ucode.h / psp_init_toc_microcode). Every distributed byte is then a
 * verbatim copy of an upstream file, and the sizes come from the file that
 * carries the image instead of from a constant somebody typed.
 *
 * Parsing a documented container header is not reverse engineering: the layout
 * is declared in Linux's amdgpu_ucode.h, which this file cites field by field.
 * The microcode itself is moved as opaque bytes and is never disassembled.
 *
 * Freestanding: no libc, no allocation, no logging. Every refusal is a CODE,
 * because a zero offset and a rejected file must not read the same (rule 14).
 */
#ifndef RB_FWHDR_H
#define RB_FWHDR_H

#include <stdint.h>

/* struct common_firmware_header, amdgpu_ucode.h. Byte offsets, little endian.
 * Named so the field each constant stands for is readable at the use site. */
#define RB_FWHDR_O_SIZE_BYTES          0x00u
#define RB_FWHDR_O_HEADER_SIZE_BYTES   0x04u
#define RB_FWHDR_O_HEADER_VER_MAJOR    0x08u  /* uint16 */
#define RB_FWHDR_O_HEADER_VER_MINOR    0x0au  /* uint16 */
#define RB_FWHDR_O_IP_VER_MAJOR        0x0cu  /* uint16 */
#define RB_FWHDR_O_IP_VER_MINOR        0x0eu  /* uint16 */
#define RB_FWHDR_O_UCODE_VERSION       0x10u
#define RB_FWHDR_O_UCODE_SIZE_BYTES    0x14u
#define RB_FWHDR_O_UCODE_ARRAY_OFFSET  0x18u
#define RB_FWHDR_O_CRC32               0x1cu
#define RB_FWHDR_COMMON_LEN            0x20u

/* struct rlc_firmware_header_v2_0/_v2_1/_v2_2 follow the common header. The
 * three bases are counted in words exactly as that generator derived
 * them, and the hosttest pins all three. */
#define RB_FWHDR_RLC_V2_0   (RB_FWHDR_COMMON_LEN + 18u * 4u)   /* 0x68 */
#define RB_FWHDR_RLC_V2_1   (RB_FWHDR_RLC_V2_0   + 13u * 4u)   /* 0x9c */
#define RB_FWHDR_RLC_V2_2   (RB_FWHDR_RLC_V2_1   +  4u * 4u)   /* 0xac */

/* Refusals. Never 0, and distinct per reason. */
#define RB_FWHDR_OK                 0
#define RB_FWHDR_E_NULL             1   /* no buffer */
#define RB_FWHDR_E_SHORT            2   /* too small to hold the header */
#define RB_FWHDR_E_HDRSIZE          3   /* header_size_bytes implausible */
#define RB_FWHDR_E_ZEROSIZE         4   /* ucode_size_bytes is 0 */
#define RB_FWHDR_E_OFF_IN_HDR       5   /* payload would start inside the header */
#define RB_FWHDR_E_OVERRUN          6   /* offset + size runs past the file */
#define RB_FWHDR_E_OVERFLOW         7   /* offset + size wraps */
#define RB_FWHDR_E_BAD_SUB          8   /* sub-image index out of range */
#define RB_FWHDR_E_RLC_VER          9   /* not an rlc header v2.2 */
#define RB_FWHDR_E_RLC_IP          10   /* not ip 10.3, so not this chip */
#define RB_FWHDR_E_NO_OUT          11   /* caller passed no output pointer */

/* The five RLC sub-images, in the order Apple's _gc_10_3_get_fw_constants
 * registers them. The hosttest pins each index to its TTL id. */
#define RB_FWHDR_RLC_SRLIST_CNTL    0
#define RB_FWHDR_RLC_SRLIST_GPM     1
#define RB_FWHDR_RLC_SRLIST_SRM     2
#define RB_FWHDR_RLC_LX6_IRAM       3
#define RB_FWHDR_RLC_LX6_DRAM       4
#define RB_FWHDR_RLC_SUB_COUNT      5

static inline uint32_t rb_fwhdr_u32(const uint8_t *p, uint32_t off) {
    return (uint32_t)p[off]
         | ((uint32_t)p[off + 1u] << 8)
         | ((uint32_t)p[off + 2u] << 16)
         | ((uint32_t)p[off + 3u] << 24);
}

static inline uint16_t rb_fwhdr_u16(const uint8_t *p, uint32_t off) {
    return (uint16_t)((uint32_t)p[off] | ((uint32_t)p[off + 1u] << 8));
}

/* Shared bounds check, so the main payload and every sub-image are refused for
 * the same reasons with the same codes. */
static inline int rb_fwhdr_check_range(uint32_t file_len, uint32_t hdr_len,
                                       uint32_t off, uint32_t size) {
    if (size == 0u) {
        return RB_FWHDR_E_ZEROSIZE;
    }
    if (off < hdr_len) {
        return RB_FWHDR_E_OFF_IN_HDR;
    }
    if (off > 0xFFFFFFFFu - size) {
        return RB_FWHDR_E_OVERFLOW;
    }
    if (off + size > file_len) {
        return RB_FWHDR_E_OVERRUN;
    }
    return RB_FWHDR_OK;
}

/* The main payload: what Linux hands the PSP, and what Apple's _IpiSetFwEntry
 * carries as data_ptr/size. */
static inline int rb_fwhdr_payload(const uint8_t *file, uint32_t file_len,
                                   uint32_t *out_off, uint32_t *out_size) {
    if (!file) {
        return RB_FWHDR_E_NULL;
    }
    if (!out_off || !out_size) {
        return RB_FWHDR_E_NO_OUT;
    }
    if (file_len < RB_FWHDR_COMMON_LEN) {
        return RB_FWHDR_E_SHORT;
    }

    uint32_t hdr_len = rb_fwhdr_u32(file, RB_FWHDR_O_HEADER_SIZE_BYTES);
    if (hdr_len < RB_FWHDR_COMMON_LEN || hdr_len > file_len) {
        return RB_FWHDR_E_HDRSIZE;
    }

    uint32_t off  = rb_fwhdr_u32(file, RB_FWHDR_O_UCODE_ARRAY_OFFSET);
    uint32_t size = rb_fwhdr_u32(file, RB_FWHDR_O_UCODE_SIZE_BYTES);

    int rc = rb_fwhdr_check_range(file_len, hdr_len, off, size);
    if (rc != RB_FWHDR_OK) {
        return rc;
    }

    /* Written only on OK, so a refused call cannot leave a plausible-looking
     * offset behind for a caller that ignored the code. */
    *out_off  = off;
    *out_size = size;
    return RB_FWHDR_OK;
}

/* One of the five RLC sub-images. The size field and the offset field sit
 * adjacent in that order for every one of them, which is why a table of two
 * offsets covers all five. */
static inline int rb_fwhdr_rlc_sub(const uint8_t *file, uint32_t file_len,
                                   int which,
                                   uint32_t *out_off, uint32_t *out_size) {
    if (!file) {
        return RB_FWHDR_E_NULL;
    }
    if (!out_off || !out_size) {
        return RB_FWHDR_E_NO_OUT;
    }
    if (which < 0 || which >= RB_FWHDR_RLC_SUB_COUNT) {
        return RB_FWHDR_E_BAD_SUB;
    }
    if (file_len < RB_FWHDR_RLC_V2_2) {
        return RB_FWHDR_E_SHORT;
    }

    /* v2.2 is what this chip's file carries, and the sub-image fields below
     * only exist from v2.1 on, so the version is a precondition and not a
     * detail. Refuse rather than read a field that may not be there. */
    if (rb_fwhdr_u16(file, RB_FWHDR_O_HEADER_VER_MAJOR) != 2u ||
        rb_fwhdr_u16(file, RB_FWHDR_O_HEADER_VER_MINOR) != 2u) {
        return RB_FWHDR_E_RLC_VER;
    }
    if (rb_fwhdr_u16(file, RB_FWHDR_O_IP_VER_MAJOR) != 10u ||
        rb_fwhdr_u16(file, RB_FWHDR_O_IP_VER_MINOR) != 3u) {
        return RB_FWHDR_E_RLC_IP;
    }

    static const uint32_t size_at[RB_FWHDR_RLC_SUB_COUNT] = {
        RB_FWHDR_RLC_V2_0 + 12u,   /* save_restore_list_cntl_size_bytes */
        RB_FWHDR_RLC_V2_0 + 28u,   /* save_restore_list_gpm_size_bytes */
        RB_FWHDR_RLC_V2_0 + 44u,   /* save_restore_list_srm_size_bytes */
        RB_FWHDR_RLC_V2_1 +  0u,   /* rlc_iram_ucode_size_bytes */
        RB_FWHDR_RLC_V2_1 +  8u,   /* rlc_dram_ucode_size_bytes */
    };
    static const uint32_t off_at[RB_FWHDR_RLC_SUB_COUNT] = {
        RB_FWHDR_RLC_V2_0 + 16u,
        RB_FWHDR_RLC_V2_0 + 32u,
        RB_FWHDR_RLC_V2_0 + 48u,
        RB_FWHDR_RLC_V2_1 +  4u,
        RB_FWHDR_RLC_V2_1 + 12u,
    };

    uint32_t hdr_len = rb_fwhdr_u32(file, RB_FWHDR_O_HEADER_SIZE_BYTES);
    if (hdr_len < RB_FWHDR_RLC_V2_2 || hdr_len > file_len) {
        return RB_FWHDR_E_HDRSIZE;
    }

    uint32_t size = rb_fwhdr_u32(file, size_at[which]);
    uint32_t off  = rb_fwhdr_u32(file, off_at[which]);

    int rc = rb_fwhdr_check_range(file_len, hdr_len, off, size);
    if (rc != RB_FWHDR_OK) {
        return rc;
    }

    *out_off  = off;
    *out_size = size;
    return RB_FWHDR_OK;
}

#endif /* RB_FWHDR_H */
