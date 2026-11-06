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
 *
 * 2E8 TOOK THE READING (boot 447): FE_SEL is 0x1 DIGA on the panel's DIG0 and
 * **0 on DIG2**, so the connection is genuinely absent and 2E1's missing step
 * is real. And Linux says that is BY DESIGN rather than an omission -
 * `command_table_helper2_dce112.c:94` on `dig_encoder_sel_to_atom`: "On any
 * ASIC after DCE80, we manually program the DIG_FE selection (see
 * connect_dig_be_to_fe function of the link encoder), so translation should
 * always return 0 (no FE)." The VBIOS transmitter command carries no front
 * end on purpose and the FE connection is the DRIVER's write, so no firmware
 * bring-up could ever have made it. 2E7's hope that the read might collapse
 * the candidate was worth testing and the answer is no.
 *
 * TWO FIELDS THIS DECODER ORIGINALLY SKIPPED CARRIED THE REST OF THE ANSWER,
 * which is why the raw words are published beside the summary:
 *
 * `DIG_HPD_SELECT` (bits 30:28, mask 0x70000000) reads **2 on DIG2** and 0 on
 * the other four. `dcn10_link_encoder.c:563` writes it as `hpd_source`, an
 * `enum hpd_source_id` where `HPD_SOURCEID1 = 0`, so the field is the
 * **0-based DCN HPD instance** - NOT the atom hpdsel, which 2E1 found is that
 * instance plus one. (`dcn31_dio_link_encoder.c:601` agrees from the other
 * side: DPIA's atom hpdsel 6 is "DIG_HPD_SELECT set to 5 (hpdsel - 1)".) So
 * the firmware associated HPD instance **2** with this back end - a FOURTH
 * independent instrument naming the Type-C port, after 2B4's cable-following
 * sink bit, 2B9's `AUX_HPD_SEL` and 2E7's encoder mask.
 * **Its zero is AMBIGUOUS and there is no derived bit for it**: HPD instance
 * 0 is the panel, so DIG0's 0 is a correct association and DIG1/3/4's 0 is an
 * unwritten field, and nothing in the word tells them apart (rule 14). DIG2's
 * 2 is readable as the firmware's work only because three other instruments
 * already name that instance. `FE_SEL` is the one field here whose zero is
 * unambiguous, because 0 is a documented INVALID enum value rather than a
 * default.
 *
 * `DIG_BE_EN_CNTL` (0x20b1, same 0x100 stride) carries `DIG_ENABLE` bit 0 and
 * `DIG_SYMCLK_BE_ON` bit 8. **The driver only ever READS it** -
 * `dcn10_is_dig_enabled()` at `dcn10_link_encoder.c:540` is its single
 * reference in the whole of `docs/linux-ref/`, with no writer - so whatever
 * sets it is the firmware's own transmitter path. That makes it a SECOND
 * PRODUCT of the `DIG1_TRANSMITTER_CONTROL` command 2E4 sent, and 2E4 judged
 * that command on `RDPCSTX2_PHY_CNTL6` alone: Linux's own test for "is this
 * transmitter up" was never sampled. Read-only here too; the max dword moves
 * to 0x24b1, still below rb_enc_dw_max()'s 0x251e, so still no new mapping
 * and still no write surface.
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
#define RB_DIGBE_HPD_SEL_MASK    0x70000000u
#define RB_DIGBE_HPD_SEL_SHIFT   28u

/* DIG_BE_EN_CNTL, the same five instances at the same stride. */
#define RB_DIGBE_EN_DW_0         0x20b1u
#define RB_DIGBE_EN_DIG_ENABLE   0x00000001u
#define RB_DIGBE_EN_SYMCLK_ON    0x00000100u

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

static inline uint32_t rb_digbe_en_dw(uint32_t i)
{ return RB_DIGBE_EN_DW_0 + i * RB_DIGBE_DW_STRIDE; }

/* The bound both blocks must fit under. EN_CNTL sits one dword above
 * DIG_BE_CNTL on every instance, so it is the EN block that sets the max -
 * computed from the two rather than written down, since 2DD is what reading a
 * plausible wrong address costs. */
static inline uint32_t rb_digbe_dw_max(void)
{
    uint32_t a = rb_digbe_dw(RB_DIGBE_COUNT - 1u);
    uint32_t b = rb_digbe_en_dw(RB_DIGBE_COUNT - 1u);

    return a > b ? a : b;
}

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

/* The 0-based DCN HPD instance (dcn10_link_encoder.c:563 writes hpd_source,
 * and HPD_SOURCEID1 = 0), NOT 2E1's atom hpdsel, which is this plus one.
 * There is deliberately no rb_digbe_hpd_valid() or rb_digbe_touched(): 0 is
 * both "HPD instance 0" and "never written" and the word cannot separate
 * them, so the caller gets the field and the ambiguity is stated rather than
 * decided by an invented predicate. */
static inline uint32_t rb_digbe_hpd_sel(uint32_t w)
{ return (w & RB_DIGBE_HPD_SEL_MASK) >> RB_DIGBE_HPD_SEL_SHIFT; }

static inline int rb_digbe_en_enabled(uint32_t w)
{ return (w & RB_DIGBE_EN_DIG_ENABLE) != 0u; }

static inline int rb_digbe_en_symclk_on(uint32_t w)
{ return (w & RB_DIGBE_EN_SYMCLK_ON) != 0u; }

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

/* The same shape over DIG_BE_EN_CNTL. An all-ones word would read as enabled
 * here - this register has no specific-value test to fall back on, unlike
 * FE_SEL's one-hot and DIG_MODE's enum - so the guard 2E7 found DEAD in the
 * DIG_BE_CNTL decoder is LOAD-BEARING in this one, which is the same split
 * rb_enc and rb_digbe already showed. Its break-it check fires. */
static inline int rb_digbe_en_word_plausible(uint32_t w)
{ return w != 0xFFFFFFFFu; }

static inline uint32_t rb_digbe_enabled_mask(const uint32_t *w)
{
    uint32_t m = 0u;
    uint32_t i;

    if (!w) { return 0u; }
    for (i = 0u; i < RB_DIGBE_COUNT; i++) {
        if (rb_digbe_en_word_plausible(w[i]) && rb_digbe_en_enabled(w[i])) {
            m |= (1u << i);
        }
    }
    return m;
}

/* Which instances' words moved across a send. Used on BOTH blocks, so the
 * transmitter command's effect on DIG_BE_CNTL and on DIG_BE_EN_CNTL are
 * separate readings rather than one merged bool - 2E6 is what conflating two
 * no-ops with different causes costs. A refusal cannot reach this: the
 * resample only runs when the send did. */
static inline uint32_t rb_digbe_diff_mask(const uint32_t *a, const uint32_t *b)
{
    uint32_t m = 0u;
    uint32_t i;

    if (!a || !b) { return 0u; }
    for (i = 0u; i < RB_DIGBE_COUNT; i++) {
        if (a[i] != b[i]) { m |= (1u << i); }
    }
    return m;
}

#endif /* RB_DIGBE_H */
