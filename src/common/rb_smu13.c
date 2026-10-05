/* SPDX-License-Identifier: MIT */
/* SMU 13 power management for Rembrandt. See rb_smu13.h. */
#include "rb_smu13.h"

/* MP1_SMN_C2PMSG_66 / _82 / _90, base index 0 (mp_13_0_2_offset.h). */
#define MP1_C2PMSG_66 0x0282
#define MP1_C2PMSG_82 0x0292
#define MP1_C2PMSG_90 0x029a

/* yellow_carp_ppt.c: regSMUIO_GFX_MISC_CNTL, base index 0. */
#define RB_HWID_SMUIO 4                                   /* soc15_hw_ip.h */
#define SMUIO_GFX_MISC_CNTL 0x00c5
#define SMUIO_GFX_MISC_CNTL__PWR_GFXOFF_STATUS_MASK  0x6u
#define SMUIO_GFX_MISC_CNTL__PWR_GFXOFF_STATUS_SHIFT 1

#define SMU_13_VCLK_SHIFT 16                              /* smu_v13_0.h */
#define TEMP_UNITS_PER_CENTIGRADE 1000                    /* SMU_TEMPERATURE_UNITS_PER_CENTIGRADES */

/* Linux: adev->usec_timeout (100000) * 20, polled at 1 us. */
#define DEFAULT_TIMEOUT_US 2000000u

#define Q RB_SMU13_ALLOW_QUERY
#define T RB_SMU13_ALLOW_TABLES
#define G RB_SMU13_ALLOW_GFXOFF
#define C RB_SMU13_ALLOW_CLOCKS
#define P RB_SMU13_ALLOW_MMPOWER
static const uint8_t msg_group[RB_SMU13_MSG_COUNT] = {
    [RB_SMU13_MSG_TestMessage]             = Q,
    [RB_SMU13_MSG_GetSmuVersion]           = Q,
    [RB_SMU13_MSG_GetDriverIfVersion]      = Q,
    [RB_SMU13_MSG_GetEnabledSmuFeatures]   = Q,
    [RB_SMU13_MSG_GetGfxclkFrequency]      = Q,
    [RB_SMU13_MSG_GetFclkFrequency]        = Q,
    [RB_SMU13_MSG_SetDriverDramAddrHigh]   = T,
    [RB_SMU13_MSG_SetDriverDramAddrLow]    = T,
    [RB_SMU13_MSG_TransferTableSmu2Dram]   = T,
    [RB_SMU13_MSG_TransferTableDram2Smu]   = T,
    [RB_SMU13_MSG_EnableGfxOff]            = G,
    [RB_SMU13_MSG_AllowGfxOff]             = G,
    [RB_SMU13_MSG_DisallowGfxOff]          = G,
    [RB_SMU13_MSG_SetHardMinVcn]           = C,
    [RB_SMU13_MSG_SetHardMinSocclkByFreq]  = C,
    [RB_SMU13_MSG_SetSoftMinVcn]           = C,
    [RB_SMU13_MSG_SetSoftMaxGfxClk]        = C,
    [RB_SMU13_MSG_SetHardMinGfxClk]        = C,
    [RB_SMU13_MSG_SetSoftMaxSocclkByFreq]  = C,
    [RB_SMU13_MSG_SetSoftMaxFclkByFreq]    = C,
    [RB_SMU13_MSG_SetSoftMaxVcn]           = C,
    [RB_SMU13_MSG_SetPowerLimitPercentage] = C,
    [RB_SMU13_MSG_SetHardMinFclkByFreq]    = C,
    [RB_SMU13_MSG_SetSoftMinSocclkByFreq]  = C,
    [RB_SMU13_MSG_PowerDownVcn]            = P,
    [RB_SMU13_MSG_PowerUpVcn]              = P,
    [RB_SMU13_MSG_PowerDownJpeg]           = P,
    [RB_SMU13_MSG_PowerUpJpeg]             = P,
    [RB_SMU13_MSG_PrepareMp1ForUnload]     = RB_SMU13_ALLOW_UNLOAD,
    [RB_SMU13_MSG_GfxDeviceDriverReset]    = RB_SMU13_ALLOW_RESET,
};
#undef Q
#undef T
#undef G
#undef C
#undef P

uint32_t rb_smu13_msg_group(uint8_t msg)
{
    return msg < RB_SMU13_MSG_COUNT ? msg_group[msg] : 0;
}

static uint32_t table_size(uint32_t id)
{
    switch (id) {
    case RB_SMU13_TABLE_WATERMARKS:  return sizeof(struct rb_smu13_watermarks);
    case RB_SMU13_TABLE_DPMCLOCKS:   return sizeof(struct rb_smu13_dpm_clocks);
    case RB_SMU13_TABLE_SMU_METRICS: return sizeof(struct rb_smu13_metrics);
    default:                         return 0;
    }
}

