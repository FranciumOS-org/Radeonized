/* SPDX-License-Identifier: MIT */
/*
 * Host test for src/common: runs rb_probe against a simulated Rembrandt and
 * fuzzes rb_disc_parse. Built with ASan/UBSan by `make hosttest`, which
 * `make kext` runs first. A crash here would have been a panic in the kext.
 *
 * The fake chip:
 *   - a 512 KB register BAR with the fixed registers and every register in
 *     rb_regs.h set to a value derived from its offset;
 *   - MM_INDEX / MM_INDEX_HI / MM_DATA emulation over a sparse GPU memory that
 *     holds a synthetic discovery binary at the offset under test and a
 *     pattern everywhere else;
 *   - an optional BAR0 window onto GPU memory offset 0;
 *   - a log of every register touched, so tests can assert what was read and
 *     that nothing but the index pair was ever written.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "rb_discovery.h"
#include "rb_probe.h"
#include "rb_regs.h"
#include "rb_dcn_deny.h"
#include "rb_smu.h"

int smu13_tests(const struct rb_disc *real_table);   /* smu13_test.c */

static int failures;
#define CHECK(c) do { if (!(c)) { printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)

/* ---------------------------------------------------------------- blob --- */

struct ipdesc {
    uint16_t hw_id; uint8_t inst, major, minor, rev;
    uint8_t nbase; uint32_t base[6];
};

/* Bases chosen so every rb_regs.h register lands inside 512 KB; MP0/MP1 at
 * 0x16000 so MP0 C2PMSG_33 resolves to the fixed 0x16061. */
static const struct ipdesc rembrandt_ips[] = {
    { RB_HWID_GC,    0, 10, 3, 3, 2, { 0x1260, 0xA000 } },
    { RB_HWID_MP0,   0, 13, 0, 8, 1, { 0x16000 } },
    { RB_HWID_MP1,   0, 13, 0, 8, 1, { 0x16000 } },
    { RB_HWID_MMHUB, 0, 2, 3, 0, 3, { 0x13200, 0x1A000, 0x2408 } },
    { RB_HWID_DMU,   0, 3, 1, 2, 5, { 0x12, 0xC0, 0x34C0, 0x9000, 0x2403 } },
    { RB_HWID_NBIF,  0, 7, 2, 0, 3, { 0x0, 0x14, 0xD20 } },
    { 42 /*SDMA0*/,  0, 5, 2, 3, 1, { 0x1260 } },
    { 12 /*VCN*/,    0, 3, 1, 1, 2, { 0x7800, 0x7E00 } },
};

static void st16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void st32(uint8_t *p, uint32_t v) { st16(p, (uint16_t)v); st16(p + 2, (uint16_t)(v >> 16)); }
static uint16_t sum16(const uint8_t *p, uint32_t n) { uint16_t s = 0; while (n--) s = (uint16_t)(s + *p++); return s; }

/* Build a v1 binary (header v1, IP table v3, GC table v1.0, harvest table)
 * the way the firmware lays it out. Returns its size. */
static uint32_t build_blob(uint8_t *b, uint32_t cap, const struct ipdesc *ips, unsigned nips,
                           uint16_t ip_table_version)
{
    uint32_t ip_off = 64, o, die_off, gc_off, hv_off, size;
    memset(b, 0, cap);
    st32(b, RB_DISC_BINARY_SIGNATURE);
    st16(b + 4, 1); st16(b + 6, 0);

    /* IP discovery header + die 0 */
    st32(b + ip_off, RB_DISC_TABLE_SIGNATURE);
    st16(b + ip_off + 4, ip_table_version);
    st16(b + ip_off + 12, 1);              /* num_dies */
    die_off = ip_off + 80;
    st16(b + ip_off + 14, 0); st16(b + ip_off + 16, (uint16_t)die_off);
    st16(b + die_off, 0); st16(b + die_off + 2, (uint16_t)nips);
    o = die_off + 4;
    for (unsigned i = 0; i < nips; i++) {
        st16(b + o, ips[i].hw_id); b[o + 2] = ips[i].inst; b[o + 3] = ips[i].nbase;
        b[o + 4] = ips[i].major; b[o + 5] = ips[i].minor; b[o + 6] = ips[i].rev; b[o + 7] = 0;
        o += 8;
        for (unsigned k = 0; k < ips[i].nbase; k++, o += 4)
            st32(b + o, ips[i].base[k]);
    }
    st16(b + ip_off + 6, (uint16_t)(o - ip_off));   /* IP table size */

    /* GC info v1.0: 12-byte header + 19 fields. 680M: 1 SE, 2 SA, 3 WGP/SA. */
    gc_off = (o + 15) & ~15u;
    st32(b + gc_off, RB_DISC_GC_TABLE_ID); st16(b + gc_off + 4, 1); st16(b + gc_off + 6, 0);
    st32(b + gc_off + 8, 12 + 19 * 4);
    st32(b + gc_off + 12 + RB_GC_NUM_SE * 4, 1);
    st32(b + gc_off + 12 + RB_GC_NUM_WGP0_PER_SA * 4, 3);
    st32(b + gc_off + 12 + RB_GC_NUM_SA_PER_SE * 4, 2);
    st32(b + gc_off + 12 + RB_GC_WAVE_SIZE * 4, 32);

    /* Harvest: VCN instance 1 harvested. */
    hv_off = (gc_off + 12 + 19 * 4 + 15) & ~15u;
    st32(b + hv_off, RB_DISC_HARVEST_SIGNATURE); st32(b + hv_off + 4, 0);
    st16(b + hv_off + 8, 12); b[hv_off + 10] = 1;
    size = hv_off + 8 + 32 * 4;

    /* table_list: IP, GC, HARVEST */
    st16(b + 12 + 0 * 8, (uint16_t)ip_off);
    st16(b + 12 + 0 * 8 + 2, sum16(b + ip_off, o - ip_off));
    st16(b + 12 + 1 * 8, (uint16_t)gc_off);
    st16(b + 12 + 1 * 8 + 2, sum16(b + gc_off, 12 + 19 * 4));
    st16(b + 12 + 2 * 8, (uint16_t)hv_off);
    st16(b + 12 + 2 * 8 + 2, sum16(b + hv_off, 8 + 32 * 4));

    st16(b + 10, (uint16_t)size);
    st16(b + 8, sum16(b + 10, size - 10));
    (void)cap;
    return size;
}

