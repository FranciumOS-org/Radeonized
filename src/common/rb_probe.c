/* SPDX-License-Identifier: MIT */
/* Read-only probe of a Rembrandt-class AMD APU. See rb_probe.h. */
#include "rb_probe.h"
#include "rb_regs.h"

static void zero(void *p, uint32_t n)
{
    volatile uint8_t *b = (volatile uint8_t *)p;
    for (uint32_t i = 0; i < n; i++)
        b[i] = 0;
}

/* All register traffic goes through these two. */
static int rreg(const struct rb_hw *hw, uint32_t dw, uint32_t *out)
{
    if (dw >= hw->mmio_len / 4)
        return -1;
    *out = hw->rreg(hw->ctx, dw * 4);
    return 0;
}

/* The write policy: only the MMIO index pair, nothing else, ever. */
static void wreg_index(const struct rb_hw *hw, struct rb_probe_result *r, uint32_t dw, uint32_t v)
{
    if (dw != RB_mmMM_INDEX && dw != RB_mmMM_INDEX_HI) {
        r->refused_writes++;
        return;
    }
    r->index_writes++;
    hw->wreg(hw->ctx, dw * 4, v);
}

/* Read `size` bytes (multiple of 4) of GPU memory at `pos` through
 * MM_INDEX/MM_INDEX_HI/MM_DATA, as amdgpu_device_mm_access does: bit 31 of
 * MM_INDEX selects memory, MM_INDEX_HI holds pos >> 31. */
static void mm_read(const struct rb_hw *hw, struct rb_probe_result *r,
                    uint64_t pos, uint8_t *dst, uint32_t size)
{
    uint32_t hi = 0xFFFFFFFFu;
    for (uint32_t i = 0; i < size; i += 4) {
        uint64_t p = pos + i;
        uint32_t v = 0, tmp = (uint32_t)(p >> 31);
        wreg_index(hw, r, RB_mmMM_INDEX, ((uint32_t)p) | 0x80000000u);
        if (tmp != hi) {
            wreg_index(hw, r, RB_mmMM_INDEX_HI, tmp);
            hi = tmp;
        }
        (void)rreg(hw, RB_mmMM_DATA, &v);
        dst[i] = (uint8_t)v;
        dst[i + 1] = (uint8_t)(v >> 8);
        dst[i + 2] = (uint8_t)(v >> 16);
        dst[i + 3] = (uint8_t)(v >> 24);
    }
}

static uint32_t mm_read32(const struct rb_hw *hw, struct rb_probe_result *r, uint64_t pos)
{
    uint8_t b[4];
    mm_read(hw, r, pos, b, 4);
    return (uint32_t)b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
}

/* Is BAR0 a straight window onto GPU memory from offset 0? Compare the bytes
 * just read through MM_DATA with BAR0 at the same offset. Only reads BAR0, and
 * never its first 16 MB, which the boot framebuffer driver has mapped. */
#define RB_BAR0_KEEP_OUT (16u << 20)

static void bar0_compare(const struct rb_hw *hw, struct rb_probe_result *r)
{
    if (!hw->fb_read || r->disc_offset < RB_BAR0_KEEP_OUT ||
        r->disc_offset > hw->fb_len || r->disc_size > hw->fb_len - r->disc_offset)
        return;
    if (hw->fb_read(hw->ctx, r->disc_offset, r->bar0_bin, r->disc_size))
        return;
    r->bar0_checked = 1;
    r->bar0_matches = 1;
    for (uint32_t i = 0; i < r->disc_size; i++)
        if (r->bar0_bin[i] != r->disc_bin[i]) {
            r->bar0_matches = 0;
            return;
        }
}

static void sample_regs(const struct rb_hw *hw, struct rb_probe_result *r)
{
    for (uint32_t i = 0; i < RB_NREG_DEFS && r->nregs < RB_PROBE_MAX_REGS; i++) {
        const struct rb_reg_def *def = &rb_reg_defs[i];
        struct rb_reg_sample *s = &r->regs[r->nregs++];
        const struct rb_ip *ip = rb_disc_find(&r->disc, def->hw_id, 0);
        s->name = def->name;
        if (def->level > r->level) {
            s->status = RB_REG_LEVEL;
            continue;
        }
        if (!ip || rb_disc_reg(&r->disc, def->hw_id, 0, def->base_idx, def->reg, &s->dw)) {
            s->status = RB_REG_NO_IP;
            continue;
        }
        if (ip->major != def->want_major || ip->minor < def->minor_lo || ip->minor > def->minor_hi) {
            s->status = RB_REG_VERSION;
            continue;
        }
        s->status = rreg(hw, s->dw, &s->value) ? RB_REG_OUT_OF_BAR : RB_REG_OK;
    }
}