uint32_t rb_smu13_table_size_max(void)
{
    uint32_t a = table_size(RB_SMU13_TABLE_WATERMARKS), b = table_size(RB_SMU13_TABLE_DPMCLOCKS),
             c = table_size(RB_SMU13_TABLE_SMU_METRICS), m = a;
    if (b > m) m = b;
    if (c > m) m = c;
    return m;
}

static void zero(void *p, uint32_t n)
{
    volatile uint8_t *b = (volatile uint8_t *)p;
    for (uint32_t i = 0; i < n; i++)
        b[i] = 0;
}

static void copy(void *dst, const void *src, uint32_t n)
{
    volatile uint8_t *d = (volatile uint8_t *)dst;
    const volatile uint8_t *s = (const volatile uint8_t *)src;
    for (uint32_t i = 0; i < n; i++)
        d[i] = s[i];
}

static uint32_t rd(const struct rb_smu13 *s, uint32_t dw)
{
    return s->hw->rreg(s->hw->ctx, dw * 4);
}

/* The write policy: the three mailbox registers, nothing else. */
static void wr(struct rb_smu13 *s, uint32_t dw, uint32_t v)
{
    if (dw != s->dw_msg && dw != s->dw_arg && dw != s->dw_resp) {
        s->refused_writes++;
        return;
    }
    s->writes++;
    s->hw->wreg(s->hw->ctx, dw * 4, v);
}

/* __smu_msg_v1_poll_stat */
static uint32_t poll_resp(const struct rb_smu13 *s, uint32_t *polls)
{
    uint32_t v = 0, n;
    for (n = 0; n < s->timeout_us; n++) {
        v = rd(s, s->dw_resp);
        if (v != RB_SMU13_RESP_NONE)
            break;
        s->hw->delay_us(s->hw->ctx, 1);
    }
    if (polls)
        *polls = n;
    return v;
}

static int known_resp(uint32_t v)
{
    return v == RB_SMU13_RESP_OK || v == RB_SMU13_RESP_FAILED || v == RB_SMU13_RESP_UNKNOWN_CMD ||
           v == RB_SMU13_RESP_REJ_PREREQ || v == RB_SMU13_RESP_REJ_BUSY;
}

int rb_smu13_init(struct rb_smu13 *s, const struct rb_hw *hw, const struct rb_disc *d,
                  const struct rb_smu13_dma *dma, uint32_t timeout_us)
{
    const struct rb_ip *mp1;
    uint32_t dw;

    if (!s)
        return RB_SMU13_E_INVAL;
    zero(s, sizeof(*s));
    s->dead = 1;                                   /* until everything checks out */
    if (!hw || !hw->rreg || !hw->wreg || !hw->delay_us || !d)
        return RB_SMU13_E_INVAL;
    s->hw = hw;
    s->timeout_us = timeout_us ? timeout_us : DEFAULT_TIMEOUT_US;

    mp1 = rb_disc_find(d, RB_HWID_MP1, 0);
    if (!mp1 || mp1->nbase < 1)
        return RB_SMU13_E_VERSION;
    s->mp1_major = mp1->major;
    s->mp1_minor = mp1->minor;
    s->mp1_rev = mp1->revision;
    /* smu_set_funcs: yellow_carp_set_ppt_funcs for 13.0.1, 13.0.3, 13.0.8. */
    if (mp1->major != 13 || mp1->minor != 0 ||
        (mp1->revision != 1 && mp1->revision != 3 && mp1->revision != 8))
        return RB_SMU13_E_VERSION;

    s->dw_msg = mp1->base[0] + MP1_C2PMSG_66;
    s->dw_arg = mp1->base[0] + MP1_C2PMSG_82;
    s->dw_resp = mp1->base[0] + MP1_C2PMSG_90;
    if (s->dw_msg >= hw->mmio_len / 4 || s->dw_arg >= hw->mmio_len / 4 ||
        s->dw_resp >= hw->mmio_len / 4)
        return RB_SMU13_E_INVAL;
    if (rb_disc_reg(d, RB_HWID_SMUIO, 0, 0, SMUIO_GFX_MISC_CNTL, &dw) == 0 && dw < hw->mmio_len / 4)
        s->dw_gfx_misc_cntl = dw;

    if (dma) {
        if (!dma->cpu || !dma->mc || dma->size < rb_smu13_table_size_max())
            return RB_SMU13_E_INVAL;
        copy(&s->dma, dma, sizeof(s->dma));
    }
    s->allowed = RB_SMU13_ALLOW_QUERY;
    s->dead = 0;
    return RB_SMU13_OK;
}

void rb_smu13_allow(struct rb_smu13 *s, uint32_t groups)
{
    if (s)
        s->allowed |= groups;
}

