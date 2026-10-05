/* SPDX-License-Identifier: MIT */
/*
 * SMU 13 (MP1 13.0.1 / 13.0.3 / 13.0.8, "Yellow Carp" = Rembrandt) power
 * management, ported from Linux pm/swsmu/smu13/yellow_carp_ppt.c and the
 * smu_v13_0.c / smu_cmn.c helpers it uses. This is the layer Apple's SMU
 * function list (smu_11_0_7_create_function_pointer_list's 27 slots, see
 * docs/APPLE-STACK.md B3) is to be built on.
 *
 * Freestanding C, no allocation; hardware through struct rb_hw (rb_probe.h)
 * like the probe, so tools/hosttest runs it against a simulated SMU.
 *
 * Transport: the same mailbox and protocol as rb_smu.c (C2PMSG_66 message,
 * _82 argument, _90 response; smu_msg_v1_send_msg). The only registers
 * written are those three; SMUIO_GFX_MISC_CNTL is read for the GFXOFF status.
 *
 * Policy: messages are grouped (RB_SMU13_ALLOW_*). rb_smu13_init() allows
 * only RB_SMU13_ALLOW_QUERY, the messages the probe already sends on this
 * machine. Every other group changes the chip's state and has to be allowed
 * by the caller with rb_smu13_allow() before any of its messages is sent; a
 * message outside the allowed groups returns RB_SMU13_E_NOT_ALLOWED without
 * touching the mailbox. A timeout makes the context dead: nothing more is sent
 * until rb_smu13_init() again.
 *
 * Table transfers (DPM clocks, metrics, watermarks) go through a DMA buffer
 * the caller owns (struct rb_smu13_dma): the SMU reads and writes it at its
 * GPU (MC) address, the CPU at `cpu`.
 */
#ifndef RB_SMU13_H
#define RB_SMU13_H

#include <stdint.h>
#include "rb_discovery.h"
#include "rb_probe.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------- messages (ppsmc.h) --- */
/* smu_v13_0_1_ppsmc.h; only those yellow_carp_message_map maps. */
#define RB_SMU13_MSG_TestMessage               0x01
#define RB_SMU13_MSG_GetSmuVersion             0x02
#define RB_SMU13_MSG_GetDriverIfVersion        0x03
#define RB_SMU13_MSG_EnableGfxOff              0x04
#define RB_SMU13_MSG_PowerDownVcn              0x06
#define RB_SMU13_MSG_PowerUpVcn                0x07
#define RB_SMU13_MSG_SetHardMinVcn             0x08
#define RB_SMU13_MSG_PrepareMp1ForUnload       0x0C
#define RB_SMU13_MSG_SetDriverDramAddrHigh     0x0D
#define RB_SMU13_MSG_SetDriverDramAddrLow      0x0E
#define RB_SMU13_MSG_TransferTableSmu2Dram     0x0F
#define RB_SMU13_MSG_TransferTableDram2Smu     0x10
#define RB_SMU13_MSG_GfxDeviceDriverReset      0x11
#define RB_SMU13_MSG_GetEnabledSmuFeatures     0x12
#define RB_SMU13_MSG_SetHardMinSocclkByFreq    0x13
#define RB_SMU13_MSG_SetSoftMinVcn             0x15
#define RB_SMU13_MSG_GetGfxclkFrequency        0x17
#define RB_SMU13_MSG_GetFclkFrequency          0x18
#define RB_SMU13_MSG_AllowGfxOff               0x19
#define RB_SMU13_MSG_DisallowGfxOff            0x1A
#define RB_SMU13_MSG_SetSoftMaxGfxClk          0x1B
#define RB_SMU13_MSG_SetHardMinGfxClk          0x1C
#define RB_SMU13_MSG_SetSoftMaxSocclkByFreq    0x1D
#define RB_SMU13_MSG_SetSoftMaxFclkByFreq      0x1E
#define RB_SMU13_MSG_SetSoftMaxVcn             0x1F
#define RB_SMU13_MSG_SetPowerLimitPercentage   0x20
#define RB_SMU13_MSG_PowerDownJpeg             0x21
#define RB_SMU13_MSG_PowerUpJpeg               0x22
#define RB_SMU13_MSG_SetHardMinFclkByFreq      0x23
#define RB_SMU13_MSG_SetSoftMinSocclkByFreq    0x24
#define RB_SMU13_MSG_COUNT                     0x29

