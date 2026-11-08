/* rb_phyfsm.h - DMUB_CMD__VBIOS_TRANSMITTER_SET_PHY_FSM (sub-type 29).
 *
 * BUILD-ONLY. This header composes the payload and decodes a reply. It sends
 * nothing: there is no inbox push here, and the dmubbl verb that uses it
 * prints the bytes and stops. Sending command 29 is a WRITE of a new command
 * to the firmware and is the user's decision (the card's rule for SMU message
 * groups, same spirit), so the decision point is kept out of this file
 * entirely rather than guarded inside it.
 *
 * WHY THIS COMMAND, AFTER THREE MILESTONES OF REGISTER SURVEYS.
 * 307 measured PHY2 at PHY_RESET 1 with no TX_CLOCK_ON and no TX_FIFO_EN, and
 * the live panel at PHY_RESET 0 with both clocks and the FIFO. 308 spent a
 * milestone on the SRAM-load handshake. 309/309b read nine registers and could
 * not tell PHY2 from PHY3 at all. Then dmub_cmd.h's own enum:
 *
 *     DMUB_PHY_FSM_POWER_UP_DEFAULT = 0
 *     DMUB_PHY_FSM_RESET              1   <- PHY2's measured state
 *     DMUB_PHY_FSM_RESET_RELEASED     2   <- the minimal ask
 *     DMUB_PHY_FSM_SRAM_LOAD_DONE     3   <- 308's whole milestone, as a STATE
 *     DMUB_PHY_FSM_INITIALIZED        4
 *     ...
 *     DMUB_PHY_FSM_TX_EN             10   <- the live panel's state
 *
 * Those three readings are this state machine's OUTPUT. The machine has a
 * command, and nobody has ever sent it - not us, not upstream. A grep for
 * SET_PHY_FSM / set_phy_fsm / phy_fsm / PHY_FSM over docs/linux-ref/, src/ and
 * tools/ finds the enum, the two structs, the union member (immediately after
 * query_dp_alt's) and three `uint8_t use_phy_fsm` flags inside PSR
 * copy_settings - and no code that builds command 29. Upstream only asks the
 * firmware to use the FSM internally during PSR power up/down. That is
 * consistent with 307's finding that DCN31 NULLs every dpcs_* bringup
 * function: the FSM is the firmware's, and this is the one command into it.
 *
 * THE STATUS FIELD IS THE POINT, not the picture.
 * 307 closed on a ceiling: transmitter_control_v1_7 calls
 * transmitter_control_dmcub (a void) and returns BP_RESULT_OK unconditionally,
 * sent DM_DMUB_WAIT_TYPE_WAIT, so `ran` is a mailbox ack and Linux cannot tell
 * whether the table executed either. Command 29's payload is documented "Data
 * passed in/out" and carries a uint32_t status. That is the execution evidence
 * command 1 structurally cannot give. The reply path itself is already proven
 * on this machine: query_dp_alt (26) sits one union member earlier, goes out
 * on the same ring, uses WAIT_TYPE_WAIT_WITH_REPLY, and has returned a
 * ROM-matching answer on five boots.
 *
 * THE UNITS ARE A FACTOR OF 100, AND THE FIELD NAMES ARE NOT THE SAME.
 * A first draft of 310's journal entry said the two commands "share a field
 * name" and warned of a factor of ten. Both halves were wrong, and the source
 * settles it:
 *     atom v1.7 : symclk_units.symclk_10khz   - 10 kHz units, 54000 = 540 MHz
 *     command 29: symclk_100Hz                - 100 Hz units, 5400000
 * So the conversion is x100 and it lives in rb_phyfsm_symclk_from_10khz()
 * where the hosttest can pin it, not in a comment where the first draft of
 * this one got it wrong twice.
 *
 * `mode` IS ZERO AND ZERO IS NOT A VALUE (rule 32's shape).
 * ATOM_ENCODER_MODE_DP = 0 (atomfirmware.h:95) and
 * ATOM_TRANSMITTER_DIGMODE_V6_DP = 0 (atombios.h:1588), so DP is 0 in both
 * encodings and the field we want is indistinguishable from a field we forgot
 * to write. rb_phyfsm_build() therefore takes mode explicitly - there is no
 * default - and the builder's caller must name it.
 *
 * TWO BREAKS THAT THE TEST DID NOT CATCH, recorded rather than papered over
 * (rule 48: a break that fires nothing is a RESULT).
 *   Q. Turning the name table's `st >= count` bound into `st >` is caught by
 *      **-Werror**, not by the test: the compiler sees the out-of-bounds read
 *      first. That guard is held by the build, so the test is NOT its
 *      coverage. The -Werror trap for the seventh time, this time benign.
 *   V. Writing `state` as a single byte instead of rb_dmub_put32 fires
 *      nothing, and nothing is wrong. The slot is zeroed first and the
 *      builder refuses every state >= RB_PHYFSM_STATE_COUNT (17), so no
 *      legal value exceeds 255 and the two writes are byte-identical on the
 *      wire. The distinction carries no information in the builder. It is
 *      real in the DECODER, which reads 32 bits, and that asymmetry is why
 *      the 32-bit write stays: the field is an int in the struct.
 *
 * WHAT IS NOT ESTABLISHED, and must be said wherever this is reported:
 * that DMCUB 4.0.15.47 implements sub-type 29 at all (our image is one build,
 * the header is upstream's); that the firmware acts on it for a DP-alt port;
 * and that RESET -> RESET_RELEASED is a legal direct transition rather than
 * one step of a ladder. The status field is what would answer the first two.
 */
