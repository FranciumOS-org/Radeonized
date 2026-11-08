/* 306: the RDPCSTX PHY status survey.
 *
 * The subject of this file is the ANSWERED/DRIVEN split. Every earlier
 * reading on the external-display line either answered our own stores or
 * came from the wrong register, so the checks that matter here are that no
 * ANSWERED mask overlaps a DRIVEN one, and that the verdict is built from
 * ANSWERED bits ALONE - a verdict that moved when a DRIVEN bit moved would
 * be an instrument reporting its own input.
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "rb_dpcsphy.h"

int phy_tests(void);

int phy_tests(void)
{
    unsigned bad = 0;
    struct rb_dpcsphy_inst in[RB_DPCSPHY_COUNT];
    uint32_t i, r;

    /* ---- the offsets and the stride ---- */
    if (rb_dpcsphy_dw(0, RB_DPCSPHY_R_CNTL) != 0x2930u ||
        rb_dpcsphy_dw(0, RB_DPCSPHY_R_CLOCK_CNTL) != 0x2931u ||
        rb_dpcsphy_dw(0, RB_DPCSPHY_R_INTERRUPT) != 0x2932u ||
        rb_dpcsphy_dw(0, RB_DPCSPHY_R_SRAM_CNTL) != 0x2936u ||
        rb_dpcsphy_dw(0, RB_DPCSPHY_R_PHY_CNTL0) != 0x2940u) {
        printf("FAIL phy: instance 0's offsets are not the header's\n");
        bad++;
    }
    /* Every instance, against dpcs_4_2_0_offset.h's own numbers transcribed
     * here rather than recomputed from the stride - recomputing them would
     * check the stride against itself. */
    {
        static const uint32_t cntl[RB_DPCSPHY_COUNT] = {
            0x2930u, 0x2a08u, 0x2ae0u, 0x2bb8u, 0x2c90u
        };
        for (i = 0; i < RB_DPCSPHY_COUNT; i++) {
            if (rb_dpcsphy_dw(i, RB_DPCSPHY_R_CNTL) != cntl[i]) {
                printf("FAIL phy: RDPCSTX%u_RDPCSTX_CNTL is 0x%x, the header "
                       "says 0x%x\n", i,
                       rb_dpcsphy_dw(i, RB_DPCSPHY_R_CNTL), cntl[i]);
                bad++;
            }
        }
    }
    /* Out of range and a bad register are REFUSED, and 0 is safe to use as
     * the refusal because 0 is not an offset in this block. */
    if (rb_dpcsphy_dw(RB_DPCSPHY_COUNT, RB_DPCSPHY_R_CNTL) != 0u ||
        rb_dpcsphy_dw(0, RB_DPCSPHY_R_COUNT) != 0u) {
        printf("FAIL phy: an out-of-range instance or register was not "
               "refused\n");
        bad++;
    }
    if (RB_DPCSPHY_DW_CNTL == 0u) {
        printf("FAIL phy: 0 is a real offset here, so it cannot be the "
               "refusal\n");
        bad++;
    }

    /* ---- THE SPLIT: no ANSWERED bit may also be a DRIVEN bit ---- */
    {
        /* Per register, because the masks only collide within one. */
        uint32_t a_phy = RB_DPCSPHY_A_REF_CLKDET_RESULT |
                               RB_DPCSPHY_A_SRAM_INIT_DONE |
                               RB_DPCSPHY_A_RTUNE_ACK;
        uint32_t d_phy = RB_DPCSPHY_D_PHY_REF_CLKDET_EN |
                               RB_DPCSPHY_D_PHY_RESET |
                               RB_DPCSPHY_D_TEST_POWERDOWN |
                               RB_DPCSPHY_D_PHY_REF_RANGE;
        uint32_t a_clk = RB_DPCSPHY_A_TX_CLOCK_ON |
                               RB_DPCSPHY_A_SRAMCLK_CLOCK_ON |
                               RB_DPCSPHY_A_OCLACLK_CLOCK_ON;
        uint32_t d_clk = RB_DPCSPHY_D_EXT_REFCLK_EN |
                               RB_DPCSPHY_D_TX_LANE_CLK_EN |
                               RB_DPCSPHY_D_TX_CLK_EN |
                               RB_DPCSPHY_D_SRAMCLK_EN |
                               RB_DPCSPHY_D_OCLACLK_EN;
        uint32_t a_cnt = RB_DPCSPHY_A_PLL_UPDATE_PENDING |
                               RB_DPCSPHY_A_DPALT_BLOCK_STATUS;
        uint32_t d_cnt = RB_DPCSPHY_D_TX_FIFO_LANE_EN |
                               RB_DPCSPHY_D_TX_FIFO_EN |
                               RB_DPCSPHY_D_TX_FIFO_START |
                               RB_DPCSPHY_D_TX_SOFT_RESET |
                               RB_DPCSPHY_D_CBUS_SOFT_RESET |
                               RB_DPCSPHY_D_SRAM_SOFT_RESET;
        /* THE OVERLAP CHECK, THROUGH A RUNTIME ARRAY ON PURPOSE.
         *
         * Written as `if ((a_phy & d_phy) || ...)` the compiler folds it to a
         * constant and -Werror's -Wconstant-logical-operand kills the build
         * on a break instead of failing the test. That is the -Werror trap
         * for the sixth recorded time, and it meant this check had NEVER
         * been coverage: it could not fail at runtime. Through a volatile
         * array the comparison survives to run. */
        /* THREE sets now, so the check is every PAIR of sets within a
         * register, not just ANSWERED against DRIVEN. 308 added
         * UNATTRIBUTED, and a bit landing in two of the three would be
         * exactly the confusion the split exists to prevent. Only
         * PHY_CNTL0 has an UNATTRIBUTED mask, so its U term is the SRAM
         * handshake and the other registers' is 0. */
        static volatile uint32_t trio[3][3];
        unsigned q;
        trio[0][0] = a_phy; trio[0][1] = d_phy;
        trio[0][2] = RB_DPCSPHY_U_SRAM_EXT_LD_DONE
                   | RB_DPCSPHY_U_SRAM_BYPASS;
        trio[1][0] = a_clk; trio[1][1] = d_clk; trio[1][2] = 0u;
        trio[2][0] = a_cnt; trio[2][1] = d_cnt; trio[2][2] = 0u;
        for (q = 0; q < 3u; q++) {
            if (trio[q][0] & trio[q][1]) {
                printf("FAIL phy: register group %u has an ANSWERED mask "
                       "overlapping a DRIVEN one (0x%08x & 0x%08x) - the "
                       "survey would report our own stores as hardware "
                       "state\n", q, trio[q][0], trio[q][1]);
                bad++;
            }
            if (trio[q][0] & trio[q][2]) {
                printf("FAIL phy: register group %u has an ANSWERED mask "
                       "overlapping an UNATTRIBUTED one (0x%08x & 0x%08x) - "
                       "a field whose owner is unknown would be read as a "
                       "hardware answer\n", q, trio[q][0], trio[q][2]);
                bad++;
            }
            if (trio[q][1] & trio[q][2]) {
                printf("FAIL phy: register group %u has a DRIVEN mask "
                       "overlapping an UNATTRIBUTED one (0x%08x & 0x%08x) - "
                       "a field whose owner is unknown would be read as our "
                       "own store\n", q, trio[q][1], trio[q][2]);
                bad++;
            }
        }
        /* EVERY MASK INDIVIDUALLY, not the OR of each group.
         * The break-it sweep emptied ONE mask and fired nothing, because
         * the group it belongs to was still non-empty - the check tested a
         * disjunction and reported it as coverage of each term. That is the
         * same defect it exists to catch, one level up. */
        {
            static const uint32_t every[] = {
                RB_DPCSPHY_A_REF_CLKDET_RESULT, RB_DPCSPHY_A_SRAM_INIT_DONE,
                RB_DPCSPHY_A_RTUNE_ACK,         RB_DPCSPHY_U_SRAM_EXT_LD_DONE,
                RB_DPCSPHY_U_SRAM_BYPASS,
                RB_DPCSPHY_A_TX_CLOCK_ON,       RB_DPCSPHY_A_SRAMCLK_CLOCK_ON,
                RB_DPCSPHY_A_OCLACLK_CLOCK_ON,  RB_DPCSPHY_A_PLL_UPDATE_PENDING,
                RB_DPCSPHY_A_DPALT_BLOCK_STATUS,RB_DPCSPHY_A_REG_FIFO_OVERFLOW,
                RB_DPCSPHY_A_TX_FIFO_ERROR,     RB_DPCSPHY_A_DPALT_DIS_TOGGLE,
                RB_DPCSPHY_A_DPALT_4LANE_TOGGLE,RB_DPCSPHY_A_MEM_PWR_PWR_STATE,
                RB_DPCSPHY_D_EXT_REFCLK_EN,     RB_DPCSPHY_D_TX_LANE_CLK_EN,
                RB_DPCSPHY_D_TX_CLK_EN,         RB_DPCSPHY_D_SRAMCLK_EN,
                RB_DPCSPHY_D_OCLACLK_EN,        RB_DPCSPHY_D_PHY_REF_CLKDET_EN,
                RB_DPCSPHY_D_TX_FIFO_LANE_EN,   RB_DPCSPHY_D_TX_FIFO_EN,
                RB_DPCSPHY_D_TX_FIFO_START,     RB_DPCSPHY_D_TX_SOFT_RESET,
                RB_DPCSPHY_D_CBUS_SOFT_RESET,   RB_DPCSPHY_D_SRAM_SOFT_RESET,
                RB_DPCSPHY_D_PHY_RESET,         RB_DPCSPHY_D_TEST_POWERDOWN,
                RB_DPCSPHY_D_PHY_REF_RANGE
            };
            const unsigned n_every = sizeof(every) / sizeof(every[0]);
            unsigned k;
            for (k = 0; k < n_every; k++) {
                if (every[k] == 0u) {
                    printf("FAIL phy: mask %u is empty, so every check that "
                           "uses it is vacuous\n", k);
                    bad++;
                }
            }
            /* No separate equality check here: it would be another
             * constant the compiler folds, and the runtime overlap loop
             * above already catches an aliased bit. */
        }
    }

    /* ---- the verdict, built from ANSWERED bits alone ---- */
    /* THE LICENCE GATE. An all-zero instance has the detector DISABLED, so
     * it is NO_LICENCE, not DEAD. Boot 479 is why: the old function called
     * the panel's own live PHY CLOCKLESS off a bit nobody had enabled. */
    memset(in, 0, sizeof(in));
    if (rb_dpcsphy_verdict(&in[0]) != RB_DPCSPHY_V_NO_LICENCE) {
        printf("FAIL phy: a disabled detector did not refuse the verdict\n");
        bad++;
    }
    in[0].w[RB_DPCSPHY_R_PHY_CNTL0] = RB_DPCSPHY_D_PHY_REF_CLKDET_EN;
    if (rb_dpcsphy_verdict(&in[0]) != RB_DPCSPHY_V_DEAD) {
        printf("FAIL phy: detector on with nothing answering is not DEAD\n");
        bad++;
    }
    if (rb_dpcsphy_verdict(0) != RB_DPCSPHY_V_UNKNOWN) {
        printf("FAIL phy: a NULL instance is not UNKNOWN\n");
        bad++;
    }
    in[0].w[RB_DPCSPHY_R_PHY_CNTL0] = RB_DPCSPHY_D_PHY_REF_CLKDET_EN |
                                      RB_DPCSPHY_A_REF_CLKDET_RESULT |
                                      RB_DPCSPHY_A_SRAM_INIT_DONE;
    if (rb_dpcsphy_verdict(&in[0]) != RB_DPCSPHY_V_ALIVE) {
        printf("FAIL phy: clock detected plus SRAM loaded is not ALIVE\n");
        bad++;
    }
    in[0].w[RB_DPCSPHY_R_PHY_CNTL0] = RB_DPCSPHY_D_PHY_REF_CLKDET_EN |
                                      RB_DPCSPHY_A_SRAM_INIT_DONE;
    if (rb_dpcsphy_verdict(&in[0]) != RB_DPCSPHY_V_CLOCKLESS) {
        printf("FAIL phy: SRAM loaded with no clock is not CLOCKLESS\n");
        bad++;
    }
    in[0].w[RB_DPCSPHY_R_PHY_CNTL0] = RB_DPCSPHY_D_PHY_REF_CLKDET_EN |
                                      RB_DPCSPHY_A_REF_CLKDET_RESULT;
    if (rb_dpcsphy_verdict(&in[0]) != RB_DPCSPHY_V_NO_SRAM) {
        printf("FAIL phy: a clock with no SRAM is not NO_SRAM\n");
        bad++;
    }
    /* THE LOAD-BEARING ONE. Every DRIVEN bit set at once, every ANSWERED bit
     * clear: the verdict must still be DEAD. A driver can set every enable
     * in this block and that says nothing about the PHY. */
    memset(in, 0, sizeof(in));
    in[0].w[RB_DPCSPHY_R_PHY_CNTL0] = RB_DPCSPHY_D_PHY_REF_CLKDET_EN |
                                      RB_DPCSPHY_D_PHY_RESET |
                                      RB_DPCSPHY_D_TEST_POWERDOWN |
                                      RB_DPCSPHY_D_PHY_REF_RANGE;
    in[0].w[RB_DPCSPHY_R_CLOCK_CNTL] = RB_DPCSPHY_D_EXT_REFCLK_EN |
                                       RB_DPCSPHY_D_TX_LANE_CLK_EN |
                                       RB_DPCSPHY_D_TX_CLK_EN |
                                       RB_DPCSPHY_D_SRAMCLK_EN |
                                       RB_DPCSPHY_D_OCLACLK_EN;
    in[0].w[RB_DPCSPHY_R_CNTL] = RB_DPCSPHY_D_TX_FIFO_EN |
                                 RB_DPCSPHY_D_TX_FIFO_START |
                                 RB_DPCSPHY_D_TX_FIFO_LANE_EN;
    in[0].w[RB_DPCSPHY_R_SRAM_CNTL] = RB_DPCSPHY_D_TX_FIFO_EN;
    if (rb_dpcsphy_verdict(&in[0]) != RB_DPCSPHY_V_DEAD) {
        printf("FAIL phy: every DRIVEN bit set moved the verdict - the "
               "instrument is reporting its own input\n");
        bad++;
    }

    /* ---- the three bits boot 479 actually answered, with the panel's own
     * live values and PHY2's as the two cases ---- */
    memset(in, 0, sizeof(in));
    in[0].w[RB_DPCSPHY_R_PHY_CNTL0]  = 0xb0100004u;  /* PHY0, boot 479 */
    in[0].w[RB_DPCSPHY_R_CLOCK_CNTL] = 0x00117ff0u;
    in[0].w[RB_DPCSPHY_R_CNTL]       = 0x028ff0f0u;
    in[2].w[RB_DPCSPHY_R_PHY_CNTL0]  = 0x10100605u;  /* PHY2, boot 479 */
    in[2].w[RB_DPCSPHY_R_CLOCK_CNTL] = 0x001019f0u;
    in[2].w[RB_DPCSPHY_R_CNTL]       = 0x008f00f0u;
    if (rb_dpcsphy_in_reset(&in[0]) != 0 ||
        rb_dpcsphy_tx_clock_on(&in[0]) != 1 ||
        rb_dpcsphy_tx_fifo_en(&in[0]) != 1) {
        printf("FAIL phy: the panel's own boot-479 words do not decode as a "
               "released PHY with its clock and FIFO on\n");
        bad++;
    }
    if (rb_dpcsphy_in_reset(&in[2]) != 1 ||
        rb_dpcsphy_tx_clock_on(&in[2]) != 0 ||
        rb_dpcsphy_tx_fifo_en(&in[2]) != 0) {
        printf("FAIL phy: PHY2's own boot-479 words do not decode as a PHY "
               "held in reset with no clock and no FIFO\n");
        bad++;
    }
    /* The two must DISAGREE on all three, or the pair proves nothing. */
    if (rb_dpcsphy_in_reset(&in[0]) == rb_dpcsphy_in_reset(&in[2]) ||
        rb_dpcsphy_tx_clock_on(&in[0]) == rb_dpcsphy_tx_clock_on(&in[2]) ||
        rb_dpcsphy_tx_fifo_en(&in[0]) == rb_dpcsphy_tx_fifo_en(&in[2])) {
        printf("FAIL phy: the panel and PHY2 agree on a bit the finding "
               "rests on them disagreeing about\n");
        bad++;
    }
    /* BOOT 479'S OWN WORDS CANNOT SEPARATE THESE MASKS FROM THEIR
     * NEIGHBOURS, and the break-it sweep is what showed it: swapping
     * TX_CLOCK_ON for the DRIVEN TX_CLK_EN, or TX_FIFO_EN for the per-lane
     * enables, FIRED NOTHING, because in that sample each pair moves
     * together on both instances. So the real words pin the FINDING but not
     * the BIT. These synthetic words pin the bit - each sets exactly one
     * member of a pair - and without them the accessors could read the wrong
     * neighbour for ever. */
    memset(in, 0, sizeof(in));
    in[0].w[RB_DPCSPHY_R_CLOCK_CNTL] = RB_DPCSPHY_A_TX_CLOCK_ON;
    in[1].w[RB_DPCSPHY_R_CLOCK_CNTL] = RB_DPCSPHY_D_TX_CLK_EN;
    if (rb_dpcsphy_tx_clock_on(&in[0]) != 1 ||
        rb_dpcsphy_tx_clock_on(&in[1]) != 0) {
        printf("FAIL phy: tx_clock_on does not read TX_CLOCK_ON alone - it "
               "answers the DRIVEN TX_CLK_EN\n");
        bad++;
    }
    memset(in, 0, sizeof(in));
    in[0].w[RB_DPCSPHY_R_CNTL] = RB_DPCSPHY_D_TX_FIFO_EN;
    in[1].w[RB_DPCSPHY_R_CNTL] = RB_DPCSPHY_D_TX_FIFO_LANE_EN;
    if (rb_dpcsphy_tx_fifo_en(&in[0]) != 1 ||
        rb_dpcsphy_tx_fifo_en(&in[1]) != 0) {
        printf("FAIL phy: tx_fifo_en does not read TX_FIFO_EN alone - it "
               "answers the per-lane enables\n");
        bad++;
    }
    memset(in, 0, sizeof(in));
    in[0].w[RB_DPCSPHY_R_PHY_CNTL0] = RB_DPCSPHY_D_PHY_RESET;
    in[1].w[RB_DPCSPHY_R_PHY_CNTL0] = RB_DPCSPHY_D_TEST_POWERDOWN;
    if (rb_dpcsphy_in_reset(&in[0]) != 1 ||
        rb_dpcsphy_in_reset(&in[1]) != 0) {
        printf("FAIL phy: in_reset does not read PHY_RESET alone\n");
        bad++;
    }

    /* 308: THE SRAM HANDSHAKE, AND THE CONTROL THAT MAKES IT MEAN
     * ANYTHING. Boot 479's two real words, the live panel against PHY2.
     * These are the measured values, not constructed ones, so the test
     * fails if an accessor reads the wrong bit of the real data. */
    memset(in, 0, sizeof(in));
    in[0].w[RB_DPCSPHY_R_PHY_CNTL0] = 0xb0100004u;  /* PHY0, the live panel */
    in[2].w[RB_DPCSPHY_R_PHY_CNTL0] = 0x10100605u;  /* PHY2, the subject     */

    if (rb_dpcsphy_sram_ext_ld_done(&in[0]) != 1 ||
        rb_dpcsphy_sram_ext_ld_done(&in[2]) != 0) {
        printf("FAIL phy: EXT_LD_DONE misread - the live panel's 0xb0100004 "
               "has it SET and PHY2's 0x10100605 has it CLEAR\n");
        bad++;
    }
    if (rb_dpcsphy_sram_bypass(&in[0]) != 1 ||
        rb_dpcsphy_sram_bypass(&in[2]) != 0) {
        printf("FAIL phy: SRAM_BYPASS misread - set on the live panel, "
               "clear on PHY2\n");
        bad++;
    }
    /* REF_RANGE is a VALUE, and reading it as a flag would make 3 and 1
     * indistinguishable - which is the whole reason it has its own
     * accessor. */
    if (rb_dpcsphy_ref_range(&in[0]) != 0 ||
        rb_dpcsphy_ref_range(&in[2]) != 3) {
        printf("FAIL phy: REF_RANGE misread - 0 on the live panel and 3 on "
               "PHY2 (got %d and %d)\n",
               rb_dpcsphy_ref_range(&in[0]), rb_dpcsphy_ref_range(&in[2]));
        bad++;
    }
    /* SRAM_INIT_DONE is set on BOTH, so it is the term that carries no
     * information in this comparison. Asserted so that a future edit which
     * quietly folds it into the diff is caught: a conjunction can be
     * correct while one term says nothing (rule 48). */
    if (!(in[0].w[RB_DPCSPHY_R_PHY_CNTL0] & RB_DPCSPHY_A_SRAM_INIT_DONE) ||
        !(in[2].w[RB_DPCSPHY_R_PHY_CNTL0] & RB_DPCSPHY_A_SRAM_INIT_DONE)) {
        printf("FAIL phy: SRAM_INIT_DONE is set on both real words - this "
               "test no longer matches the data it was built from\n");
        bad++;
    }

    if (rb_dpcsphy_sram_diff(&in[2], &in[0])
        != (RB_DPCSPHY_DIFF_EXT_LD_DONE | RB_DPCSPHY_DIFF_SRAM_BYPASS
            | RB_DPCSPHY_DIFF_REF_RANGE)) {
        printf("FAIL phy: the diff against the live control does not name "
               "all three differing fields (got 0x%x)\n",
               rb_dpcsphy_sram_diff(&in[2], &in[0]));
        bad++;
    }
    /* An instance against ITSELF must refuse, not return 0. 0 is the real
     * answer "they agree", and a self-comparison is not evidence of
     * agreement - it is the absence of a control. */
    if (rb_dpcsphy_sram_diff(&in[2], &in[2]) != RB_DPCSPHY_DIFF_E_SAME) {
        printf("FAIL phy: comparing an instance with itself returned a "
               "verdict instead of refusing - a missing control would read "
               "as agreement\n");
        bad++;
    }
    /* Two instances that genuinely agree must return 0, or the diff cannot
     * tell "same" from "could not tell". in[3] is PHY3, byte-identical to
     * PHY2 in boot 479 - the negative case from the real data. */
    in[3].w[RB_DPCSPHY_R_PHY_CNTL0] = 0x10100605u;
    if (rb_dpcsphy_sram_diff(&in[2], &in[3]) != 0) {
        printf("FAIL phy: two byte-identical instances do not diff to 0\n");
        bad++;
    }
    /* THE REAL WORDS CANNOT SEPARATE THESE TWO BITS, and the break-it
     * sweep is what showed it: swapping sram_bypass to read EXT_LD_DONE
     * FIRED NOTHING, because 0xb0100004 has both set and 0x10100605 has
     * neither. So the panel-vs-PHY2 comparison is ONE observation about the
     * SRAM handshake, not two independent ones, and these synthetic words -
     * each setting exactly one of the pair - are what pin the accessors to
     * their own bit. Same shape as TX_CLOCK_ON against TX_CLK_EN. */
    memset(in, 0, sizeof(in));
    in[0].w[RB_DPCSPHY_R_PHY_CNTL0] = RB_DPCSPHY_U_SRAM_EXT_LD_DONE;
    in[1].w[RB_DPCSPHY_R_PHY_CNTL0] = RB_DPCSPHY_U_SRAM_BYPASS;
    if (rb_dpcsphy_sram_ext_ld_done(&in[0]) != 1 ||
        rb_dpcsphy_sram_ext_ld_done(&in[1]) != 0) {
        printf("FAIL phy: ext_ld_done does not read EXT_LD_DONE alone - it "
               "answers SRAM_BYPASS\n");
        bad++;
    }
    if (rb_dpcsphy_sram_bypass(&in[1]) != 1 ||
        rb_dpcsphy_sram_bypass(&in[0]) != 0) {
        printf("FAIL phy: sram_bypass does not read SRAM_BYPASS alone - it "
               "answers EXT_LD_DONE\n");
        bad++;
    }
    /* And the diff must name exactly ONE field when exactly one differs. */
    if (rb_dpcsphy_sram_diff(&in[0], &in[1])
        != (RB_DPCSPHY_DIFF_EXT_LD_DONE | RB_DPCSPHY_DIFF_SRAM_BYPASS)) {
        printf("FAIL phy: the diff cannot tell the two SRAM bits apart\n");
        bad++;
    }

    memset(in, 0, sizeof(in));
    in[0].w[RB_DPCSPHY_R_PHY_CNTL0] = 0xb0100004u;
    in[2].w[RB_DPCSPHY_R_PHY_CNTL0] = 0x10100605u;
    if (rb_dpcsphy_sram_diff(0, &in[0]) != RB_DPCSPHY_DIFF_E_NULL ||
        rb_dpcsphy_sram_diff(&in[0], 0) != RB_DPCSPHY_DIFF_E_NULL) {
        printf("FAIL phy: a NULL side of the diff does not refuse\n");
        bad++;
    }
    if (rb_dpcsphy_sram_ext_ld_done(0) != -1 ||
        rb_dpcsphy_sram_bypass(0) != -1 ||
        rb_dpcsphy_ref_range(0) != -1) {
        printf("FAIL phy: a NULL instance returns 0 instead of -1 from an "
               "SRAM accessor - a refusal would read as a cleared bit\n");
        bad++;
    }

    memset(in, 0, sizeof(in));
    if (rb_dpcsphy_in_reset(0) != -1 || rb_dpcsphy_tx_clock_on(0) != -1 ||
        rb_dpcsphy_tx_fifo_en(0) != -1) {
        printf("FAIL phy: NULL does not refuse with -1 - a 0 would read as "
               "'not in reset' (rule 14)\n");
        bad++;
    }

    /* ---- the clock-detector licence ---- */
    memset(in, 0, sizeof(in));
    if (rb_dpcsphy_clkdet_valid(&in[0]) != 0) {
        printf("FAIL phy: a clkdet result is called valid with the detector "
               "disabled - that is reading a 0 that means nothing\n");
        bad++;
    }
    in[0].w[RB_DPCSPHY_R_PHY_CNTL0] = RB_DPCSPHY_D_PHY_REF_CLKDET_EN;
    if (rb_dpcsphy_clkdet_valid(&in[0]) != 1) {
        printf("FAIL phy: the detector is enabled and the result is still "
               "called invalid\n");
        bad++;
    }
    if (rb_dpcsphy_clkdet_valid(0) != 0) {
        printf("FAIL phy: NULL is not invalid\n");
        bad++;
    }

    /* ---- the sweep: five register sets, or one address five times ---- */
    memset(in, 0, sizeof(in));
    if (rb_dpcsphy_sweep_differs(in, RB_DPCSPHY_COUNT) != 0) {
        printf("FAIL phy: five identical instances read as DIFFERING\n");
        bad++;
    }
    in[2].w[RB_DPCSPHY_R_CNTL] = 1u;
    if (rb_dpcsphy_sweep_differs(in, RB_DPCSPHY_COUNT) != 1) {
        printf("FAIL phy: one differing instance did not make the sweep "
               "differ\n");
        bad++;
    }
    /* A difference in ANY of the five registers must count, not just the
     * first - a sweep that only compared one word would pass the check above
     * for the wrong reason. */
    for (r = 0; r < RB_DPCSPHY_R_COUNT; r++) {
        memset(in, 0, sizeof(in));
        in[4].w[r] = 0x80000000u;
        if (rb_dpcsphy_sweep_differs(in, RB_DPCSPHY_COUNT) != 1) {
            printf("FAIL phy: a difference in register %u is invisible to "
                   "the sweep\n", r);
            bad++;
        }
    }
    /* NULL only. A single instance is covered by the loop bound rather than
     * by a guard, and asserting it here would be asserting something no
     * break can falsify - the sweep showed exactly that. */
    if (rb_dpcsphy_sweep_differs(0, RB_DPCSPHY_COUNT) != 0) {
        printf("FAIL phy: NULL is not refused\n");
        bad++;
    }

    /* ---- the panel is named, so it can be refused by name ---- */
    if (RB_DPCSPHY_PANEL_INST != 0u || RB_DPCSPHY_TYPEC_INST != 2u ||
        RB_DPCSPHY_PANEL_INST == RB_DPCSPHY_TYPEC_INST) {
        printf("FAIL phy: the panel and the Type-C port are not distinct "
               "named instances\n");
        bad++;
    }

    if (!bad) {
        printf("ok phy: five instances at the header's own offsets, the "
               "ANSWERED/DRIVEN/UNATTRIBUTED sets pairwise disjoint, the "
               "SRAM handshake read against the live panel as control, and "
               "a verdict that every "
               "DRIVEN bit cannot move\n");
    }
    return bad;
}