/* Mode argument of GfxDeviceDriverReset (SMU_RESET_MODE_2). */
#define RB_SMU13_RESET_MODE_2 2

/* ------------------------------------------------------- message groups --- */
#define RB_SMU13_ALLOW_QUERY    0x01u  /* TestMessage, versions, feature mask, Get*Frequency */
#define RB_SMU13_ALLOW_TABLES   0x02u  /* driver table address, table transfers */
#define RB_SMU13_ALLOW_GFXOFF   0x04u  /* EnableGfxOff, Allow/DisallowGfxOff */
#define RB_SMU13_ALLOW_CLOCKS   0x08u  /* hard/soft min/max of GFX, SOC, FCLK, VCN; power limit */
#define RB_SMU13_ALLOW_MMPOWER  0x10u  /* VCN and JPEG power up/down */
#define RB_SMU13_ALLOW_UNLOAD   0x20u  /* PrepareMp1ForUnload */
#define RB_SMU13_ALLOW_RESET    0x40u  /* GfxDeviceDriverReset */

/* --------------------------------------------------------------- errors --- */
#define RB_SMU13_OK               0
#define RB_SMU13_E_INVAL         -1   /* bad argument or context */
#define RB_SMU13_E_NOT_ALLOWED   -2   /* message group not allowed: nothing sent */
#define RB_SMU13_E_BUSY          -3   /* mailbox busy or in an unknown state: nothing sent */
#define RB_SMU13_E_TIMEOUT       -4   /* sent, no answer; the context is now dead */
#define RB_SMU13_E_FW            -5   /* the SMU answered with an error (last_resp) */
#define RB_SMU13_E_VERSION       -6   /* not an MP1 13.0.1/3/8, or interface != 4 */
#define RB_SMU13_E_NODATA        -7   /* needs a table that is not loaded */
#define RB_SMU13_E_DEAD          -8   /* an earlier timeout: nothing sent */

/* Response codes (PPSMC_Result_*). */
#define RB_SMU13_RESP_NONE         0x00
#define RB_SMU13_RESP_OK           0x01
#define RB_SMU13_RESP_FAILED       0xFF
#define RB_SMU13_RESP_UNKNOWN_CMD  0xFE
#define RB_SMU13_RESP_REJ_PREREQ   0xFD
#define RB_SMU13_RESP_REJ_BUSY     0xFC

/* SMU13_YELLOW_CARP_DRIVER_IF_VERSION: the tables below match this. */
#define RB_SMU13_DRIVER_IF_VERSION 4

/* --------------------------------------- tables (driver_if_yellow_carp.h) --- */
/* Same layouts as Linux's DpmClocks_t, SmuMetrics_t, Watermarks_t;
 * tools/hosttest checks every field offset against the Linux header. */

#define RB_SMU13_TABLE_WATERMARKS  1
#define RB_SMU13_TABLE_DPMCLOCKS   4
#define RB_SMU13_TABLE_SMU_METRICS 7

#define RB_SMU13_NUM_DPM_LEVELS    8
#define RB_SMU13_NUM_DF_PSTATES    4
#define RB_SMU13_NUM_WM_RANGES     4

struct rb_smu13_df_pstate {
    uint32_t fclk, memclk, voltage;
    uint8_t  wck_ratio, spare[3];
};

struct rb_smu13_dpm_clocks {            /* DpmClocks_t; MHz, mV with 2 fraction bits */
    uint32_t dcf_clocks[RB_SMU13_NUM_DPM_LEVELS];
    uint32_t disp_clocks[RB_SMU13_NUM_DPM_LEVELS];
    uint32_t dpp_clocks[RB_SMU13_NUM_DPM_LEVELS];
    uint32_t soc_clocks[RB_SMU13_NUM_DPM_LEVELS];
    uint32_t vclocks[RB_SMU13_NUM_DPM_LEVELS];
    uint32_t dclocks[RB_SMU13_NUM_DPM_LEVELS];
    uint32_t soc_voltage[RB_SMU13_NUM_DPM_LEVELS];
    struct rb_smu13_df_pstate df_pstate[RB_SMU13_NUM_DF_PSTATES];
    uint8_t  num_dcf_levels, num_disp_levels, num_soc_levels, num_vcn_levels, num_df_pstates;
    uint8_t  spare[3];
    uint32_t min_gfx_clk, max_gfx_clk;
};