#ifndef RB_PHYFSM_H
#define RB_PHYFSM_H

#include <stdint.h>
#include "rb_dmub.h"
#include "rb_dpcsphy.h"
#include "rb_dpcsphy.h"

#define RB_DMUB_CMD_VBIOS_SET_PHY_FSM   29u

/* struct dmub_rb_cmd_transmitter_set_phy_fsm_data, laid out by the C rules
 * Linux itself compiles it with (alignment 4, so two implicit pads and a tail
 * pad). Linux sizes payload_bytes off sizeof(data), as it does for
 * query_dp_alt (4) and NOT off the union - the same deliberate reproduction
 * rb_dpaltq.h records.
 *
 *   0  phy_id        uint8
 *   1  mode          uint8
 *   2  lane_num      uint8
 *   3  (pad)
 *   4  symclk_100Hz  uint32  little-endian
 *   8  test_mode.mode  uint8
 *   9  test_mode.pat0  uint8
 *  10  test_mode.pad[2]
 *  12  state         enum -> int32 little-endian
 *  16  status        uint32  little-endian   <- OUT
 *  20  pad           uint8
 *  21  (tail pad to 24)
 */
#define RB_PHYFSM_PAYLOAD            24u

#define RB_PHYFSM_OFF_PHYID           0u
#define RB_PHYFSM_OFF_MODE            1u
#define RB_PHYFSM_OFF_LANENUM         2u
#define RB_PHYFSM_OFF_SYMCLK          4u
#define RB_PHYFSM_OFF_TM_MODE         8u
#define RB_PHYFSM_OFF_TM_PAT0         9u
#define RB_PHYFSM_OFF_STATE          12u
#define RB_PHYFSM_OFF_STATUS         16u
#define RB_PHYFSM_OFF_PAD            20u

#define RB_PHYFSM_PHY_COUNT           5u
#define RB_PHYFSM_PANEL_INST          0u   /* the live panel - refused by name */
#define RB_PHYFSM_TYPEC_INST          2u   /* UNIPHY_C, our port              */

/* The OUT field's sentinel, rb_dpaltq.h's move: status is an output, so a
 * firmware that never wrote it leaves whatever we put there, and a prefill of
 * 0 is indistinguishable from a genuine status of 0 (rule 14 at the firmware
 * boundary). 0xFFFFFFFF is not a plausible status word. It is a DEVIATION
 * from a Linux-exact zeroed command, so it is selectable and testable. */
