/* rb_digbe.h - DIG_BE_CNTL, read-only: is the DIG front end connected to the
 * back end on the Type-C link encoder?
 *
 * 2E1 recorded `dig_encoder_sel_to_atom()` returning 0 UNCONDITIONALLY as a
 * MISSING STEP and not a free field, naming
 * `DIG_BE_CNTL.DIG_FE_SOURCE_SELECT` (per
 * `dcn10_link_encoder_connect_dig_be_to_fe`) as a write of ours. 2E4 and 2E6
 * then both measured a STILL verdict with that step unreproduced, so it is
 * one of exactly two remaining candidates (the other is the pixel clock,
 * VBIOS sub-type 2).
 *
 * 2E7 found it costs a READ to ask rather than a write to try: the firmware
 * configured the Type-C link encoder for the sink's own two lanes (DP2
 * `DP_UDI_LANES` 1, against 0 on the three connectors never brought up), so
 * it may well have made this connection too - and if it did, the candidate
 * collapses without a single write.
 *
 * OFFSETS, from `docs/linux-ref/dcn_3_1_2_offset.h` (not recalled):
 *   regDIG0_DIG_BE_CNTL 0x20b0, DIG1 0x21b0, DIG2 0x22b0, DIG3 0x23b0,
 *   DIG4 0x24b0 - stride 0x100, the SAME as the DP_CONFIG block, BASE_IDX 2.
 * The max dword is 0x24b0, BELOW rb_enc_dw_max()'s 0x251e, so this is inside
 * an aperture already mapped: no new mapping and NO WRITE SURFACE. There is
 * deliberately no setter in this header - 2E1's write, if it is still wanted
 * after the reading, is its own decision.
 *
 * THE FIELD READS THE ANSWER DIRECTLY, which is why this is worth a read:
 * `DIG_FE_SOURCE_SELECT` (bits 14:8, mask 0x7F00) is a **ONE-HOT MASK, NOT AN
 * INDEX** - `dcn10_link_encoder.c:54-58` gives INVALID **0x0**, DIGA 0x1,
 * DIGB 0x2, DIGC 0x4, DIGD 0x8. So zero means UNCONNECTED and any single bit
 * names which front end. Reading it as an index would make 0x4 "front end 4"
 * instead of DIGC, which is the trap 2E1 hit in the other direction with
 * hpdsel.
 *
 * `DIG_MODE` (bits 18:16, mask 0x70000) from `dcn10_link_encoder_setup`'s own
 * switch: 0 DP/eDP SST, 1 LVDS, 2 TMDS-DVI, 3 TMDS-HDMI, 5 DP MST. DP = 0
 * agrees with 2E1's `signal_type_to_atom_dig_mode`.
 */
#ifndef RB_DIGBE_H
#define RB_DIGBE_H

#include <stdint.h>

#define RB_DIGBE_DW_0            0x20b0u
#define RB_DIGBE_DW_STRIDE       0x100u
#define RB_DIGBE_COUNT           5u

#define RB_DIGBE_FE_SEL_MASK     0x00007F00u
#define RB_DIGBE_FE_SEL_SHIFT    8u
#define RB_DIGBE_MODE_MASK       0x00070000u
#define RB_DIGBE_MODE_SHIFT      16u

/* dcn10_link_encoder.c:54-58 - a one-hot mask, 0 = INVALID. */
#define RB_DIGBE_FE_INVALID      0x0u
#define RB_DIGBE_FE_DIGA         0x1u
#define RB_DIGBE_FE_DIGB         0x2u
#define RB_DIGBE_FE_DIGC         0x4u
#define RB_DIGBE_FE_DIGD         0x8u

/* dcn10_link_encoder_setup()'s switch. */
#define RB_DIGBE_MODE_DP_SST     0u
#define RB_DIGBE_MODE_LVDS       1u
#define RB_DIGBE_MODE_TMDS_DVI   2u
#define RB_DIGBE_MODE_TMDS_HDMI  3u
#define RB_DIGBE_MODE_DP_MST     5u

