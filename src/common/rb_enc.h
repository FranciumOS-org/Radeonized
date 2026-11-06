/* rb_enc.h - the DIO link encoder's two writes that precede the DIG1
 * transmitter command.
 *
 * 2E4 sent DIG1_TRANSMITTER_CONTROL ENABLE to PHY2 and the firmware CONSUMED
 * it (RPTR advanced over our slot) while PHY2's RDPCSTX_PHY_CNTL6 stayed
 * byte-identical. 2E2 named the live alternative to "the firmware discards
 * VBIOS commands" in advance: `enc1_configure_encoder`'s two writes, which
 * `dcn10_link_encoder_enable_dp_output` performs BEFORE the command and which
 * 2E2 deliberately did not reproduce. This file is those two writes.
 *
 * RULE 46, AND A NEAR MISS WORTH THE COMMENT. 2E1 recorded "on DCN 3.1
 * enabling a DP output IS the VBIOS transmitter command", read off
 * `dcn20_link_encoder_enable_dp_output`. But DCN 3.1 has its OWN method -
 * `dcn31_link_enc_funcs.enable_dp_output = dcn31_link_encoder_enable_dp_output`
 * - and it BRANCHES on `link_enc_cfg_is_transmitter_mappable()`:
 *
 *   not mappable -> dcn20_... -> dcn10_... -> the VBIOS transmitter command
 *   mappable     -> a DIFFERENT command entirely, DPIA_DIG1_CONTROL, with
 *                   enc_id instead of a phyid, `lanenum` = lane_count (NOT
 *                   lane_count - 1), and hpdsel HARDCODED to 6 because the
 *                   HPD pin is "unused by DPIA".
 *
 * The mappable branch is DP tunnelled over USB4 (DPIA), and this port is not
 * that: 2BB read real DPCD over the NATIVE AUX channel, 2B9 found AUX2 bound
 * to HPD2 through `AUX_HPD_SEL`, and 2BE recorded no USB4 router at all
 * (`SPThunderboltDataType`: "No hardware was found"). DP alt mode switches
 * lanes in the Type-C PD controller and the DP comes out of the GPU natively;
 * DPIA needs a router. `DP_IS_USB_C` being set on this path (2E1) is about
 * the connector, NOT about tunnelling - two different facts that one bit is
 * easy to read as one.
 *
 * So 2E1's conclusion survives and its REASON did not exist: the transmitter
 * command is right for this port because the port is not a DPIA, which is a
 * fact that needed establishing rather than assuming. Both branches call
 * `enc1_configure_encoder`, so these two writes are needed either way.
 *
 * THE TWO WRITES ARE NOT THE SAME KIND, which is the thing a transcription
 * gets wrong. From dcn10_link_encoder.c, read not recalled:
 *
 *     REG_SET(DP_CONFIG, 0, DP_UDI_LANES, lane_count - LANE_COUNT_ONE);
 *     REG_UPDATE(DP_DPHY_SCRAM_CNTL, DPHY_SCRAMBLER_ADVANCE, 1);
 *
 * `REG_SET` takes an INIT VALUE of 0: dc's `generic_reg_set_ex` assigns the
 * init value, applies the fields and writes, so every other bit of DP_CONFIG
 * goes to zero. That is only safe because DP_CONFIG has exactly ONE field in
 * dcn_3_1_2_sh_mask.h - `DP_UDI_LANES`, bits 1:0 - so there is no "everything
 * else" to clobber. Checked rather than assumed, because the same pattern on
 * the scrambler register would destroy three fields.
 * `REG_UPDATE` is a read-modify-write, and DP_DPHY_SCRAM_CNTL HAS four fields
 * (SCRAMBLER_DIS, ADVANCE, BS_COUNT, KCODE), so preserving the live word is
 * required and not a courtesy.
 *
 * `LANE_COUNT_ONE` is 1 (dc_dp_types.h:34), so two lanes write 1.
 *
 * THE INSTANCE IS THE PHYID, established from the table and not inferred.
 * `dcn31_link_encoder_create` passes `&link_enc_regs[enc_init_data->transmitter]`,
 * and `link_enc_regs[]` is built by `link_regs(id, phyid)` as
 * `[0]=(0,A) [1]=(1,B) [2]=(2,C) [3]=(3,D) [4]=(4,E)`, whose body is
 * `LE_DCN31_REG_LIST(id)` + `UNIPHY_DCN2_REG_LIST(phyid)` +
 * `DPCS_DCN31_REG_LIST(id)`. One index serves the DIO block, the UNIPHY and
 * the DPCS block, so link encoder 2 IS DP2_* IS RDPCSTX2 IS UNIPHY_C - which
 * is exactly 2E1's Type-C transmitter, reached four independent ways.
 *
 * Instance 0 is the panel's encoder and is REFUSED BY NAME, as rb_dig.h
 * refuses PHY0 and rb_pg.h refuses DOMAIN0.
 *
 * Offsets from docs/linux-ref/dcn_3_1_2_offset.h, both BASE_IDX 2 - the same
 * base HUBP, OTG, HPD, AUX and the PG domains already go through, so no new
 * mapping. The highest dword is DP4's 0x251e, which sits BELOW
 * rb_dpalt_dw_max()'s 0x2ca6, so the aperture 2C0 already checks separately
 * covers this too; the hosttest pins that relation so a change to either one
 * cannot silently put a write outside the mapping (2DD is what a readable
 * wrong address costs).
 */