#define RB_PHYFSM_STATUS_SENTINEL    0xFFFFFFFFu
#define RB_PHYFSM_STATUS_LINUX       0x00000000u

/* The units conversion, in code so the hosttest pins it. */
#define RB_PHYFSM_SYMCLK_PER_10KHZ   100u
/* HBR2, derived not pasted: rb_dig.h's RB_DIG_TYPEC_SYMCLK is 54000 in
 * 10 kHz units. */
#define RB_PHYFSM_TYPEC_SYMCLK_10KHZ 54000u
/* The MSI MP2412C's lane count. Named rather than literal so the builder's
 * caller and the hosttest cannot drift apart. */
#define RB_PHYFSM_TYPEC_LANES        2u

enum rb_phyfsm_state {
    RB_PHYFSM_POWER_UP_DEFAULT = 0,
    RB_PHYFSM_RESET            = 1,
    RB_PHYFSM_RESET_RELEASED   = 2,
    RB_PHYFSM_SRAM_LOAD_DONE   = 3,
    RB_PHYFSM_INITIALIZED      = 4,
    RB_PHYFSM_CALIBRATED       = 5,
    RB_PHYFSM_CALIBRATED_LP    = 6,
    RB_PHYFSM_CALIBRATED_PG    = 7,
    RB_PHYFSM_POWER_DOWN       = 8,
    RB_PHYFSM_PLL_EN           = 9,
    RB_PHYFSM_TX_EN            = 10,
    RB_PHYFSM_TX_EN_TEST_MODE  = 11,
    RB_PHYFSM_FAST_LP          = 12,
    RB_PHYFSM_P2_PLL_OFF_CPM   = 13,
    RB_PHYFSM_P2_PLL_OFF_PG    = 14,
    RB_PHYFSM_P2_PLL_OFF       = 15,
    RB_PHYFSM_P2_PLL_ON        = 16,
    RB_PHYFSM_STATE_COUNT      = 17
};

/* Refusals, numbered clear of rb_pg's 20-25, rb_dig's 30-37, rb_dpaltq's
 * 50-55 and rb_dptx's 66, so a code never leaves which surface produced it in
 * doubt (rule 14). */
enum rb_phyfsm_ret {
    RB_PHYFSM_OK             = 0,
    RB_PHYFSM_E_NULL         = 70,
    RB_PHYFSM_E_BAD_PHY      = 71,  /* outside 0..4                          */
    RB_PHYFSM_E_PANEL        = 72,  /* PHY0 is the live panel's              */
    RB_PHYFSM_E_BAD_STATE    = 73,  /* outside the enum                      */
    RB_PHYFSM_E_STATE_DENIED = 74,  /* in the enum, not on the allowlist     */
    RB_PHYFSM_E_BAD_LANES    = 75,  /* 0, or above 4                         */
    RB_PHYFSM_E_NO_SYMCLK    = 76,  /* zero symbol clock                     */
    RB_PHYFSM_E_NO_REPLY     = 77,  /* status still the sentinel             */
    RB_PHYFSM_E_PHY_ECHO     = 78,  /* phy_id came back as another phy       */
    RB_PHYFSM_E_STATE_ECHO   = 79,  /* state came back as another state      */
    /* The send path's own, above the builder's so a code never leaves it in
     * doubt whether the payload or the experiment refused (rule 14). */
    RB_PHYFSM_E_NOT_ARMED    = 80,  /* -rbgpufbfsm absent                    */
    RB_PHYFSM_E_NO_SURVEY    = 81,  /* the PHY survey did not run this call  */
    RB_PHYFSM_E_NO_LICENCE   = 82   /* it ran, and the PANEL reads dead      */
};