static inline uint32_t rb_digbe_dw(uint32_t i)
{ return RB_DIGBE_DW_0 + i * RB_DIGBE_DW_STRIDE; }

static inline uint32_t rb_digbe_dw_max(void)
{ return rb_digbe_dw(RB_DIGBE_COUNT - 1u); }

static inline uint32_t rb_digbe_fe_sel(uint32_t w)
{ return (w & RB_DIGBE_FE_SEL_MASK) >> RB_DIGBE_FE_SEL_SHIFT; }

static inline uint32_t rb_digbe_mode(uint32_t w)
{ return (w & RB_DIGBE_MODE_MASK) >> RB_DIGBE_MODE_SHIFT; }

/* NO all-ones plausibility guard here, and the absence is the finding.
 *
 * 2E7 wrote one (rejecting 0xFFFFFFFF) by analogy with rb_enc, where such a
 * guard is load-bearing: an all-ones word satisfies both of rb_enc's field
 * tests, so without it absence decoded as all five encoders configured. Here
 * TWO break-it checks fired NOTHING - removing it changed no assertion,
 * because an all-ones word gives FE_SEL 0x7f (not one-hot) and DIG_MODE 7
 * (not a mode Linux writes), so both specific-value tests already reject it.
 * It was dead in both call sites.
 *
 * DELETED rather than kept with a comment, because a guard no break can make
 * matter is inert, and "redundant" and "inert" differ (rule 6). Worse, the
 * comment justifying it claimed the mode path was not subsumed - which the
 * second break refuted at once - and the test asserting it PASSED while its
 * stated reason was wrong: 2BC's "a case that passes for the wrong reason is
 * not coverage", caught by a break rather than by rereading.
 *
 * The all-ones assertions stay, as coverage of the tests that DO reject it.
 * General: the same defensive check is load-bearing in one decoder and dead
 * in another, so run a break per guard and treat one that fires nothing as a
 * result rather than a formality. */

/* Connected means a NON-ZERO ONE-HOT selection. A value with more than one
 * bit set names no front end, so it is refused rather than decoded to its
 * lowest bit - reading a one-hot mask as an index is 2E1's hpdsel trap with
 * the arrow reversed. This test is also what makes a separate all-ones guard
 * unnecessary (see above): FE_SEL 0x7f has seven bits set. */
static inline int rb_digbe_fe_one_hot(uint32_t sel)
{ return sel != 0u && (sel & (sel - 1u)) == 0u; }

/* The only values dcn10_link_encoder_setup() writes are 0, 1, 2, 3 and 5 -
 * 4, 6 and 7 are not modes, so a word decoding to one of those is not a
 * reading of this field. 7 is what an unmapped block gives (rule 14: a wrong
 * address that is READABLE answers plausibly). */
static inline int rb_digbe_mode_valid(uint32_t w)
{
    uint32_t m;

    m = rb_digbe_mode(w);
    return m == RB_DIGBE_MODE_DP_SST || m == RB_DIGBE_MODE_LVDS
        || m == RB_DIGBE_MODE_TMDS_DVI || m == RB_DIGBE_MODE_TMDS_HDMI
        || m == RB_DIGBE_MODE_DP_MST;
}

static inline int rb_digbe_connected(uint32_t w)
{
    return rb_digbe_fe_one_hot(rb_digbe_fe_sel(w));
}

/* A bitmask over the five instances, so one key carries the survey and the
 * comparison against 2E7's 0x05 is a single read. */
static inline uint32_t rb_digbe_connected_mask(const uint32_t *w)
{
    uint32_t m = 0u;
    uint32_t i;

    if (!w) { return 0u; }
    for (i = 0u; i < RB_DIGBE_COUNT; i++) {
        if (rb_digbe_connected(w[i])) { m |= (1u << i); }
    }
    return m;
}

#endif /* RB_DIGBE_H */