int rb_smu13_send(struct rb_smu13 *s, uint8_t msg, uint32_t arg, uint32_t *out)
{
    uint32_t pre, group;

    if (!s || !s->hw)
        return RB_SMU13_E_INVAL;
    if (s->dead)
        return RB_SMU13_E_DEAD;
    group = rb_smu13_msg_group(msg);
    if (!group)
        return RB_SMU13_E_INVAL;
    if (!(s->allowed & group))
        return RB_SMU13_E_NOT_ALLOWED;

    s->last_msg = msg;
    s->last_arg = arg;
    s->last_polls = 0;

    pre = poll_resp(s, 0);
    s->last_resp = pre;
    if (pre == RB_SMU13_RESP_NONE) {
        s->dead = 1;                  /* the previous message never finished */
        return RB_SMU13_E_BUSY;
    }
    if (!known_resp(pre))
        return RB_SMU13_E_BUSY;

    wr(s, s->dw_resp, 0);
    wr(s, s->dw_arg, arg);
    wr(s, s->dw_msg, msg);
    s->nmsgs++;

    s->last_resp = poll_resp(s, &s->last_polls);
    if (s->last_resp == RB_SMU13_RESP_NONE) {
        s->dead = 1;
        return RB_SMU13_E_TIMEOUT;
    }
    if (s->last_resp != RB_SMU13_RESP_OK)
        return RB_SMU13_E_FW;
    if (out)
        *out = rd(s, s->dw_arg);
    return RB_SMU13_OK;
}

/* ------------------------------------------------------ versions, features */

int rb_smu13_check_fw_version(struct rb_smu13 *s)
{
    uint32_t ifv = 0, ver = 0;
    int e = rb_smu13_send(s, RB_SMU13_MSG_GetDriverIfVersion, 0, &ifv);
    if (e)
        return e;
    e = rb_smu13_send(s, RB_SMU13_MSG_GetSmuVersion, 0, &ver);
    if (e)
        return e;
    s->if_version = ifv;
    s->smu_version = ver;
    s->have_versions = 1;
    /* Linux only logs a mismatch; the table layouts here depend on it. */
    return ifv == RB_SMU13_DRIVER_IF_VERSION ? RB_SMU13_OK : RB_SMU13_E_VERSION;
}

int rb_smu13_get_enabled_mask(struct rb_smu13 *s, uint64_t *mask)
{
    uint32_t lo = 0, hi = 0;
    int e = rb_smu13_send(s, RB_SMU13_MSG_GetEnabledSmuFeatures, 0, &lo);
    if (e)
        return e;
    e = rb_smu13_send(s, RB_SMU13_MSG_GetEnabledSmuFeatures, 1, &hi);
    if (e)
        return e;
    s->features = ((uint64_t)hi << 32) | lo;
    s->have_features = 1;
    if (mask)
        *mask = s->features;
    return RB_SMU13_OK;
}

int rb_smu13_is_dpm_running(struct rb_smu13 *s)
{
    uint64_t m;
    int e = rb_smu13_get_enabled_mask(s, &m);
    if (e)
        return e;
    return (m & RB_SMU13_DPM_FEATURES) == RB_SMU13_DPM_FEATURES;
}

/* --------------------------------------------------------------- tables */

int rb_smu13_set_driver_table_location(struct rb_smu13 *s)
{
    int e;
    if (!s || !s->dma.cpu)
        return RB_SMU13_E_INVAL;
    e = rb_smu13_send(s, RB_SMU13_MSG_SetDriverDramAddrHigh, (uint32_t)(s->dma.mc >> 32), 0);
    if (!e)
        e = rb_smu13_send(s, RB_SMU13_MSG_SetDriverDramAddrLow, (uint32_t)s->dma.mc, 0);
    s->table_location_set = e == RB_SMU13_OK;
    return e;
}

int rb_smu13_update_table(struct rb_smu13 *s, uint32_t table_id, uint16_t arg,
                          void *data, uint32_t size, int drv2smu)
{
    uint32_t want = table_size(table_id), param;
    int e;

    if (!s || !data || !want || size != want)
        return RB_SMU13_E_INVAL;
    if (!s->dma.cpu || want > s->dma.size)
        return RB_SMU13_E_INVAL;
    if (!s->table_location_set)
        return RB_SMU13_E_NODATA;
    if (s->dead)
        return RB_SMU13_E_DEAD;
    if (!(s->allowed & RB_SMU13_ALLOW_TABLES))
        return RB_SMU13_E_NOT_ALLOWED;

    if (drv2smu) {
        copy(s->dma.cpu, data, want);
        if (s->dma.flush)
            s->dma.flush(s->hw->ctx);
    }
    param = ((uint32_t)arg << 16) | (table_id & 0xFFFF);
    e = rb_smu13_send(s, drv2smu ? RB_SMU13_MSG_TransferTableDram2Smu : RB_SMU13_MSG_TransferTableSmu2Dram,
                      param, 0);
    if (e)
        return e;
    if (!drv2smu) {
        if (s->dma.invalidate)
            s->dma.invalidate(s->hw->ctx);
        copy(data, s->dma.cpu, want);
    }
    return RB_SMU13_OK;
}

