/* SPDX-License-Identifier: MIT */
/* 302: the DIO stream encoder, read-only.
 *
 * Three things here are worth a case rather than a glance:
 *
 *   1. THE OFFSETS, pinned per register AND per instance against
 *      dcn_3_1_2_offset.h read directly. The instance stride is 0x100 and
 *      uniform, which is exactly the situation in which a formula looks right
 *      - 2FA found three different strides inside one function for registers
 *      programmed together for one pipe. So the stride is asserted to be
 *      CONSISTENT with the pinned values and never used to produce them.
 *
 *   2. `rb_strenc_dw_max()`, which must come from the highest ADDRESS and not
 *      from the last enum slot. DIG_FE_CNTL is last in the enum and lowest in
 *      memory (0x208b against DP_SEC_FRAMING4's 0x2130), so a bound taken from
 *      the final slot is short by 0xa5 dwords and the shortfall lands on the
 *      high instances. An aperture bound that is too small is a dereference
 *      outside the mapping (rule 19).
 *
 *   3. THE FIVE-WAY VERDICT on DP_VID_STREAM_CNTL, and in particular that
 *      ENABLE and STATUS stay SEPARATE. Publishing the bit we wrote as though
 *      it were the bit the hardware answered is 2E6's defect in a new
 *      register, and `ASKED_NOT_RUNNING` - the state upstream's own REG_WAIT
 *      exists for - is the single most informative thing this register can say
 *      about DIG2 after a write. It must not fold into either neighbour.
 */
#include <stdio.h>
#include <stdint.h>
#include "rb_strenc.h"

int strenc_tests(void);

