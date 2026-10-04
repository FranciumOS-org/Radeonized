/* SPDX-License-Identifier: MIT */
/*
 * Read-only probe of a Rembrandt-class AMD APU, with no driver attached.
 *
 * Hardware access goes through struct rb_hw so that the same code runs in the
 * kext (real MMIO) and in tools/hosttest (a simulated chip). Freestanding C,
 * no allocation: the caller owns struct rb_probe_result (~40 KB; allocate it,
 * never put it on the kernel stack).
 *
 * The only register writes this code can make are to MM_INDEX / MM_INDEX_HI,
 * to read GPU memory through MM_DATA the way amdgpu_device_mm_access does.
 * Their original values are restored afterwards. Any other write is refused
 * by rb_probe.c itself, before it reaches rb_hw.
 */
#ifndef RB_PROBE_H
#define RB_PROBE_H

#include <stdint.h>
#include "rb_discovery.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Probe levels. 1 is the default: blocks that are powered whenever the
 * machine is on (PSP/SMU mailboxes) or that the boot display already uses
 * (memory hub). 2 adds the display engine, which is lit (the boot display
 * scans out through it) and whose OTG state Linux reads at init. 3 adds the
 * graphics core, which may be power-gated with no driver loaded. */
#define RB_LEVEL_SAFE 1
#define RB_LEVEL_DC   2
#define RB_LEVEL_GC   3
#define RB_LEVEL_MAX  RB_LEVEL_GC

struct rb_hw {
    void *ctx;
    uint32_t mmio_len;   /* bytes mapped of the register BAR (BAR5) */
    uint32_t (*rreg)(void *ctx, uint32_t byte_off);
    void     (*wreg)(void *ctx, uint32_t byte_off, uint32_t value);
    /* Optional framebuffer BAR (BAR0) read, for the cross-check only:
     * copy `size` bytes at `byte_off` into dst; 0 on success. NULL: skipped.
     * fb_len is the BAR's length. */
    uint64_t fb_len;
    int (*fb_read)(void *ctx, uint64_t byte_off, uint8_t *dst, uint32_t size);
};

enum rb_reg_status {
    RB_REG_OK = 0,
    RB_REG_NO_IP,         /* the IP or its base segment is not in the table */
    RB_REG_VERSION,       /* the IP version differs from the one the offset is for */
    RB_REG_OUT_OF_BAR,    /* offset past the mapped register BAR */
    RB_REG_LEVEL,         /* above the requested probe level: not read */
};

struct rb_reg_sample {
    const char *name;
    uint32_t dw;          /* absolute dword offset (when resolved) */
    uint32_t value;
    uint8_t  status;      /* enum rb_reg_status */
};

#define RB_PROBE_BIN_MAX  (16u << 10)
#define RB_PROBE_MAX_REGS 64

enum rb_disc_source {
    RB_SRC_NONE = 0,
    RB_SRC_DEFAULT,       /* (MEMSIZE << 20) - 64 KB held the signature */
    RB_SRC_SCRATCH,       /* DRIVER_SCRATCH_0/1/2 gave offset and size */
    RB_SRC_DEFAULT_NOSIG, /* no signature anywhere; default offset read anyway, as Linux does */
};

struct rb_probe_result {
    int level;
    const char *fail;     /* first fatal problem, or NULL */

    uint32_t memsize_mb;  /* RCC_CONFIG_MEMSIZE: carve-out size in MB */
    uint32_t scratch[3];  /* DRIVER_SCRATCH_0..2 */
    uint32_t c2pmsg33;    /* MP0_SMN_C2PMSG_33 at its fixed offset */
    uint32_t ipd_version; /* IP_DISCOVERY_VERSION */
    uint32_t mm_index_saved, mm_index_hi_saved;

    uint8_t  disc_source; /* enum rb_disc_source */
    uint64_t disc_offset; /* offset in GPU memory */
    uint32_t disc_size;   /* bytes read into disc_bin */
    uint8_t  bar0_checked;   /* 1 if disc_offset was inside BAR0 (past its first
                              * 16 MB, the boot framebuffer) and compared */
    uint8_t  bar0_matches;   /* 1 if BAR0 at disc_offset held the same bytes */
    uint8_t  disc_bin[RB_PROBE_BIN_MAX];
    uint8_t  bar0_bin[RB_PROBE_BIN_MAX];   /* scratch for the cross-check */

    int parse_err;        /* rb_disc_parse result; meaningful if disc_size != 0 */
    struct rb_disc disc;

    uint32_t nregs;
    struct rb_reg_sample regs[RB_PROBE_MAX_REGS];

    uint32_t index_writes;     /* MM_INDEX / MM_INDEX_HI writes made */
    uint32_t refused_writes;   /* writes refused by the policy (must stay 0) */
};

/* Run the probe at `level` (RB_LEVEL_SAFE .. RB_LEVEL_MAX). Fills `r`
 * (zeroed first). Returns 0 if the discovery table was read and parsed,
 * -1 otherwise (r->fail says why; whatever was read is still in r). */
int rb_probe_run(const struct rb_hw *hw, int level, struct rb_probe_result *r);

#ifdef __cplusplus
}
#endif

#endif /* RB_PROBE_H */
