/*
 * rb_dio.h - the DIO per-link encoder-type mux, read-only.
 *
 * 2FC. Six registers, DIO_LINK{A..F}_CNTL, one per UNIPHY link. Each selects
 * WHICH KIND OF ENCODER drives that PHY: the ordinary DIG (8b/10b DP and
 * TMDS), HDMI FRL, or DP 128b/132b. Until 2FC this kext had never read them,
 * and `grep -rn DIO_LINK src/` found nothing.
 *
 * WHY IT IS HERE. Three boots (459, 461, 464) programmed the DTO, the raster,
 * the DIG front end and a training pattern on DIG2, read every write back
 * changed, and the sink still reported no clock recovery with
 * `RDPCSTX2_PHY_CNTL6` unmoved at 0x00063333. A mux between DIG2 and PHY2
 * pointed somewhere else would produce exactly that: our writes land, the
 * readbacks agree, and the symbols never reach the lane. This register is
 * where that mux lives, so it gets read before anything else is built.
 *
 * SOURCE, per rule 46 - the method's version grouping, not just the offsets:
 *
 *   docs/linux-ref/dcn31_dio_link_encoder.c:381
 *       `.set_dio_phy_mux = dcn31_link_encoder_set_dio_phy_mux` in
 *       `dcn31_link_enc_funcs`, and dcn31_resource.c builds its link encoders
 *       from that table, so this function IS the one this chip's `dc` reaches.
 *   docs/linux-ref/dcn31_dio_link_encoder.c:145
 *       the body: one `DIO_LINK<x>_CNTL` per `enum transmitter`, writing
 *       ENC_TYPE_SEL always and the two HPO_*_ENC_SEL fields only for FRL and
 *       for 128b/132b.
 *   link_encoder.h:129 (fetched, not vendored - no code follows it)
 *       enum encoder_type_select { ENCODER_TYPE_DIG = 0,
 *                                  ENCODER_TYPE_HDMI_FRL = 1,
 *                                  ENCODER_TYPE_DP_128B132B = 2 };
 *
 * WHAT THIS READ CANNOT DO, SAID BEFORE THE VALUES. `ENCODER_TYPE_DIG` is
 * **zero**, which is also what an unprogrammed register, a gated aperture and
 * a wrong address all read. So an all-zero sweep is NOT a positive control and
 * NOT a confirmation that the mux is on DIG - it is the one reading this block
 * cannot interpret. This is 2zw's shape and `DPALT_DISABLE`'s shape over
 * again, and it is why the sweep verdict below has a DIFFERS state that is the
 * only one carrying information, exactly as `rb_dpalt_active_mask()` is the
 * control for the PHY blocks.
 *
 * Consequently the EXPECTED outcome of this read is "hypothesis dead": our
 * link is HBR2 8b/10b, neither FRL nor 128b/132b, so the value Linux would
 * program for it is 0 and the reset value is 0 too. A non-zero ENC_TYPE_SEL on
 * our link would be the finding; zero closes the line cheaply and that is
 * worth a read on a boot that is happening anyway.
 *
 * Read-only. There is no setter in this file and no caller may add one without
 * first establishing, on hardware, that the live word decodes as this register
 * (rule 47 asked of a write).
 */
#ifndef RB_DIO_H
#define RB_DIO_H

#include <stdint.h>

/* dcn_3_1_2_offset.h:11737-11747. All six are BASE_IDX 2 and consecutive;
 * the stride is 1 dword, which is why each offset is pinned individually in
 * the hosttest rather than derived from the first (the OTG block in 2FA held
 * three different strides in one function). */
#define RB_DIO_COUNT          6u
#define RB_DIO_DW_LINKA       0x1f04u
#define RB_DIO_DW_STRIDE      0x1u
#define RB_DIO_DW_INVALID     0u

/* dcn_3_1_2_sh_mask.h:44278-44283 (LINKA) and :44292-44297 (LINKC): the
 * field layout is identical on every instance. */
#define RB_DIO_ENC_TYPE_SEL_MASK      0x00000003u
#define RB_DIO_ENC_TYPE_SEL_SHIFT     0u
#define RB_DIO_HPO_HDMI_ENC_SEL_MASK  0x00000070u
#define RB_DIO_HPO_HDMI_ENC_SEL_SHIFT 4u
#define RB_DIO_HPO_DP_ENC_SEL_MASK    0x00000700u
#define RB_DIO_HPO_DP_ENC_SEL_SHIFT   8u

/* link_encoder.h:129. The names are upstream's; the values are upstream's. */
#define RB_DIO_ENC_TYPE_DIG          0u
#define RB_DIO_ENC_TYPE_HDMI_FRL     1u
#define RB_DIO_ENC_TYPE_DP_128B132B  2u

