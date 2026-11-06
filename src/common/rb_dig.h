/* rb_dig.h - the DMUB DIG1 transmitter control command, for the Type-C port.
 *
 * 2E2. This is the first real bring-up step for the external display, and
 * also the sixth and best instrument for the question 2DB-2DF could not
 * close: does this firmware EXECUTE a DMUB_CMD__VBIOS command at all? It
 * targets a PHY driving nothing, so a wrong guess cannot cost the panel; its
 * product is a register this project has already read on this chip with a
 * measured positive control; and it is a step forward whether or not the
 * answer is yes.
 *
 * 2DC DECLINED THIS COMMAND FOR TWO REASONS AND BOTH ARE NOW DISCHARGED.
 *
 * (1) "Its payload comes in two layouts and init_transmitter_control chooses
 *     between them on the atom BIOS command-table revision read from ROM;
 *     this project cannot read that ROM." The second half was wrong by an
 *     oversight, not by an inference: `firmware/680m-vbios.rom` has been on
 *     disk since 2026-10-05 and compiled into the plugin as `vbios_data[]`
 *     since 2ks. Read it (tools/re/vbios_dispobj.py, --cmdtable) and the
 *     revision question answers itself in a way no experiment could:
 *
 *       cmd_table 0xa480, 81 entries, and only EIGHT are non-zero -
 *       0 asic_init, 6, 13 enabledisppowergating, 19, 22, 41 getsmuclockinfo,
 *       53, 66. Index 76 is `dig1transmittercontrol` and it is **ZERO**.
 *       So are index 4 `digxencodercontrol` and index 12 `setpixelclock`.
 *
 *     There is no atom bytecode for the transmitter on this board at all.
 *     `BIOS_CMD_TABLE_REVISION` therefore returns false with crev left at
 *     **0**, which `init_transmitter_control`'s switch sends to
 *     `transmitter_control_fallback` - and that function is four lines:
 *
 *         if (dmub_srv && dc->debug.dmub_command_table)
 *                 return transmitter_control_v1_7(bp, cntl);
 *         return BP_RESULT_FAILURE;
 *
 *     `dcn31_resource.c:934` sets `.dmub_command_table = true` (2BE read it).
 *     **So the layout is v1_7, decided by the ROM, and the two-arm
 *     experiment 2E0 designed is unnecessary.** The ROM also corroborates
 *     the parse independently: index 13 reads frev 2 crev 1, which is
 *     exactly `enable_disp_power_gating_parameters_v2_1`'s name in
 *     atomfirmware.h, so the command-table walk is checked against a fact it
 *     was not fitted to.
 *
 *     It cuts the other way too, and this is the stronger half: with no atom
 *     table for the transmitter, the encoder or the pixel clock, **the DMUB
 *     VBIOS sub-types are the ONLY route to a display output on this board.**
 *     There is no register-level atom fallback to retreat to.
 *
 * (2) "Its `action` field is ATOM_TRANSMITER_ACTION_xxx and those values
 *     appear in dmub_cmd.h only as the words of a comment." True then, fixed
 *     in 2E0: `atomfirmware.h` is vendored (it is `dmub_cmd.h:43`'s own
 *     include) and carries `enum atom_dig_transmitter_control_action` with
 *     ENABLE = 1, and `bios_parser_types.h` carries the enum the code
 *     actually assigns, which agrees numerically by construction.
 *
 * THE PAYLOAD, byte for byte from `dmub_dig_transmitter_control_data_v1_7`
 * (dmub_cmd.h:3106). Natural alignment, uint32 at 4, total 60 bytes, and the
 * header's payload_bytes is 60 - `sizeof(union) - sizeof(header)`, which
 * both DMUB senders in command_table2.c compute identically:
 *
 *    0  phyid                     0=UNIPHYA .. 5=UNIPHYF
 *    1  action                    ATOM_TRANSMITTER_ACTION_* / bp_ equivalent
 *    2  mode_laneset.digmode      enum atom_encode_mode_def
 *    3  lanenum
 *    4  symclk_units.symclk_10khz  (uint32, little-endian)
 *    8  hpdsel
 *    9  digfe_sel
 *   10  connobj_id
 *   11  HPO_instance
 *   12  TxFFELaneSel
 *   13  skip_phy_ssc_reduction
 *   14  reserved2[2]
 *   16  reserved3[11]
 *
 * AND v1_6 IS THE SAME BYTES. `dig_transmitter_control_parameters_v1_6`
 * (atomfirmware.h:4358) is phyid/action/digmode/lanenum, symclk_10khz at 4,
 * hpdsel/digfe_sel/connobj_id at 8/9/10, then `reserved` at 11 - so for an
 * ENABLE the two layouts differ in **exactly one byte, offset 11**, where
 * v1_7 writes HPO_instance and v1_6 writes zero. Both senders derive
 * payload_bytes from the same union, so even that field matches. 2E0 framed
 * this as a two-arm experiment with a register for a product; it is in fact
 * a one-byte difference, and the ROM says which arm. Recorded because the
 * next reader will otherwise re-derive the arms.
 *
 * WHY HPO_instance IS 244 AND IS NOT A TRANSCRIPTION ERROR.
 * `transmitter_control_v1_7` computes
 *     hpo_instance = (uint8_t)cntl->hpo_engine_id - ENGINE_ID_HPO_DP_0
 * whenever `dc_is_dp_signal(signal)`; `dcn10_link_encoder_enable_dp_output`
 * never assigns `hpo_engine_id` and initialises `cntl` to `{ 0 }`;
 * `ENGINE_ID_DIGA` is 0 and `ENGINE_ID_HPO_DP_0` is 12
 * (grph_object_id.h:177). So the byte is `(uint8_t)(0 - 12)` = **244** on
 * every non-HPO DP enable Linux performs on this generation, and the
 * firmware must ignore the field at digmode DP. It is REPRODUCED rather than
 * normalised to 0: a tidied payload is a different command from the one with
 * field evidence behind it, and if 244 mattered the difference would be
 * invisible in the result. `rb_dig_params.hpo_instance` carries it so the
 * value is a parameter and not a magic number, and `rb_dig_typec_enable()`
 * is the only thing that fills it.
 *
 * THE BOARD'S OWN VALUES, derived in 2E1 from the VBIOS display-object table
 * and corroborated by three independent hardware readings (2B4's HPD
 * instance 2, 2B9's AUX_HPD_SEL 2, 2C1's lit PHY0 being the panel on
 * UNIPHY_A). Every one of the five translations is read from a vendored file:
 *
 *   phyid   2   TRANSMITTER_UNIPHY_C; phy_id_to_atom is the IDENTITY
 *               (command_table_helper.c:297 -> ATOM_PHY_ID_UNIPHY*,
 *               atombios.h:1496 = 0..6) and dcn31_resource.c:419 ties the
 *               index to DPCS_DCN31_REG_LIST(id), so this is also RDPCSTX2
 *   action  1   ATOM_TRANSMITTER_ACTION_ENABLE (atomfirmware.h:4385) ==
 *               TRANSMITTER_CONTROL_ENABLE (bios_parser_types.h:68)
 *   digmode 0   ATOM_TRANSMITTER_DIGMODE_V6_DP (atombios.h:1588) for
 *               SIGNAL_TYPE_DISPLAY_PORT (command_table_helper2_dce112.c:36)
 *   lanenum 2   LANE_COUNT_TWO (dc_dp_types.h), which is 2BB's DPCD reading
 *               from the sink itself and is what DP alt mode leaves for DP
 *   hpdsel  3   HPD instance 2 PLUS ONE. hpd_sel_to_atom maps
 *               HPD_SOURCEID1 (= 0, grph_object_defs.h:45) to byte 1
 *               (command_table_helper2_dce112.c:63,
 *               ATOM_TRANSMITTER_V6_HPD<n>_SEL = n, atomfirmware.h:4417),
 *               and link_factory.c:603 is `hpd_src = pin_id - 1` against the
 *               VBIOS record's pin_id 3. **dmub_cmd.h's "=2: HPD2" names HPD
 *               instance 1 - the OTHER DisplayPort connector on this die.**
 *   symclk  54000  LINK_RATE_HIGH2 0x14 (dc_dp_types.h) x
 *               LINK_RATE_REF_FREQ_IN_KHZ 27000 (link_service_types.h:52)
 *               / 10, per transmitter_control_v1_7's `pixel_clock / 10`
 *
 * digfe_sel is 0 and that is NOT a free field. `dig_encoder_sel_to_atom`
 * has no switch and returns 0 unconditionally, with the reason in its own
 * comment: "On any ASIC after DCE80, we manually program the DIG_FE
 * selection (see connect_dig_be_to_fe function of the link encoder)". So the
 * command does not connect the DIG front end to the back end;
 * `DIG_BE_CNTL.DIG_FE_SOURCE_SELECT` does, and that is a separate write of
 * ours and a separate step. connobj_id is 0 because
 * `dcn10_link_encoder_enable_dp_output` never assigns it either.
 *
 * WHAT THIS FILE DOES NOT DO. It builds a command and decides the refusals.
 * It performs no register write: `enc1_configure_encoder`'s two writes
 * (`DP_CONFIG.DP_UDI_LANES`, `DP_DPHY_SCRAM_CNTL.DPHY_SCRAMBLER_ADVANCE`)
 * are NOT reproduced, deliberately - they are the link encoder's own
 * registers, a block this project has never written, and the experiment is
 * whether the FIRMWARE executes the command. Omitting them means an ENABLE
 * that moves the PHY has moved it without them, and an ENABLE that does
 * nothing has one more candidate explanation than it would otherwise. Said
 * here so the null is not over-read (rule 14): a null is "no effect with the
 * encoder unconfigured", not "the firmware discards VBIOS commands".
 *
 * THE PRODUCT is RDPCSTX2_RDPCSTX_PHY_CNTL6, which rb_dpalt.h already
 * addresses and 2C1 already measured on this chip: PHY0 (the live panel)
 * reads 0x00194444 - four MPLLs on, PSTATE 0 on every lane, ref clock
 * enabled and requested - as the positive control for what an enabled PHY
 * looks like, against PHY2's dark 0x00063333. The verdict is the DIFFERENCE
 * across the send, so no claim about any field's polarity is needed.
 */