int rb_smu13_set_default_dpm_table(struct rb_smu13 *s)
{
    const struct rb_smu13_dpm_clocks *c;
    int e;

    if (!s)
        return RB_SMU13_E_INVAL;
    /* Read in place (no 300-byte stack copy); have_clocks gates every use. */
    s->have_clocks = 0;
    e = rb_smu13_update_table(s, RB_SMU13_TABLE_DPMCLOCKS, 0, &s->clocks, sizeof(s->clocks), 0);
    if (e)
        return e;
    c = &s->clocks;
    /* Linux indexes the arrays with these counts unchecked; refuse a table
     * whose counts would run past them. */
    if (c->num_dcf_levels > RB_SMU13_NUM_DPM_LEVELS || c->num_disp_levels > RB_SMU13_NUM_DPM_LEVELS ||
        c->num_soc_levels > RB_SMU13_NUM_DPM_LEVELS || c->num_vcn_levels > RB_SMU13_NUM_DPM_LEVELS ||
        c->num_df_pstates > RB_SMU13_NUM_DF_PSTATES)
        return RB_SMU13_E_NODATA;
    s->have_clocks = 1;
    /* yellow_carp_set_fine_grain_gfx_freq_parameters */
    s->gfx_default_hard_min = c->min_gfx_clk;
    s->gfx_default_soft_max = c->max_gfx_clk;
    s->gfx_actual_hard_min = 0;
    s->gfx_actual_soft_max = 0;
    return RB_SMU13_OK;
}

int rb_smu13_get_metrics(struct rb_smu13 *s, struct rb_smu13_metrics *m)
{
    return rb_smu13_update_table(s, RB_SMU13_TABLE_SMU_METRICS, 0, m, sizeof(*m), 0);
}

/* yellow_carp_get_ss_power_percent, with Linux's 16-bit intermediates. */
static void ss_power_percent(const struct rb_smu13_metrics *m, uint32_t *apu, uint32_t *dgpu)
{
    uint32_t apu_boost = 0, dgpu_boost = 0;
    uint16_t apu_limit, dgpu_limit = 0, apu_power, dgpu_power;

    apu_power = (uint16_t)(m->apu_power / 1000);
    apu_limit = m->stapm_opn_limit;
    if (apu_power > apu_limit && apu_limit != 0)
        apu_boost = ((uint32_t)(apu_power - apu_limit) * 100) / apu_limit;
    apu_boost = apu_boost > 100 ? 100 : apu_boost;

    dgpu_power = (uint16_t)(m->dgpu_power / 1000);
    if (m->stapm_current_limit > m->stapm_opn_limit)
        dgpu_limit = (uint16_t)(m->stapm_current_limit - m->stapm_opn_limit);
    if (dgpu_power > dgpu_limit && dgpu_limit != 0)
        dgpu_boost = ((uint32_t)(dgpu_power - dgpu_limit) * 100) / dgpu_limit;
    dgpu_boost = dgpu_boost > 100 ? 100 : dgpu_boost;

    if (dgpu_boost >= apu_boost)
        apu_boost = 0;
    else
        dgpu_boost = 0;
    *apu = apu_boost;
    *dgpu = dgpu_boost;
}

int rb_smu13_read_sensor(struct rb_smu13 *s, enum rb_smu13_sensor sensor, uint32_t *value)
{
    struct rb_smu13_metrics m;
    uint32_t apu, dgpu;
    int e;

    if (!value)
        return RB_SMU13_E_INVAL;
    e = rb_smu13_get_metrics(s, &m);
    if (e)
        return e;
    switch (sensor) {
    case RB_SMU13_SENSOR_GPU_LOAD:      *value = m.gfx_activity / 100u; break;
    case RB_SMU13_SENSOR_VCN_LOAD:      *value = m.uvd_activity / 100u; break;
    case RB_SMU13_SENSOR_INPUT_POWER:   *value = m.current_socket_power; break;
    case RB_SMU13_SENSOR_EDGE_TEMP:     *value = m.gfx_temp / 100u * TEMP_UNITS_PER_CENTIGRADE; break;
    case RB_SMU13_SENSOR_HOTSPOT_TEMP:  *value = m.soc_temp / 100u * TEMP_UNITS_PER_CENTIGRADE; break;
    case RB_SMU13_SENSOR_MCLK:          *value = (uint32_t)m.memclk * 100u; break;
    case RB_SMU13_SENSOR_SCLK:          *value = (uint32_t)m.gfxclk * 100u; break;
    case RB_SMU13_SENSOR_VDDGFX:        *value = m.voltage[0]; break;
    case RB_SMU13_SENSOR_VDDNB:         *value = m.voltage[1]; break;
    case RB_SMU13_SENSOR_SS_APU_SHARE:  ss_power_percent(&m, &apu, &dgpu); *value = apu; break;
    case RB_SMU13_SENSOR_SS_DGPU_SHARE: ss_power_percent(&m, &apu, &dgpu); *value = dgpu; break;
    default:                            return RB_SMU13_E_INVAL;
    }
    return RB_SMU13_OK;
}