/* The states we are willing to ask for. RESET and POWER_DOWN would undo the
 * thing we are trying to cause; TX_EN_TEST_MODE drives a test pattern, and
 * 2F8 established that a wrong pattern DEGRADES this sink until a reboot; the
 * P2_* and *_LP / *_PG states are power states with no bearing on bringup.
 * This is an allowlist rather than a denylist so a state added to the enum
 * later is refused by default. */
static inline int rb_phyfsm_state_allowed(uint32_t st)
{
    switch (st) {
    case RB_PHYFSM_RESET_RELEASED:
    case RB_PHYFSM_SRAM_LOAD_DONE:
    case RB_PHYFSM_INITIALIZED:
    case RB_PHYFSM_CALIBRATED:
    case RB_PHYFSM_PLL_EN:
    case RB_PHYFSM_TX_EN:
        return 1;
    default:
        return 0;
    }
}

/* rule 32: the name table has a length and the bound is checked against the
 * table, not against a constant written beside it. */
static inline const char *rb_phyfsm_state_name(uint32_t st)
{
    static const char *const names[] = {
        "POWER_UP_DEFAULT", "RESET", "RESET_RELEASED", "SRAM_LOAD_DONE",
        "INITIALIZED", "CALIBRATED", "CALIBRATED_LP", "CALIBRATED_PG",
        "POWER_DOWN", "PLL_EN", "TX_EN", "TX_EN_TEST_MODE", "FAST_LP",
        "P2_PLL_OFF_CPM", "P2_PLL_OFF_PG", "P2_PLL_OFF", "P2_PLL_ON"
    };
    if (st >= (uint32_t)(sizeof(names) / sizeof(names[0]))) {
        return "UNKNOWN";
    }
    return names[st];
}

/* 100 Hz units from the atom command's 10 kHz units. */
static inline uint32_t rb_phyfsm_symclk_from_10khz(uint32_t symclk_10khz)
{
    return symclk_10khz * RB_PHYFSM_SYMCLK_PER_10KHZ;
}

struct rb_phyfsm_req {
    uint32_t phy_id;
    uint32_t mode;           /* 0 = DP, and 0 is not a default - see above */
    uint32_t lane_num;
    uint32_t symclk_100hz;
    uint32_t state;
    uint32_t status_fill;
};

struct rb_phyfsm_reply {
    uint32_t phy_id;
    uint32_t state;
    uint32_t status;
};

/* Composes the 4-byte dmub header plus the 24-byte payload into buf, which
 * must hold RB_DMUB_CMD_SIZE bytes. Writes NOTHING on refusal, so a caller
 * that ignores the code cannot send a half-built command (the shape
 * rb_strenc_derive_msa() uses). */
static inline int rb_phyfsm_build(uint8_t *buf, const struct rb_phyfsm_req *q)
{
    uint32_t i;

    if (!buf || !q)                               { return RB_PHYFSM_E_NULL; }
    if (q->phy_id >= RB_PHYFSM_PHY_COUNT)         { return RB_PHYFSM_E_BAD_PHY; }
    if (q->phy_id == RB_PHYFSM_PANEL_INST)        { return RB_PHYFSM_E_PANEL; }
    if (q->state >= (uint32_t)RB_PHYFSM_STATE_COUNT) { return RB_PHYFSM_E_BAD_STATE; }
    if (!rb_phyfsm_state_allowed(q->state))       { return RB_PHYFSM_E_STATE_DENIED; }
    if (q->lane_num == 0u || q->lane_num > 4u)    { return RB_PHYFSM_E_BAD_LANES; }
    if (q->symclk_100hz == 0u)                    { return RB_PHYFSM_E_NO_SYMCLK; }

    for (i = 0u; i < RB_DMUB_CMD_SIZE; i++) { buf[i] = 0u; }

    rb_dmub_put32(buf, 0u,
                  rb_dmub_hdr(RB_DMUB_CMD_VBIOS, RB_DMUB_CMD_VBIOS_SET_PHY_FSM,
                              RB_PHYFSM_PAYLOAD));

    buf[4u + RB_PHYFSM_OFF_PHYID]   = (uint8_t)q->phy_id;
    buf[4u + RB_PHYFSM_OFF_MODE]    = (uint8_t)q->mode;
    buf[4u + RB_PHYFSM_OFF_LANENUM] = (uint8_t)q->lane_num;
    rb_dmub_put32(buf, 4u + RB_PHYFSM_OFF_SYMCLK, q->symclk_100hz);
    rb_dmub_put32(buf, 4u + RB_PHYFSM_OFF_STATE,  q->state);
    rb_dmub_put32(buf, 4u + RB_PHYFSM_OFF_STATUS, q->status_fill);

    return RB_PHYFSM_OK;
}