struct rb_smu13_metrics {               /* SmuMetrics_t */
    uint16_t gfxclk, socclk, vclk, dclk, memclk, spare;        /* MHz */
    uint16_t gfx_activity, uvd_activity;                       /* centi-percent */
    uint16_t voltage[2], current[2], power[2];                 /* mV, mA, mW: VDD, SOC */
    uint16_t core_freq[8], core_power[8], core_temp[8];
    uint16_t l3_freq, l3_temp;
    uint16_t gfx_temp, soc_temp;                               /* centi-degrees C */
    uint16_t throttler_status;
    uint16_t current_socket_power;                             /* mW */
    uint16_t stapm_opn_limit, stapm_current_limit;             /* W */
    uint32_t apu_power, dgpu_power;                            /* mW */
    uint16_t vdd_tdc, soc_tdc, vdd_edc, soc_edc;               /* mA */
    uint16_t infra_cpu_max_freq, infra_gfx_max_freq;           /* MHz */
    uint16_t skin_temp, device_state;
};

struct rb_smu13_wm_row {                /* WatermarkRowGeneric_t */
    uint16_t min_clock, max_clock, min_mclk, max_mclk;
    uint8_t  wm_setting, wm_type, padding[2];
};

struct rb_smu13_watermarks {            /* Watermarks_t; row 0 SOCCLK, 1 DCFCLK */
    struct rb_smu13_wm_row row[2][RB_SMU13_NUM_WM_RANGES];
    uint32_t mmhub_padding[7];
};

/* One watermark set as DC hands it over (pp_smu_wm_set_range). */
struct rb_smu13_wm_set {
    uint8_t  wm_inst;
    uint16_t min_fill_mhz, max_fill_mhz, min_drain_mhz, max_drain_mhz;
};

/* --------------------------------------------------- feature bits (pmfw) --- */
#define RB_SMU13_FEATURE_CCLK_DPM     0
#define RB_SMU13_FEATURE_VCN_DPM     11
#define RB_SMU13_FEATURE_FCLK_DPM    13
#define RB_SMU13_FEATURE_SOCCLK_DPM  14
#define RB_SMU13_FEATURE_MP0CLK_DPM  15
#define RB_SMU13_FEATURE_LCLK_DPM    16
#define RB_SMU13_FEATURE_SHUBCLK_DPM 17
#define RB_SMU13_FEATURE_DCFCLK_DPM  18
#define RB_SMU13_FEATURE_GFX_DPM     19
/* yellow_carp_dpm_features: all set = "DPM is running". */
#define RB_SMU13_DPM_FEATURES \
    ((1ull << RB_SMU13_FEATURE_CCLK_DPM) | (1ull << RB_SMU13_FEATURE_VCN_DPM) | \
     (1ull << RB_SMU13_FEATURE_FCLK_DPM) | (1ull << RB_SMU13_FEATURE_SOCCLK_DPM) | \
     (1ull << RB_SMU13_FEATURE_MP0CLK_DPM) | (1ull << RB_SMU13_FEATURE_LCLK_DPM) | \
     (1ull << RB_SMU13_FEATURE_SHUBCLK_DPM) | (1ull << RB_SMU13_FEATURE_DCFCLK_DPM) | \
     (1ull << RB_SMU13_FEATURE_GFX_DPM))

/* ---------------------------------------------------------------- types --- */
enum rb_smu13_clk {
    RB_SMU13_CLK_GFX,       /* SMU_GFXCLK / SMU_SCLK */
    RB_SMU13_CLK_SOC,
    RB_SMU13_CLK_FCLK,
    RB_SMU13_CLK_MCLK,      /* SMU_MCLK / SMU_UCLK */
    RB_SMU13_CLK_VCLK,
    RB_SMU13_CLK_DCLK,
};

enum rb_smu13_level {       /* amd_dpm_forced_level */
    RB_SMU13_LEVEL_AUTO,
    RB_SMU13_LEVEL_LOW,
    RB_SMU13_LEVEL_HIGH,
    RB_SMU13_LEVEL_MANUAL,
    RB_SMU13_LEVEL_PROFILE_STANDARD,
    RB_SMU13_LEVEL_PROFILE_MIN_SCLK,
    RB_SMU13_LEVEL_PROFILE_MIN_MCLK,
    RB_SMU13_LEVEL_PROFILE_PEAK,
    RB_SMU13_LEVEL_PROFILE_EXIT,
};