/* ----------------------------------------------------------- fake chip --- */

#define MMIO_LEN (512u << 10)

struct fake {
    uint32_t mmio[MMIO_LEN / 4];
    uint8_t  touched[MMIO_LEN / 4];   /* bit0 read, bit1 written */
    uint32_t index, index_hi;
    uint64_t blob_pos; uint8_t blob[RB_PROBE_BIN_MAX]; uint32_t blob_len;
    uint64_t fb_len;
    int all_ones;
    uint32_t bad_writes, hi_writes;

    /* Simulated SMU mailbox (off unless smu_enable() was called). */
    int smu_on;
    uint32_t smu_msg, smu_arg, smu_resp;     /* dword offsets */
    uint32_t smu_latency;                    /* delay_us calls before answering */
    uint32_t smu_countdown;
    int smu_pending;
    uint32_t smu_fail_msg, smu_fail_resp;    /* answer smu_fail_resp to this message */
    int smu_zeroed, smu_arg_set;             /* protocol order since the last message */
    uint32_t smu_order_errors, smu_msgs, delays;
};

#define SMU_NEVER 0xFFFFFFFFu
#define SMU_VERSION_SIM 0x00410F02u   /* program 0, 65.15.2 */

static void smu_answer(struct fake *f)
{
    uint32_t msg = f->mmio[f->smu_msg], arg = f->mmio[f->smu_arg], out = 0, resp = RB_SMU_RESP_OK;
    switch (msg) {
    case RB_SMU_MSG_GetSmuVersion:         out = SMU_VERSION_SIM; break;
    case RB_SMU_MSG_GetDriverIfVersion:    out = 4; break;
    case RB_SMU_MSG_GetEnabledSmuFeatures: out = arg == 0 ? 0x8C0FFFFFu : 0x000000F1u; break;
    default:                               resp = RB_SMU_RESP_UNKNOWN_CMD; break;
    }
    if (msg == f->smu_fail_msg) resp = f->smu_fail_resp;
    if (resp == RB_SMU_RESP_OK) f->mmio[f->smu_arg] = out;
    f->mmio[f->smu_resp] = resp;
    f->smu_pending = 0;
}

static void f_delay(void *ctx, uint32_t us)
{
    struct fake *f = ctx;
    f->delays += us;
    if (f->smu_pending && f->smu_latency != SMU_NEVER) {
        if (f->smu_countdown <= us) smu_answer(f);
        else f->smu_countdown -= us;
    }
}

static void smu_enable(struct fake *f, uint32_t base, uint32_t initial_resp, uint32_t latency)
{
    f->smu_on = 1;
    f->smu_msg = base + 0x282; f->smu_arg = base + 0x292; f->smu_resp = base + 0x29a;
    f->mmio[f->smu_msg] = 0; f->mmio[f->smu_arg] = 0; f->mmio[f->smu_resp] = initial_resp;
    f->smu_latency = latency;
}

static uint32_t pattern(uint64_t pos) { return (uint32_t)(pos * 2654435761u) ^ 0xA5A5A5A5u; }

static uint32_t mem32(struct fake *f, uint64_t pos)
{
    if (pos >= f->blob_pos && pos + 4 <= f->blob_pos + sizeof(f->blob)) {
        const uint8_t *p = f->blob + (pos - f->blob_pos);
        return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
    }
    return pattern(pos);
}

static uint32_t f_rreg(void *ctx, uint32_t off)
{
    struct fake *f = ctx;
    uint32_t dw = off / 4;
    if (off % 4 || dw >= MMIO_LEN / 4) { f->bad_writes += 1000; return 0; }
    f->touched[dw] |= 1;
    if (f->all_ones) return 0xFFFFFFFFu;
    if (dw == RB_mmMM_DATA) {
        uint64_t pos = ((uint64_t)f->index_hi << 31) | (f->index & 0x7FFFFFFFu);
        return mem32(f, pos);
    }
    if (dw == RB_mmMM_INDEX) return f->index;
    if (dw == RB_mmMM_INDEX_HI) return f->index_hi;
    return f->mmio[dw];
}

static void f_wreg(void *ctx, uint32_t off, uint32_t v)
{
    struct fake *f = ctx;
    uint32_t dw = off / 4;
    if (dw >= MMIO_LEN / 4) { f->bad_writes++; return; }
    f->touched[dw] |= 2;
    if (dw == RB_mmMM_INDEX) f->index = v;
    else if (dw == RB_mmMM_INDEX_HI) { f->index_hi = v; f->hi_writes++; }
    else if (f->smu_on && dw == f->smu_resp) { f->mmio[dw] = v; if (v == 0) f->smu_zeroed = 1; }
    else if (f->smu_on && dw == f->smu_arg) { f->mmio[dw] = v; f->smu_arg_set = 1; }
    else if (f->smu_on && dw == f->smu_msg) {
        /* Linux order: response cleared, argument written, then the message. */
        if (!f->smu_zeroed || !f->smu_arg_set || f->smu_pending) f->smu_order_errors++;
        f->smu_zeroed = f->smu_arg_set = 0;
        f->mmio[dw] = v;
        f->smu_msgs++;
        f->smu_pending = 1;
        f->smu_countdown = f->smu_latency;
        if (f->smu_latency == 0) smu_answer(f);
    }
    else f->bad_writes++;
}

