/* SPDX-License-Identifier: MIT */
/*
 * AMD IP discovery table parser.
 *
 * The table is a binary the firmware leaves at the top of the GPU's memory
 * carve-out; it lists every IP block on the chip (GC, MP0, DCN, ...) with its
 * version and the MMIO base addresses its registers sit at. Layout and checks
 * follow Linux: docs/linux-ref/discovery.h and amdgpu_discovery.c
 * (amdgpu_discovery_init, _table_check, _reg_base_init).
 *
 * Freestanding C: no libc, no allocation, no kernel types. The same file is
 * compiled into the kext and into the host tools (tools/rbdump, the hosttest).
 * Every read is bounds-checked against the buffer length; a malformed table
 * yields an error code, never an out-of-bounds read.
 */
#ifndef RB_DISCOVERY_H
#define RB_DISCOVERY_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RB_DISC_BINARY_SIGNATURE   0x28211407u
#define RB_DISC_TABLE_SIGNATURE    0x53445049u  /* "IPDS" */
#define RB_DISC_GC_TABLE_ID        0x4347u      /* "GC" */
#define RB_DISC_HARVEST_SIGNATURE  0x56524148u  /* "HARV" */
#define RB_DISC_VCN_TABLE_ID       0x004E4356u
#define RB_DISC_MALL_TABLE_ID      0x4C4C414Du

/* amdgpu_discovery.h */
#define RB_DISC_TMR_SIZE    (10u << 10)
#define RB_DISC_TMR_OFFSET  (64u << 10)

#define RB_DISC_MAX_IPS     128
#define RB_DISC_MAX_BASES   8
#define RB_DISC_MAX_HARVEST 32

/* Hardware IDs used by this project (docs/linux-ref/soc15_hw_ip.h). */
#define RB_HWID_MP1    1
#define RB_HWID_GC     11
#define RB_HWID_MMHUB  34
#define RB_HWID_NBIF   108
#define RB_HWID_MP0    255
#define RB_HWID_DMU    271

enum {
    RB_DISC_OK = 0,
    RB_DISC_E_ARG = -1,          /* NULL buffer or output */
    RB_DISC_E_SHORT = -2,        /* buffer smaller than the binary header */
    RB_DISC_E_SIGNATURE = -3,    /* binary signature is not 0x28211407 */
    RB_DISC_E_VERSION = -4,      /* binary header version not 0, 1 or 2 */
    RB_DISC_E_SIZE = -5,         /* binary_size larger than the buffer or too small */
    RB_DISC_E_NO_IP_TABLE = -6,  /* IP discovery table offset is 0 or out of bounds */
    RB_DISC_E_IP_SIGNATURE = -7, /* IP discovery table signature is not "IPDS" */
    RB_DISC_E_DIE = -8,          /* bad die count, die offset or die id */
    RB_DISC_E_IP_BOUNDS = -9,    /* an IP entry runs past the end of the binary */
};

/* Bits in rb_disc.check_flags: checks Linux makes that failed. Linux refuses
 * the whole table when any of them is set; this parser records them and keeps
 * going while the structure stays in bounds, so a probe still reports what it
 * could read. */
#define RB_DISC_F_BINARY_CHECKSUM  (1u << 0)
#define RB_DISC_F_IP_CHECKSUM      (1u << 1)
#define RB_DISC_F_GC_SIGNATURE     (1u << 2)
#define RB_DISC_F_GC_CHECKSUM      (1u << 3)
#define RB_DISC_F_HARVEST_SIG      (1u << 4)
#define RB_DISC_F_HARVEST_CHECKSUM (1u << 5)
#define RB_DISC_F_VCN_SIGNATURE    (1u << 6)
#define RB_DISC_F_VCN_CHECKSUM     (1u << 7)
#define RB_DISC_F_MALL_SIGNATURE   (1u << 8)
#define RB_DISC_F_TABLE_BOUNDS     (1u << 9)   /* a table offset/size runs past the binary */
#define RB_DISC_F_IPS_TRUNCATED    (1u << 10)  /* more than RB_DISC_MAX_IPS entries */
#define RB_DISC_F_BASES_TRUNCATED  (1u << 11)  /* an entry had more than RB_DISC_MAX_BASES */
#define RB_DISC_F_IP_INVALID       (1u << 12)  /* entry Linux skips (instance >= 48 or hw_id >= 300) */

