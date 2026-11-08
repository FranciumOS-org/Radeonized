/* 310: SET_PHY_FSM (DMUB_CMD__VBIOS sub-type 29), build-only.
 *
 * Each case pins something the source settled and a comment got wrong at
 * least once, or something only a test can hold:
 *  - the payload size is 24, from the C layout of the data struct with its
 *    two implicit pads and its tail pad - NOT 4 (query_dp_alt's) and not 60
 *    (the transmitter's). Three sizes, three commands, one union;
 *  - the field offsets, each transcribed from the struct rather than
 *    recomputed from the one before it;
 *  - the units are x100, because the two commands' fields are NEITHER the
 *    same name NOR a factor of ten apart, and 310's first draft said both;
 *  - `mode` 0 is DP and is also what an unwritten byte looks like, so the
 *    builder must have no default;
 *  - the status sentinel gives NO_REPLY its own code;
 *  - the allowlist is an allowlist: a state added to the enum later is
 *    refused, and RESET / POWER_DOWN / TX_EN_TEST_MODE are refused now;
 *  - the echo checks compare against the REQUEST, since a buffer compared
 *    with itself always agrees.
 */
#include <stdio.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>

#include "rb_phyfsm.h"

int phyfsm_tests(void);

static int fails;

static void ck(int cond, const char *what)
{
    if (!cond) { printf("FAIL phyfsm: %s\n", what); fails++; }
}

/* A request that is valid in every field, so a case that changes one field
 * changes only that one. */
static void base(struct rb_phyfsm_req *q)
{
    q->phy_id       = RB_PHYFSM_TYPEC_INST;
    q->mode         = 0u;                       /* ATOM_ENCODER_MODE_DP */
    q->lane_num     = 2u;                       /* the MSI MP2412C      */
    q->symclk_100hz = rb_phyfsm_symclk_from_10khz(RB_PHYFSM_TYPEC_SYMCLK_10KHZ);
    q->state        = RB_PHYFSM_RESET_RELEASED;
    q->status_fill  = RB_PHYFSM_STATUS_SENTINEL;
}

/* The firmware's side: clear RET and write a status that is not the fill. */
static void answer(uint8_t *buf, uint32_t status)
{
    rb_dmub_put32(buf, 0, rb_dmub_get32(buf, 0) & ~RB_DMUB_HDR_RET_STATUS);
    rb_dmub_put32(buf, 4u + RB_PHYFSM_OFF_STATUS, status);
}