int rb_smu13_set_watermarks(struct rb_smu13 *s,
                            const struct rb_smu13_wm_set *reader, uint32_t nreader,
                            const struct rb_smu13_wm_set *writer, uint32_t nwriter)
{
    struct rb_smu13_watermarks t;
    enum { WM_SOCCLK = 0, WM_DCFCLK = 1 };

    if (!s || nreader > RB_SMU13_NUM_WM_RANGES || nwriter > RB_SMU13_NUM_WM_RANGES ||
        (nreader && !reader) || (nwriter && !writer))
        return RB_SMU13_E_INVAL;
    zero(&t, sizeof(t));
    for (uint32_t i = 0; i < nreader; i++) {
        t.row[WM_DCFCLK][i].min_clock = reader[i].min_drain_mhz;
        t.row[WM_DCFCLK][i].max_clock = reader[i].max_drain_mhz;
        t.row[WM_DCFCLK][i].min_mclk = reader[i].min_fill_mhz;
        t.row[WM_DCFCLK][i].max_mclk = reader[i].max_fill_mhz;
        t.row[WM_DCFCLK][i].wm_setting = reader[i].wm_inst;
    }
    for (uint32_t i = 0; i < nwriter; i++) {
        t.row[WM_SOCCLK][i].min_clock = writer[i].min_fill_mhz;
        t.row[WM_SOCCLK][i].max_clock = writer[i].max_fill_mhz;
        t.row[WM_SOCCLK][i].min_mclk = writer[i].min_drain_mhz;
        t.row[WM_SOCCLK][i].max_mclk = writer[i].max_drain_mhz;
        t.row[WM_SOCCLK][i].wm_setting = writer[i].wm_inst;
    }
    return rb_smu13_update_table(s, RB_SMU13_TABLE_WATERMARKS, 0, &t, sizeof(t), 1);
}

/* ------------------------------------------------------- power control */

int rb_smu13_enable_gfxoff(struct rb_smu13 *s)
{
    return rb_smu13_send(s, RB_SMU13_MSG_EnableGfxOff, 0, 0);
}

int rb_smu13_gfx_off_control(struct rb_smu13 *s, int enable)
{
    return rb_smu13_send(s, enable ? RB_SMU13_MSG_AllowGfxOff : RB_SMU13_MSG_DisallowGfxOff, 0, 0);
}

int rb_smu13_get_gfxoff_status(struct rb_smu13 *s, uint32_t *status)
{
    if (!s || !s->hw || !status || !s->dw_gfx_misc_cntl)
        return RB_SMU13_E_INVAL;
    *status = (rd(s, s->dw_gfx_misc_cntl) & SMUIO_GFX_MISC_CNTL__PWR_GFXOFF_STATUS_MASK) >>
              SMUIO_GFX_MISC_CNTL__PWR_GFXOFF_STATUS_SHIFT;
    return RB_SMU13_OK;
}

int rb_smu13_set_vcn_power(struct rb_smu13 *s, int on)
{
    return rb_smu13_send(s, on ? RB_SMU13_MSG_PowerUpVcn : RB_SMU13_MSG_PowerDownVcn, 0, 0);
}

int rb_smu13_set_jpeg_power(struct rb_smu13 *s, int on)
{
    return rb_smu13_send(s, on ? RB_SMU13_MSG_PowerUpJpeg : RB_SMU13_MSG_PowerDownJpeg, 0, 0);
}

int rb_smu13_prepare_unload(struct rb_smu13 *s)
{
    return rb_smu13_send(s, RB_SMU13_MSG_PrepareMp1ForUnload, 0, 0);
}

int rb_smu13_mode2_reset(struct rb_smu13 *s)
{
    return rb_smu13_send(s, RB_SMU13_MSG_GfxDeviceDriverReset, RB_SMU13_RESET_MODE_2, 0);
}

/* --------------------------------------------------------------- clocks */

/* yellow_carp_clk_dpm_is_enabled: 1/0, or <0 without a feature mask. */
static int clk_dpm_enabled(const struct rb_smu13 *s, enum rb_smu13_clk clk)
{
    uint32_t bit;
    if (!s->have_features)
        return RB_SMU13_E_NODATA;
    switch (clk) {
    case RB_SMU13_CLK_MCLK:
    case RB_SMU13_CLK_FCLK: bit = RB_SMU13_FEATURE_FCLK_DPM; break;
    case RB_SMU13_CLK_GFX:  bit = RB_SMU13_FEATURE_GFX_DPM; break;
    case RB_SMU13_CLK_SOC:  bit = RB_SMU13_FEATURE_SOCCLK_DPM; break;
    case RB_SMU13_CLK_VCLK:
    case RB_SMU13_CLK_DCLK: bit = RB_SMU13_FEATURE_VCN_DPM; break;
    default:                return 1;
    }
    return (int)((s->features >> bit) & 1);
}