static int f_fb_read(void *ctx, uint64_t off, uint8_t *dst, uint32_t size)
{
    struct fake *f = ctx;
    if (off < (16u << 20) || off + size > f->fb_len) { f->bad_writes += 1000; return -1; }
    for (uint32_t i = 0; i < size; i += 4) {
        uint32_t v = mem32(f, off + i);
        dst[i] = (uint8_t)v; dst[i + 1] = (uint8_t)(v >> 8);
        dst[i + 2] = (uint8_t)(v >> 16); dst[i + 3] = (uint8_t)(v >> 24);
    }
    return 0;
}

static struct fake *fake_new(uint32_t memsize_mb, uint64_t blob_pos, uint64_t fb_len,
                             const struct ipdesc *ips, unsigned nips)
{
    struct fake *f = calloc(1, sizeof(*f));
    f->blob_len = build_blob(f->blob, sizeof(f->blob), ips, nips, 3);
    f->blob_pos = blob_pos;
    f->fb_len = fb_len;
    for (uint32_t i = 0; i < MMIO_LEN / 4; i++)
        f->mmio[i] = 0x10000000u | i;          /* readable: value encodes offset */
    f->mmio[RB_mmRCC_CONFIG_MEMSIZE] = memsize_mb;
    f->mmio[RB_mmDRIVER_SCRATCH_0] = 0;
    f->mmio[RB_mmDRIVER_SCRATCH_1] = 0;
    f->mmio[RB_mmDRIVER_SCRATCH_2] = 0;
    f->mmio[RB_mmMP0_SMN_C2PMSG_33] = 0x80000000u;
    f->mmio[RB_mmIP_DISCOVERY_VERSION] = 2;
    f->index = 0x1234; f->index_hi = 0x5;      /* must come back unchanged */
    return f;
}

static struct rb_hw hw_of(struct fake *f)
{
    struct rb_hw hw = { f, MMIO_LEN, f_rreg, f_wreg, f->fb_len, f_fb_read, f_delay };
    return hw;
}

static const struct rb_reg_sample *find_sample(const struct rb_probe_result *r, const char *name)
{
    for (uint32_t i = 0; i < r->nregs; i++)
        if (!strcmp(r->regs[i].name, name)) return &r->regs[i];
    return NULL;
}

/* Common post-conditions for any run. */
static void check_policy(struct fake *f, const struct rb_probe_result *r)
{
    CHECK(f->bad_writes == 0);
    CHECK(r->refused_writes == 0);
    CHECK(f->index == 0x1234);
    CHECK(f->index_hi == 0x5);
    for (uint32_t i = 0; i < MMIO_LEN / 4; i++)
        if ((f->touched[i] & 2) && i != RB_mmMM_INDEX && i != RB_mmMM_INDEX_HI &&
            !(f->smu_on && (i == f->smu_msg || i == f->smu_arg || i == f->smu_resp))) {
            printf("  FAIL write to dword 0x%x\n", i); failures++;
        }
}

#define NIPS (sizeof(rembrandt_ips) / sizeof(rembrandt_ips[0]))

static void test_default_path(void)
{
    printf("default location, 512 MB carve-out, BAR0 256 MB\n");
    uint32_t mem = 512;
    struct fake *f = fake_new(mem, ((uint64_t)mem << 20) - RB_DISC_TMR_OFFSET, 256u << 20, rembrandt_ips, NIPS);
    struct rb_hw hw = hw_of(f);
    struct rb_probe_result *r = calloc(1, sizeof(*r));
    CHECK(rb_probe_run(&hw, RB_LEVEL_SAFE, r) == 0);
    CHECK(r->fail == NULL);
    CHECK(r->disc_source == RB_SRC_DEFAULT);
    CHECK(r->disc_offset == ((uint64_t)mem << 20) - RB_DISC_TMR_OFFSET);
    CHECK(r->disc_size == RB_DISC_TMR_SIZE);
    CHECK(r->bar0_checked == 0);          /* past the 256 MB window */
    CHECK(r->parse_err == RB_DISC_OK);
    CHECK(r->disc.check_flags == 0);
    CHECK(r->disc.nips == NIPS);
    CHECK(r->disc.ip_table_version == 3);
    CHECK(r->disc.gc.present == 1 && r->disc.gc.v1[RB_GC_NUM_SA_PER_SE] == 2);
    CHECK(r->disc.nharvest == 1 && r->disc.harvest[0].hw_id == 12 && r->disc.harvest[0].inst == 1);
    const struct rb_ip *gc = rb_disc_find(&r->disc, RB_HWID_GC, 0);
    CHECK(gc && gc->major == 10 && gc->minor == 3 && gc->revision == 3 && gc->base[1] == 0xA000);

    const struct rb_reg_sample *s = find_sample(r, "MP0_C2PMSG_33");
    CHECK(s && s->status == RB_REG_OK && s->dw == RB_mmMP0_SMN_C2PMSG_33 && s->value == 0x80000000u);
    s = find_sample(r, "MP1_C2PMSG_90");
    CHECK(s && s->status == RB_REG_OK && s->dw == 0x16000 + 0x29a && s->value == (0x10000000u | (0x16000 + 0x29a)));
    s = find_sample(r, "MMMC_VM_FB_LOCATION_BASE");
    CHECK(s && s->status == RB_REG_OK && s->dw == 0x1A000 + 0x8ec);
    s = find_sample(r, "GRBM_STATUS");
    CHECK(s && s->status == RB_REG_LEVEL);
    s = find_sample(r, "OTG0_CONTROL");
    CHECK(s && s->status == RB_REG_LEVEL);

    /* Level 1 must not have touched GC or DCN space at all. */
    CHECK(!(f->touched[0x1260 + 0xda4] & 1));
    CHECK(!(f->touched[0xA000 + 0x4c00] & 1));
    CHECK(!(f->touched[0x34C0 + 0x1b41] & 1));
    CHECK(f->hi_writes == 2 + 1);   /* signature read, table read, restore */
    check_policy(f, r);
    free(r); free(f);
}