struct rb_ip {
    uint16_t hw_id;
    uint16_t die;
    uint8_t  inst;
    uint8_t  nbase;      /* as stored in the table */
    uint8_t  major, minor, revision;
    uint8_t  sub_revision, variant;  /* table version >= 3, else 0 */
    uint8_t  harvest;                /* table version < 3, else 0 */
    uint32_t base[RB_DISC_MAX_BASES];  /* dword offsets into the MMIO BAR */
};

/* The fields common to gc_info v1.0 - v1.3, in table order. */
enum {
    RB_GC_NUM_SE, RB_GC_NUM_WGP0_PER_SA, RB_GC_NUM_WGP1_PER_SA, RB_GC_NUM_RB_PER_SE,
    RB_GC_NUM_GL2C, RB_GC_NUM_GPRS, RB_GC_NUM_MAX_GS_THDS, RB_GC_GS_TABLE_DEPTH,
    RB_GC_GSPRIM_BUFF_DEPTH, RB_GC_PARAMETER_CACHE_DEPTH, RB_GC_DOUBLE_OFFCHIP_LDS_BUFFER,
    RB_GC_WAVE_SIZE, RB_GC_MAX_WAVES_PER_SIMD, RB_GC_MAX_SCRATCH_SLOTS_PER_CU, RB_GC_LDS_SIZE,
    RB_GC_NUM_SC_PER_SE, RB_GC_NUM_SA_PER_SE, RB_GC_NUM_PACKER_PER_SC, RB_GC_NUM_GL2A,
    RB_GC_V1_FIELDS
};

struct rb_gc_info {
    uint8_t  present;   /* 0 none, 1 v1.x parsed, 2 other version (fields zero) */
    uint16_t version_major, version_minor;
    uint32_t v1[RB_GC_V1_FIELDS];
};

struct rb_disc {
    uint16_t bin_version_major, bin_version_minor;
    uint16_t bin_size;
    uint16_t ip_table_version;
    uint8_t  base_addr_64_bit;
    uint16_t num_dies;
    uint32_t check_flags;
    uint32_t nips;
    struct rb_ip ips[RB_DISC_MAX_IPS];
    struct rb_gc_info gc;
    uint32_t nharvest;
    struct { uint16_t hw_id; uint8_t inst; } harvest[RB_DISC_MAX_HARVEST];
};

/* Parse `len` bytes at `bin` into `out` (zeroed first). Returns RB_DISC_OK or
 * a negative RB_DISC_E_* code; on RB_DISC_OK, out->check_flags may still list
 * failed checks. */
int rb_disc_parse(const uint8_t *bin, uint32_t len, struct rb_disc *out);

const char *rb_disc_strerror(int err);
const char *rb_hwid_name(uint16_t hw_id);   /* "GC", "MP0", ...; "?" if unknown */

/* First entry with this hw_id and instance, or NULL. */
const struct rb_ip *rb_disc_find(const struct rb_disc *d, uint16_t hw_id, uint8_t inst);

/* Absolute dword offset of register `reg` (a Linux regFOO value) in base
 * segment `base_idx` (regFOO_BASE_IDX) of IP hw_id/inst, as Linux's
 * SOC15_REG_OFFSET computes it. Returns 0 on success, -1 if the IP or the base
 * segment is missing. */
int rb_disc_reg(const struct rb_disc *d, uint16_t hw_id, uint8_t inst,
                uint8_t base_idx, uint32_t reg, uint32_t *out_dw);

#ifdef __cplusplus
}
#endif

#endif /* RB_DISCOVERY_H */