int phyfsm_tests(void)
{
    uint8_t buf[RB_DMUB_CMD_SIZE];
    struct rb_phyfsm_req q;
    struct rb_phyfsm_reply r;
    uint32_t i, hdr;
    int rc;

    fails = 0;

    /* ---- the command code and the payload size ---- */
    ck(RB_DMUB_CMD_VBIOS_SET_PHY_FSM == 29u, "sub-type is 29");
    ck(RB_PHYFSM_PAYLOAD == 24u, "payload is 24, not 4 and not 60");
    ck(RB_PHYFSM_PAYLOAD <= (RB_DMUB_CMD_SIZE - 4u), "payload fits a slot");
    /* the header's payload field is six bits - 24 must survive the round
     * trip, which 60 also does, so this is not vacuous for either */
    ck(rb_dmub_hdr_payload(rb_dmub_hdr(RB_DMUB_CMD_VBIOS,
                                       RB_DMUB_CMD_VBIOS_SET_PHY_FSM,
                                       RB_PHYFSM_PAYLOAD)) == RB_PHYFSM_PAYLOAD,
       "payload survives the header field");

    /* ---- the offsets, transcribed from the struct ---- */
    ck(RB_PHYFSM_OFF_PHYID   ==  0u, "phy_id at 0");
    ck(RB_PHYFSM_OFF_MODE    ==  1u, "mode at 1");
    ck(RB_PHYFSM_OFF_LANENUM ==  2u, "lane_num at 2");
    ck(RB_PHYFSM_OFF_SYMCLK  ==  4u, "symclk at 4 - a pad at 3");
    ck(RB_PHYFSM_OFF_TM_MODE ==  8u, "test_mode.mode at 8");
    ck(RB_PHYFSM_OFF_TM_PAT0 ==  9u, "test_mode.pat0 at 9");
    ck(RB_PHYFSM_OFF_STATE   == 12u, "state at 12 - test_mode.pad[2] at 10");
    ck(RB_PHYFSM_OFF_STATUS  == 16u, "status at 16");
    ck(RB_PHYFSM_OFF_PAD     == 20u, "pad at 20");
    /* every offset inside the payload, and all distinct */
    {
        const uint32_t off[] = {
            RB_PHYFSM_OFF_PHYID, RB_PHYFSM_OFF_MODE, RB_PHYFSM_OFF_LANENUM,
            RB_PHYFSM_OFF_SYMCLK, RB_PHYFSM_OFF_TM_MODE, RB_PHYFSM_OFF_TM_PAT0,
            RB_PHYFSM_OFF_STATE, RB_PHYFSM_OFF_STATUS, RB_PHYFSM_OFF_PAD
        };
        uint32_t j;
        for (i = 0u; i < sizeof(off)/sizeof(off[0]); i++) {
            ck(off[i] < RB_PHYFSM_PAYLOAD, "offset inside the payload");
            for (j = i + 1u; j < sizeof(off)/sizeof(off[0]); j++) {
                ck(off[i] != off[j], "offsets distinct");
            }
        }
    }

    /* ---- the units: x100, not x10, and not the same field name ---- */
    ck(RB_PHYFSM_SYMCLK_PER_10KHZ == 100u, "10 kHz -> 100 Hz is x100");
    ck(rb_phyfsm_symclk_from_10khz(54000u) == 5400000u, "HBR2 is 5400000");
    ck(rb_phyfsm_symclk_from_10khz(RB_PHYFSM_TYPEC_SYMCLK_10KHZ) == 5400000u,
       "the named HBR2 constant converts to 5400000");
    ck(rb_phyfsm_symclk_from_10khz(54000u) != 540000u,
       "NOT a factor of ten - 310's first draft said ten");
    ck(rb_phyfsm_symclk_from_10khz(0u) == 0u, "zero converts to zero");

    /* ---- state names: the table's own length bounds it (rule 32) ---- */
    ck(strcmp(rb_phyfsm_state_name(RB_PHYFSM_RESET), "RESET") == 0,
       "state 1 is RESET - PHY2's measured state");
    ck(strcmp(rb_phyfsm_state_name(RB_PHYFSM_RESET_RELEASED),
              "RESET_RELEASED") == 0, "state 2 is RESET_RELEASED");
    ck(strcmp(rb_phyfsm_state_name(RB_PHYFSM_SRAM_LOAD_DONE),
              "SRAM_LOAD_DONE") == 0, "state 3 is SRAM_LOAD_DONE - 308's");
    ck(strcmp(rb_phyfsm_state_name(RB_PHYFSM_TX_EN), "TX_EN") == 0,
       "state 10 is TX_EN - the live panel's");
    ck(strcmp(rb_phyfsm_state_name(RB_PHYFSM_P2_PLL_ON), "P2_PLL_ON") == 0,
       "the last in-range state names itself, so the bound is not off by one");
    ck(strcmp(rb_phyfsm_state_name(RB_PHYFSM_STATE_COUNT), "UNKNOWN") == 0,
       "one past the end is UNKNOWN");
    ck(strcmp(rb_phyfsm_state_name(0xffffffffu), "UNKNOWN") == 0,
       "a wild state is UNKNOWN, not a read out of bounds");
    /* no in-range state is left unnamed */
    for (i = 0u; i < (uint32_t)RB_PHYFSM_STATE_COUNT; i++) {
        ck(strcmp(rb_phyfsm_state_name(i), "UNKNOWN") != 0,
           "every state in the enum has a name");
    }

    /* ---- the allowlist ---- */
    ck(rb_phyfsm_state_allowed(RB_PHYFSM_RESET_RELEASED) == 1, "allow 2");
    ck(rb_phyfsm_state_allowed(RB_PHYFSM_SRAM_LOAD_DONE) == 1, "allow 3");
    ck(rb_phyfsm_state_allowed(RB_PHYFSM_TX_EN) == 1, "allow 10");
    ck(rb_phyfsm_state_allowed(RB_PHYFSM_RESET) == 0,
       "refuse RESET - it would undo what we want");
    ck(rb_phyfsm_state_allowed(RB_PHYFSM_POWER_DOWN) == 0, "refuse POWER_DOWN");
    ck(rb_phyfsm_state_allowed(RB_PHYFSM_TX_EN_TEST_MODE) == 0,
       "refuse TX_EN_TEST_MODE - 2F8: a pattern can degrade this sink");
    ck(rb_phyfsm_state_allowed(RB_PHYFSM_P2_PLL_OFF) == 0, "refuse P2_PLL_OFF");
    ck(rb_phyfsm_state_allowed(RB_PHYFSM_POWER_UP_DEFAULT) == 0,
       "refuse POWER_UP_DEFAULT");
    ck(rb_phyfsm_state_allowed((uint32_t)RB_PHYFSM_STATE_COUNT) == 0,
       "a state added to the enum later is refused by default");

    /* ---- build: the happy path, byte for byte ---- */
    base(&q);
    memset(buf, 0xA5, sizeof(buf));
    rc = rb_phyfsm_build(buf, &q);
    ck(rc == RB_PHYFSM_OK, "a valid request builds");
    hdr = rb_dmub_get32(buf, 0);
    ck(rb_dmub_hdr_type(hdr) == RB_DMUB_CMD_VBIOS, "type is VBIOS (128)");
    ck(rb_dmub_hdr_sub(hdr) == RB_DMUB_CMD_VBIOS_SET_PHY_FSM, "sub-type 29");
    ck(rb_dmub_hdr_payload(hdr) == RB_PHYFSM_PAYLOAD, "header says 24");
    ck(buf[4u + RB_PHYFSM_OFF_PHYID]   == 2u, "phy_id 2 = UNIPHYC");
    ck(buf[4u + RB_PHYFSM_OFF_MODE]    == 0u, "mode 0 = DP");
    ck(buf[4u + RB_PHYFSM_OFF_LANENUM] == 2u, "lane_num 2");
    ck(rb_dmub_get32(buf, 4u + RB_PHYFSM_OFF_SYMCLK) == 5400000u,
       "symclk is in 100 Hz units on the wire");
    ck(rb_dmub_get32(buf, 4u + RB_PHYFSM_OFF_STATE) == RB_PHYFSM_RESET_RELEASED,
       "state 2 on the wire as a 32-bit enum");
    ck(rb_dmub_get32(buf, 4u + RB_PHYFSM_OFF_STATUS) == RB_PHYFSM_STATUS_SENTINEL,
       "status pre-filled with the sentinel");
    ck(buf[4u + RB_PHYFSM_OFF_TM_MODE] == 0u, "test_mode.mode zeroed");
    ck(buf[4u + RB_PHYFSM_OFF_TM_PAT0] == 0u, "test_mode.pat0 zeroed");
    ck(buf[4u + RB_PHYFSM_OFF_PAD]     == 0u, "pad zeroed");
    /* the 0xA5 fill is gone from the whole slot, tail included */
    for (i = 0u; i < RB_DMUB_CMD_SIZE; i++) {
        ck(buf[i] != 0xA5u, "no byte of the slot is left unwritten");
    }

    /* ---- build: every refusal, and NOTHING written on any of them ---- */
    ck(rb_phyfsm_build(NULL, &q) == RB_PHYFSM_E_NULL, "NULL buf refused");
    ck(rb_phyfsm_build(buf, NULL) == RB_PHYFSM_E_NULL, "NULL req refused");

    base(&q); q.phy_id = RB_PHYFSM_PHY_COUNT;
    memset(buf, 0xA5, sizeof(buf));
    ck(rb_phyfsm_build(buf, &q) == RB_PHYFSM_E_BAD_PHY, "phy 5 refused");
    ck(buf[0] == 0xA5u, "a refused build writes nothing");

    base(&q); q.phy_id = RB_PHYFSM_PANEL_INST;
    memset(buf, 0xA5, sizeof(buf));
    ck(rb_phyfsm_build(buf, &q) == RB_PHYFSM_E_PANEL,
       "PHY0 refused BY NAME - it is the live panel's");
    ck(buf[0] == 0xA5u, "the panel refusal writes nothing");

    base(&q); q.state = (uint32_t)RB_PHYFSM_STATE_COUNT;
    ck(rb_phyfsm_build(buf, &q) == RB_PHYFSM_E_BAD_STATE, "state 17 refused");
    base(&q); q.state = RB_PHYFSM_RESET;
    ck(rb_phyfsm_build(buf, &q) == RB_PHYFSM_E_STATE_DENIED,
       "RESET is in the enum and off the allowlist - a DIFFERENT code");
    base(&q); q.lane_num = 0u;
    ck(rb_phyfsm_build(buf, &q) == RB_PHYFSM_E_BAD_LANES, "0 lanes refused");
    base(&q); q.lane_num = 5u;
    ck(rb_phyfsm_build(buf, &q) == RB_PHYFSM_E_BAD_LANES, "5 lanes refused");
    base(&q); q.symclk_100hz = 0u;
    ck(rb_phyfsm_build(buf, &q) == RB_PHYFSM_E_NO_SYMCLK, "no symclk refused");
    /* dark PHY3 is a legal target: this is a survey command, and 309's
     * control instance must be reachable or the A/B has no control */
    base(&q); q.phy_id = 3u;
    ck(rb_phyfsm_build(buf, &q) == RB_PHYFSM_OK, "dark PHY3 is allowed");

    /* ---- decode ---- */
    base(&q);
    rb_phyfsm_build(buf, &q);
    ck(rb_phyfsm_decode(buf, &q, &r) == RB_PHYFSM_E_NO_REPLY,
       "status still the sentinel is NO_REPLY, not a status of -1");
    ck(rb_phyfsm_decode(NULL, &q, &r) == RB_PHYFSM_E_NULL, "decode NULL got");
    ck(rb_phyfsm_decode(buf, NULL, &r) == RB_PHYFSM_E_NULL, "decode NULL req");
    ck(rb_phyfsm_decode(buf, &q, NULL) == RB_PHYFSM_E_NULL, "decode NULL out");

    answer(buf, 0u);
    ck(rb_phyfsm_decode(buf, &q, &r) == RB_PHYFSM_OK,
       "a status of 0 under the sentinel fill is a REAL answer");
    ck(r.status == 0u && r.phy_id == 2u && r.state == RB_PHYFSM_RESET_RELEASED,
       "the reply is read out of the right offsets");

    answer(buf, 7u);
    ck(rb_phyfsm_decode(buf, &q, &r) == RB_PHYFSM_OK, "a non-zero status is OK");
    ck(r.status == 7u, "the status word is reported, not judged");

    /* the Linux-exact fill: a genuine status of 0 then reads as NO_REPLY,
     * which is the ambiguity rb_dpaltq.h records rather than a defect */
    base(&q); q.status_fill = RB_PHYFSM_STATUS_LINUX;
    rb_phyfsm_build(buf, &q);
    answer(buf, 0u);
    ck(rb_phyfsm_decode(buf, &q, &r) == RB_PHYFSM_E_NO_REPLY,
       "under the zero fill a true status of 0 is indistinguishable - "
       "the sentinel is why the deviation exists");

    /* the echoes, compared against the REQUEST */
    base(&q);
    rb_phyfsm_build(buf, &q);
    answer(buf, 1u);
    buf[4u + RB_PHYFSM_OFF_PHYID] = 3u;
    ck(rb_phyfsm_decode(buf, &q, &r) == RB_PHYFSM_E_PHY_ECHO,
       "a reply for another phy is caught");
    base(&q);
    rb_phyfsm_build(buf, &q);
    answer(buf, 1u);
    rb_dmub_put32(buf, 4u + RB_PHYFSM_OFF_STATE, RB_PHYFSM_TX_EN);
    ck(rb_phyfsm_decode(buf, &q, &r) == RB_PHYFSM_E_STATE_ECHO,
       "a reply naming another state is caught");

    /* ---- the refusal codes are distinct and clear of the other surfaces ---- */
    {
        const int codes[] = {
            RB_PHYFSM_E_NULL, RB_PHYFSM_E_BAD_PHY, RB_PHYFSM_E_PANEL,
            RB_PHYFSM_E_BAD_STATE, RB_PHYFSM_E_STATE_DENIED,
            RB_PHYFSM_E_BAD_LANES, RB_PHYFSM_E_NO_SYMCLK,
            RB_PHYFSM_E_NO_REPLY, RB_PHYFSM_E_PHY_ECHO, RB_PHYFSM_E_STATE_ECHO
        };
        uint32_t j;
        for (i = 0u; i < sizeof(codes)/sizeof(codes[0]); i++) {
            ck(codes[i] >= 70 && codes[i] <= 79,
               "a phyfsm code is in 70..79, clear of 20-25/30-37/50-55/66");
            ck(codes[i] != RB_PHYFSM_OK, "no refusal collides with OK");
            for (j = i + 1u; j < sizeof(codes)/sizeof(codes[0]); j++) {
                ck(codes[i] != codes[j], "refusal codes distinct");
            }
        }
    }

    /* ---- 310c: the verdict, whose whole point is the CONTROLS ---- */
    {
        const uint32_t N = (uint32_t)RB_DPCSPHY_R_COUNT;
        uint32_t b[RB_DPCSPHY_COUNT * RB_DPCSPHY_R_COUNT];
        uint32_t a[RB_DPCSPHY_COUNT * RB_DPCSPHY_R_COUNT];
        const uint32_t SUBJ = RB_PHYFSM_TYPEC_INST;
        const uint32_t RSTREG = SUBJ * N + RB_PHYFSM_VERDICT_REG;

        ck(RB_PHYFSM_VERDICT_REG == RB_DPCSPHY_R_PHY_CNTL0,
           "the verdict rests on PHY_CNTL0, where PHY_RESET lives");

        /* the real shape: PHY2 in reset, nothing else moving */
        for (i = 0u; i < RB_DPCSPHY_COUNT * N; i++) { b[i] = 0u; a[i] = 0u; }
        b[RSTREG] = RB_DPCSPHY_D_PHY_RESET;
        a[RSTREG] = RB_DPCSPHY_D_PHY_RESET;
        ck(rb_phyfsm_moved(&b[SUBJ * N], &a[SUBJ * N]) == 0u,
           "nothing changed is an empty mask");
        ck(rb_phyfsm_verdict(b, a, SUBJ) == RB_PHYFSM_V_NOTHING,
           "not one bit anywhere is V_NOTHING - 309b's own result");

        /* the release we are after */
        a[RSTREG] = 0u;
        ck(rb_phyfsm_moved(&b[SUBJ * N], &a[SUBJ * N])
               == (1u << RB_PHYFSM_VERDICT_REG),
           "the mask names WHICH register moved, not just that one did");
        ck(rb_phyfsm_verdict(b, a, SUBJ) == RB_PHYFSM_V_RELEASED,
           "1 -> 0 on the subject with the controls still is V_RELEASED");

        /* THE WHOLE POINT: the same release with a control moving too */
        a[RB_PHYFSM_PANEL_INST * N + RB_DPCSPHY_R_CNTL] = 0xdeadu;
        ck(rb_phyfsm_verdict(b, a, SUBJ) == RB_PHYFSM_V_CONTAMINATED,
           "a release is CONTAMINATED when a control moved - rule 17, and "
           "this must NOT read as V_RELEASED");
        a[RB_PHYFSM_PANEL_INST * N + RB_DPCSPHY_R_CNTL] = 0u;
        ck(rb_phyfsm_verdict(b, a, SUBJ) == RB_PHYFSM_V_RELEASED,
           "and it goes back to V_RELEASED when the control is clean, so "
           "the contamination check is not simply always on");

        /* a dark control moving contaminates too, not only the panel */
        a[RB_DPCSPHY_DARK_USBC_INST * N + RB_DPCSPHY_R_CNTL2] = 1u;
        ck(rb_phyfsm_verdict(b, a, SUBJ) == RB_PHYFSM_V_CONTAMINATED,
           "the DARK USB-C control contaminates as much as the panel does");
        a[RB_DPCSPHY_DARK_USBC_INST * N + RB_DPCSPHY_R_CNTL2] = 0u;

        /* moved but not released */
        a[RSTREG] = RB_DPCSPHY_D_PHY_RESET;
        a[SUBJ * N + RB_DPCSPHY_R_CLOCK_CNTL] = 0x400u;
        ck(rb_phyfsm_verdict(b, a, SUBJ) == RB_PHYFSM_V_MOVED,
           "the subject changed with PHY_RESET still set is V_MOVED, a "
           "DIFFERENT verdict from V_RELEASED");

        /* reset going the WRONG way must never read as released */
        for (i = 0u; i < RB_DPCSPHY_COUNT * N; i++) { b[i] = 0u; a[i] = 0u; }
        b[RSTREG] = 0u;
        a[RSTREG] = RB_DPCSPHY_D_PHY_RESET;
        ck(rb_phyfsm_verdict(b, a, SUBJ) != RB_PHYFSM_V_RELEASED,
           "0 -> 1 is the PHY being PUT in reset and is not a release");

        /* a subject already out of reset cannot be 'released' by this */
        for (i = 0u; i < RB_DPCSPHY_COUNT * N; i++) { b[i] = 0u; a[i] = 0u; }
        a[SUBJ * N + RB_DPCSPHY_R_CLOCK_CNTL] = 0x400u;
        ck(rb_phyfsm_verdict(b, a, SUBJ) == RB_PHYFSM_V_MOVED,
           "a subject that was never in reset reports MOVED, not RELEASED");

        ck(rb_phyfsm_verdict(NULL, a, SUBJ) == RB_PHYFSM_V_UNKNOWN,
           "NULL before is UNKNOWN, not NOTHING");
        ck(rb_phyfsm_verdict(b, NULL, SUBJ) == RB_PHYFSM_V_UNKNOWN,
           "NULL after is UNKNOWN");
        ck(rb_phyfsm_verdict(b, a, RB_DPCSPHY_COUNT) == RB_PHYFSM_V_UNKNOWN,
           "a subject outside the instance range is UNKNOWN");
        ck(rb_phyfsm_moved(NULL, a) == 0u, "NULL to moved() is an empty mask");
        ck(rb_phyfsm_moved(b, NULL) == 0u, "NULL to moved() either way");
    }

    /* ---- the send path's own refusal codes ---- */
    ck(RB_PHYFSM_E_NOT_ARMED  == 80, "NOT_ARMED is 80");
    ck(RB_PHYFSM_E_NO_SURVEY  == 81, "NO_SURVEY is 81");
    ck(RB_PHYFSM_E_NO_LICENCE == 82, "NO_LICENCE is 82");
    ck(RB_PHYFSM_E_NO_SURVEY != RB_PHYFSM_E_NO_LICENCE,
       "\"the survey did not run\" and \"it ran and the panel reads dead\" "
       "are DIFFERENT codes - absence is not a result");
    ck(RB_PHYFSM_TYPEC_LANES == 2u, "the sink is 2 lanes");

    if (fails == 0) { printf("phyfsm: ok\n"); }
    return fails;
}