enum rb_smu13_sensor {      /* amd_pp_sensors yellow_carp_read_sensor answers */
    RB_SMU13_SENSOR_GPU_LOAD,       /* percent */
    RB_SMU13_SENSOR_VCN_LOAD,       /* percent */
    RB_SMU13_SENSOR_INPUT_POWER,    /* CurrentSocketPower, mW */
    RB_SMU13_SENSOR_EDGE_TEMP,      /* millidegrees C */
    RB_SMU13_SENSOR_HOTSPOT_TEMP,   /* millidegrees C */
    RB_SMU13_SENSOR_MCLK,           /* 10 kHz units (MHz * 100) */
    RB_SMU13_SENSOR_SCLK,           /* 10 kHz units */
    RB_SMU13_SENSOR_VDDGFX,         /* mV */
    RB_SMU13_SENSOR_VDDNB,          /* mV */
    RB_SMU13_SENSOR_SS_APU_SHARE,   /* percent */
    RB_SMU13_SENSOR_SS_DGPU_SHARE,  /* percent */
};

/* The driver table: DMA memory the SMU can reach. size must hold the largest
 * table (rb_smu13_table_size_max()). flush/invalidate: HDP flush before the
 * SMU reads it and invalidate after it wrote (amdgpu_hdp_flush/_invalidate);
 * may be NULL when the memory is coherent. */
struct rb_smu13_dma {
    void    *cpu;
    uint64_t mc;
    uint32_t size;
    void (*flush)(void *ctx);
    void (*invalidate)(void *ctx);
};

struct rb_smu13 {
    const struct rb_hw *hw;
    uint32_t timeout_us;
    uint32_t allowed;              /* RB_SMU13_ALLOW_* */
    uint32_t mp1_major, mp1_minor, mp1_rev;
    uint32_t dw_msg, dw_arg, dw_resp;
    uint32_t dw_gfx_misc_cntl;     /* SMUIO_GFX_MISC_CNTL, 0 if no SMUIO */
    int dead;

    struct rb_smu13_dma dma;
    int table_location_set;

    /* State read from the SMU. */
    uint32_t smu_version, if_version;
    uint64_t features;
    int have_versions, have_features;
    struct rb_smu13_dpm_clocks clocks;
    int have_clocks;

    /* yellow_carp_set_fine_grain_gfx_freq_parameters / od_edit. */
    uint32_t gfx_default_hard_min, gfx_default_soft_max;
    uint32_t gfx_actual_hard_min, gfx_actual_soft_max;

    /* Accounting. */
    uint32_t nmsgs, writes, refused_writes;
    uint8_t  last_msg;
    uint32_t last_arg, last_resp, last_polls;
};

/* ----------------------------------------------------------- operations --- */
/* Each returns RB_SMU13_OK or a negative RB_SMU13_E_*. Linux function in []. */

/* Bind to the chip: MP1 13.0.1/3/8 from the discovery table, mailbox and
 * SMUIO registers, `dma` as the driver table (may be NULL: no table
 * operations). Sends nothing. Allows RB_SMU13_ALLOW_QUERY only.
 * timeout_us 0: 2 s (Linux's). */
int rb_smu13_init(struct rb_smu13 *s, const struct rb_hw *hw, const struct rb_disc *d,
                  const struct rb_smu13_dma *dma, uint32_t timeout_us);
void rb_smu13_allow(struct rb_smu13 *s, uint32_t groups);
uint32_t rb_smu13_msg_group(uint8_t msg);   /* RB_SMU13_ALLOW_* of a message, 0 if unknown */
uint32_t rb_smu13_table_size_max(void);

/* One message, any group the context allows. out may be NULL. */
int rb_smu13_send(struct rb_smu13 *s, uint8_t msg, uint32_t arg, uint32_t *out);

/* [smu_cmn_check_fw_version] versions; E_VERSION if the interface is not 4. */
int rb_smu13_check_fw_version(struct rb_smu13 *s);
/* [smu_cmn_get_enabled_mask] */
int rb_smu13_get_enabled_mask(struct rb_smu13 *s, uint64_t *mask);
/* [yellow_carp_is_dpm_running] 1 running, 0 not, <0 error. */
int rb_smu13_is_dpm_running(struct rb_smu13 *s);
/* [smu_v13_0_set_driver_table_location] */
int rb_smu13_set_driver_table_location(struct rb_smu13 *s);
/* [smu_cmn_update_table] table_id RB_SMU13_TABLE_*; drv2smu 1 = write. */
int rb_smu13_update_table(struct rb_smu13 *s, uint32_t table_id, uint16_t arg,
                          void *data, uint32_t size, int drv2smu);
