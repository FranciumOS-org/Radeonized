/* SPDX-License-Identifier: MIT */
/*
 * Register offsets the probe reads. Values are Linux's regFOO / mmFOO dword
 * offsets and their _BASE_IDX, from torvalds/linux at the commit in
 * docs/linux-ref/UPSTREAM.md:
 *
 *   fixed   amdgpu_discovery.c ("consistent across all the SOCs")
 *   MP0/MP1 include/asic_reg/mp/mp_13_0_2_offset.h (used by psp_v13_0.c and
 *           yellow_carp_ppt.c for Rembrandt's PSP/SMU 13.0.1/13.0.3/13.0.8)
 *   MMHUB   include/asic_reg/mmhub/mmhub_2_3_0_offset.h (gmc_v10_0.c drives
 *           MMHUB 2.3.0, 2.4.0 and 2.4.1 with mmhub_v2_3.c, which uses it)
 *   GC      include/asic_reg/gc/gc_10_3_0_offset.h (Linux uses it for GC 10.3.3)
 *   DCN     include/asic_reg/dcn/dcn_3_1_2_offset.h (DCN 3.1.2 and 3.1.3 are
 *           both DCN_VERSION_3_1 in Linux; dcn31_resource.c uses this header)
 *
 * Every register here is only READ. The single exception is the
 * MM_INDEX / MM_INDEX_HI pair, which selects what MM_DATA reads; see
 * rb_probe.c.
 */
#ifndef RB_REGS_H
#define RB_REGS_H

#include <stdint.h>
#include "rb_discovery.h"
#include "rb_probe.h"

/* Fixed dword offsets into the MMIO BAR. */
#define RB_mmMM_INDEX            0x0
#define RB_mmMM_DATA             0x1
#define RB_mmMM_INDEX_HI         0x6
#define RB_mmDRIVER_SCRATCH_0    0x94
#define RB_mmDRIVER_SCRATCH_1    0x95
#define RB_mmDRIVER_SCRATCH_2    0x96
#define RB_mmRCC_CONFIG_MEMSIZE  0xde3
#define RB_mmMP0_SMN_C2PMSG_33   0x16061
#define RB_mmIP_DISCOVERY_VERSION 0x16A00

struct rb_reg_def {
    const char *name;
    uint16_t hw_id;
    uint8_t  base_idx;
    uint8_t  level;
    uint8_t  want_major;   /* IP major version the offset is valid for */
    uint8_t  minor_lo, minor_hi;   /* ... and its minor range (Linux's grouping) */
    uint32_t reg;
};