static void test_levels(void)
{
    printf("level 2 reads DCN but not GC\n");
    struct fake *f = fake_new(512, (512ull << 20) - RB_DISC_TMR_OFFSET, 256u << 20, rembrandt_ips, NIPS);
    struct rb_hw hw = hw_of(f);
    struct rb_probe_result *r = calloc(1, sizeof(*r));
    CHECK(rb_probe_run(&hw, RB_LEVEL_DC, r) == 0);
    const struct rb_reg_sample *s = find_sample(r, "OTG0_CONTROL");
    CHECK(s && s->status == RB_REG_OK && s->dw == 0x34C0 + 0x1b41);
    s = find_sample(r, "GRBM_STATUS");
    CHECK(s && s->status == RB_REG_LEVEL);
    CHECK(!(f->touched[0x1260 + 0xda4] & 1));
    CHECK(!(f->touched[0xA000 + 0x4c00] & 1));
    check_policy(f, r);
    free(r); free(f);

    printf("level 3 reads GC and DCN\n");
    f = fake_new(512, (512ull << 20) - RB_DISC_TMR_OFFSET, 256u << 20, rembrandt_ips, NIPS);
    hw = hw_of(f);
    r = calloc(1, sizeof(*r));
    CHECK(rb_probe_run(&hw, RB_LEVEL_GC, r) == 0);
    s = find_sample(r, "GRBM_STATUS");
    CHECK(s && s->status == RB_REG_OK && s->dw == 0x1260 + 0xda4);
    s = find_sample(r, "RLC_CNTL");
    CHECK(s && s->status == RB_REG_OK && s->dw == 0xA000 + 0x4c00);
    s = find_sample(r, "HUBP3_PRIMARY_ADDR_HI");
    CHECK(s && s->status == RB_REG_OK && s->dw == 0x34C0 + 0x89f);
    CHECK(r->nregs == RB_NREG_DEFS);
    check_policy(f, r);
    free(r); free(f);
}

/* 2oq: the OTG0 timing registers a published framebuffer will be built from.
 * Guards three things an ordinary edit could silently break: that they are
 * read at level 2 at the right absolute dword, that the instance strides
 * rb_regs.h documents still hold (a wrong stride is how a future pipe-1
 * extension would read the wrong register), and that no two entries share a
 * name (find_sample returns the first match, so a duplicate would shadow). */
static void test_dcn_denylist(void)
{
    /* 2pm: the generated dcn302 -> dcn312 deny list. Apple's DAL is dcn302
     * code; these 299 dword offsets land on a DIFFERENT register on this
     * chip, so our write hook must refuse exactly these and pass the rest.
     *
     * Three properties, because a wrong deny list is silent either way: too
     * small and DAL writes a wrong register, too large and DAL is crippled
     * for no reason. */

    /* 1. Strictly ascending - rb_dcn_is_denied binary-searches it. */
    for (uint32_t i = 1; i < RB_DCN_DENY_COUNT; i++) {
        if (rb_dcn_deny[i - 1] >= rb_dcn_deny[i]) {
            printf("  FAIL deny list not ascending at %u: %#x >= %#x\n",
                   i, rb_dcn_deny[i - 1], rb_dcn_deny[i]);
            failures++;
            break;
        }
    }

    /* 2. Every entry is found by the search. Guards the search, not the data. */
    for (uint32_t i = 0; i < RB_DCN_DENY_COUNT; i++) {
        if (!rb_dcn_is_denied(rb_dcn_deny[i])) {
            printf("  FAIL rb_dcn_is_denied missed its own entry %#x at %u\n",
                   rb_dcn_deny[i], i);
            failures++;
            break;
        }
    }

    /* 3. The cases that matter, by name, from docs/linux-ref.
     *
     * DENIED: DCHUBBUB_SDPIF_CFG0 moved (74 of 77 DCHUBBUB registers did).
     * ALLOWED: the scanout path is identical in 3.0.2 and 3.1.2, and blocking
     * it would stop DAL doing the one thing it CAN get right on this chip. */
    CHECK(rb_dcn_is_denied(0x0048fu));    /* DCHUBBUB_SDPIF_CFG0 - moved */
    CHECK(!rb_dcn_is_denied(0x01b2au));   /* OTG0_OTG_H_TOTAL - identical */
    CHECK(!rb_dcn_is_denied(0x01b41u));   /* OTG0_OTG_CONTROL - identical */
    CHECK(!rb_dcn_is_denied(0x005e5u));   /* HUBP0_DCSURF_SURFACE_CONFIG */

    /* 4. Past the end of the table, so the search cannot run off it. */
    CHECK(!rb_dcn_is_denied(0xffffffffu));

    printf("dcn deny: %u offsets, sorted, searchable; OTG/HUBP allowed, "
           "DCHUBBUB denied\n", RB_DCN_DENY_COUNT);
}