#ifndef RB_DIG_H
#define RB_DIG_H

#include <stdint.h>
#include "rb_dmub.h"
#include "rb_dpalt.h"

#define RB_DMUB_CMD_VBIOS_DIG1_TRANSMITTER   1u   /* dmub_cmd.h sub-type */
#define RB_DIG_PAYLOAD                      60u   /* sizeof(union) - header */

/* Payload byte offsets, written out so no struct layout crosses the boundary
 * to firmware and so the hosttest can pin each one. */
#define RB_DIG_OFF_PHYID       0u
#define RB_DIG_OFF_ACTION      1u
#define RB_DIG_OFF_DIGMODE     2u
#define RB_DIG_OFF_LANENUM     3u
#define RB_DIG_OFF_SYMCLK      4u
#define RB_DIG_OFF_HPDSEL      8u
#define RB_DIG_OFF_DIGFE       9u
#define RB_DIG_OFF_CONNOBJ    10u
#define RB_DIG_OFF_HPO        11u

/* atomfirmware.h:4382 enum atom_dig_transmitter_control_action, and
 * bios_parser_types.h:65 enum bp_transmitter_control_action, which agree
 * numerically by construction. Only the two this file will send are named;
 * adding one is a decision, not a convenience. */
#define RB_DIG_ACTION_DISABLE  0u
#define RB_DIG_ACTION_ENABLE   1u