/* Decodes a slot that came back. `sent` is what rb_phyfsm_build() was asked
 * for, so the echo checks compare against the request and not against the
 * buffer's own bytes - a buffer compared with itself always agrees. */
static inline int rb_phyfsm_decode(const uint8_t *got,
                                   const struct rb_phyfsm_req *sent,
                                   struct rb_phyfsm_reply *out)
{
    if (!got || !sent || !out) { return RB_PHYFSM_E_NULL; }

    out->phy_id = got[4u + RB_PHYFSM_OFF_PHYID];
    out->state  = rb_dmub_get32(got, 4u + RB_PHYFSM_OFF_STATE);
    out->status = rb_dmub_get32(got, 4u + RB_PHYFSM_OFF_STATUS);

    /* The licence first: with the status still at the fill we were not
     * answered, and nothing else in the slot means anything. */
    if (out->status == sent->status_fill) { return RB_PHYFSM_E_NO_REPLY; }

    if (out->phy_id != sent->phy_id)      { return RB_PHYFSM_E_PHY_ECHO; }
    if (out->state  != sent->state)       { return RB_PHYFSM_E_STATE_ECHO; }

    return RB_PHYFSM_OK;
}

/* ---- 310c: the verdict, and WHY IT NEEDS THE CONTROLS IN THE SAME SAMPLE ----
 *
 * The thing this command is asked to do has a register that answers it:
 * RDPCSTX PHY_CNTL0's PHY_RESET, 1 on PHY2 and 0 on the live panel (307). So
 * the verdict is "did PHY2's PHY_RESET clear", and it is read from
 * rb_dpcsphy.h's own accessor rather than a bit redefined here.
 *
 * But a reading on PHY2 alone cannot say the command did it. Rule 17: a
 * counter that moves in a non-interleaved comparison has not been shown to
 * move BECAUSE of the change. The four other instances are sampled in the
 * same before/after pair, and if any of THEM moves, whatever happened was not
 * per-port and the PHY2 reading is not attributable to a command that names
 * phyid 2 - CONTAMINATED, which is a distinct verdict and not a success.
 * 309b's extended A/B is the null this is measured against: the atom ENABLE
 * moved none of the nine on any instance.
 *
 * `moved` is a MASK of which registers changed, not a bool, so "PHY2 changed"
 * can be told from "PHY2's PHY_RESET changed" (rule 14's shape: a refusal is a
 * code, and so is a result).
 */

/* The register index whose bit the verdict rests on. Named so the dependency
 * on rb_dpcsphy.h is explicit rather than a bare 4. */
#define RB_PHYFSM_VERDICT_REG   RB_DPCSPHY_R_PHY_CNTL0

enum rb_phyfsm_verdict {
    RB_PHYFSM_V_UNKNOWN      = 0,  /* not sampled                           */
    RB_PHYFSM_V_NO_SEND      = 1,  /* refused before the push               */
    RB_PHYFSM_V_NOTHING      = 2,  /* not one bit moved anywhere            */
    RB_PHYFSM_V_RELEASED     = 3,  /* PHY2's PHY_RESET cleared, controls still */
    RB_PHYFSM_V_MOVED        = 4,  /* PHY2 changed, PHY_RESET still set      */
    RB_PHYFSM_V_CONTAMINATED = 5   /* a CONTROL moved - not per-port         */
};