static void test_dcn_timing_regs(void)
{
    printf("level 2 reads OTG0's full timing and HUBP0's pitch\n");
    struct fake *f = fake_new(512, (512ull << 20) - RB_DISC_TMR_OFFSET,
                              256u << 20, rembrandt_ips, NIPS);
    struct rb_hw hw = hw_of(f);
    struct rb_probe_result *r = calloc(1, sizeof(*r));
    CHECK(rb_probe_run(&hw, RB_LEVEL_DC, r) == 0);

    static const struct { const char *name; uint32_t dw; } want[] = {
        { "OTG0_STATUS",            0x1b49 },
        { "OTG0_STATUS_POSITION",   0x1b4a },
        { "OTG0_FRAME_COUNT",       0x1b4c },
        { "OTG0_H_BLANK_START_END", 0x1b2b },
        { "OTG0_H_SYNC_A",          0x1b2c },
        { "OTG0_H_SYNC_A_CNTL",     0x1b2d },
        { "OTG0_V_BLANK_START_END", 0x1b36 },
        { "OTG0_V_SYNC_A",          0x1b37 },
        { "OTG0_V_SYNC_A_CNTL",     0x1b38 },
        { "HUBP0_SURFACE_PITCH",    0x0607 },
        { "HUBP0_SURFACE_CONTROL",  0x061a },
        { "BL_PWM_CNTL",            0x2f19 },
        { "BL_PWM_CNTL2",           0x2f1a },
        { "BL_PWM_PERIOD_CNTL",     0x2f1b },
        { "BL_PWM_GRP1_REG_LOCK",   0x2f1c },
    };
    for (size_t i = 0; i < sizeof(want) / sizeof(want[0]); i++) {
        const struct rb_reg_sample *s = find_sample(r, want[i].name);
        CHECK(s && s->status == RB_REG_OK && s->dw == 0x34C0 + want[i].dw);
    }
    check_policy(f, r);
    free(r); free(f);

    printf("the instance strides rb_regs.h documents still hold\n");
    uint32_t otg[4] = { 0, 0, 0, 0 }, hubp[4] = { 0, 0, 0, 0 };
    for (size_t i = 0; i < RB_NREG_DEFS; i++) {
        const char *n = rb_reg_defs[i].name;
        for (int k = 0; k < 4; k++) {
            char a[32], b[32];
            snprintf(a, sizeof(a), "OTG%d_H_TOTAL", k);
            snprintf(b, sizeof(b), "HUBP%d_SURFACE_CONFIG", k);
            if (!strcmp(n, a)) otg[k]  = rb_reg_defs[i].reg;
            if (!strcmp(n, b)) hubp[k] = rb_reg_defs[i].reg;
        }
    }
    for (int k = 0; k < 4; k++) { CHECK(otg[k] != 0); CHECK(hubp[k] != 0); }
    for (int k = 1; k < 4; k++) {
        CHECK(otg[k]  - otg[k - 1]  == 0x80);
        CHECK(hubp[k] - hubp[k - 1] == 0xdc);
    }

    printf("no two register definitions share a name\n");
    for (size_t i = 0; i < RB_NREG_DEFS; i++)
        for (size_t j = i + 1; j < RB_NREG_DEFS; j++)
            CHECK(strcmp(rb_reg_defs[i].name, rb_reg_defs[j].name) != 0);
}

static void test_bar0_window(void)
{
    printf("small carve-out inside BAR0: BAR0 cross-check\n");
    struct fake *f = fake_new(128, (128ull << 20) - RB_DISC_TMR_OFFSET, 256u << 20, rembrandt_ips, NIPS);
    struct rb_hw hw = hw_of(f);
    struct rb_probe_result *r = calloc(1, sizeof(*r));
    CHECK(rb_probe_run(&hw, RB_LEVEL_SAFE, r) == 0);
    CHECK(r->bar0_checked == 1 && r->bar0_matches == 1);
    check_policy(f, r);
    free(r); free(f);
}

static void test_high_offset(void)
{
    printf("4 GB carve-out: MM_INDEX_HI path\n");
    struct fake *f = fake_new(4096, (4096ull << 20) - RB_DISC_TMR_OFFSET, 256u << 20, rembrandt_ips, NIPS);
    struct rb_hw hw = hw_of(f);
    struct rb_probe_result *r = calloc(1, sizeof(*r));
    CHECK(rb_probe_run(&hw, RB_LEVEL_SAFE, r) == 0);
    CHECK(r->disc_source == RB_SRC_DEFAULT && r->disc.nips == NIPS);
    check_policy(f, r);
    free(r); free(f);
}

static void test_scratch_path(void)
{
    printf("table found through DRIVER_SCRATCH\n");
    uint64_t pos = 300ull << 20;
    struct fake *f = fake_new(512, pos, 256u << 20, rembrandt_ips, NIPS);
    f->mmio[RB_mmDRIVER_SCRATCH_0] = (uint32_t)pos;
    f->mmio[RB_mmDRIVER_SCRATCH_1] = 0;
    f->mmio[RB_mmDRIVER_SCRATCH_2] = 8192;
    struct rb_hw hw = hw_of(f);
    struct rb_probe_result *r = calloc(1, sizeof(*r));
    CHECK(rb_probe_run(&hw, RB_LEVEL_SAFE, r) == 0);
    CHECK(r->disc_source == RB_SRC_SCRATCH && r->disc_offset == pos && r->disc_size == 8192);
    check_policy(f, r);
    free(r); free(f);

    printf("scratch pointer outside the carve-out is refused\n");
    f = fake_new(512, 0, 256u << 20, rembrandt_ips, NIPS);
    f->mmio[RB_mmDRIVER_SCRATCH_0] = 0;
    f->mmio[RB_mmDRIVER_SCRATCH_1] = 0x10;  /* 64 GB */
    f->mmio[RB_mmDRIVER_SCRATCH_2] = 8192;
    hw = hw_of(f);
    r = calloc(1, sizeof(*r));
    CHECK(rb_probe_run(&hw, RB_LEVEL_SAFE, r) == -1);
    CHECK(r->fail && strstr(r->fail, "outside"));
    check_policy(f, r);
    free(r); free(f);
}