/* atombios.h:1588 */
#define RB_DIG_DIGMODE_DP      0u

/* The panel. 2C1 measured PHY0 as the only lit PHY on this die and 2E1's ROM
 * parse says why: display path 0 is eDP on UNIPHY_A. A transmitter command
 * aimed here could take the display down, so it is refused BY NAME, exactly
 * as rb_pg.h refuses DOMAIN0. */
#define RB_DIG_PHY_PANEL       0u
#define RB_DIG_PHY_MAX         5u   /* UNIPHYA..UNIPHYF per the payload doc */

/* 2E1, from firmware/680m-vbios.rom display path 2. */
#define RB_DIG_TYPEC_PHYID     2u
#define RB_DIG_TYPEC_HPD_INST  2u
#define RB_DIG_TYPEC_HPDSEL    (RB_DIG_TYPEC_HPD_INST + 1u)
#define RB_DIG_TYPEC_LANES     2u
#define RB_DIG_TYPEC_SYMCLK    54000u
#define RB_DIG_HPO_DP_NONE     244u  /* (uint8_t)(0 - ENGINE_ID_HPO_DP_0) */

/* Refusal codes. Distinct values, never a bool: a send that did not happen
 * and a send that happened and did nothing must not read alike (rule 14,
 * and 2zj is what merging them costs). Numbered above rb_pg.h's 20-25 so one
 * published field can never be read against the wrong table. */