/* A mask of which of the RB_DPCSPHY_R_COUNT registers differ for one
 * instance. Bit r set means register r changed. */
static inline uint32_t rb_phyfsm_moved(const uint32_t *before,
                                       const uint32_t *after)
{
    uint32_t r, m = 0u;
    if (!before || !after) { return 0u; }
    for (r = 0u; r < (uint32_t)RB_DPCSPHY_R_COUNT; r++) {
        if (before[r] != after[r]) { m |= (1u << r); }
    }
    return m;
}

/* before/after are RB_DPCSPHY_COUNT x RB_DPCSPHY_R_COUNT, flattened
 * [inst * RB_DPCSPHY_R_COUNT + reg] - the layout the kext's phyInst[] array
 * already has. `subject` is the instance the command named. */
static inline int rb_phyfsm_verdict(const uint32_t *before,
                                    const uint32_t *after,
                                    uint32_t subject)
{
    const uint32_t stride = (uint32_t)RB_DPCSPHY_R_COUNT;
    uint32_t i, subjMoved, anyMoved = 0u, ctlMoved = 0u;
    int rstBefore, rstAfter;

    if (!before || !after)                  { return RB_PHYFSM_V_UNKNOWN; }
    if (subject >= RB_DPCSPHY_COUNT)        { return RB_PHYFSM_V_UNKNOWN; }

    for (i = 0u; i < RB_DPCSPHY_COUNT; i++) {
        const uint32_t m = rb_phyfsm_moved(&before[i * stride],
                                           &after[i * stride]);
        if (m) { anyMoved = 1u; }
        if (m && i != subject) { ctlMoved = 1u; }
    }
    subjMoved = rb_phyfsm_moved(&before[subject * stride],
                                &after[subject * stride]);

    /* A subject that moved while a control moved too is not evidence about
     * the subject (rule 17), so CONTAMINATED must win over any success.
     *
     * A first draft of this comment claimed the ORDER of these two lines was
     * load-bearing - "the ordering is the finding, not a precedence
     * convenience". The break-it sweep swapped them and fired NOTHING, and
     * the sweep is right: ctlMoved implies anyMoved, so `!anyMoved` can never
     * be true when a control moved and the order is unobservable. What is
     * load-bearing is only that CONTAMINATED precedes the RELEASED test
     * below, which the "release accepts reset either way" break covers.
     * A comment is not a test, and this one was a claim the data refused. */
    if (ctlMoved)   { return RB_PHYFSM_V_CONTAMINATED; }
    if (!anyMoved)  { return RB_PHYFSM_V_NOTHING; }

    /* The bit, off rb_dpcsphy.h's own mask rather than one redefined here.
     * A first draft called rb_dpcsphy_in_reset() on `&before[...]`, which is
     * a uint32_t* where that function wants a struct rb_dpcsphy_inst* - it
     * compiled with a warning and would have read the wrong memory.
     * `-Wall` caught it and the hosttest would not have: nothing called this
     * function yet. A function with no caller has no coverage, whatever
     * tests exist (rule 48, and rule 46's "a delegate that exists is not a
     * delegate that is REACHED"). The kext now calls it. */
    rstBefore = (before[subject * stride + RB_PHYFSM_VERDICT_REG] &
                 RB_DPCSPHY_D_PHY_RESET) ? 1 : 0;
    rstAfter  = (after[subject * stride + RB_PHYFSM_VERDICT_REG] &
                 RB_DPCSPHY_D_PHY_RESET) ? 1 : 0;

    /* A value outside 0/1 must never read as "released" (rule 14), so the
     * test is for exactly 1 -> exactly 0 and everything else falls through. */
    if (rstBefore != 1 || rstAfter != 0) {
        return subjMoved ? RB_PHYFSM_V_MOVED : RB_PHYFSM_V_NOTHING;
    }
    return RB_PHYFSM_V_RELEASED;
}

#endif /* RB_PHYFSM_H */