static void test_no_device(void)
{
    printf("device reads all-ones\n");
    struct fake *f = fake_new(512, 0, 0, rembrandt_ips, NIPS);
    f->all_ones = 1;
    struct rb_hw hw = hw_of(f);
    struct rb_probe_result *r = calloc(1, sizeof(*r));
    CHECK(rb_probe_run(&hw, RB_LEVEL_SAFE, r) == -1);
    CHECK(r->fail && strstr(r->fail, "all-ones"));
    CHECK(r->index_writes == 0);
    CHECK(f->bad_writes == 0);
    free(r); free(f);

    printf("no table at the default offset, no scratch\n");
    f = fake_new(512, 1ull << 40, 0, rembrandt_ips, NIPS);   /* blob nowhere reachable */
    hw = hw_of(f);
    r = calloc(1, sizeof(*r));
    CHECK(rb_probe_run(&hw, RB_LEVEL_SAFE, r) == -1);
    CHECK(r->disc_source == RB_SRC_DEFAULT_NOSIG);
    CHECK(r->parse_err == RB_DISC_E_SIGNATURE);
    check_policy(f, r);
    free(r); free(f);

    printf("carve-out size 0: stops before touching MM_INDEX\n");
    f = fake_new(0, 0, 0, rembrandt_ips, NIPS);
    f->mmio[RB_mmDRIVER_SCRATCH_2] = 8192;
    hw = hw_of(f);
    r = calloc(1, sizeof(*r));
    CHECK(rb_probe_run(&hw, RB_LEVEL_SAFE, r) == -1);
    CHECK(r->fail && strstr(r->fail, "system memory"));
    CHECK(r->index_writes == 0);
    check_policy(f, r);
    free(r); free(f);
}

static void test_version_gate(void)
{
    printf("unexpected MMHUB version: its registers are skipped\n");
    struct ipdesc ips[NIPS];
    memcpy(ips, rembrandt_ips, sizeof(ips));
    for (unsigned i = 0; i < NIPS; i++)
        if (ips[i].hw_id == RB_HWID_MMHUB) ips[i].minor = 0;
    struct fake *f = fake_new(512, (512ull << 20) - RB_DISC_TMR_OFFSET, 0, ips, NIPS);
    struct rb_hw hw = hw_of(f);
    struct rb_probe_result *r = calloc(1, sizeof(*r));
    CHECK(rb_probe_run(&hw, RB_LEVEL_GC, r) == 0);
    const struct rb_reg_sample *s = find_sample(r, "MMMC_VM_FB_OFFSET");
    CHECK(s && s->status == RB_REG_VERSION);
    CHECK(!(f->touched[0x1A000 + 0x8d7] & 1));
    check_policy(f, r);
    free(r); free(f);

    printf("MMHUB 2.4 (Linux: mmhub_v2_3) is accepted\n");
    for (unsigned i = 0; i < NIPS; i++)
        if (ips[i].hw_id == RB_HWID_MMHUB) ips[i].minor = 4;
    f = fake_new(512, (512ull << 20) - RB_DISC_TMR_OFFSET, 0, ips, NIPS);
    hw = hw_of(f);
    r = calloc(1, sizeof(*r));
    CHECK(rb_probe_run(&hw, RB_LEVEL_SAFE, r) == 0);
    s = find_sample(r, "MMMC_VM_FB_OFFSET");
    CHECK(s && s->status == RB_REG_OK);
    check_policy(f, r);
    free(r); free(f);
}

/* The table read from the real 680M (tests/data/rembrandt-680m-*.bin): parse
 * it, then run the whole probe with it placed where that machine has it
 * (4096 MB carve-out, so the MM_INDEX_HI path), at the highest level. */
static void test_real_table(const char *path, const uint8_t *bin, uint32_t len)
{
    static struct rb_disc d;
    const struct rb_ip *ip;
    printf("real table %s\n", path);
    CHECK(rb_disc_parse(bin, len, &d) == RB_DISC_OK);
    CHECK(d.check_flags == 0);
    CHECK(d.ip_table_version == 4 && d.base_addr_64_bit == 0);   /* v4, 32-bit bases */
    CHECK(d.nips == 34);
    ip = rb_disc_find(&d, RB_HWID_GC, 0);
    CHECK(ip && ip->major == 10 && ip->minor == 3 && ip->revision == 3 && ip->base[0] == 0x1260 && ip->base[1] == 0xa000);
    ip = rb_disc_find(&d, RB_HWID_DMU, 0);
    CHECK(ip && ip->major == 3 && ip->minor == 1 && ip->revision == 3 && ip->base[2] == 0x34c0);
    ip = rb_disc_find(&d, RB_HWID_MMHUB, 0);
    CHECK(ip && ip->major == 2 && ip->minor == 4 && ip->base[1] == 0x1a000);
    ip = rb_disc_find(&d, RB_HWID_MP0, 0);
    CHECK(ip && ip->major == 13 && ip->minor == 0 && ip->revision == 3 && ip->base[0] == 0x16000);
    CHECK(d.gc.present == 1 && d.gc.v1[RB_GC_NUM_SE] == 1 && d.gc.v1[RB_GC_NUM_SA_PER_SE] == 2 &&
          d.gc.v1[RB_GC_NUM_WGP0_PER_SA] == 3);   /* 1 x 2 x 3 WGP = 12 CUs */

    struct fake *f = fake_new(4096, 0xffff0000ull, 256u << 20, rembrandt_ips, 0);
    memset(f->blob, 0, sizeof(f->blob));
    memcpy(f->blob, bin, len < sizeof(f->blob) ? len : sizeof(f->blob));
    struct rb_hw hw = hw_of(f);
    struct rb_probe_result *r = calloc(1, sizeof(*r));
    CHECK(rb_probe_run(&hw, RB_LEVEL_MAX, r) == 0);
    CHECK(r->disc_source == RB_SRC_DEFAULT && r->disc_offset == 0xffff0000ull);
    CHECK(r->disc.nips == 34);
    for (uint32_t i = 0; i < r->nregs; i++)
        if (r->regs[i].status != RB_REG_OK) {
            printf("  FAIL %s not read on the real table (status %u)\n", r->regs[i].name, r->regs[i].status);
            failures++;
        }
    const struct rb_reg_sample *s = find_sample(r, "MP0_C2PMSG_33");
    CHECK(s && s->dw == RB_mmMP0_SMN_C2PMSG_33);
    s = find_sample(r, "MMMC_VM_FB_LOCATION_BASE");
    CHECK(s && s->dw == 0x1a000 + 0x8ec);
    s = find_sample(r, "RLC_CNTL");
    CHECK(s && s->dw == 0xa000 + 0x4c00);

    smu_enable(f, 0x16000, RB_SMU_RESP_OK, 3);
    struct rb_smu_result *sm = calloc(1, sizeof(*sm));
    CHECK(rb_smu_query(&hw, &r->disc, 1000, sm) == 0);
    CHECK(sm->dw_msg == 0x16282 && sm->dw_arg == 0x16292 && sm->dw_resp == 0x1629a);
    CHECK(sm->mp1_major == 13 && sm->mp1_minor == 0 && sm->mp1_rev == 3);
    CHECK(sm->refused_writes == 0 && f->smu_order_errors == 0);
    free(sm);
    check_policy(f, r);
    free(r); free(f);
}