int rb_smu13_get_dpm_level_count(struct rb_smu13 *s, enum rb_smu13_clk clk, uint32_t *count)
{
    if (!s || !count)
        return RB_SMU13_E_INVAL;
    if (!s->have_clocks)
        return RB_SMU13_E_NODATA;
    switch (clk) {
    case RB_SMU13_CLK_SOC:  *count = s->clocks.num_soc_levels; break;
    case RB_SMU13_CLK_VCLK:
    case RB_SMU13_CLK_DCLK: *count = s->clocks.num_vcn_levels; break;
    case RB_SMU13_CLK_MCLK:
    case RB_SMU13_CLK_FCLK: *count = s->clocks.num_df_pstates; break;
    default:                return RB_SMU13_E_INVAL;
    }
    return RB_SMU13_OK;
}

int rb_smu13_get_dpm_freq_by_index(struct rb_smu13 *s, enum rb_smu13_clk clk,
                                   uint32_t level, uint32_t *mhz)
{
    const struct rb_smu13_dpm_clocks *c;
    if (!s || !mhz)
        return RB_SMU13_E_INVAL;
    if (!s->have_clocks)
        return RB_SMU13_E_NODATA;
    c = &s->clocks;
    switch (clk) {
    case RB_SMU13_CLK_SOC:
        if (level >= c->num_soc_levels) return RB_SMU13_E_INVAL;
        *mhz = c->soc_clocks[level];
        break;
    case RB_SMU13_CLK_VCLK:
        if (level >= c->num_vcn_levels) return RB_SMU13_E_INVAL;
        *mhz = c->vclocks[level];
        break;
    case RB_SMU13_CLK_DCLK:
        if (level >= c->num_vcn_levels) return RB_SMU13_E_INVAL;
        *mhz = c->dclocks[level];
        break;
    case RB_SMU13_CLK_MCLK:
        if (level >= c->num_df_pstates) return RB_SMU13_E_INVAL;
        *mhz = c->df_pstate[level].memclk;
        break;
    case RB_SMU13_CLK_FCLK:
        if (level >= c->num_df_pstates) return RB_SMU13_E_INVAL;
        *mhz = c->df_pstate[level].fclk;
        break;
    default:
        return RB_SMU13_E_INVAL;
    }
    return RB_SMU13_OK;
}

int rb_smu13_get_current_clk(struct rb_smu13 *s, enum rb_smu13_clk clk, uint32_t *mhz)
{
    struct rb_smu13_metrics m;
    int e;
    if (!mhz)
        return RB_SMU13_E_INVAL;
    switch (clk) {
    case RB_SMU13_CLK_FCLK: return rb_smu13_send(s, RB_SMU13_MSG_GetFclkFrequency, 0, mhz);
    case RB_SMU13_CLK_GFX:  return rb_smu13_send(s, RB_SMU13_MSG_GetGfxclkFrequency, 0, mhz);
    case RB_SMU13_CLK_SOC:
    case RB_SMU13_CLK_VCLK:
    case RB_SMU13_CLK_DCLK:
    case RB_SMU13_CLK_MCLK:
        break;
    default:
        return RB_SMU13_E_INVAL;
    }
    e = rb_smu13_get_metrics(s, &m);
    if (e)
        return e;
    *mhz = clk == RB_SMU13_CLK_SOC ? m.socclk : clk == RB_SMU13_CLK_VCLK ? m.vclk :
           clk == RB_SMU13_CLK_DCLK ? m.dclk : m.memclk;
    return RB_SMU13_OK;
}

int rb_smu13_get_dpm_ultimate_freq(struct rb_smu13 *s, enum rb_smu13_clk clk,
                                   uint32_t *min, uint32_t *max)
{
    const struct rb_smu13_dpm_clocks *c;
    uint32_t max_level = 0, min_level = 0;
    int en, e;

    if (!s)
        return RB_SMU13_E_INVAL;
    en = clk_dpm_enabled(s, clk);
    if (en < 0)
        return en;
    if (!en)
        return RB_SMU13_E_NODATA;    /* Linux: VBIOS boot value */
    if (!s->have_clocks)
        return RB_SMU13_E_NODATA;
    c = &s->clocks;

    if (max) {
        switch (clk) {
        case RB_SMU13_CLK_GFX:  *max = c->max_gfx_clk; break;
        case RB_SMU13_CLK_MCLK:
        case RB_SMU13_CLK_FCLK: max_level = 0; break;
        case RB_SMU13_CLK_SOC:  max_level = (uint32_t)c->num_soc_levels - 1; break;
        case RB_SMU13_CLK_VCLK:
        case RB_SMU13_CLK_DCLK: max_level = (uint32_t)c->num_vcn_levels - 1; break;
        default:                return RB_SMU13_E_INVAL;
        }
        if (clk != RB_SMU13_CLK_GFX) {
            e = rb_smu13_get_dpm_freq_by_index(s, clk, max_level, max);
            if (e)
                return e;
        }
    }
    if (min) {
        switch (clk) {
        case RB_SMU13_CLK_GFX:  *min = c->min_gfx_clk; break;
        case RB_SMU13_CLK_MCLK:
        case RB_SMU13_CLK_FCLK: min_level = (uint32_t)c->num_df_pstates - 1; break;
        case RB_SMU13_CLK_SOC:
        case RB_SMU13_CLK_VCLK:
        case RB_SMU13_CLK_DCLK: min_level = 0; break;
        default:                return RB_SMU13_E_INVAL;
        }
        if (clk != RB_SMU13_CLK_GFX) {
            e = rb_smu13_get_dpm_freq_by_index(s, clk, min_level, min);
            if (e)
                return e;
        }
    }
    return RB_SMU13_OK;
}