#define RB_DIG_OK              0u
#define RB_DIG_REFUSE_DISARMED 30u  /* -rbgpufbdig absent                   */
#define RB_DIG_REFUSE_NOREGS   31u  /* no aperture to read the product in   */
#define RB_DIG_REFUSE_PANEL    32u  /* phyid is the panel's PHY             */
#define RB_DIG_REFUSE_PHY      33u  /* phyid out of range                   */
#define RB_DIG_REFUSE_ACTION   34u  /* action is not one we will send       */
#define RB_DIG_REFUSE_LANES    35u  /* lanenum not 1, 2 or 4                */
#define RB_DIG_REFUSE_HPDSEL   36u  /* hpdsel outside 1..6                  */
#define RB_DIG_REFUSE_DARKREF  37u  /* the control PHY does not read lit    */

struct rb_dig_params {
    uint32_t phyid;
    uint32_t action;
    uint32_t digmode;
    uint32_t lanenum;
    uint32_t symclk_10khz;
    uint32_t hpdsel;
    uint32_t digfe_sel;
    uint32_t connobj_id;
    uint32_t hpo_instance;
};

/* The Type-C port's ENABLE, every field from the derivation above. This is
 * the ONLY producer of a parameter set in this file, so no caller can
 * assemble a half-derived one. */
static inline void rb_dig_typec_enable(struct rb_dig_params *p)
{
    if (!p) { return; }
    p->phyid        = RB_DIG_TYPEC_PHYID;
    p->action       = RB_DIG_ACTION_ENABLE;
    p->digmode      = RB_DIG_DIGMODE_DP;
    p->lanenum      = RB_DIG_TYPEC_LANES;
    p->symclk_10khz = RB_DIG_TYPEC_SYMCLK;
    p->hpdsel       = RB_DIG_TYPEC_HPDSEL;
    p->digfe_sel    = 0u;                      /* always, by the helper */
    p->connobj_id   = 0u;                      /* never assigned by dc  */
    p->hpo_instance = RB_DIG_HPO_DP_NONE;      /* what Linux sends      */
}

/* dc_dp_types.h enum dc_lane_count: 1, 2, 4 (and 8, which DP never uses -
 * LANE_COUNT_DP_MAX is LANE_COUNT_FOUR). */
static inline int rb_dig_lanes_valid(uint32_t n)
{
    return (n == 1u || n == 2u || n == 4u) ? 1 : 0;
}

/* Would this command be sent, and if not, why?
 *
 * `refLit` is whether the CONTROL PHY reads lit - not the target. The whole
 * verdict is a difference between a dark PHY before and after, and 2C1's
 * PHY0 at 0x00194444 is what "lit" looks like on this die. If the control
 * does not read lit then the PHY_CNTL6 block is not answering the way the
 * one measured reading says it does, and a difference read out of it would
 * be uninterpretable - the same reason rb_aux.h refuses when its own
 * reference instance is not enabled, and the reason 2zw's six milestones
 * happened. */
static inline uint32_t rb_dig_refuse(const struct rb_dig_params *p,
                                     int armed, int haveRegs, int refLit)
{
    if (!armed)    { return RB_DIG_REFUSE_DISARMED; }
    if (!haveRegs) { return RB_DIG_REFUSE_NOREGS; }
    if (!p)        { return RB_DIG_REFUSE_PHY; }

    if (p->phyid > RB_DIG_PHY_MAX)      { return RB_DIG_REFUSE_PHY; }
    if (p->phyid >= RB_DPALT_COUNT)     { return RB_DIG_REFUSE_PHY; }
    if (p->phyid == RB_DIG_PHY_PANEL)   { return RB_DIG_REFUSE_PANEL; }

    if (p->action != RB_DIG_ACTION_ENABLE &&
        p->action != RB_DIG_ACTION_DISABLE) { return RB_DIG_REFUSE_ACTION; }

    if (!rb_dig_lanes_valid(p->lanenum)) { return RB_DIG_REFUSE_LANES; }

    /* atomfirmware.h:4417 defines HPD1_SEL..HPD6_SEL as 1..6; 0 is "not
     * assigned", which for an ENABLE of a connector we have measured a sink
     * on would be a command about nothing. */
    if (p->hpdsel == 0u || p->hpdsel > 6u) { return RB_DIG_REFUSE_HPDSEL; }

    if (!refLit) { return RB_DIG_REFUSE_DARKREF; }

    return RB_DIG_OK;
}