static const struct rb_reg_def rb_reg_defs[] = {
    /* PSP (MP0). C2PMSG_35 bit 31: bootloader ready. C2PMSG_81 != 0: secure
     * OS running (psp_v13_0_is_sos_alive). C2PMSG_33 bit 31: IFWI init done. */
    { "MP0_C2PMSG_33",  RB_HWID_MP0, 0, RB_LEVEL_SAFE, 13, 0, 0xff, 0x0061 },
    { "MP0_C2PMSG_35",  RB_HWID_MP0, 0, RB_LEVEL_SAFE, 13, 0, 0xff, 0x0063 },
    { "MP0_C2PMSG_81",  RB_HWID_MP0, 0, RB_LEVEL_SAFE, 13, 0, 0xff, 0x0091 },
    /* SMU (MP1) mailbox: 66 message, 82 argument, 90 response. */
    { "MP1_C2PMSG_66",  RB_HWID_MP1, 0, RB_LEVEL_SAFE, 13, 0, 0xff, 0x0282 },
    { "MP1_C2PMSG_82",  RB_HWID_MP1, 0, RB_LEVEL_SAFE, 13, 0, 0xff, 0x0292 },
    { "MP1_C2PMSG_90",  RB_HWID_MP1, 0, RB_LEVEL_SAFE, 13, 0, 0xff, 0x029a },
    /* Where the carve-out sits: FB_LOCATION in 16 MB units of GPU address
     * space, FB_OFFSET its system-physical address in 16 MB units. */
    { "MMMC_VM_FB_OFFSET",        RB_HWID_MMHUB, 1, RB_LEVEL_SAFE, 2, 3, 4, 0x08d7 },
    { "MMMC_VM_FB_LOCATION_BASE", RB_HWID_MMHUB, 1, RB_LEVEL_SAFE, 2, 3, 4, 0x08ec },
    { "MMMC_VM_FB_LOCATION_TOP",  RB_HWID_MMHUB, 1, RB_LEVEL_SAFE, 2, 3, 4, 0x08ed },
    { "MMMC_VM_AGP_BASE",         RB_HWID_MMHUB, 1, RB_LEVEL_SAFE, 2, 3, 4, 0x08f0 },

    /* Level 3: graphics core. */
    { "GRBM_STATUS",              RB_HWID_GC, 0, RB_LEVEL_GC, 10, 3, 3, 0x0da4 },
    { "GRBM_STATUS2",             RB_HWID_GC, 0, RB_LEVEL_GC, 10, 3, 3, 0x0da2 },
    { "CP_STAT",                  RB_HWID_GC, 0, RB_LEVEL_GC, 10, 3, 3, 0x0f40 },
    { "CP_ME_CNTL",               RB_HWID_GC, 0, RB_LEVEL_GC, 10, 3, 3, 0x0f56 },
    { "GCMC_VM_FB_OFFSET",        RB_HWID_GC, 0, RB_LEVEL_GC, 10, 3, 3, 0x16e7 },
    { "GCMC_VM_FB_LOCATION_BASE", RB_HWID_GC, 0, RB_LEVEL_GC, 10, 3, 3, 0x16fc },
    { "GCMC_VM_FB_LOCATION_TOP",  RB_HWID_GC, 0, RB_LEVEL_GC, 10, 3, 3, 0x16fd },
    { "RLC_CNTL",                 RB_HWID_GC, 1, RB_LEVEL_GC, 10, 3, 3, 0x4c00 },
    { "RLC_STAT",                 RB_HWID_GC, 1, RB_LEVEL_GC, 10, 3, 3, 0x4c04 },

    /* Level 2: display engine (DCN 3.1.x), which pipe scans out what. */
    { "DMCUB_CNTL",       RB_HWID_DMU, 2, RB_LEVEL_DC, 3, 1, 1, 0x01f6 },
    { "DMCUB_SCRATCH0",   RB_HWID_DMU, 2, RB_LEVEL_DC, 3, 1, 1, 0x01e3 },
    { "OTG0_CONTROL",     RB_HWID_DMU, 2, RB_LEVEL_DC, 3, 1, 1, 0x1b41 },
    { "OTG0_H_TOTAL",     RB_HWID_DMU, 2, RB_LEVEL_DC, 3, 1, 1, 0x1b2a },
    { "OTG0_V_TOTAL",     RB_HWID_DMU, 2, RB_LEVEL_DC, 3, 1, 1, 0x1b2f },
    { "OTG1_CONTROL",     RB_HWID_DMU, 2, RB_LEVEL_DC, 3, 1, 1, 0x1bc1 },
    { "OTG1_H_TOTAL",     RB_HWID_DMU, 2, RB_LEVEL_DC, 3, 1, 1, 0x1baa },
    { "OTG1_V_TOTAL",     RB_HWID_DMU, 2, RB_LEVEL_DC, 3, 1, 1, 0x1baf },
    { "OTG2_CONTROL",     RB_HWID_DMU, 2, RB_LEVEL_DC, 3, 1, 1, 0x1c41 },
    { "OTG2_H_TOTAL",     RB_HWID_DMU, 2, RB_LEVEL_DC, 3, 1, 1, 0x1c2a },
    { "OTG2_V_TOTAL",     RB_HWID_DMU, 2, RB_LEVEL_DC, 3, 1, 1, 0x1c2f },
    { "OTG3_CONTROL",     RB_HWID_DMU, 2, RB_LEVEL_DC, 3, 1, 1, 0x1cc1 },
    { "OTG3_H_TOTAL",     RB_HWID_DMU, 2, RB_LEVEL_DC, 3, 1, 1, 0x1caa },
    { "OTG3_V_TOTAL",     RB_HWID_DMU, 2, RB_LEVEL_DC, 3, 1, 1, 0x1caf },
    { "HUBP0_SURFACE_CONFIG",  RB_HWID_DMU, 2, RB_LEVEL_DC, 3, 1, 1, 0x05e5 },
    { "HUBP0_PRIMARY_ADDR",    RB_HWID_DMU, 2, RB_LEVEL_DC, 3, 1, 1, 0x060a },
    { "HUBP0_PRIMARY_ADDR_HI", RB_HWID_DMU, 2, RB_LEVEL_DC, 3, 1, 1, 0x060b },
    { "HUBP1_SURFACE_CONFIG",  RB_HWID_DMU, 2, RB_LEVEL_DC, 3, 1, 1, 0x06c1 },
    { "HUBP1_PRIMARY_ADDR",    RB_HWID_DMU, 2, RB_LEVEL_DC, 3, 1, 1, 0x06e6 },
    { "HUBP1_PRIMARY_ADDR_HI", RB_HWID_DMU, 2, RB_LEVEL_DC, 3, 1, 1, 0x06e7 },
    { "HUBP2_SURFACE_CONFIG",  RB_HWID_DMU, 2, RB_LEVEL_DC, 3, 1, 1, 0x079d },
    { "HUBP2_PRIMARY_ADDR",    RB_HWID_DMU, 2, RB_LEVEL_DC, 3, 1, 1, 0x07c2 },
    { "HUBP2_PRIMARY_ADDR_HI", RB_HWID_DMU, 2, RB_LEVEL_DC, 3, 1, 1, 0x07c3 },
    { "HUBP3_SURFACE_CONFIG",  RB_HWID_DMU, 2, RB_LEVEL_DC, 3, 1, 1, 0x0879 },
    { "HUBP3_PRIMARY_ADDR",    RB_HWID_DMU, 2, RB_LEVEL_DC, 3, 1, 1, 0x089e },
    { "HUBP3_PRIMARY_ADDR_HI", RB_HWID_DMU, 2, RB_LEVEL_DC, 3, 1, 1, 0x089f },

    /* 2oq: the full timing of the pipe that is actually scanning out, so a
     * framebuffer can be published with the real numbers instead of the boot
     * framebuffer's single 0 Hz mode.
     *
     * OTG0 ONLY, deliberately. The probe reads all four OTGn_CONTROLs and
     * boot 2oq measured OTG0_CONTROL = 0x00011201 (OTG_MASTER_EN set) with
     * OTG1-3 at 0x300 (off), so OTG0 is the live pipe on this machine. If a
     * later boot shows another pipe live, extending is mechanical: the OTG
     * instance stride is 0x80 dwords (OTG1_H_TOTAL 0x1baa - OTG0 0x1b2a) and
     * the HUBP stride is 0xdc (HUBP1_SURFACE_CONFIG 0x06c1 - HUBP0 0x05e5).
     *
     * FRAME_COUNT is the one that answers the question H_TOTAL and V_TOTAL
     * cannot: read it twice a known interval apart and the difference IS the
     * refresh rate, with no need to know the pixel clock or trust a panel
     * descriptor. Read-only, so it stays inside the probe's write rules. */
    { "OTG0_STATUS",            RB_HWID_DMU, 2, RB_LEVEL_DC, 3, 1, 1, 0x1b49 },
    { "OTG0_STATUS_POSITION",   RB_HWID_DMU, 2, RB_LEVEL_DC, 3, 1, 1, 0x1b4a },
    { "OTG0_FRAME_COUNT",       RB_HWID_DMU, 2, RB_LEVEL_DC, 3, 1, 1, 0x1b4c },
    { "OTG0_H_BLANK_START_END", RB_HWID_DMU, 2, RB_LEVEL_DC, 3, 1, 1, 0x1b2b },
    { "OTG0_H_SYNC_A",          RB_HWID_DMU, 2, RB_LEVEL_DC, 3, 1, 1, 0x1b2c },
    { "OTG0_H_SYNC_A_CNTL",     RB_HWID_DMU, 2, RB_LEVEL_DC, 3, 1, 1, 0x1b2d },
    { "OTG0_V_BLANK_START_END", RB_HWID_DMU, 2, RB_LEVEL_DC, 3, 1, 1, 0x1b36 },
    { "OTG0_V_SYNC_A",          RB_HWID_DMU, 2, RB_LEVEL_DC, 3, 1, 1, 0x1b37 },
    { "OTG0_V_SYNC_A_CNTL",     RB_HWID_DMU, 2, RB_LEVEL_DC, 3, 1, 1, 0x1b38 },
    { "HUBP0_SURFACE_PITCH",    RB_HWID_DMU, 2, RB_LEVEL_DC, 3, 1, 1, 0x0607 },
    { "HUBP0_SURFACE_CONTROL",  RB_HWID_DMU, 2, RB_LEVEL_DC, 3, 1, 1, 0x061a },
};

#define RB_NREG_DEFS (sizeof(rb_reg_defs) / sizeof(rb_reg_defs[0]))

#endif /* RB_REGS_H */