static inline uint32_t rb_dio_dw(unsigned i)
{
    if (i >= RB_DIO_COUNT) { return RB_DIO_DW_INVALID; }
    return RB_DIO_DW_LINKA + (uint32_t)i * RB_DIO_DW_STRIDE;
}

static inline uint32_t rb_dio_dw_max(void)   /* 0x1f09 */
{
    return RB_DIO_DW_LINKA + (RB_DIO_COUNT - 1u) * RB_DIO_DW_STRIDE;
}

static inline uint32_t rb_dio_enc_type(uint32_t w)
{
    return (w & RB_DIO_ENC_TYPE_SEL_MASK) >> RB_DIO_ENC_TYPE_SEL_SHIFT;
}

static inline uint32_t rb_dio_hpo_hdmi_sel(uint32_t w)
{
    return (w & RB_DIO_HPO_HDMI_ENC_SEL_MASK) >> RB_DIO_HPO_HDMI_ENC_SEL_SHIFT;
}

static inline uint32_t rb_dio_hpo_dp_sel(uint32_t w)
{
    return (w & RB_DIO_HPO_DP_ENC_SEL_MASK) >> RB_DIO_HPO_DP_ENC_SEL_SHIFT;
}

/* Per-instance verdict. 0 is NOT a passing value - an unwritten field must
 * never read as a result (2C0's defect, caught before it shipped). */
enum rb_dio_sel {
    RB_DIO_SEL_NOT_SAMPLED   = 0,
    /* ENC_TYPE_SEL == 0. The ordinary DIG path - AND the reset value, AND
     * what a wrong address or a gated aperture reads. Ambiguous by
     * construction, and the name says so. */
    RB_DIO_SEL_DIG_OR_UNSET  = 1,
    RB_DIO_SEL_HDMI_FRL      = 2,
    RB_DIO_SEL_DP_128B132B   = 3,
    /* ENC_TYPE_SEL == 3. The field is two bits wide and upstream's enum has
     * three values, so this encoding is not one upstream writes. Reported
     * rather than folded into DIG, because it would mean the word is not this
     * register (rule 14: a refusal is a code). */
    RB_DIO_SEL_RESERVED      = 4
};

static inline enum rb_dio_sel rb_dio_sel(uint32_t w)
{
    switch (rb_dio_enc_type(w)) {
    case RB_DIO_ENC_TYPE_DIG:         return RB_DIO_SEL_DIG_OR_UNSET;
    case RB_DIO_ENC_TYPE_HDMI_FRL:    return RB_DIO_SEL_HDMI_FRL;
    case RB_DIO_ENC_TYPE_DP_128B132B: return RB_DIO_SEL_DP_128B132B;
    default:                          return RB_DIO_SEL_RESERVED;
    }
}

/* Mask of instances whose ENC_TYPE_SEL is NOT the ambiguous zero. Non-zero
 * means at least one link is explicitly muxed to something, which is the only
 * thing that proves these six words are the six registers. */
static inline uint32_t rb_dio_nonzero_mask(const uint32_t *w, unsigned count)
{
    uint32_t m = 0;
    unsigned i;

    if (!w) { return 0; }
    if (count > RB_DIO_COUNT) { count = RB_DIO_COUNT; }
    for (i = 0; i < count; i++) {
        if ((w[i] & (RB_DIO_ENC_TYPE_SEL_MASK |
                     RB_DIO_HPO_HDMI_ENC_SEL_MASK |
                     RB_DIO_HPO_DP_ENC_SEL_MASK)) != 0u) {
            m |= (uint32_t)1u << i;
        }
    }
    return m;
}

enum rb_dio_sweep {
    RB_DIO_SWEEP_NOT_SAMPLED = 0,
    /* At least two instances differ in the three defined fields. The block is
     * live and the address is right: the ONLY verdict here that is a control. */
    RB_DIO_SWEEP_DIFFERS     = 1,
    /* Every instance identical. Says nothing on its own - see the header
     * comment. Not an error and not a confirmation. */
    RB_DIO_SWEEP_UNIFORM     = 2,
    RB_DIO_SWEEP_BAD_ARGS    = 3
};

static inline enum rb_dio_sweep rb_dio_sweep_verdict(const uint32_t *w,
                                                     unsigned count)
{
    unsigned i;
    uint32_t first;
    const uint32_t fields = RB_DIO_ENC_TYPE_SEL_MASK |
                            RB_DIO_HPO_HDMI_ENC_SEL_MASK |
                            RB_DIO_HPO_DP_ENC_SEL_MASK;

    if (!w || count == 0u || count > RB_DIO_COUNT) {
        return RB_DIO_SWEEP_BAD_ARGS;
    }

    first = w[0] & fields;
    for (i = 1; i < count; i++) {
        if ((w[i] & fields) != first) { return RB_DIO_SWEEP_DIFFERS; }
    }
    return RB_DIO_SWEEP_UNIFORM;
}

#endif /* RB_DIO_H */