int strenc_tests(void)
{
    int bad = 0;

    /* The per-register base offsets, against dcn_3_1_2_offset.h:
     *   DP0_DP_PIXEL_FORMAT        0x2109   DP0_DP_VID_STREAM_CNTL  0x210c
     *   DP0_DP_MSA_COLORIMETRY     0x210a   DP0_DP_STEER_FIFO       0x210d
     *   DP0_DP_MSA_MISC            0x210e   DP0_DP_VID_TIMING       0x2110
     *   DP0_DP_MSA_TIMING_PARAM1   0x214c   DP0_DP_VID_N            0x2111
     *   DP0_DP_MSA_TIMING_PARAM2   0x214d   DP0_DP_VID_M            0x2112
     *   DP0_DP_MSA_TIMING_PARAM3   0x214e   DP0_DP_SEC_FRAMING4     0x2130
     *   DP0_DP_MSA_TIMING_PARAM4   0x214f   DIG0_DIG_FE_CNTL        0x208b
     * All BASE_IDX 2. Note they are NOT contiguous and NOT in enum order -
     * which is why each one is written out here. */
    {
        static const uint32_t want[RB_STRENC_REG_COUNT] = {
            0x2109, 0x210a, 0x210e, 0x214c, 0x214d, 0x214e, 0x214f,
            0x2130, 0x210c, 0x210d, 0x2110, 0x2111, 0x2112, 0x208b
        };
        unsigned r;
        for (r = 0; r < RB_STRENC_REG_COUNT; r++) {
            if (rb_strenc_reg_base((enum rb_strenc_reg)r) != want[r]) {
                printf("FAIL strenc: reg[%u] base = 0x%x want 0x%x\n",
                       r, rb_strenc_reg_base((enum rb_strenc_reg)r), want[r]);
                bad++;
            }
        }
        /* An out-of-range slot must refuse, not fall through to a neighbour. */
        if (rb_strenc_reg_base((enum rb_strenc_reg)RB_STRENC_REG_COUNT)
                != RB_STRENC_DW_INVALID) {
            printf("FAIL strenc: an out-of-range slot was answered\n");
            bad++;
        }
        if (RB_STRENC_REG_COUNT != 14) {
            printf("FAIL strenc: REG_COUNT is %d, not 14 - a slot was added "
                   "without its offset case\n", (int)RB_STRENC_REG_COUNT);
            bad++;
        }
    }

    /* Per-instance addresses, pinned for every instance of two registers at
     * opposite ends of the map - the highest (SEC_FRAMING4) and the lowest
     * (DIG_FE_CNTL), since a stride defect shows up at the ends. */
    {
        static const uint32_t sec[RB_STRENC_COUNT] =
            { 0x2130, 0x2230, 0x2330, 0x2430, 0x2530 };
        static const uint32_t fe[RB_STRENC_COUNT] =
            { 0x208b, 0x218b, 0x228b, 0x238b, 0x248b };
        unsigned i;
        for (i = 0; i < RB_STRENC_COUNT; i++) {
            if (rb_strenc_dw(i, RB_STRENC_R_SEC_FRAMING4) != sec[i]) {
                printf("FAIL strenc: SEC_FRAMING4[%u] = 0x%x want 0x%x\n",
                       i, rb_strenc_dw(i, RB_STRENC_R_SEC_FRAMING4), sec[i]);
                bad++;
            }
            if (rb_strenc_dw(i, RB_STRENC_R_DIG_FE_CNTL) != fe[i]) {
                printf("FAIL strenc: DIG_FE_CNTL[%u] = 0x%x want 0x%x\n",
                       i, rb_strenc_dw(i, RB_STRENC_R_DIG_FE_CNTL), fe[i]);
                bad++;
            }
        }
        /* The Type-C instance by name, because every experiment on this line
         * is about instance 2 specifically and a silent off-by-one there would
         * program DIG3. */
        if (rb_strenc_dw(RB_STRENC_TYPEC_INST,
                          RB_STRENC_R_VID_STREAM_CNTL) != 0x230cu) {
            printf("FAIL strenc: DIG2 VID_STREAM_CNTL = 0x%x want 0x230c\n",
                   rb_strenc_dw(RB_STRENC_TYPEC_INST,
                                 RB_STRENC_R_VID_STREAM_CNTL));
            bad++;
        }
        if (RB_STRENC_PANEL_INST != 0u || RB_STRENC_TYPEC_INST != 2u) {
            printf("FAIL strenc: panel/typec instance constants moved\n");
            bad++;
        }
        /* The stride is asserted CONSISTENT with the pinned values, never used
         * to derive them: this holds only because both tables above are
         * literal. */
        if (sec[1] - sec[0] != RB_STRENC_INST_STRIDE ||
            fe[4] - fe[3] != RB_STRENC_INST_STRIDE) {
            printf("FAIL strenc: the pinned offsets contradict the stride\n");
            bad++;
        }
        /* Out of range refuses for every register, not just the first. */
        if (rb_strenc_dw(RB_STRENC_COUNT, RB_STRENC_R_VID_STREAM_CNTL)
                != RB_STRENC_DW_INVALID ||
            rb_strenc_dw(RB_STRENC_COUNT, RB_STRENC_R_DIG_FE_CNTL)
                != RB_STRENC_DW_INVALID) {
            printf("FAIL strenc: an out-of-range instance was answered\n");
            bad++;
        }
    }

    /* THE BOUND, and this case has already paid for itself: the first draft
     * of rb_strenc.h computed it from DP_SEC_FRAMING4 (0x2130), which looks
     * like the top of the block and is not - the four DP_MSA_TIMING_PARAM
     * registers sit at 0x214c..0x214f, above it. The pinned value below and
     * the derived maximum under it caught that before any boot. The value is
     * written out rather than computed, so a bound taken from the wrong slot
     * fails here instead of at a dereference (rule 19). */
    {
        unsigned i, r;
        uint32_t hi = 0;

        if (rb_strenc_dw_max() != 0x254fu) {
            printf("FAIL strenc: dw_max = 0x%x want 0x254f\n",
                   rb_strenc_dw_max());
            bad++;
        }
        /* And it really is the maximum over every (instance, register) this
         * file will read - the check that catches a slot added later with a
         * higher address and no bound update. */
        for (i = 0; i < RB_STRENC_COUNT; i++) {
            for (r = 0; r < RB_STRENC_REG_COUNT; r++) {
                const uint32_t dw = rb_strenc_dw(i, (enum rb_strenc_reg)r);
                if (dw > hi) { hi = dw; }
            }
        }
        if (hi != rb_strenc_dw_max()) {
            printf("FAIL strenc: dw_max 0x%x is not the true maximum 0x%x\n",
                   rb_strenc_dw_max(), hi);
            bad++;
        }
    }

    /* The fields, each in isolation so a swapped pair cannot pass. */
    {
        if (!rb_strenc_enabled(0x00000001u) ||
            rb_strenc_status(0x00000001u)) {
            printf("FAIL strenc: ENABLE alone decoded wrong\n");
            bad++;
        }
        if (!rb_strenc_status(0x00010000u) ||
            rb_strenc_enabled(0x00010000u)) {
            printf("FAIL strenc: STATUS alone decoded wrong\n");
            bad++;
        }
        /* Neither may pick up a neighbouring bit. DIS_DEFER is bit 3 and the
         * field between them is wide and undecoded. */
        if (rb_strenc_enabled(0xfffffffeu & ~0x00010000u) ||
            rb_strenc_status(0xfffeffffu & ~0x00000001u)) {
            printf("FAIL strenc: a neighbouring bit leaked into ENABLE or "
                   "STATUS\n");
            bad++;
        }
        /* HTOTAL is the high half, VTOTAL the low: the panel's own 2106/1129
         * packed as the chip packs them. Getting these the wrong way round
         * would make the base check pass on a square mode and fail on every
         * real one. */
        if (rb_strenc_htotal(0x083a0469u) != 0x083au ||
            rb_strenc_vtotal(0x083a0469u) != 0x0469u) {
            printf("FAIL strenc: HTOTAL/VTOTAL halves swapped\n");
            bad++;
        }
        if (rb_strenc_htotal(0x083a0469u) != 2106u ||
            rb_strenc_vtotal(0x083a0469u) != 1129u) {
            printf("FAIL strenc: the panel's own totals do not decode\n");
            bad++;
        }
        if (rb_strenc_source_otg(0x00000003u) != 3u ||
            rb_strenc_source_otg(0xfffffff8u) != 0u) {
            printf("FAIL strenc: DIG_SOURCE_SELECT decoded wrong\n");
            bad++;
        }
        if (!rb_strenc_dig_started(0x00000400u) ||
            rb_strenc_dig_started(0x000003ffu)) {
            printf("FAIL strenc: DIG_START decoded wrong\n");
            bad++;
        }
    }

    /* THE FIVE-WAY VERDICT. Each state pinned, and the two that exist only to
     * stop a conflation are pinned hardest. */
    {
        if (rb_strenc_verdict(0x00010001u) != RB_STRENC_RUNNING) {
            printf("FAIL strenc: enable+status is not RUNNING\n");
            bad++;
        }
        /* The one that matters after a write: asked for, not granted. It must
         * NOT read as RUNNING (which would be believing our own write) and
         * must NOT read as OFF (which would hide the refusal). */
        if (rb_strenc_verdict(0x00000001u) != RB_STRENC_ASKED_NOT_RUNNING) {
            printf("FAIL strenc: enable-without-status is not "
                   "ASKED_NOT_RUNNING\n");
            bad++;
        }
        if (rb_strenc_verdict(0x00010000u)
                != RB_STRENC_STATUS_WITHOUT_ENABLE) {
            printf("FAIL strenc: status-without-enable is not its own code\n");
            bad++;
        }
        if (rb_strenc_verdict(0x00000000u) != RB_STRENC_OFF) {
            printf("FAIL strenc: both clear is not OFF\n");
            bad++;
        }
        /* An unsampled slot must not read as any real state, and OFF must not
         * BE the unsampled code - "no stream" and "never looked" are two
         * facts (rule 14). */
        if (RB_STRENC_NOT_SAMPLED != 0 ||
            RB_STRENC_OFF == RB_STRENC_NOT_SAMPLED) {
            printf("FAIL strenc: NOT_SAMPLED collides with OFF\n");
            bad++;
        }
        /* All five codes distinct, so none can be folded later. */
        if (RB_STRENC_RUNNING == RB_STRENC_ASKED_NOT_RUNNING ||
            RB_STRENC_ASKED_NOT_RUNNING == RB_STRENC_OFF ||
            RB_STRENC_STATUS_WITHOUT_ENABLE == RB_STRENC_OFF) {
            printf("FAIL strenc: two verdict codes collide\n");
            bad++;
        }
    }

    /* THE BASE CHECK, the gate on everything else. */
    {
        const uint32_t panel = (2106u << 16) | 1129u;

        if (rb_strenc_base_verdict(panel, 2106u, 1129u)
                != RB_STRENC_BASE_CONFIRMED) {
            printf("FAIL strenc: the panel's own timing is not CONFIRMED\n");
            bad++;
        }
        /* H_TOTAL does not move with DRR, so a mismatch there is an address
         * fault and must be reported as the strong code. */
        if (rb_strenc_base_verdict(panel, 2107u, 1129u)
                != RB_STRENC_BASE_HTOTAL_BAD) {
            printf("FAIL strenc: a wrong H_TOTAL is not HTOTAL_BAD\n");
            bad++;
        }
        /* V_TOTAL inside the DRR window still confirms - this is the 2FB
         * lesson, where 143.83 Hz was taken as true while the panel sat at
         * 61.40 and a measurement was misconverted. */
        if (rb_strenc_base_verdict(panel, 2106u, 1129u + 300u)
                != RB_STRENC_BASE_CONFIRMED) {
            printf("FAIL strenc: a DRR-shifted V_TOTAL did not confirm\n");
            bad++;
        }
        if (rb_strenc_base_verdict(panel, 2106u,
                                    1129u + RB_STRENC_VTOTAL_SLACK + 1u)
                != RB_STRENC_BASE_VTOTAL_ODD) {
            printf("FAIL strenc: a V_TOTAL past the window is not "
                   "VTOTAL_ODD\n");
            bad++;
        }
        /* The window is symmetric: a V_TOTAL BELOW the live one by more than
         * the slack is just as odd, and an unsigned subtraction in the wrong
         * order is the classic way that case passes. */
        if (rb_strenc_base_verdict((2106u << 16) | 1u, 2106u, 1129u)
                != RB_STRENC_BASE_VTOTAL_ODD) {
            printf("FAIL strenc: the V_TOTAL window is not symmetric\n");
            bad++;
        }
        /* Zero is its own code and must never confirm, even against a zero
         * expectation - which is why the NO_EXPECTED test comes first in the
         * function. */
        if (rb_strenc_base_verdict(0u, 2106u, 1129u)
                != RB_STRENC_BASE_ZERO) {
            printf("FAIL strenc: a zero word is not BASE_ZERO\n");
            bad++;
        }
        if (rb_strenc_base_verdict(0u, 0u, 0u) != RB_STRENC_BASE_NO_EXPECTED ||
            rb_strenc_base_verdict(panel, 0u, 1129u)
                != RB_STRENC_BASE_NO_EXPECTED ||
            rb_strenc_base_verdict(panel, 2106u, 0u)
                != RB_STRENC_BASE_NO_EXPECTED) {
            printf("FAIL strenc: a missing reference is not NO_EXPECTED\n");
            bad++;
        }
        if (RB_STRENC_BASE_NOT_SAMPLED != 0 ||
            RB_STRENC_BASE_CONFIRMED == 0) {
            printf("FAIL strenc: BASE_NOT_SAMPLED/CONFIRMED numbering\n");
            bad++;
        }
    }

    /* The sweep and the running mask. Unlike rb_dio.h, UNIFORM here is a
     * POSITIVE DEFECT: instance 0 runs a stream and the others do not, so five
     * identical words mean the instance arithmetic did nothing. */
    {
        uint32_t c[RB_STRENC_COUNT] = { 0, 0, 0, 0, 0 };

        if (rb_strenc_sweep_verdict(c, RB_STRENC_COUNT)
                != RB_STRENC_SWEEP_UNIFORM) {
            printf("FAIL strenc: identical words are not UNIFORM\n");
            bad++;
        }
        /* The expected live sample: the panel running, nothing else. */
        c[0] = 0x00010001u;
        if (rb_strenc_sweep_verdict(c, RB_STRENC_COUNT)
                != RB_STRENC_SWEEP_DIFFERS) {
            printf("FAIL strenc: the panel-only sample is not DIFFERS\n");
            bad++;
        }
        if (rb_strenc_running_mask(c, RB_STRENC_COUNT) != 0x01u) {
            printf("FAIL strenc: running mask = 0x%x want 0x01\n",
                   rb_strenc_running_mask(c, RB_STRENC_COUNT));
            bad++;
        }
        /* A mask, not a count and not an index - 2FB's two misreads. Bit 2
         * must be bit 2 and not the value 2. */
        c[2] = 0x00010001u;
        if (rb_strenc_running_mask(c, RB_STRENC_COUNT) != 0x05u) {
            printf("FAIL strenc: mask = 0x%x want 0x05 (bits 0 and 2)\n",
                   rb_strenc_running_mask(c, RB_STRENC_COUNT));
            bad++;
        }
        /* ASKED_NOT_RUNNING must NOT set the mask: that is the whole point of
         * keeping ENABLE and STATUS apart. */
        c[2] = 0x00000001u;
        if (rb_strenc_running_mask(c, RB_STRENC_COUNT) != 0x01u) {
            printf("FAIL strenc: an enable-without-status set the running "
                   "mask\n");
            bad++;
        }
        /* A difference on instance 0 specifically - the word the sweep
         * compares everything against, so a case that only ever varies the
         * others can pass for the wrong reason (rule 48, the hole the 2FC
         * break-it run found). */
        c[0] = 0x00000000u; c[1] = 0x00000000u;
        c[2] = 0x00000000u; c[3] = 0x00000000u; c[4] = 0x00000000u;
        c[0] = 0x00010001u;
        if (rb_strenc_sweep_verdict(c, RB_STRENC_COUNT)
                != RB_STRENC_SWEEP_DIFFERS) {
            printf("FAIL strenc: a difference on instance 0 is not DIFFERS\n");
            bad++;
        }
        if (rb_strenc_sweep_verdict(0, RB_STRENC_COUNT)
                != RB_STRENC_SWEEP_BAD_ARGS ||
            rb_strenc_sweep_verdict(c, 0) != RB_STRENC_SWEEP_BAD_ARGS ||
            rb_strenc_sweep_verdict(c, RB_STRENC_COUNT + 1u)
                != RB_STRENC_SWEEP_BAD_ARGS) {
            printf("FAIL strenc: bad args not refused by the sweep\n");
            bad++;
        }
        if (rb_strenc_running_mask(0, RB_STRENC_COUNT) != 0u) {
            printf("FAIL strenc: NULL did not give a zero mask\n");
            bad++;
        }
        if (RB_STRENC_SWEEP_NOT_SAMPLED != 0 ||
            RB_STRENC_SWEEP_DIFFERS == 0) {
            printf("FAIL strenc: sweep NOT_SAMPLED/DIFFERS numbering\n");
            bad++;
        }
    }

    if (!bad) {
        printf("ok strenc: %u instances x %d registers, the base check and "
               "the ENABLE/STATUS split\n",
               RB_STRENC_COUNT, (int)RB_STRENC_REG_COUNT);
    }
    return bad;
}