#ifndef RB_ENC_H
#define RB_ENC_H

#include <stdint.h>

/* regDP0_DP_CONFIG 0x210b, regDP1_ 0x220b, ... - stride 0x100, 5 instances. */
#define RB_ENC_DW_DP_CONFIG_0   0x210bu
#define RB_ENC_DW_SCRAM_0       0x211eu
#define RB_ENC_DW_STRIDE        0x100u
#define RB_ENC_COUNT            5u

/* The panel's own link encoder. Never a target. */
#define RB_ENC_PANEL            0u

/* DP_CONFIG: one field, bits 1:0. */
#define RB_ENC_UDI_LANES_MASK   0x00000003u
/* DP_DPHY_SCRAM_CNTL: DPHY_SCRAMBLER_ADVANCE, bit 4 of four fields. */
#define RB_ENC_SCRAM_ADVANCE    0x00000010u

/* Refusals, numbered clear of rb_pg.h's 20-25 and rb_dig.h's 30-37 so a code
 * read against the wrong table cannot decode plausibly (rule 14). */
#define RB_ENC_OK               0u
#define RB_ENC_REFUSE_DISARMED  40u  /* -rbgpufbdig absent                   */
#define RB_ENC_REFUSE_NOREGS    41u  /* no writable aperture                 */
#define RB_ENC_REFUSE_PANEL     42u  /* instance 0 is the panel's encoder    */
#define RB_ENC_REFUSE_INST      43u  /* instance outside 0..4                */
#define RB_ENC_REFUSE_LANES     44u  /* lane count not 1, 2 or 4             */
#define RB_ENC_REFUSE_DENIED    45u  /* dword on the dcn302/dcn312 deny list */

static inline uint32_t rb_enc_dw_config(uint32_t i)
{
    return RB_ENC_DW_DP_CONFIG_0 + i * RB_ENC_DW_STRIDE;
}

static inline uint32_t rb_enc_dw_scram(uint32_t i)
{
    return RB_ENC_DW_SCRAM_0 + i * RB_ENC_DW_STRIDE;
}

/* The highest dword this header can ask for: DP4's scrambler at 0x251e. */
static inline uint32_t rb_enc_dw_max(void)
{
    return rb_enc_dw_scram(RB_ENC_COUNT - 1u);
}

/* REG_SET with an init value of 0 - the WHOLE word, not an RMW. Safe only
 * because DP_CONFIG has no other field; see the header comment. */
static inline uint32_t rb_enc_dp_config_word(uint32_t lanes)
{
    return (lanes - 1u) & RB_ENC_UDI_LANES_MASK;
}

/* REG_UPDATE - read-modify-write, preserving BS_COUNT and KCODE. */
static inline uint32_t rb_enc_scram_word(uint32_t live)
{
    return live | RB_ENC_SCRAM_ADVANCE;
}

static inline int rb_enc_lanes_ok(uint32_t lanes)
{
    return lanes == 1u || lanes == 2u || lanes == 4u;
}