int rb_probe_run(const struct rb_hw *hw, int level, struct rb_probe_result *r)
{
    uint32_t sig;
    uint64_t def_off = 0;

    if (!r)
        return -1;
    zero(r, sizeof(*r));
    r->level = level;
    if (!hw || !hw->rreg || !hw->wreg) {
        r->fail = "no hardware access";
        return -1;
    }
    /* Everything below reads fixed offsets up to IP_DISCOVERY_VERSION. */
    if (hw->mmio_len / 4 <= RB_mmIP_DISCOVERY_VERSION) {
        r->fail = "register BAR too small";
        return -1;
    }

    (void)rreg(hw, RB_mmRCC_CONFIG_MEMSIZE, &r->memsize_mb);
    (void)rreg(hw, RB_mmDRIVER_SCRATCH_0, &r->scratch[0]);
    (void)rreg(hw, RB_mmDRIVER_SCRATCH_1, &r->scratch[1]);
    (void)rreg(hw, RB_mmDRIVER_SCRATCH_2, &r->scratch[2]);
    (void)rreg(hw, RB_mmMP0_SMN_C2PMSG_33, &r->c2pmsg33);
    (void)rreg(hw, RB_mmIP_DISCOVERY_VERSION, &r->ipd_version);

    if (r->memsize_mb == 0xFFFFFFFFu) {
        r->fail = "registers read all-ones: device not responding";
        return -1;
    }
    if (r->memsize_mb == 0) {
        /* Linux then treats the table as living in system memory (scratch
         * registers or ACPI give the address) and reads it with memremap.
         * MM_DATA would read the wrong thing: stop. */
        r->fail = "carve-out size is 0: table in system memory, not supported";
        return -1;
    }

    (void)rreg(hw, RB_mmMM_INDEX, &r->mm_index_saved);
    (void)rreg(hw, RB_mmMM_INDEX_HI, &r->mm_index_hi_saved);

    /* amdgpu_discovery_get_tmr_info, bare-metal path. */
    r->disc_size = RB_DISC_TMR_SIZE;
    if (r->memsize_mb > (RB_DISC_TMR_OFFSET >> 20)) {
        def_off = ((uint64_t)r->memsize_mb << 20) - RB_DISC_TMR_OFFSET;
        sig = mm_read32(hw, r, def_off);
        if (sig == RB_DISC_BINARY_SIGNATURE) {
            r->disc_source = RB_SRC_DEFAULT;
            r->disc_offset = def_off;
        }
    }
    if (r->disc_source == RB_SRC_NONE) {
        if (r->scratch[2]) {
            r->disc_source = RB_SRC_SCRATCH;
            r->disc_offset = ((uint64_t)r->scratch[1] << 32) | r->scratch[0];
            r->disc_size = r->scratch[2];
        } else {
            r->disc_source = RB_SRC_DEFAULT_NOSIG;
            r->disc_offset = def_off;
        }
    }
    if (r->disc_size > RB_PROBE_BIN_MAX)
        r->disc_size = RB_PROBE_BIN_MAX;
    r->disc_size &= ~3u;
    /* Linux trusts the scratch pointer; this probe only reads inside the
     * carve-out it was told about. */
    if (r->disc_offset > ((uint64_t)r->memsize_mb << 20) ||
        r->disc_size > ((uint64_t)r->memsize_mb << 20) - r->disc_offset) {
        r->fail = "table location lies outside the carve-out: not read";
        r->disc_size = 0;
    }

    if (r->disc_size)
        mm_read(hw, r, r->disc_offset, r->disc_bin, r->disc_size);

    wreg_index(hw, r, RB_mmMM_INDEX_HI, r->mm_index_hi_saved);
    wreg_index(hw, r, RB_mmMM_INDEX, r->mm_index_saved);

    if (!r->disc_size) {
        if (!r->fail)
            r->fail = "table size is 0";
        return -1;
    }
    bar0_compare(hw, r);

    r->parse_err = rb_disc_parse(r->disc_bin, r->disc_size, &r->disc);
    if (r->parse_err != RB_DISC_OK) {
        r->fail = rb_disc_strerror(r->parse_err);
        return -1;
    }
    sample_regs(hw, r);
    return 0;
}