/* Build the 64-byte command. Byte by byte, like every other builder in
 * rb_dmub.h, so no compiler's struct packing is trusted across the boundary
 * to firmware. */
static inline void rb_dig_build_transmitter(uint8_t *buf,
                                            const struct rb_dig_params *p)
{
    uint32_t i;

    if (!buf) { return; }
    for (i = 0; i < RB_DMUB_CMD_SIZE; i++) { buf[i] = 0u; }
    if (!p) { return; }

    rb_dmub_put32(buf, 0, rb_dmub_hdr(RB_DMUB_CMD_VBIOS,
                                      RB_DMUB_CMD_VBIOS_DIG1_TRANSMITTER,
                                      RB_DIG_PAYLOAD));

    buf[4u + RB_DIG_OFF_PHYID]   = (uint8_t)p->phyid;
    buf[4u + RB_DIG_OFF_ACTION]  = (uint8_t)p->action;
    buf[4u + RB_DIG_OFF_DIGMODE] = (uint8_t)p->digmode;
    buf[4u + RB_DIG_OFF_LANENUM] = (uint8_t)p->lanenum;
    rb_dmub_put32(buf, 4u + RB_DIG_OFF_SYMCLK, p->symclk_10khz);
    buf[4u + RB_DIG_OFF_HPDSEL]  = (uint8_t)p->hpdsel;
    buf[4u + RB_DIG_OFF_DIGFE]   = (uint8_t)p->digfe_sel;
    buf[4u + RB_DIG_OFF_CONNOBJ] = (uint8_t)p->connobj_id;
    buf[4u + RB_DIG_OFF_HPO]     = (uint8_t)p->hpo_instance;
    /* TxFFELaneSel, skip_phy_ssc_reduction, reserved2 and reserved3 stay
     * zero, which is what both senders' `memset(&cmd, 0, sizeof(cmd))`
     * leaves them. */
}

/* Is this PHY_CNTL6 word a PHY in operation?
 *
 * Deliberately NOT a polarity claim about any single bit. 2C1 measured one
 * lit PHY and four dark ones on this die, and the two groups differ in every
 * one of these fields at once: PHY0 0x00194444 has ref clock enabled and
 * requested with all four MPLLs on and PSTATE 0, while PHY1-4 at 0x000x3333
 * have the clock off, the MPLLs off and PSTATE 3 on every lane. So "lit" is
 * the conjunction, which is what that reading supports; one bit of it would
 * be an inference the reading does not carry. */
static inline int rb_dig_phy_lit(uint32_t w)
{
    unsigned tx;

    if (!rb_dpalt_ref_clk_en(w)) { return 0; }
    for (tx = 0; tx < RB_DPALT_TX_COUNT; tx++) {
        if (!rb_dpalt_tx_mpll_en(w, tx)) { return 0; }
        if (rb_dpalt_tx_pstate(w, tx) != 0u) { return 0; }
    }
    return 1;
}

/* The verdict. `before` and `after` are the target PHY's own PHY_CNTL6.
 *
 * Three outcomes, not two, because they point at three different next steps:
 *  RB_DIG_V_LIT       the PHY came up - the firmware executes VBIOS commands
 *                     and the bring-up path is open.
 *  RB_DIG_V_MOVED     something changed but the PHY is not lit by the
 *                     conjunction above. The firmware acted; the command or
 *                     the missing encoder setup is incomplete. Read the word.
 *  RB_DIG_V_STILL     byte-identical. Either the firmware discards the
 *                     command or the encoder setup this file deliberately
 *                     omits is required first. NOT "the firmware discards
 *                     VBIOS commands" on its own. */
#define RB_DIG_V_STILL  0u
#define RB_DIG_V_MOVED  1u
#define RB_DIG_V_LIT    2u

static inline uint32_t rb_dig_verdict(uint32_t before, uint32_t after)
{
    if (rb_dig_phy_lit(after) && !rb_dig_phy_lit(before)) {
        return RB_DIG_V_LIT;
    }
    return before == after ? RB_DIG_V_STILL : RB_DIG_V_MOVED;
}

#endif /* RB_DIG_H */