int rb_smu13_set_soft_freq_range(struct rb_smu13 *s, enum rb_smu13_clk clk,
                                 uint32_t min, uint32_t max)
{
    uint8_t msg_min, msg_max;
    int en, e;

    if (!s)
        return RB_SMU13_E_INVAL;
    en = clk_dpm_enabled(s, clk);
    if (en < 0)
        return en;
    if (!en)
        return RB_SMU13_E_INVAL;
    switch (clk) {
    case RB_SMU13_CLK_GFX:
        msg_min = RB_SMU13_MSG_SetHardMinGfxClk;
        msg_max = RB_SMU13_MSG_SetSoftMaxGfxClk;
        break;
    case RB_SMU13_CLK_FCLK:
        msg_min = RB_SMU13_MSG_SetHardMinFclkByFreq;
        msg_max = RB_SMU13_MSG_SetSoftMaxFclkByFreq;
        break;
    case RB_SMU13_CLK_SOC:
        msg_min = RB_SMU13_MSG_SetHardMinSocclkByFreq;
        msg_max = RB_SMU13_MSG_SetSoftMaxSocclkByFreq;
        break;
    case RB_SMU13_CLK_VCLK:
    case RB_SMU13_CLK_DCLK:
        msg_min = RB_SMU13_MSG_SetHardMinVcn;
        msg_max = RB_SMU13_MSG_SetSoftMaxVcn;
        break;
    default:
        return RB_SMU13_E_INVAL;
    }
    /* As Linux: only VCLK is shifted into the upper half. */
    if (clk == RB_SMU13_CLK_VCLK) {
        min <<= SMU_13_VCLK_SHIFT;
        max <<= SMU_13_VCLK_SHIFT;
    }
    e = rb_smu13_send(s, msg_min, min, 0);
    if (e)
        return e;
    return rb_smu13_send(s, msg_max, max, 0);
}

static uint32_t lowest_bit(uint32_t m)  { uint32_t i = 0; while (!(m & 1)) { m >>= 1; i++; } return i; }
static uint32_t highest_bit(uint32_t m) { uint32_t i = 0; while (m >>= 1) i++; return i; }

int rb_smu13_force_clk_levels(struct rb_smu13 *s, enum rb_smu13_clk clk, uint32_t mask)
{
    uint32_t lo = mask ? lowest_bit(mask) : 0, hi = mask ? highest_bit(mask) : 0;
    uint32_t fmin = 0, fmax = 0;
    int e;

    switch (clk) {
    case RB_SMU13_CLK_SOC:
    case RB_SMU13_CLK_FCLK:
    case RB_SMU13_CLK_VCLK:
    case RB_SMU13_CLK_DCLK:
        break;
    default:
        return RB_SMU13_E_INVAL;
    }
    e = rb_smu13_get_dpm_freq_by_index(s, clk, lo, &fmin);
    if (e)
        return e;
    e = rb_smu13_get_dpm_freq_by_index(s, clk, hi, &fmax);
    if (e)
        return e;
    return rb_smu13_set_soft_freq_range(s, clk, fmin, fmax);
}

/* yellow_carp_get_umd_pstate_clk_default */
static uint32_t umd_pstate_clk(const struct rb_smu13 *s, enum rb_smu13_clk clk)
{
    int v8 = s->mp1_rev == 8;            /* 13.0.8; else 13.0.1 / 13.0.3 */
    switch (clk) {
    case RB_SMU13_CLK_GFX:  return v8 ? 533 : 700;
    case RB_SMU13_CLK_SOC:  return v8 ? 533 : 678;
    case RB_SMU13_CLK_FCLK: return v8 ? 800 : 1800;
    default:                return 0;
    }
}