static inline uint32_t rb_enc_refuse(uint32_t inst, uint32_t lanes,
                                     int armed, int have_regs, int denied)
{
    if (!armed)                  { return RB_ENC_REFUSE_DISARMED; }
    if (!have_regs)              { return RB_ENC_REFUSE_NOREGS; }
    if (inst == RB_ENC_PANEL)    { return RB_ENC_REFUSE_PANEL; }
    if (inst >= RB_ENC_COUNT)    { return RB_ENC_REFUSE_INST; }
    if (!rb_enc_lanes_ok(lanes)) { return RB_ENC_REFUSE_LANES; }
    if (denied)                  { return RB_ENC_REFUSE_DENIED; }
    return RB_ENC_OK;
}

/* The READBACK decides whether the configure took, never the store (rules
 * 13/20). Both halves are required: the lane field has to read the value
 * asked for and the advance bit has to be set. */
static inline int rb_enc_applied(uint32_t cfg_back, uint32_t scram_back,
                                 uint32_t lanes)
{
    if ((cfg_back & RB_ENC_UDI_LANES_MASK) != rb_enc_dp_config_word(lanes)) {
        return 0;
    }
    return (scram_back & RB_ENC_SCRAM_ADVANCE) != 0u;
}

/* 2E6: `rb_enc_applied()` ALONE HAS NO CONTROL, and boot 444 is why. Link
 * encoder 2 read DP_CONFIG 0x00000001 and SCRAM_CNTL 0x0101FF10 BEFORE our
 * write - already exactly what enc1_configure_encoder(2 lanes) would store -
 * so both writes were no-ops and `applied` read 1 whether the store landed,
 * was dropped, or was never issued. A readback that agrees with the target is
 * not evidence of a write when the target was already present (rule 20's
 * shape). This is the quantity a reader actually wants beside it. */
static inline int rb_enc_changed(uint32_t cfg_before, uint32_t cfg_after,
                                 uint32_t scram_before, uint32_t scram_after)
{
    return (cfg_before != cfg_after) || (scram_before != scram_after);
}

/* The survey, 2E6's open question: 0x1 / 0x0101FF10 on instance 2 is either
 * the block's reset state or the FIRMWARE's own setup of that connector, and
 * one instance cannot say (rule 18). The discriminator is the other four. If
 * instances 0 and 2 are configured and 1, 3 and 4 are not, that is the
 * firmware having brought up exactly the pair 2B4's HPD sink mask 0x05 and
 * 2B9's AUX_EN mask 0x05 independently name - and the 2 lanes agree with what
 * 2BB read from the sink's own DPCD. All five alike is a reset default and
 * says nothing.
 *
 * "Configured" is the CONJUNCTION of the two fields enc1_configure_encoder
 * writes. 2E7 MEASURED which term does the work, and it is not the one this
 * comment used to name: DP_DPHY_SCRAM_CNTL reads 0x0101FF10 on ALL FIVE
 * instances on boot 446, the three never brought up included, so
 * DPHY_SCRAMBLER_ADVANCE is a RESET DEFAULT carrying no information - the
 * LANE COUNT is the entire discriminator here (DP0 3 = 4 lanes, DP2 1 = 2
 * lanes, DP1/3/4 0). The term stays, because another part may reset
 * differently; what is recorded is that a conjunction can be correct while
 * one term is inert, and a passing test says nothing about which term
 * discriminated. The original reason - DP_UDI_LANES is 0 for a ONE-lane link
 * and so indistinguishable from untouched - is still true and is why the lane
 * term cannot stand alone either. */
static inline int rb_enc_looks_configured(uint32_t cfg, uint32_t scram)
{
    /* An all-ones word is an unmapped or powered-down block, not a
     * configuration - and it would otherwise satisfy both field tests and
     * report every instance configured. 2DD's shape: reject an implausible
     * sample before decoding it, rather than decoding it plausibly. */
    if (cfg == 0xFFFFFFFFu || scram == 0xFFFFFFFFu) { return 0; }
    if ((scram & RB_ENC_SCRAM_ADVANCE) == 0u)       { return 0; }
    return (cfg & RB_ENC_UDI_LANES_MASK) != 0u;
}

/* A bitmask over the five instances, so one key carries the whole survey and
 * the comparison against 0x05 is a single read. */
static inline uint32_t rb_enc_configured_mask(const uint32_t *cfg,
                                              const uint32_t *scram)
{
    uint32_t m = 0u;
    uint32_t i;

    if (!cfg || !scram) { return 0u; }
    for (i = 0u; i < RB_ENC_COUNT; i++) {
        if (rb_enc_looks_configured(cfg[i], scram[i])) { m |= (1u << i); }
    }
    return m;
}

#endif /* RB_ENC_H */