/* [yellow_carp_set_default_dpm_tables + set_fine_grain_gfx_freq_parameters] */
int rb_smu13_set_default_dpm_table(struct rb_smu13 *s);
/* [smu_cmn_get_metrics_table] always fresh (no cache). */
int rb_smu13_get_metrics(struct rb_smu13 *s, struct rb_smu13_metrics *m);
/* [yellow_carp_read_sensor] */
int rb_smu13_read_sensor(struct rb_smu13 *s, enum rb_smu13_sensor sensor, uint32_t *value);
/* [yellow_carp_set_watermarks_table] writes the table on every call. */
int rb_smu13_set_watermarks(struct rb_smu13 *s,
                            const struct rb_smu13_wm_set *reader, uint32_t nreader,
                            const struct rb_smu13_wm_set *writer, uint32_t nwriter);

/* [yellow_carp_post_smu_init] EnableGfxOff. */
int rb_smu13_enable_gfxoff(struct rb_smu13 *s);
/* [smu_v13_0_gfx_off_control] AllowGfxOff / DisallowGfxOff. */
int rb_smu13_gfx_off_control(struct rb_smu13 *s, int enable);
/* [yellow_carp_get_gfxoff_status] 0 in GFXOFF, 1 leaving, 2 not in GFXOFF,
 * 3 entering. */
int rb_smu13_get_gfxoff_status(struct rb_smu13 *s, uint32_t *status);
/* [yellow_carp_dpm_set_vcn_enable / _jpeg_enable] */
int rb_smu13_set_vcn_power(struct rb_smu13 *s, int on);
int rb_smu13_set_jpeg_power(struct rb_smu13 *s, int on);
/* [yellow_carp_system_features_control(false)] PrepareMp1ForUnload. */
int rb_smu13_prepare_unload(struct rb_smu13 *s);
/* [yellow_carp_mode2_reset] */
int rb_smu13_mode2_reset(struct rb_smu13 *s);

/* Clocks, all in MHz. Need rb_smu13_set_default_dpm_table() and the feature
 * mask (rb_smu13_get_enabled_mask()) first. */
/* [yellow_carp_get_current_clk_freq] */
int rb_smu13_get_current_clk(struct rb_smu13 *s, enum rb_smu13_clk clk, uint32_t *mhz);
/* [yellow_carp_get_dpm_level_count] */
int rb_smu13_get_dpm_level_count(struct rb_smu13 *s, enum rb_smu13_clk clk, uint32_t *count);
/* [yellow_carp_get_dpm_freq_by_index] */
int rb_smu13_get_dpm_freq_by_index(struct rb_smu13 *s, enum rb_smu13_clk clk,
                                   uint32_t level, uint32_t *mhz);
/* [yellow_carp_get_dpm_ultimate_freq] min/max may be NULL. E_NODATA when the
 * clock's DPM is off (Linux then uses VBIOS boot values, not available here). */
int rb_smu13_get_dpm_ultimate_freq(struct rb_smu13 *s, enum rb_smu13_clk clk,
                                   uint32_t *min, uint32_t *max);
/* [yellow_carp_set_soft_freq_limited_range] */
int rb_smu13_set_soft_freq_range(struct rb_smu13 *s, enum rb_smu13_clk clk,
                                 uint32_t min, uint32_t max);
/* [yellow_carp_force_clk_levels] SOC, FCLK, VCLK, DCLK. */
int rb_smu13_force_clk_levels(struct rb_smu13 *s, enum rb_smu13_clk clk, uint32_t mask);
/* [yellow_carp_set_performance_level] */
int rb_smu13_set_performance_level(struct rb_smu13 *s, enum rb_smu13_level level);

/* The bring-up Linux does in smu_hw_init / smu_smc_hw_setup for this ASIC,
 * read-and-configure part only: versions (interface must be 4), feature
 * mask, driver table location, DPM clock table. Needs ALLOW_TABLES. Does not
 * touch GFXOFF, clocks or VCN/JPEG power: those are separate calls. */
int rb_smu13_bringup(struct rb_smu13 *s);

const char *rb_smu13_strerror(int err);

#ifdef __cplusplus
}
#endif

#endif /* RB_SMU13_H */