/* yellow_carp_get_dpm_profile_freq; ultimate-freq errors ignored as in Linux. */
static void profile_freq(struct rb_smu13 *s, enum rb_smu13_level level, enum rb_smu13_clk clk,
                         uint32_t *min, uint32_t *max)
{
    uint32_t limit = umd_pstate_clk(s, clk);
    switch (clk) {
    case RB_SMU13_CLK_GFX:
        if (level == RB_SMU13_LEVEL_PROFILE_PEAK)
            rb_smu13_get_dpm_ultimate_freq(s, clk, 0, &limit);
        else if (level == RB_SMU13_LEVEL_PROFILE_MIN_SCLK)
            rb_smu13_get_dpm_ultimate_freq(s, clk, &limit, 0);
        break;
    case RB_SMU13_CLK_SOC:
        if (level == RB_SMU13_LEVEL_PROFILE_PEAK)
            rb_smu13_get_dpm_ultimate_freq(s, clk, 0, &limit);
        break;
    case RB_SMU13_CLK_FCLK:
        if (level == RB_SMU13_LEVEL_PROFILE_PEAK)
            rb_smu13_get_dpm_ultimate_freq(s, clk, 0, &limit);
        else if (level == RB_SMU13_LEVEL_PROFILE_MIN_MCLK)
            rb_smu13_get_dpm_ultimate_freq(s, clk, &limit, 0);
        break;
    case RB_SMU13_CLK_VCLK:
    case RB_SMU13_CLK_DCLK:
        rb_smu13_get_dpm_ultimate_freq(s, clk, 0, &limit);
        break;
    default:
        break;
    }
    *min = *max = limit;
}

int rb_smu13_set_performance_level(struct rb_smu13 *s, enum rb_smu13_level level)
{
    static const enum rb_smu13_clk clks[5] = {
        RB_SMU13_CLK_GFX, RB_SMU13_CLK_FCLK, RB_SMU13_CLK_SOC, RB_SMU13_CLK_VCLK, RB_SMU13_CLK_DCLK,
    };
    uint32_t mn[5], mx[5];
    int e;

    if (!s)
        return RB_SMU13_E_INVAL;
    zero(mn, sizeof(mn));
    zero(mx, sizeof(mx));
    for (int i = 0; i < 5; i++) {
        switch (level) {
        case RB_SMU13_LEVEL_HIGH:
            rb_smu13_get_dpm_ultimate_freq(s, clks[i], 0, &mx[i]);
            mn[i] = mx[i];
            break;
        case RB_SMU13_LEVEL_LOW:
            rb_smu13_get_dpm_ultimate_freq(s, clks[i], &mn[i], 0);
            mx[i] = mn[i];
            break;
        case RB_SMU13_LEVEL_AUTO:
            rb_smu13_get_dpm_ultimate_freq(s, clks[i], &mn[i], &mx[i]);
            break;
        case RB_SMU13_LEVEL_PROFILE_STANDARD:
        case RB_SMU13_LEVEL_PROFILE_MIN_SCLK:
        case RB_SMU13_LEVEL_PROFILE_MIN_MCLK:
        case RB_SMU13_LEVEL_PROFILE_PEAK:
            profile_freq(s, level, clks[i], &mn[i], &mx[i]);
            break;
        case RB_SMU13_LEVEL_MANUAL:
        case RB_SMU13_LEVEL_PROFILE_EXIT:
            return RB_SMU13_OK;
        default:
            return RB_SMU13_E_INVAL;
        }
    }
    for (int i = 0; i < 5; i++) {
        if (!mn[i] || !mx[i])
            continue;
        e = rb_smu13_set_soft_freq_range(s, clks[i], mn[i], mx[i]);
        if (e)
            return e;
        if (clks[i] == RB_SMU13_CLK_GFX) {
            s->gfx_actual_hard_min = mn[i];
            s->gfx_actual_soft_max = mx[i];
        }
    }
    return RB_SMU13_OK;
}

/* -------------------------------------------------------------- bring-up */

int rb_smu13_bringup(struct rb_smu13 *s)
{
    int e = rb_smu13_check_fw_version(s);
    if (e)
        return e;
    e = rb_smu13_get_enabled_mask(s, 0);
    if (e)
        return e;
    e = rb_smu13_set_driver_table_location(s);
    if (e)
        return e;
    return rb_smu13_set_default_dpm_table(s);
}

const char *rb_smu13_strerror(int err)
{
    switch (err) {
    case RB_SMU13_OK:            return "ok";
    case RB_SMU13_E_INVAL:       return "invalid argument";
    case RB_SMU13_E_NOT_ALLOWED: return "message group not allowed (nothing sent)";
    case RB_SMU13_E_BUSY:        return "mailbox busy (nothing sent)";
    case RB_SMU13_E_TIMEOUT:     return "no answer from the SMU";
    case RB_SMU13_E_FW:          return "SMU answered with an error";
    case RB_SMU13_E_VERSION:     return "unsupported SMU version";
    case RB_SMU13_E_NODATA:      return "required table not loaded";
    case RB_SMU13_E_DEAD:        return "SMU context dead after a timeout (nothing sent)";
    default:                     return "unknown error";
    }
}
