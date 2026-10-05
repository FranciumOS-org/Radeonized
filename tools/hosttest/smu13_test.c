/* SPDX-License-Identifier: MIT */
/*
 * rb_smu13 against a simulated Yellow Carp SMU. The simulation keeps its
 * tables in Linux's own structs (docs/linux-ref/smu13_driver_if_yellow_carp.h),
 * so the library's layouts are checked against Linux, not against a copy of
 * themselves. Register addresses come from this machine's real discovery
 * table (MP1 13.0.3 at 0x16000, SMUIO at 0x16800).
 *
 * Called from hosttest.c's main(); returns the number of failures.
 */
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "rb_discovery.h"
#include "rb_probe.h"
#include "rb_smu13.h"
#include "smu13_driver_if_yellow_carp.h"   /* Linux, unmodified */

static int fails;
#define CHECK(c) do { if (!(c)) { printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

/* ------------------------------------------------- layouts against Linux --- */

#define SAME(rbt, lt, rbf, lf) \
    _Static_assert(offsetof(rbt, rbf) == offsetof(lt, lf) && sizeof(((rbt *)0)->rbf) == sizeof(((lt *)0)->lf), \
                   #rbt "." #rbf " vs " #lt "." #lf)

_Static_assert(sizeof(struct rb_smu13_dpm_clocks) == sizeof(DpmClocks_t), "DpmClocks_t size");
SAME(struct rb_smu13_dpm_clocks, DpmClocks_t, dcf_clocks, DcfClocks);
SAME(struct rb_smu13_dpm_clocks, DpmClocks_t, disp_clocks, DispClocks);
SAME(struct rb_smu13_dpm_clocks, DpmClocks_t, dpp_clocks, DppClocks);
SAME(struct rb_smu13_dpm_clocks, DpmClocks_t, soc_clocks, SocClocks);
SAME(struct rb_smu13_dpm_clocks, DpmClocks_t, vclocks, VClocks);
SAME(struct rb_smu13_dpm_clocks, DpmClocks_t, dclocks, DClocks);
SAME(struct rb_smu13_dpm_clocks, DpmClocks_t, soc_voltage, SocVoltage);
SAME(struct rb_smu13_dpm_clocks, DpmClocks_t, df_pstate, DfPstateTable);
SAME(struct rb_smu13_dpm_clocks, DpmClocks_t, num_dcf_levels, NumDcfClkLevelsEnabled);
SAME(struct rb_smu13_dpm_clocks, DpmClocks_t, num_disp_levels, NumDispClkLevelsEnabled);
SAME(struct rb_smu13_dpm_clocks, DpmClocks_t, num_soc_levels, NumSocClkLevelsEnabled);
SAME(struct rb_smu13_dpm_clocks, DpmClocks_t, num_vcn_levels, VcnClkLevelsEnabled);
SAME(struct rb_smu13_dpm_clocks, DpmClocks_t, num_df_pstates, NumDfPstatesEnabled);
SAME(struct rb_smu13_dpm_clocks, DpmClocks_t, min_gfx_clk, MinGfxClk);
SAME(struct rb_smu13_dpm_clocks, DpmClocks_t, max_gfx_clk, MaxGfxClk);
_Static_assert(sizeof(struct rb_smu13_df_pstate) == sizeof(DfPstateTable_t), "DfPstateTable_t size");
SAME(struct rb_smu13_df_pstate, DfPstateTable_t, fclk, FClk);
SAME(struct rb_smu13_df_pstate, DfPstateTable_t, memclk, MemClk);
SAME(struct rb_smu13_df_pstate, DfPstateTable_t, voltage, Voltage);
SAME(struct rb_smu13_df_pstate, DfPstateTable_t, wck_ratio, WckRatio);

_Static_assert(sizeof(struct rb_smu13_metrics) == sizeof(SmuMetrics_t), "SmuMetrics_t size");
SAME(struct rb_smu13_metrics, SmuMetrics_t, gfxclk, GfxclkFrequency);
SAME(struct rb_smu13_metrics, SmuMetrics_t, socclk, SocclkFrequency);
SAME(struct rb_smu13_metrics, SmuMetrics_t, vclk, VclkFrequency);
SAME(struct rb_smu13_metrics, SmuMetrics_t, dclk, DclkFrequency);
SAME(struct rb_smu13_metrics, SmuMetrics_t, memclk, MemclkFrequency);
SAME(struct rb_smu13_metrics, SmuMetrics_t, gfx_activity, GfxActivity);
SAME(struct rb_smu13_metrics, SmuMetrics_t, uvd_activity, UvdActivity);
SAME(struct rb_smu13_metrics, SmuMetrics_t, voltage, Voltage);
SAME(struct rb_smu13_metrics, SmuMetrics_t, current, Current);
SAME(struct rb_smu13_metrics, SmuMetrics_t, power, Power);
SAME(struct rb_smu13_metrics, SmuMetrics_t, core_freq, CoreFrequency);
SAME(struct rb_smu13_metrics, SmuMetrics_t, core_power, CorePower);
SAME(struct rb_smu13_metrics, SmuMetrics_t, core_temp, CoreTemperature);
SAME(struct rb_smu13_metrics, SmuMetrics_t, l3_freq, L3Frequency);
SAME(struct rb_smu13_metrics, SmuMetrics_t, l3_temp, L3Temperature);
SAME(struct rb_smu13_metrics, SmuMetrics_t, gfx_temp, GfxTemperature);
SAME(struct rb_smu13_metrics, SmuMetrics_t, soc_temp, SocTemperature);
SAME(struct rb_smu13_metrics, SmuMetrics_t, throttler_status, ThrottlerStatus);
SAME(struct rb_smu13_metrics, SmuMetrics_t, current_socket_power, CurrentSocketPower);
SAME(struct rb_smu13_metrics, SmuMetrics_t, stapm_opn_limit, StapmOpnLimit);
SAME(struct rb_smu13_metrics, SmuMetrics_t, stapm_current_limit, StapmCurrentLimit);
SAME(struct rb_smu13_metrics, SmuMetrics_t, apu_power, ApuPower);
SAME(struct rb_smu13_metrics, SmuMetrics_t, dgpu_power, dGpuPower);
SAME(struct rb_smu13_metrics, SmuMetrics_t, vdd_tdc, VddTdcValue);
SAME(struct rb_smu13_metrics, SmuMetrics_t, soc_tdc, SocTdcValue);
SAME(struct rb_smu13_metrics, SmuMetrics_t, vdd_edc, VddEdcValue);
SAME(struct rb_smu13_metrics, SmuMetrics_t, soc_edc, SocEdcValue);
SAME(struct rb_smu13_metrics, SmuMetrics_t, infra_cpu_max_freq, InfrastructureCpuMaxFreq);
SAME(struct rb_smu13_metrics, SmuMetrics_t, infra_gfx_max_freq, InfrastructureGfxMaxFreq);
SAME(struct rb_smu13_metrics, SmuMetrics_t, skin_temp, SkinTemp);
SAME(struct rb_smu13_metrics, SmuMetrics_t, device_state, DeviceState);

_Static_assert(sizeof(struct rb_smu13_watermarks) == sizeof(Watermarks_t), "Watermarks_t size");
SAME(struct rb_smu13_watermarks, Watermarks_t, row, WatermarkRow);
SAME(struct rb_smu13_watermarks, Watermarks_t, mmhub_padding, MmHubPadding);
_Static_assert(sizeof(struct rb_smu13_wm_row) == sizeof(WatermarkRowGeneric_t), "row size");
SAME(struct rb_smu13_wm_row, WatermarkRowGeneric_t, min_clock, MinClock);
SAME(struct rb_smu13_wm_row, WatermarkRowGeneric_t, max_clock, MaxClock);
SAME(struct rb_smu13_wm_row, WatermarkRowGeneric_t, min_mclk, MinMclk);
SAME(struct rb_smu13_wm_row, WatermarkRowGeneric_t, max_mclk, MaxMclk);
SAME(struct rb_smu13_wm_row, WatermarkRowGeneric_t, wm_setting, WmSetting);
SAME(struct rb_smu13_wm_row, WatermarkRowGeneric_t, wm_type, WmType);

_Static_assert(RB_SMU13_TABLE_WATERMARKS == TABLE_WATERMARKS && RB_SMU13_TABLE_DPMCLOCKS == TABLE_DPMCLOCKS &&
               RB_SMU13_TABLE_SMU_METRICS == TABLE_SMU_METRICS, "table ids");
_Static_assert(RB_SMU13_DRIVER_IF_VERSION == SMU13_YELLOW_CARP_DRIVER_IF_VERSION, "interface version");
_Static_assert(RB_SMU13_NUM_DPM_LEVELS == NUM_SOCCLK_DPM_LEVELS && RB_SMU13_NUM_DF_PSTATES == NUM_DF_PSTATE_LEVELS &&
               RB_SMU13_NUM_WM_RANGES == NUM_WM_RANGES, "array sizes");

/* --------------------------------------------------- simulated SMU ------- */

#define MMIO_DW (0x20000u)              /* 512 KB */
#define DW_MSG  (0x16000u + 0x282)
#define DW_ARG  (0x16000u + 0x292)
#define DW_RESP (0x16000u + 0x29a)
#define DW_GFX_MISC_CNTL (0x16800u + 0xc5)
#define NEVER 0xFFFFFFFFu

/* This machine's answers (third probe, 2026-10-04). */
#define REAL_SMU_VERSION 0x04453900u
#define REAL_FEATURES    0x1497e1fdebfffdffull

struct sim {
    uint32_t mmio[MMIO_DW];
    uint8_t  written[MMIO_DW];
    uint32_t latency;               /* delay_us calls before answering; NEVER = hang */
    uint32_t countdown;
    int pending;
    int zeroed, arg_set;
    uint32_t order_errors, msgs, bad_access;

    uint32_t if_version;
    uint64_t features;
    uint32_t fail_msg, fail_resp;

    uint32_t dram_hi, dram_lo;
    uint8_t *dram; uint64_t dram_mc; uint32_t dram_size;
    DpmClocks_t clocks;
    SmuMetrics_t metrics;
    Watermarks_t wm; uint32_t wm_writes;

    uint32_t count[64], last_arg[64];
    uint32_t seq[256]; uint32_t nseq;           /* message order */
    int gfxoff_enabled, gfxoff_allowed, vcn_on, jpeg_on, unloaded;

    uint32_t flushes, invalidates;
    uint32_t msgs_at_flush, msgs_at_invalidate;
};

static uint32_t sim_answer(struct sim *f, uint32_t msg, uint32_t arg, uint32_t *out)
{
    uint64_t addr = ((uint64_t)f->dram_hi << 32) | f->dram_lo;
    uint32_t id = arg & 0xFFFF;
    if (msg < 64) { f->count[msg]++; f->last_arg[msg] = arg; }
    if (f->nseq < 256) f->seq[f->nseq++] = msg;
    switch (msg) {
    case RB_SMU13_MSG_TestMessage:          *out = arg + 1; break;
    case RB_SMU13_MSG_GetSmuVersion:        *out = REAL_SMU_VERSION; break;
    case RB_SMU13_MSG_GetDriverIfVersion:   *out = f->if_version; break;
    case RB_SMU13_MSG_GetEnabledSmuFeatures:
        if (arg > 1) return RB_SMU13_RESP_REJ_PREREQ;
        *out = arg ? (uint32_t)(f->features >> 32) : (uint32_t)f->features;
        break;
    case RB_SMU13_MSG_SetDriverDramAddrHigh: f->dram_hi = arg; break;
    case RB_SMU13_MSG_SetDriverDramAddrLow:  f->dram_lo = arg; break;
    case RB_SMU13_MSG_TransferTableSmu2Dram:
        if (!f->dram || addr != f->dram_mc || (arg >> 16)) return RB_SMU13_RESP_FAILED;
        if (id == TABLE_DPMCLOCKS) memcpy(f->dram, &f->clocks, sizeof(f->clocks));
        else if (id == TABLE_SMU_METRICS) memcpy(f->dram, &f->metrics, sizeof(f->metrics));
        else return RB_SMU13_RESP_FAILED;
        break;
    case RB_SMU13_MSG_TransferTableDram2Smu:
        if (!f->dram || addr != f->dram_mc || id != TABLE_WATERMARKS) return RB_SMU13_RESP_FAILED;
        memcpy(&f->wm, f->dram, sizeof(f->wm));
        f->wm_writes++;
        break;
    case RB_SMU13_MSG_GetGfxclkFrequency:   *out = f->metrics.GfxclkFrequency; break;
    case RB_SMU13_MSG_GetFclkFrequency:     *out = 1600; break;
    case RB_SMU13_MSG_EnableGfxOff:         f->gfxoff_enabled = 1; break;
    case RB_SMU13_MSG_AllowGfxOff:          f->gfxoff_allowed = 1; break;
    case RB_SMU13_MSG_DisallowGfxOff:       f->gfxoff_allowed = 0; break;
    case RB_SMU13_MSG_PowerUpVcn:           f->vcn_on = 1; break;
    case RB_SMU13_MSG_PowerDownVcn:         f->vcn_on = 0; break;
    case RB_SMU13_MSG_PowerUpJpeg:          f->jpeg_on = 1; break;
    case RB_SMU13_MSG_PowerDownJpeg:        f->jpeg_on = 0; break;
    case RB_SMU13_MSG_PrepareMp1ForUnload:  f->unloaded = 1; break;
    case RB_SMU13_MSG_GfxDeviceDriverReset: if (arg != 2) return RB_SMU13_RESP_REJ_PREREQ; break;
    case RB_SMU13_MSG_SetHardMinVcn: case RB_SMU13_MSG_SetHardMinSocclkByFreq:
    case RB_SMU13_MSG_SetSoftMinVcn: case RB_SMU13_MSG_SetSoftMaxGfxClk:
    case RB_SMU13_MSG_SetHardMinGfxClk: case RB_SMU13_MSG_SetSoftMaxSocclkByFreq:
    case RB_SMU13_MSG_SetSoftMaxFclkByFreq: case RB_SMU13_MSG_SetSoftMaxVcn:
    case RB_SMU13_MSG_SetPowerLimitPercentage: case RB_SMU13_MSG_SetHardMinFclkByFreq:
    case RB_SMU13_MSG_SetSoftMinSocclkByFreq:
        break;
    default:
        return RB_SMU13_RESP_UNKNOWN_CMD;
    }
    return RB_SMU13_RESP_OK;
}

static void sim_finish(struct sim *f)
{
    uint32_t out = 0, msg = f->mmio[DW_MSG], arg = f->mmio[DW_ARG];
    uint32_t resp = sim_answer(f, msg, arg, &out);
    if (msg == f->fail_msg) resp = f->fail_resp;
    if (resp == RB_SMU13_RESP_OK) f->mmio[DW_ARG] = out;
    f->mmio[DW_RESP] = resp;
    f->pending = 0;
}

static uint32_t s_rreg(void *ctx, uint32_t off)
{
    struct sim *f = ctx;
    if (off % 4 || off / 4 >= MMIO_DW) { f->bad_access++; return 0; }
    return f->mmio[off / 4];
}

static void s_wreg(void *ctx, uint32_t off, uint32_t v)
{
    struct sim *f = ctx;
    uint32_t dw = off / 4;
    if (off % 4 || dw >= MMIO_DW) { f->bad_access++; return; }
    f->written[dw] = 1;
    if (dw == DW_RESP) { f->mmio[dw] = v; if (v == 0) f->zeroed = 1; }
    else if (dw == DW_ARG) { f->mmio[dw] = v; f->arg_set = 1; }
    else if (dw == DW_MSG) {
        if (!f->zeroed || !f->arg_set || f->pending) f->order_errors++;
        f->zeroed = f->arg_set = 0;
        f->mmio[dw] = v;
        f->msgs++;
        f->pending = 1;
        f->countdown = f->latency;
        if (f->latency == 0) sim_finish(f);
    }
    else f->bad_access++;
}

static void s_delay(void *ctx, uint32_t us)
{
    struct sim *f = ctx;
    if (f->pending && f->latency != NEVER) {
        if (f->countdown <= us) sim_finish(f);
        else f->countdown -= us;
    }
}

static void s_flush(void *ctx)      { struct sim *f = ctx; f->flushes++; f->msgs_at_flush = f->msgs; }
static void s_invalidate(void *ctx) { struct sim *f = ctx; f->invalidates++; f->msgs_at_invalidate = f->msgs; }

/* A 680M-shaped DPM table (values made up but ordered as the firmware
 * orders them: DF P-states highest first). */
static void sim_tables(struct sim *f)
{
    DpmClocks_t *c = &f->clocks;
    static const uint32_t soc[] = { 400, 600, 800, 1000, 1200 };
    static const uint32_t vclk[] = { 400, 700, 1000 }, dclk[] = { 350, 600, 900 };
    static const uint32_t fclk[] = { 1600, 1400, 1200, 400 }, mclk[] = { 3200, 2800, 2400, 800 };
    memset(c, 0, sizeof(*c));
    for (int i = 0; i < 5; i++) c->SocClocks[i] = soc[i];
    for (int i = 0; i < 3; i++) { c->VClocks[i] = vclk[i]; c->DClocks[i] = dclk[i]; }
    for (int i = 0; i < 4; i++) {
        c->DfPstateTable[i].FClk = fclk[i]; c->DfPstateTable[i].MemClk = mclk[i];
        c->DfPstateTable[i].Voltage = 3000 + 100u * i; c->DfPstateTable[i].WckRatio = (uint8_t)(i + 1);
    }
    for (int i = 0; i < 8; i++) { c->DcfClocks[i] = 100 + i; c->DispClocks[i] = 200 + i; c->DppClocks[i] = 300 + i; c->SocVoltage[i] = 700 + i; }
    c->NumDcfClkLevelsEnabled = 8; c->NumDispClkLevelsEnabled = 8;
    c->NumSocClkLevelsEnabled = 5; c->VcnClkLevelsEnabled = 3; c->NumDfPstatesEnabled = 4;
    c->MinGfxClk = 200; c->MaxGfxClk = 2200;

    SmuMetrics_t *m = &f->metrics;
    memset(m, 0, sizeof(*m));
    m->GfxclkFrequency = 1800; m->SocclkFrequency = 800; m->VclkFrequency = 700; m->DclkFrequency = 600;
    m->MemclkFrequency = 2400; m->GfxActivity = 4567; m->UvdActivity = 1234;
    m->Voltage[0] = 950; m->Voltage[1] = 800;
    m->GfxTemperature = 6789; m->SocTemperature = 7012;
    m->CurrentSocketPower = 25000;
    m->StapmOpnLimit = 35; m->StapmCurrentLimit = 45;
    m->ApuPower = 42000; m->dGpuPower = 5000;
    m->ThrottlerStatus = 0x21;
}

static uint8_t dram_buf[4096];

static struct sim *sim_new(uint32_t latency)
{
    struct sim *f = calloc(1, sizeof(*f));
    f->latency = latency;
    f->if_version = 4;
    f->features = REAL_FEATURES;
    f->mmio[DW_RESP] = RB_SMU13_RESP_OK;     /* idle, as found on the hardware */
    f->mmio[DW_GFX_MISC_CNTL] = 0x2u << 1 | 0x1;   /* "not in GFXOFF" + an unrelated bit */
    f->dram = dram_buf; f->dram_mc = 0xF400100000ull; f->dram_size = sizeof(dram_buf);
    memset(dram_buf, 0xEE, sizeof(dram_buf));
    sim_tables(f);
    return f;
}

static struct rb_hw sim_hw(struct sim *f)
{
    struct rb_hw hw = { f, MMIO_DW * 4, s_rreg, s_wreg, 0, 0, s_delay };
    return hw;
}

static struct rb_smu13_dma sim_dma(struct sim *f)
{
    struct rb_smu13_dma d = { f->dram, f->dram_mc, f->dram_size, s_flush, s_invalidate };
    return d;
}

/* Only the three mailbox registers were ever written, in Linux's order. */
static void check_writes(struct sim *f, const struct rb_smu13 *s)
{
    CHECK(f->bad_access == 0);
    CHECK(f->order_errors == 0);
    CHECK(s->refused_writes == 0);
    for (uint32_t i = 0; i < MMIO_DW; i++)
        if (f->written[i] && i != DW_MSG && i != DW_ARG && i != DW_RESP) {
            printf("  FAIL write to dword 0x%x\n", i); fails++;
        }
}

/* ------------------------------------------------------------- tests --- */

static const struct rb_disc *real;

static void test_init(void)
{
    struct rb_smu13 s;
    struct sim *f = sim_new(3);
    struct rb_hw hw = sim_hw(f);
    struct rb_smu13_dma dma = sim_dma(f);
    struct rb_disc *d = malloc(sizeof(*d));

    printf("smu13: init on the real table\n");
    CHECK(rb_smu13_init(&s, &hw, real, &dma, 1000) == RB_SMU13_OK);
    CHECK(s.mp1_major == 13 && s.mp1_minor == 0 && s.mp1_rev == 3);
    CHECK(s.dw_msg == DW_MSG && s.dw_arg == DW_ARG && s.dw_resp == DW_RESP);
    CHECK(s.dw_gfx_misc_cntl == DW_GFX_MISC_CNTL);
    CHECK(s.allowed == RB_SMU13_ALLOW_QUERY && !s.dead);
    CHECK(f->msgs == 0);

    printf("smu13: MP1 versions outside Yellow Carp are refused, nothing sent\n");
    static const uint8_t bad[][3] = { { 12, 0, 1 }, { 13, 0, 0 }, { 13, 0, 2 }, { 13, 0, 4 }, { 13, 0, 5 },
                                      { 13, 0, 7 }, { 13, 1, 3 }, { 14, 0, 3 } };
    for (unsigned k = 0; k < sizeof(bad) / sizeof(bad[0]); k++) {
        memcpy(d, real, sizeof(*d));
        for (uint32_t i = 0; i < d->nips; i++)
            if (d->ips[i].hw_id == RB_HWID_MP1) { d->ips[i].major = bad[k][0]; d->ips[i].minor = bad[k][1]; d->ips[i].revision = bad[k][2]; }
        CHECK(rb_smu13_init(&s, &hw, d, &dma, 1000) == RB_SMU13_E_VERSION);
        CHECK(rb_smu13_send(&s, RB_SMU13_MSG_GetSmuVersion, 0, 0) == RB_SMU13_E_DEAD);
    }
    for (int rev = 1; rev <= 8; rev += (rev == 1 ? 2 : 5)) {    /* 1, 3, 8 */
        memcpy(d, real, sizeof(*d));
        for (uint32_t i = 0; i < d->nips; i++)
            if (d->ips[i].hw_id == RB_HWID_MP1) d->ips[i].revision = (uint8_t)rev;
        CHECK(rb_smu13_init(&s, &hw, d, &dma, 1000) == RB_SMU13_OK);
    }
    CHECK(f->msgs == 0);

    printf("smu13: a driver table too small, or no hardware, is refused\n");
    struct rb_smu13_dma small = dma;
    small.size = rb_smu13_table_size_max() - 1;
    CHECK(rb_smu13_init(&s, &hw, real, &small, 1000) == RB_SMU13_E_INVAL);
    struct rb_hw nohw = hw; nohw.delay_us = 0;
    CHECK(rb_smu13_init(&s, &nohw, real, &dma, 1000) == RB_SMU13_E_INVAL);
    CHECK(rb_smu13_send(&s, RB_SMU13_MSG_GetSmuVersion, 0, 0) != RB_SMU13_OK);
    CHECK(f->msgs == 0);
    free(d); free(f);
}

static void test_policy(void)
{
    struct rb_smu13 s;
    struct sim *f = sim_new(3);
    struct rb_hw hw = sim_hw(f);
    struct rb_smu13_dma dma = sim_dma(f);
    uint32_t v;

    printf("smu13: every Linux yellow_carp message has a group, nothing else does\n");
    static const uint8_t mapped[] = { 0x01, 0x02, 0x03, 0x04, 0x19, 0x1A, 0x06, 0x07, 0x08, 0x0C, 0x0D, 0x0E,
                                      0x0F, 0x10, 0x11, 0x12, 0x13, 0x15, 0x17, 0x18, 0x1B, 0x1C, 0x1D, 0x1E,
                                      0x1F, 0x20, 0x21, 0x22, 0x23, 0x24 };
    uint32_t n = 0;
    for (unsigned i = 0; i < sizeof(mapped); i++) CHECK(rb_smu13_msg_group(mapped[i]) != 0);
    for (unsigned m = 0; m < 256; m++) n += rb_smu13_msg_group((uint8_t)m) != 0;
    CHECK(n == sizeof(mapped));
    /* ForcePowerDownGfx, SetSoftMinFclk, AllowZstates, RequestActiveWgp: not used by Linux */
    CHECK(!rb_smu13_msg_group(0x0B) && !rb_smu13_msg_group(0x14) && !rb_smu13_msg_group(0x25) &&
          !rb_smu13_msg_group(0x27) && !rb_smu13_msg_group(0x00));

    printf("smu13: only queries until the caller allows more\n");
    CHECK(rb_smu13_init(&s, &hw, real, &dma, 1000) == RB_SMU13_OK);
    CHECK(rb_smu13_set_driver_table_location(&s) == RB_SMU13_E_NOT_ALLOWED);
    CHECK(rb_smu13_enable_gfxoff(&s) == RB_SMU13_E_NOT_ALLOWED);
    CHECK(rb_smu13_gfx_off_control(&s, 1) == RB_SMU13_E_NOT_ALLOWED);
    CHECK(rb_smu13_set_vcn_power(&s, 1) == RB_SMU13_E_NOT_ALLOWED);
    CHECK(rb_smu13_set_jpeg_power(&s, 1) == RB_SMU13_E_NOT_ALLOWED);
    CHECK(rb_smu13_prepare_unload(&s) == RB_SMU13_E_NOT_ALLOWED);
    CHECK(rb_smu13_mode2_reset(&s) == RB_SMU13_E_NOT_ALLOWED);
    CHECK(rb_smu13_send(&s, RB_SMU13_MSG_SetHardMinGfxClk, 1000, 0) == RB_SMU13_E_NOT_ALLOWED);
    CHECK(rb_smu13_send(&s, 0x0B, 0, 0) == RB_SMU13_E_INVAL);   /* ForcePowerDownGfx: never */
    CHECK(rb_smu13_send(&s, 0xFF, 0, 0) == RB_SMU13_E_INVAL);
    CHECK(f->msgs == 0 && s.writes == 0);
    CHECK(rb_smu13_send(&s, RB_SMU13_MSG_TestMessage, 41, &v) == RB_SMU13_OK && v == 42);
    CHECK(f->msgs == 1 && s.writes == 3);

    printf("smu13: allowing one group does not allow the others\n");
    rb_smu13_allow(&s, RB_SMU13_ALLOW_GFXOFF);
    CHECK(rb_smu13_gfx_off_control(&s, 1) == RB_SMU13_OK && f->gfxoff_allowed);
    CHECK(rb_smu13_set_vcn_power(&s, 1) == RB_SMU13_E_NOT_ALLOWED && !f->vcn_on);
    CHECK(f->msgs == 2);
    check_writes(f, &s);
    free(f);
}

/* init + allow tables + bringup; for the tests that need loaded tables. */
static struct sim *up(struct rb_smu13 *s, struct rb_hw *hw, struct rb_smu13_dma *dma, uint32_t extra)
{
    struct sim *f = sim_new(2);
    *hw = sim_hw(f);
    *dma = sim_dma(f);
    CHECK(rb_smu13_init(s, hw, real, dma, 1000) == RB_SMU13_OK);
    rb_smu13_allow(s, RB_SMU13_ALLOW_TABLES | extra);
    CHECK(rb_smu13_bringup(s) == RB_SMU13_OK);
    return f;
}

static void test_bringup(void)
{
    struct rb_smu13 s;
    struct rb_hw hw;
    struct rb_smu13_dma dma;
    struct sim *f;

    printf("smu13: bring-up with this machine's firmware answers\n");
    f = up(&s, &hw, &dma, 0);
    CHECK(s.have_versions && s.smu_version == REAL_SMU_VERSION && s.if_version == 4);
    CHECK(s.have_features && s.features == REAL_FEATURES);
    CHECK(rb_smu13_is_dpm_running(&s) == 1);
    CHECK(f->dram_hi == 0xF4 && f->dram_lo == 0x00100000);
    CHECK(s.table_location_set);
    CHECK(f->count[RB_SMU13_MSG_TransferTableSmu2Dram] == 1 && f->last_arg[RB_SMU13_MSG_TransferTableSmu2Dram] == TABLE_DPMCLOCKS);
    CHECK(s.have_clocks && memcmp(&s.clocks, &f->clocks, sizeof(f->clocks)) == 0);
    CHECK(s.clocks.num_soc_levels == 5 && s.clocks.df_pstate[3].fclk == 400 && s.clocks.max_gfx_clk == 2200);
    CHECK(s.gfx_default_hard_min == 200 && s.gfx_default_soft_max == 2200);
    CHECK(f->invalidates == 1 && f->msgs_at_invalidate == f->msgs - 2);   /* right after the transfer */
    CHECK(f->flushes == 0);
    /* Linux's order: versions, features, table address, DPM table. Nothing else. */
    static const uint32_t order[] = { 0x03, 0x02, 0x12, 0x12, 0x0D, 0x0E, 0x0F, 0x12, 0x12 };
    CHECK(f->nseq == sizeof(order) / sizeof(order[0]));
    for (uint32_t i = 0; i < f->nseq && i < sizeof(order) / sizeof(order[0]); i++) CHECK(f->seq[i] == order[i]);
    CHECK(!f->gfxoff_enabled && !f->gfxoff_allowed && !f->vcn_on && !f->jpeg_on && !f->unloaded);
    check_writes(f, &s);
    free(f);

    printf("smu13: interface version 5 stops bring-up after the version query\n");
    f = sim_new(2); f->if_version = 5;
    hw = sim_hw(f); dma = sim_dma(f);
    CHECK(rb_smu13_init(&s, &hw, real, &dma, 1000) == RB_SMU13_OK);
    rb_smu13_allow(&s, RB_SMU13_ALLOW_TABLES);
    CHECK(rb_smu13_bringup(&s) == RB_SMU13_E_VERSION);
    CHECK(f->msgs == 2 && !s.have_clocks && f->dram_hi == 0 && f->dram_lo == 0);
    free(f);

    printf("smu13: bring-up without ALLOW_TABLES sends only queries\n");
    f = sim_new(2);
    hw = sim_hw(f); dma = sim_dma(f);
    CHECK(rb_smu13_init(&s, &hw, real, &dma, 1000) == RB_SMU13_OK);
    CHECK(rb_smu13_bringup(&s) == RB_SMU13_E_NOT_ALLOWED);
    CHECK(f->msgs == 4 && f->count[0x0D] == 0);
    free(f);

    printf("smu13: tables before the table address is set: nothing sent\n");
    f = sim_new(2);
    hw = sim_hw(f); dma = sim_dma(f);
    CHECK(rb_smu13_init(&s, &hw, real, &dma, 1000) == RB_SMU13_OK);
    rb_smu13_allow(&s, RB_SMU13_ALLOW_TABLES);
    struct rb_smu13_metrics m;
    CHECK(rb_smu13_get_metrics(&s, &m) == RB_SMU13_E_NODATA && f->msgs == 0);
    CHECK(rb_smu13_update_table(&s, 3, 0, &m, sizeof(m), 0) == RB_SMU13_E_INVAL);         /* unknown id */
    CHECK(rb_smu13_update_table(&s, TABLE_SMU_METRICS, 0, &m, sizeof(m) - 2, 0) == RB_SMU13_E_INVAL);
    CHECK(f->msgs == 0);
    free(f);

    printf("smu13: DPM table with impossible level counts is refused\n");
    f = sim_new(2); f->clocks.NumSocClkLevelsEnabled = 9;
    hw = sim_hw(f); dma = sim_dma(f);
    CHECK(rb_smu13_init(&s, &hw, real, &dma, 1000) == RB_SMU13_OK);
    rb_smu13_allow(&s, RB_SMU13_ALLOW_TABLES);
    CHECK(rb_smu13_bringup(&s) == RB_SMU13_E_NODATA && !s.have_clocks);
    uint32_t c;
    CHECK(rb_smu13_get_dpm_level_count(&s, RB_SMU13_CLK_SOC, &c) == RB_SMU13_E_NODATA);
    free(f);
    f = sim_new(2); f->clocks.NumDfPstatesEnabled = 5;
    hw = sim_hw(f); dma = sim_dma(f);
    CHECK(rb_smu13_init(&s, &hw, real, &dma, 1000) == RB_SMU13_OK);
    rb_smu13_allow(&s, RB_SMU13_ALLOW_TABLES);
    CHECK(rb_smu13_bringup(&s) == RB_SMU13_E_NODATA && !s.have_clocks);
    free(f);
}

static void test_metrics(void)
{
    struct rb_smu13 s;
    struct rb_hw hw;
    struct rb_smu13_dma dma;
    struct sim *f = up(&s, &hw, &dma, 0);
    struct rb_smu13_metrics m;
    uint32_t v;

    printf("smu13: metrics and sensors (yellow_carp_read_sensor units)\n");
    CHECK(rb_smu13_get_metrics(&s, &m) == RB_SMU13_OK && memcmp(&m, &f->metrics, sizeof(m)) == 0);
    CHECK(f->last_arg[RB_SMU13_MSG_TransferTableSmu2Dram] == TABLE_SMU_METRICS);
    CHECK(rb_smu13_read_sensor(&s, RB_SMU13_SENSOR_GPU_LOAD, &v) == 0 && v == 45);
    CHECK(rb_smu13_read_sensor(&s, RB_SMU13_SENSOR_VCN_LOAD, &v) == 0 && v == 12);
    CHECK(rb_smu13_read_sensor(&s, RB_SMU13_SENSOR_INPUT_POWER, &v) == 0 && v == 25000);
    CHECK(rb_smu13_read_sensor(&s, RB_SMU13_SENSOR_EDGE_TEMP, &v) == 0 && v == 67000);
    CHECK(rb_smu13_read_sensor(&s, RB_SMU13_SENSOR_HOTSPOT_TEMP, &v) == 0 && v == 70000);
    CHECK(rb_smu13_read_sensor(&s, RB_SMU13_SENSOR_MCLK, &v) == 0 && v == 240000);
    CHECK(rb_smu13_read_sensor(&s, RB_SMU13_SENSOR_SCLK, &v) == 0 && v == 180000);
    CHECK(rb_smu13_read_sensor(&s, RB_SMU13_SENSOR_VDDGFX, &v) == 0 && v == 950);
    CHECK(rb_smu13_read_sensor(&s, RB_SMU13_SENSOR_VDDNB, &v) == 0 && v == 800);
    /* APU 42 W over a 35 W limit = 20 %; dGPU 5 W under its 10 W share = 0. */
    CHECK(rb_smu13_read_sensor(&s, RB_SMU13_SENSOR_SS_APU_SHARE, &v) == 0 && v == 20);
    CHECK(rb_smu13_read_sensor(&s, RB_SMU13_SENSOR_SS_DGPU_SHARE, &v) == 0 && v == 0);
    f->metrics.ApuPower = 30000; f->metrics.dGpuPower = 25000;   /* dGPU 25 W over 10 W: 150 % -> 100 */
    CHECK(rb_smu13_read_sensor(&s, RB_SMU13_SENSOR_SS_DGPU_SHARE, &v) == 0 && v == 100);
    CHECK(rb_smu13_read_sensor(&s, RB_SMU13_SENSOR_SS_APU_SHARE, &v) == 0 && v == 0);
    f->metrics.StapmOpnLimit = 0;                                 /* no division by zero */
    CHECK(rb_smu13_read_sensor(&s, RB_SMU13_SENSOR_SS_APU_SHARE, &v) == 0 && v == 0);
    CHECK(rb_smu13_read_sensor(&s, (enum rb_smu13_sensor)99, &v) == RB_SMU13_E_INVAL);
    CHECK(f->invalidates == f->count[RB_SMU13_MSG_TransferTableSmu2Dram]);
    check_writes(f, &s);
    free(f);
}

static void test_watermarks(void)
{
    struct rb_smu13 s;
    struct rb_hw hw;
    struct rb_smu13_dma dma;
    struct sim *f = up(&s, &hw, &dma, 0);
    const struct rb_smu13_wm_set rd[2] = { { 0, 100, 200, 300, 400 }, { 1, 110, 210, 310, 410 } };
    const struct rb_smu13_wm_set wrs[1] = { { 3, 500, 600, 700, 800 } };
    struct rb_smu13_wm_set five[5] = { { 0, 0, 0, 0, 0 } };

    printf("smu13: watermarks map as yellow_carp_set_watermarks_table\n");
    CHECK(rb_smu13_set_watermarks(&s, rd, 2, wrs, 1) == RB_SMU13_OK);
    CHECK(f->wm_writes == 1 && f->last_arg[RB_SMU13_MSG_TransferTableDram2Smu] == TABLE_WATERMARKS);
    CHECK(f->flushes == 1 && f->msgs_at_flush == f->msgs - 1);   /* flushed before the transfer */
    for (int i = 0; i < 2; i++) {
        WatermarkRowGeneric_t *r = &f->wm.WatermarkRow[WM_DCFCLK][i];
        CHECK(r->MinClock == rd[i].min_drain_mhz && r->MaxClock == rd[i].max_drain_mhz &&
              r->MinMclk == rd[i].min_fill_mhz && r->MaxMclk == rd[i].max_fill_mhz && r->WmSetting == rd[i].wm_inst);
    }
    WatermarkRowGeneric_t *w = &f->wm.WatermarkRow[WM_SOCCLK][0];
    CHECK(w->MinClock == 500 && w->MaxClock == 600 && w->MinMclk == 700 && w->MaxMclk == 800 && w->WmSetting == 3);
    CHECK(f->wm.WatermarkRow[WM_SOCCLK][1].MaxClock == 0 && f->wm.WatermarkRow[WM_DCFCLK][2].MaxClock == 0);
    uint32_t before = f->msgs;
    CHECK(rb_smu13_set_watermarks(&s, five, 5, wrs, 1) == RB_SMU13_E_INVAL);
    CHECK(rb_smu13_set_watermarks(&s, rd, 2, five, 5) == RB_SMU13_E_INVAL);
    CHECK(rb_smu13_set_watermarks(&s, 0, 1, wrs, 1) == RB_SMU13_E_INVAL);
    CHECK(f->msgs == before);
    check_writes(f, &s);
    free(f);
}

static void test_clocks(void)
{
    struct rb_smu13 s;
    struct rb_hw hw;
    struct rb_smu13_dma dma;
    struct sim *f = up(&s, &hw, &dma, 0);
    uint32_t v, mn, mx;

    printf("smu13: DPM levels and ultimate frequencies\n");
    CHECK(rb_smu13_get_dpm_level_count(&s, RB_SMU13_CLK_SOC, &v) == 0 && v == 5);
    CHECK(rb_smu13_get_dpm_level_count(&s, RB_SMU13_CLK_VCLK, &v) == 0 && v == 3);
    CHECK(rb_smu13_get_dpm_level_count(&s, RB_SMU13_CLK_FCLK, &v) == 0 && v == 4);
    CHECK(rb_smu13_get_dpm_level_count(&s, RB_SMU13_CLK_GFX, &v) == RB_SMU13_E_INVAL);
    CHECK(rb_smu13_get_dpm_freq_by_index(&s, RB_SMU13_CLK_SOC, 4, &v) == 0 && v == 1200);
    CHECK(rb_smu13_get_dpm_freq_by_index(&s, RB_SMU13_CLK_SOC, 5, &v) == RB_SMU13_E_INVAL);
    CHECK(rb_smu13_get_dpm_freq_by_index(&s, RB_SMU13_CLK_MCLK, 2, &v) == 0 && v == 2400);
    CHECK(rb_smu13_get_dpm_freq_by_index(&s, RB_SMU13_CLK_DCLK, 2, &v) == 0 && v == 900);
    CHECK(rb_smu13_get_dpm_freq_by_index(&s, RB_SMU13_CLK_VCLK, 3, &v) == RB_SMU13_E_INVAL);
    CHECK(rb_smu13_get_dpm_ultimate_freq(&s, RB_SMU13_CLK_GFX, &mn, &mx) == 0 && mn == 200 && mx == 2200);
    CHECK(rb_smu13_get_dpm_ultimate_freq(&s, RB_SMU13_CLK_FCLK, &mn, &mx) == 0 && mn == 400 && mx == 1600);
    CHECK(rb_smu13_get_dpm_ultimate_freq(&s, RB_SMU13_CLK_MCLK, &mn, &mx) == 0 && mn == 800 && mx == 3200);
    CHECK(rb_smu13_get_dpm_ultimate_freq(&s, RB_SMU13_CLK_SOC, &mn, &mx) == 0 && mn == 400 && mx == 1200);
    CHECK(rb_smu13_get_dpm_ultimate_freq(&s, RB_SMU13_CLK_VCLK, &mn, &mx) == 0 && mn == 400 && mx == 1000);
    CHECK(rb_smu13_get_dpm_ultimate_freq(&s, RB_SMU13_CLK_DCLK, 0, &mx) == 0 && mx == 900);

    printf("smu13: current clocks (messages for GFX/FCLK, metrics for the rest)\n");
    CHECK(rb_smu13_get_current_clk(&s, RB_SMU13_CLK_GFX, &v) == 0 && v == 1800 && f->count[0x17] == 1);
    CHECK(rb_smu13_get_current_clk(&s, RB_SMU13_CLK_FCLK, &v) == 0 && v == 1600 && f->count[0x18] == 1);
    CHECK(rb_smu13_get_current_clk(&s, RB_SMU13_CLK_SOC, &v) == 0 && v == 800);
    CHECK(rb_smu13_get_current_clk(&s, RB_SMU13_CLK_MCLK, &v) == 0 && v == 2400);
    CHECK(rb_smu13_get_current_clk(&s, RB_SMU13_CLK_DCLK, &v) == 0 && v == 600);

    printf("smu13: clock limits need ALLOW_CLOCKS\n");
    uint32_t before = f->msgs;
    CHECK(rb_smu13_set_soft_freq_range(&s, RB_SMU13_CLK_GFX, 400, 2000) == RB_SMU13_E_NOT_ALLOWED);
    CHECK(rb_smu13_set_performance_level(&s, RB_SMU13_LEVEL_HIGH) == RB_SMU13_E_NOT_ALLOWED);
    CHECK(f->msgs == before);

    rb_smu13_allow(&s, RB_SMU13_ALLOW_CLOCKS);
    printf("smu13: soft frequency ranges\n");
    CHECK(rb_smu13_set_soft_freq_range(&s, RB_SMU13_CLK_GFX, 400, 2000) == 0);
    CHECK(f->last_arg[0x1C] == 400 && f->last_arg[0x1B] == 2000);
    CHECK(rb_smu13_set_soft_freq_range(&s, RB_SMU13_CLK_VCLK, 400, 1000) == 0);
    CHECK(f->last_arg[0x08] == 400u << 16 && f->last_arg[0x1F] == 1000u << 16);
    CHECK(rb_smu13_set_soft_freq_range(&s, RB_SMU13_CLK_DCLK, 350, 900) == 0);
    CHECK(f->last_arg[0x08] == 350 && f->last_arg[0x1F] == 900);       /* DCLK not shifted (as Linux) */
    CHECK(rb_smu13_set_soft_freq_range(&s, RB_SMU13_CLK_FCLK, 400, 1600) == 0);
    CHECK(f->last_arg[0x23] == 400 && f->last_arg[0x1E] == 1600);
    CHECK(rb_smu13_set_soft_freq_range(&s, RB_SMU13_CLK_SOC, 600, 1000) == 0);
    CHECK(f->last_arg[0x13] == 600 && f->last_arg[0x1D] == 1000);
    CHECK(rb_smu13_set_soft_freq_range(&s, RB_SMU13_CLK_MCLK, 1, 2) == RB_SMU13_E_INVAL);

    printf("smu13: force_clk_levels picks the lowest and highest set bits\n");
    CHECK(rb_smu13_force_clk_levels(&s, RB_SMU13_CLK_SOC, 0x6) == 0);
    CHECK(f->last_arg[0x13] == 600 && f->last_arg[0x1D] == 800);
    CHECK(rb_smu13_force_clk_levels(&s, RB_SMU13_CLK_SOC, 0x40) == RB_SMU13_E_INVAL);   /* level 6 of 5 */
    CHECK(rb_smu13_force_clk_levels(&s, RB_SMU13_CLK_GFX, 0x1) == RB_SMU13_E_INVAL);

    printf("smu13: performance levels\n");
    CHECK(rb_smu13_set_performance_level(&s, RB_SMU13_LEVEL_HIGH) == 0);
    CHECK(f->last_arg[0x1C] == 2200 && f->last_arg[0x1B] == 2200);
    CHECK(f->last_arg[0x23] == 1600 && f->last_arg[0x1E] == 1600);
    CHECK(f->last_arg[0x13] == 1200 && f->last_arg[0x1D] == 1200);
    CHECK(f->last_arg[0x08] == 900 && f->last_arg[0x1F] == 900);       /* DCLK last, unshifted */
    CHECK(s.gfx_actual_hard_min == 2200 && s.gfx_actual_soft_max == 2200);
    CHECK(rb_smu13_set_performance_level(&s, RB_SMU13_LEVEL_LOW) == 0);
    CHECK(f->last_arg[0x1C] == 200 && f->last_arg[0x1B] == 200 && f->last_arg[0x23] == 400 && f->last_arg[0x13] == 400);
    CHECK(rb_smu13_set_performance_level(&s, RB_SMU13_LEVEL_AUTO) == 0);
    CHECK(f->last_arg[0x1C] == 200 && f->last_arg[0x1B] == 2200 && f->last_arg[0x23] == 400 && f->last_arg[0x1E] == 1600);
    CHECK(rb_smu13_set_performance_level(&s, RB_SMU13_LEVEL_PROFILE_STANDARD) == 0);
    CHECK(f->last_arg[0x1C] == 700 && f->last_arg[0x1B] == 700);       /* 13.0.3 UMD p-state */
    CHECK(f->last_arg[0x23] == 1800 && f->last_arg[0x13] == 678);
    CHECK(rb_smu13_set_performance_level(&s, RB_SMU13_LEVEL_PROFILE_PEAK) == 0);
    CHECK(f->last_arg[0x1B] == 2200 && f->last_arg[0x1E] == 1600 && f->last_arg[0x1D] == 1200);
    CHECK(rb_smu13_set_performance_level(&s, RB_SMU13_LEVEL_PROFILE_MIN_SCLK) == 0);
    CHECK(f->last_arg[0x1C] == 200 && f->last_arg[0x23] == 1800);
    before = f->msgs;
    CHECK(rb_smu13_set_performance_level(&s, RB_SMU13_LEVEL_MANUAL) == 0);
    CHECK(rb_smu13_set_performance_level(&s, RB_SMU13_LEVEL_PROFILE_EXIT) == 0);
    CHECK(rb_smu13_set_performance_level(&s, (enum rb_smu13_level)42) == RB_SMU13_E_INVAL);
    CHECK(f->msgs == before);

    printf("smu13: a clock whose DPM feature is off is left alone\n");
    s.features &= ~(1ull << RB_SMU13_FEATURE_GFX_DPM);
    CHECK(rb_smu13_get_dpm_ultimate_freq(&s, RB_SMU13_CLK_GFX, &mn, &mx) == RB_SMU13_E_NODATA);
    CHECK(rb_smu13_set_soft_freq_range(&s, RB_SMU13_CLK_GFX, 400, 2000) == RB_SMU13_E_INVAL);
    before = f->msgs;
    uint32_t gfx_before = f->count[0x1C];
    CHECK(rb_smu13_set_performance_level(&s, RB_SMU13_LEVEL_HIGH) == 0);
    CHECK(f->count[0x1C] == gfx_before && f->msgs == before + 8);       /* the other four clocks */
    check_writes(f, &s);
    free(f);
}

static void test_power(void)
{
    struct rb_smu13 s;
    struct rb_hw hw;
    struct rb_smu13_dma dma;
    struct sim *f = up(&s, &hw, &dma, RB_SMU13_ALLOW_GFXOFF | RB_SMU13_ALLOW_MMPOWER |
                                      RB_SMU13_ALLOW_UNLOAD | RB_SMU13_ALLOW_RESET);
    uint32_t st;

    printf("smu13: GFXOFF, VCN/JPEG power, unload, mode-2 reset\n");
    CHECK(rb_smu13_enable_gfxoff(&s) == 0 && f->gfxoff_enabled);
    CHECK(rb_smu13_gfx_off_control(&s, 1) == 0 && f->gfxoff_allowed);
    CHECK(rb_smu13_gfx_off_control(&s, 0) == 0 && !f->gfxoff_allowed);
    CHECK(rb_smu13_set_vcn_power(&s, 1) == 0 && f->vcn_on && f->last_arg[0x07] == 0);
    CHECK(rb_smu13_set_vcn_power(&s, 0) == 0 && !f->vcn_on);
    CHECK(rb_smu13_set_jpeg_power(&s, 1) == 0 && f->jpeg_on);
    CHECK(rb_smu13_set_jpeg_power(&s, 0) == 0 && !f->jpeg_on);
    CHECK(rb_smu13_mode2_reset(&s) == 0 && f->last_arg[0x11] == 2);
    CHECK(rb_smu13_prepare_unload(&s) == 0 && f->unloaded);

    printf("smu13: GFXOFF status from SMUIO_GFX_MISC_CNTL\n");
    uint32_t sent = f->msgs;
    CHECK(rb_smu13_get_gfxoff_status(&s, &st) == 0 && st == 2);
    f->mmio[DW_GFX_MISC_CNTL] = 0xFFFFFFF9u;                      /* status bits 0 = in GFXOFF */
    CHECK(rb_smu13_get_gfxoff_status(&s, &st) == 0 && st == 0);
    f->mmio[DW_GFX_MISC_CNTL] = 0x6;
    CHECK(rb_smu13_get_gfxoff_status(&s, &st) == 0 && st == 3);
    CHECK(f->msgs == sent);
    check_writes(f, &s);
    free(f);
}

static void test_failures(void)
{
    struct rb_smu13 s;
    struct rb_hw hw;
    struct rb_smu13_dma dma;
    struct sim *f;
    uint32_t v;

    printf("smu13: an error answer is returned, the context stays usable\n");
    f = sim_new(2); f->fail_msg = RB_SMU13_MSG_GetDriverIfVersion; f->fail_resp = RB_SMU13_RESP_REJ_PREREQ;
    hw = sim_hw(f); dma = sim_dma(f);
    CHECK(rb_smu13_init(&s, &hw, real, &dma, 1000) == RB_SMU13_OK);
    CHECK(rb_smu13_check_fw_version(&s) == RB_SMU13_E_FW && s.last_resp == RB_SMU13_RESP_REJ_PREREQ);
    CHECK(!s.have_versions && !s.dead);
    CHECK(rb_smu13_send(&s, RB_SMU13_MSG_GetSmuVersion, 0, &v) == 0 && v == REAL_SMU_VERSION);
    check_writes(f, &s);
    free(f);

    printf("smu13: no answer: timeout, then nothing more is sent\n");
    f = sim_new(NEVER);
    hw = sim_hw(f); dma = sim_dma(f);
    CHECK(rb_smu13_init(&s, &hw, real, &dma, 1000) == RB_SMU13_OK);
    CHECK(rb_smu13_send(&s, RB_SMU13_MSG_GetSmuVersion, 0, &v) == RB_SMU13_E_TIMEOUT);
    CHECK(s.dead && s.last_polls == 1000 && f->msgs == 1);
    CHECK(rb_smu13_send(&s, RB_SMU13_MSG_GetSmuVersion, 0, &v) == RB_SMU13_E_DEAD);
    CHECK(rb_smu13_bringup(&s) == RB_SMU13_E_DEAD);
    CHECK(f->msgs == 1);
    free(f);

    printf("smu13: mailbox stuck at 0 before sending: nothing sent, context dead\n");
    f = sim_new(2); f->mmio[DW_RESP] = RB_SMU13_RESP_NONE;
    hw = sim_hw(f); dma = sim_dma(f);
    CHECK(rb_smu13_init(&s, &hw, real, &dma, 1000) == RB_SMU13_OK);
    CHECK(rb_smu13_send(&s, RB_SMU13_MSG_GetSmuVersion, 0, &v) == RB_SMU13_E_BUSY);
    CHECK(s.dead && f->msgs == 0 && s.writes == 0);
    free(f);

    printf("smu13: mailbox holding an unknown value: nothing sent\n");
    f = sim_new(2); f->mmio[DW_RESP] = 0x42;
    hw = sim_hw(f); dma = sim_dma(f);
    CHECK(rb_smu13_init(&s, &hw, real, &dma, 1000) == RB_SMU13_OK);
    CHECK(rb_smu13_send(&s, RB_SMU13_MSG_GetSmuVersion, 0, &v) == RB_SMU13_E_BUSY);
    CHECK(f->msgs == 0 && s.writes == 0);
    free(f);

    printf("smu13: a failed table transfer leaves the caller's buffer alone\n");
    f = sim_new(2);
    hw = sim_hw(f); dma = sim_dma(f);
    CHECK(rb_smu13_init(&s, &hw, real, &dma, 1000) == RB_SMU13_OK);
    rb_smu13_allow(&s, RB_SMU13_ALLOW_TABLES);
    CHECK(rb_smu13_bringup(&s) == RB_SMU13_OK);
    f->fail_msg = RB_SMU13_MSG_TransferTableSmu2Dram; f->fail_resp = RB_SMU13_RESP_FAILED;
    struct rb_smu13_metrics m;
    memset(&m, 0x5A, sizeof(m));
    CHECK(rb_smu13_get_metrics(&s, &m) == RB_SMU13_E_FW);
    uint8_t *b = (uint8_t *)&m; int same = 1;
    for (size_t i = 0; i < sizeof(m); i++) same &= b[i] == 0x5A;
    CHECK(same);
    check_writes(f, &s);
    free(f);
}

int smu13_tests(const struct rb_disc *real_table)
{
    fails = 0;
    real = real_table;
    if (!real) {
        printf("smu13: FAIL needs tests/data/rembrandt-680m-tufa15.bin\n");
        return 1;
    }
    test_init();
    test_policy();
    test_bringup();
    test_metrics();
    test_watermarks();
    test_clocks();
    test_power();
    test_failures();
    return fails;
}