/* ------------------------------------------------------------- the SMU --- */

/* Probe at level 1, then the SMU queries; returns the SMU result (caller frees). */
static struct rb_smu_result *smu_run(struct fake *f, uint32_t timeout_us, int *rc)
{
    struct rb_hw hw = hw_of(f);
    struct rb_probe_result *r = calloc(1, sizeof(*r));
    struct rb_smu_result *s = calloc(1, sizeof(*s));
    CHECK(rb_probe_run(&hw, RB_LEVEL_SAFE, r) == 0);
    *rc = rb_smu_query(&hw, &r->disc, timeout_us, s);
    check_policy(f, r);
    CHECK(s->refused_writes == 0);
    CHECK(f->smu_order_errors == 0);
    free(r);
    return s;
}

#define SMU_FAKE() fake_new(512, (512ull << 20) - RB_DISC_TMR_OFFSET, 0, rembrandt_ips, NIPS)

static void test_smu(void)
{
    struct fake *f;
    struct rb_smu_result *s;
    int rc;

    printf("SMU: version, interface version, feature mask\n");
    f = SMU_FAKE(); smu_enable(f, 0x16000, RB_SMU_RESP_OK, 5);
    s = smu_run(f, 1000, &rc);
    CHECK(rc == 0 && s->fail == NULL);
    CHECK(s->dw_msg == 0x16282 && s->dw_arg == 0x16292 && s->dw_resp == 0x1629a);
    CHECK(s->nxfers == 4 && f->smu_msgs == 4 && s->writes == 12);
    CHECK(s->have_version && s->smu_version == SMU_VERSION_SIM);
    CHECK(s->have_if_version && s->if_version == 4);
    CHECK(s->have_features && s->features == 0x000000F18C0FFFFFull);
    CHECK(s->x[2].msg == RB_SMU_MSG_GetEnabledSmuFeatures && s->x[2].arg == 0 && s->x[3].arg == 1);
    CHECK(s->x[0].polls >= 4 && s->x[0].polls <= 6);
    free(s); free(f);

    printf("SMU: slow answer inside the timeout\n");
    f = SMU_FAKE(); smu_enable(f, 0x16000, RB_SMU_RESP_OK, 900);
    s = smu_run(f, 1000, &rc);
    CHECK(rc == 0 && s->nxfers == 4);
    free(s); free(f);

    printf("SMU: mailbox not idle (response 0): nothing sent\n");
    f = SMU_FAKE(); smu_enable(f, 0x16000, RB_SMU_RESP_NONE, 5);
    s = smu_run(f, 1000, &rc);
    CHECK(rc == -1 && s->writes == 0 && f->smu_msgs == 0 && s->nxfers == 0);
    CHECK(s->fail && strstr(s->fail, "not idle"));
    free(s); free(f);

    printf("SMU: mailbox reports busy (0xFC): nothing sent\n");
    f = SMU_FAKE(); smu_enable(f, 0x16000, RB_SMU_RESP_REJ_BUSY, 5);
    s = smu_run(f, 1000, &rc);
    CHECK(rc == -1 && s->writes == 0 && f->smu_msgs == 0);
    free(s); free(f);

    printf("SMU: never answers: one message, then stop\n");
    f = SMU_FAKE(); smu_enable(f, 0x16000, RB_SMU_RESP_OK, SMU_NEVER);
    s = smu_run(f, 1000, &rc);
    CHECK(rc == -1 && s->nxfers == 1 && f->smu_msgs == 1);
    CHECK(s->x[0].status == RB_SMU_X_TIMEOUT && s->x[0].polls == 1000);
    CHECK(!s->have_version);
    free(s); free(f);

    printf("SMU: error answer stops the sequence\n");
    f = SMU_FAKE(); smu_enable(f, 0x16000, RB_SMU_RESP_OK, 5);
    f->smu_fail_msg = RB_SMU_MSG_GetDriverIfVersion; f->smu_fail_resp = RB_SMU_RESP_UNKNOWN_CMD;
    s = smu_run(f, 1000, &rc);
    CHECK(rc == -1 && s->nxfers == 2 && f->smu_msgs == 2);
    CHECK(s->have_version && !s->have_if_version);
    CHECK(s->x[1].status == RB_SMU_X_FW_ERROR && s->x[1].resp == RB_SMU_RESP_UNKNOWN_CMD);
    free(s); free(f);

    printf("SMU: MP1 not version 13: nothing sent\n");
    {
        struct ipdesc ips[NIPS];
        memcpy(ips, rembrandt_ips, sizeof(ips));
        for (unsigned i = 0; i < NIPS; i++)
            if (ips[i].hw_id == RB_HWID_MP1) ips[i].major = 12;
        f = fake_new(512, (512ull << 20) - RB_DISC_TMR_OFFSET, 0, ips, NIPS);
        smu_enable(f, 0x16000, RB_SMU_RESP_OK, 5);
        s = smu_run(f, 1000, &rc);
        CHECK(rc == -1 && s->writes == 0 && f->smu_msgs == 0);
        free(s); free(f);
    }
}

/* ----------------------------------------------------------- the fuzz --- */

static uint64_t rng = 0x9E3779B97F4A7C15ull;
static uint32_t rnd(void) { rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17; return (uint32_t)rng; }

static void test_fuzz(void)
{
    static uint8_t good[RB_PROBE_BIN_MAX], buf[RB_PROBE_BIN_MAX];
    static struct rb_disc d;
    uint32_t n = build_blob(good, sizeof(good), rembrandt_ips, NIPS, 3);

    printf("parser: every truncation of a %u-byte table\n", n);
    for (uint32_t len = 0; len <= n; len++) {
        uint8_t *exact = malloc(len ? len : 1);   /* exact size so ASan sees overreads */
        memcpy(exact, good, len);
        int e = rb_disc_parse(exact, len, &d);
        if (len < n) CHECK(e != RB_DISC_OK || d.nips <= NIPS);
        free(exact);
    }
    CHECK(rb_disc_parse(good, n, &d) == RB_DISC_OK && d.check_flags == 0);

    printf("parser: nothing past binary_size is used\n");
    {
        /* Same table, then a copy of its IP table placed after binary_size
         * with the header pointing at it: must be rejected, not parsed. */
        uint8_t *big = malloc(n + 256);
        memcpy(big, good, n);
        memcpy(big + n, good + 64, 200);
        st16(big + 12, (uint16_t)n);
        CHECK(rb_disc_parse(big, n + 256, &d) == RB_DISC_E_NO_IP_TABLE);
        free(big);
    }

    printf("parser: 300000 random mutations\n");
    for (int it = 0; it < 300000; it++) {
        uint32_t len = n, flips = 1 + rnd() % 8;
        memcpy(buf, good, n);
        for (uint32_t k = 0; k < flips; k++) {
            uint32_t pos = rnd() % (it % 4 == 0 ? 160 : n);   /* bias toward the headers */
            buf[pos] = (uint8_t)rnd();
        }
        if (it % 7 == 0) len = rnd() % (n + 1);
        uint8_t *exact = malloc(len ? len : 1);
        memcpy(exact, buf, len);
        int e = rb_disc_parse(exact, len, &d);
        if (e == RB_DISC_OK) CHECK(d.nips <= RB_DISC_MAX_IPS && d.nharvest <= RB_DISC_MAX_HARVEST);
        free(exact);
    }

    printf("parser: 64-bit base addresses (table v4)\n");
    {
        /* Rewrite the IP list with 64-bit bases. */
        uint8_t *b = good;
        uint32_t m = build_blob(b, sizeof(good), rembrandt_ips, 1, 4);   /* GC only */
        uint32_t ip_off = 64, die_off = ip_off + 80, o = die_off + 4;
        static uint8_t v4[RB_PROBE_BIN_MAX];
        memcpy(v4, b, m);
        v4[ip_off + 78] = 1;                     /* base_addr_64_bit */
        /* entry: 8 bytes, then 2 x u64 */
        st32(v4 + o + 8, 0xC0001260u); st32(v4 + o + 12, 0x7);   /* top bits must be cleared */
        st32(v4 + o + 16, 0x0000A000u); st32(v4 + o + 20, 0x0);
        /* everything after moved by 8 bytes is irrelevant to this check; fix size so it parses */
        st16(v4 + 10, (uint16_t)m);
        CHECK(rb_disc_parse(v4, m, &d) == RB_DISC_OK);
        CHECK(d.base_addr_64_bit == 1 && d.nips == 1 && d.ips[0].base[0] == 0x1260 && d.ips[0].base[1] == 0xA000);
    }
}

int main(int argc, char **argv)
{
    /* Optional: parse real discovery binaries given on the command line. */
    static struct rb_disc real_disc;
    int have_real = 0;
    for (int i = 1; i < argc; i++) {
        static uint8_t buf[1 << 16];
        static struct rb_disc d;
        FILE *fp = fopen(argv[i], "rb");
        if (!fp) { printf("cannot open %s\n", argv[i]); failures++; continue; }
        size_t n = fread(buf, 1, sizeof(buf), fp);
        fclose(fp);
        int e = rb_disc_parse(buf, (uint32_t)n, &d);
        printf("%s: %s, %u IPs, flags 0x%x\n", argv[i], rb_disc_strerror(e), d.nips, d.check_flags);
        CHECK(e == RB_DISC_OK);
        if (strstr(argv[i], "rembrandt-680m")) {
            test_real_table(argv[i], buf, (uint32_t)n);
            real_disc = d;
            have_real = e == RB_DISC_OK;
        }
    }

    test_default_path();
    test_levels();
    test_dcn_timing_regs();
    test_dcn_denylist();
    test_bar0_window();
    test_high_offset();
    test_scratch_path();
    test_no_device();
    test_version_gate();
    test_smu();
    failures += smu13_tests(have_real ? &real_disc : NULL);
    test_fuzz();

    if (failures) {
        printf("hosttest: %d FAILED\n", failures);
        return 1;
    }
    printf("hosttest: all passed\n");
    return 0;
}
