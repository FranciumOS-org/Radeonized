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

    /* THE BASE CHECK, the gate on everything else.
     *
     * THE +1 IS UPSTREAM'S AND IS PINNED AGAINST BOOT 472'S OWN WORD. The OTG
     * register holds `total - 1` (dcn10_optc.c:191-193) and MSA holds the true
     * total (dcn10_stream_encoder.c:419-421), so MSA == OTG + 1 on both axes.
     * The first draft of rb_strenc.h compared them raw; boot 472 refused with
     * HTOTAL_BAD, reading 2107/1130 against a live 2106/1129. `measured472` is
     * that word verbatim, so the convention cannot be un-learned by an edit.
     */
    {
        const uint32_t measured472 = 0x083b046au;   /* boot 472, instance 0 */
        const uint32_t panel = (2107u << 16) | 1130u;  /* == measured472 */

        if (panel != measured472) {
            printf("FAIL strenc: the pinned panel word is not boot 472's "
                   "0x083b046a\n");
            bad++;
        }
        /* The live OTG0 values that boot 472 published alongside it. */
        if (rb_strenc_base_verdict(measured472, 2106u, 1129u)
                != RB_STRENC_BASE_CONFIRMED) {
            printf("FAIL strenc: boot 472's own sample does not CONFIRM\n");
            bad++;
        }
        /* And the raw comparison - the defect itself - must NOT confirm. */
        if (rb_strenc_base_verdict((2106u << 16) | 1129u, 2106u, 1129u)
                == RB_STRENC_BASE_CONFIRMED) {
            printf("FAIL strenc: an MSA word equal to the OTG register "
                   "confirmed, so the +1 is not being applied\n");
            bad++;
        }

        if (rb_strenc_base_verdict(panel, 2106u, 1129u)
                != RB_STRENC_BASE_CONFIRMED) {
            printf("FAIL strenc: the panel's own timing is not CONFIRMED\n");
            bad++;
        }
        /* H_TOTAL does not move with DRR, so a mismatch there is an address
         * fault and must be reported as the strong code. */
        if (rb_strenc_base_verdict(panel, 2108u, 1129u)
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
        if (rb_strenc_base_verdict((2107u << 16) | 1u, 2106u, 1129u)
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

        /* THE SECOND CHECK, which cannot drift: the ACTIVE raster. Boot 472
         * read 0x07800438 on instance 0 = 1920 x 1080, the panel exactly, and
         * DRR does not touch it (V_BLANK_START_END is what moves). */
        /* The live active raster, derived from OTG0's blank word rather than
         * from a constant. 0x007f07ff is this panel's own H word: START 2047,
         * END 127, and the blank WRAPS so the active width is START - END.
         * Getting that subtraction backwards is the obvious error and it would
         * underflow to a huge number rather than fail visibly. */
        if (RB_STRENC_DW_OTG0_H_BLANK != 0x1b2bu ||
            RB_STRENC_DW_OTG0_V_BLANK != 0x1b36u) {
            printf("FAIL strenc: the OTG0 blank offsets moved\n");
            bad++;
        }
        if (rb_strenc_active_from_blank(0x007f07ffu) != 1920u) {
            printf("FAIL strenc: the panel's H blank word gives %u, not "
                   "1920\n", rb_strenc_active_from_blank(0x007f07ffu));
            bad++;
        }
        /* END >= START is not a window; it must give 0 so the caller's
         * no-reference path takes over, never a wrapped difference. */
        if (rb_strenc_active_from_blank(0x07ff007fu) != 0u ||
            rb_strenc_active_from_blank(0u) != 0u ||
            rb_strenc_active_from_blank(0x00ff00ffu) != 0u) {
            printf("FAIL strenc: a non-window blank word was not refused\n");
            bad++;
        }
        if (rb_strenc_active_agrees(0x07800438u, 1920u, 1080u) != 1) {
            printf("FAIL strenc: boot 472's active raster does not agree\n");
            bad++;
        }
        if (rb_strenc_active_agrees(0x07800438u, 1921u, 1080u) != 0 ||
            rb_strenc_active_agrees(0x07800438u, 1920u, 1081u) != 0) {
            printf("FAIL strenc: a wrong active size agreed\n");
            bad++;
        }
        /* Halves not swapped: 1080 x 1920 must NOT agree. */
        if (rb_strenc_active_agrees(0x07800438u, 1080u, 1920u) != 0) {
            printf("FAIL strenc: the active halves are swapped\n");
            bad++;
        }
        /* Zero is a disagreement, not an agreement; no reference is -1 and
         * must be distinguishable from both (rule 14). */
        if (rb_strenc_active_agrees(0u, 1920u, 1080u) != 0) {
            printf("FAIL strenc: a zero active word agreed\n");
            bad++;
        }
        if (rb_strenc_active_agrees(0x07800438u, 0u, 1080u) != -1 ||
            rb_strenc_active_agrees(0x07800438u, 1920u, 0u) != -1) {
            printf("FAIL strenc: a missing active reference is not -1\n");
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

    /* ---- 304: the MSA derivation, against boot 473's own sample ----
     *
     * HONESTY NOTE, and it decides what this test may claim. Of the eight
     * OTG words the derivation consumes, boot 473 measured exactly four:
     * H_TOTAL 2106, H_BLANK 0x007f07ff, V_TOTAL 1129, V_BLANK 0x0014044c.
     * The sync widths and the two polarity bits were NOT read; they are
     * back-derived here FROM the MSA words so the case can run at all.
     *
     * So PARAM1, PARAM2 and PARAM4 below are a genuine hardware check -
     * measured OTG in, measured MSA out. PARAM3 is NOT: its inputs came
     * from its expected output, and asserting it against that output is
     * circular (rule 17 - never narrate a substituted value as a hardware
     * result). PARAM3's hardware check is the in-boot control, which reads
     * OTG_H_SYNC_A and OTG_V_SYNC_A for real. What this case pins for
     * PARAM3 is the arithmetic and the field placement, nothing more. */
    {
        struct rb_strenc_raster r;
        struct rb_strenc_msa d;
        enum rb_strenc_derive rc;
        /* boot 473, instance 0, measured: */
        const uint32_t live_param1 = 0x083b046au;
        const uint32_t live_param2 = 0x007f0014u;
        const uint32_t live_param4 = 0x07800438u;
        const uint32_t live_param3 = 0x003c8005u;  /* back-derived inputs */

        r.h_total     = 2106u;          /* measured */
        r.h_blank     = 0x007f07ffu;    /* measured */
        r.v_total     = 1129u;          /* measured */
        r.v_blank     = 0x0014044cu;    /* measured */
        r.h_sync      = (60u << 16);    /* back-derived from PARAM3 */
        r.v_sync      = (5u << 16);     /* back-derived from PARAM3 */
        r.h_sync_cntl = 0u;             /* back-derived: HSYNCPOL 0 */
        r.v_sync_cntl = 1u;             /* back-derived: VSYNCPOL 1 */

        rc = rb_strenc_derive_msa(&r, &d);
        if (rc != RB_STRENC_DERIVE_OK) {
            printf("FAIL strenc: the panel's own raster was refused (%d)\n",
                   (int)rc);
            bad++;
        } else {
            /* The three that are a real hardware check. */
            if (d.param1 != live_param1) {
                printf("FAIL strenc: derived PARAM1 0x%08x != measured "
                       "0x%08x\n", d.param1, live_param1);
                bad++;
            }
            if (d.param2 != live_param2) {
                printf("FAIL strenc: derived PARAM2 0x%08x != measured "
                       "0x%08x - HSTART is H_BLANK_END and VSTART is "
                       "V_BLANK_END\n", d.param2, live_param2);
                bad++;
            }
            if (d.param4 != live_param4) {
                printf("FAIL strenc: derived PARAM4 0x%08x != measured "
                       "0x%08x\n", d.param4, live_param4);
                bad++;
            }
            /* Arithmetic and field placement only - see the note. */
            if (d.param3 != live_param3) {
                printf("FAIL strenc: PARAM3 arithmetic 0x%08x != 0x%08x\n",
                       d.param3, live_param3);
                bad++;
            }
        }

        /* The mismatch mask must be zero on that sample, and must name the
         * individual word when one is wrong. */
        {
            uint32_t live4[4];
            live4[0] = live_param1; live4[1] = live_param2;
            live4[2] = live_param3; live4[3] = live_param4;
            if (rb_strenc_msa_mismatch(&d, live4) != 0u) {
                printf("FAIL strenc: the mask flagged a word that agrees\n");
                bad++;
            }
            live4[2] ^= 1u;
            if (rb_strenc_msa_mismatch(&d, live4) != 4u) {
                printf("FAIL strenc: the mask did not name PARAM3 alone\n");
                bad++;
            }
            live4[2] ^= 1u;
            live4[0] ^= 1u;
            if (rb_strenc_msa_mismatch(&d, live4) != 1u) {
                printf("FAIL strenc: the mask did not name PARAM1 alone\n");
                bad++;
            }
            if (rb_strenc_msa_mismatch(&d, 0) !=
                    RB_STRENC_MSA_MISMATCH_ARGS) {
                printf("FAIL strenc: a NULL live word list was not a "
                       "distinct code\n");
                bad++;
            }
            if (RB_STRENC_MSA_MISMATCH_ARGS <= 0xfu) {
                printf("FAIL strenc: the args code collides with a real "
                       "four-bit mismatch mask\n");
                bad++;
            }
        }

        /* The off-by-one must be LOAD-BEARING: an OTG register already
         * holding the true total must not produce the measured MSA word. */
        {
            struct rb_strenc_msa d2;
            r.h_total = 2107u;
            if (rb_strenc_derive_msa(&r, &d2) == RB_STRENC_DERIVE_OK &&
                d2.param1 == live_param1) {
                printf("FAIL strenc: the +1 is not load-bearing\n");
                bad++;
            }
            r.h_total = 2106u;
        }

        /* Every refusal reachable, and each its own code. */
        {
            struct rb_strenc_msa d2;
            struct rb_strenc_raster q;
            if (rb_strenc_derive_msa(0, &d2) != RB_STRENC_DERIVE_NO_ARGS ||
                rb_strenc_derive_msa(&r, 0) != RB_STRENC_DERIVE_NO_ARGS) {
                printf("FAIL strenc: NULL args not refused\n");
                bad++;
            }
            q = r; q.h_total = 0u;
            if (rb_strenc_derive_msa(&q, &d2) != RB_STRENC_DERIVE_TOTAL_ZERO) {
                printf("FAIL strenc: a zero total was not refused\n");
                bad++;
            }
            q = r; q.v_blank = 0u;
            if (rb_strenc_derive_msa(&q, &d2) != RB_STRENC_DERIVE_BLANK_ZERO) {
                printf("FAIL strenc: a zero blank was not refused\n");
                bad++;
            }
            /* END >= START - the wrap that is not a raster. */
            q = r; q.h_blank = (2047u << 16) | 127u;
            if (rb_strenc_derive_msa(&q, &d2) != RB_STRENC_DERIVE_BLANK_ORDER) {
                printf("FAIL strenc: an inverted blank was not refused\n");
                bad++;
            }
            /* active >= total. */
            q = r; q.h_total = 100u; q.h_blank = (1u << 16) | 2047u;
            if (rb_strenc_derive_msa(&q, &d2) != RB_STRENC_DERIVE_ACTIVE_OVER) {
                printf("FAIL strenc: active over total was not refused\n");
                bad++;
            }
            q = r; q.h_sync = 0u;
            if (rb_strenc_derive_msa(&q, &d2) != RB_STRENC_DERIVE_SYNC_ZERO) {
                printf("FAIL strenc: a zero sync width was not refused\n");
                bad++;
            }
            /* optc writes START = 0; anything else is not its raster. */
            q = r; q.v_sync = (5u << 16) | 3u;
            if (rb_strenc_derive_msa(&q, &d2) !=
                    RB_STRENC_DERIVE_SYNC_START_SET) {
                printf("FAIL strenc: a non-zero sync START was not "
                       "refused\n");
                bad++;
            }
            if (RB_STRENC_DERIVE_OK != 0) {
                printf("FAIL strenc: OK is not zero\n");
                bad++;
            }
        }

        /* A refused derivation must not have touched the output. */
        {
            struct rb_strenc_msa d3;
            struct rb_strenc_raster q = r;
            d3.param1 = 0xdeadbeefu; d3.param2 = 0xdeadbeefu;
            d3.param3 = 0xdeadbeefu; d3.param4 = 0xdeadbeefu;
            q.h_total = 0u;
            (void)rb_strenc_derive_msa(&q, &d3);
            if (d3.param1 != 0xdeadbeefu || d3.param4 != 0xdeadbeefu) {
                printf("FAIL strenc: a refused derivation wrote output\n");
                bad++;
            }
        }

        /* The polarity bits must be independent and in the right halves. */
        {
            struct rb_strenc_msa dh, dv;
            struct rb_strenc_raster q = r;
            q.h_sync_cntl = 1u; q.v_sync_cntl = 0u;
            if (rb_strenc_derive_msa(&q, &dh) != RB_STRENC_DERIVE_OK ||
                !(dh.param3 & RB_STRENC_MSA_HSYNCPOL) ||
                (dh.param3 & RB_STRENC_MSA_VSYNCPOL)) {
                printf("FAIL strenc: HSYNCPOL did not land alone\n");
                bad++;
            }
            q.h_sync_cntl = 0u; q.v_sync_cntl = 1u;
            if (rb_strenc_derive_msa(&q, &dv) != RB_STRENC_DERIVE_OK ||
                (dv.param3 & RB_STRENC_MSA_HSYNCPOL) ||
                !(dv.param3 & RB_STRENC_MSA_VSYNCPOL)) {
                printf("FAIL strenc: VSYNCPOL did not land alone\n");
                bad++;
            }
            /* Only bit 0 of the CNTL word is the polarity. */
            q.h_sync_cntl = 0xfffffffeu; q.v_sync_cntl = 0xfffffffeu;
            if (rb_strenc_derive_msa(&q, &dv) != RB_STRENC_DERIVE_OK ||
                (dv.param3 & RB_STRENC_MSA_HSYNCPOL) ||
                (dv.param3 & RB_STRENC_MSA_VSYNCPOL)) {
                printf("FAIL strenc: a neighbouring CNTL bit leaked into "
                       "the polarity\n");
                bad++;
            }
        }
    }

    /* ---- 304: the unblank's M/N ----
     *
     * The panel is the control here too, and it is a check on the FORMULA,
     * not on a value we would write: DP_VID_M_N_GEN_EN is set on instance 0,
     * so the panel's DP_VID_M is hardware-generated and drifts (0xa2a6 in
     * boot 472, 0xa2a4 in boot 473). The formula must land near it without
     * being asserted equal to it. */
    {
        uint32_t m;
        /* The panel: eDP at HBR, pixel clock ~342.0-343.1 MHz. */
        m = rb_strenc_m_vid(342012u, RB_STRENC_LINK_RATE_HBR);
        if (m < 41000u || m > 42000u) {
            printf("FAIL strenc: m_vid %u is nowhere near the panel's live "
                   "0xa2a4 (41636)\n", m);
            bad++;
        }
        /* The same clock at HBR2 must be half of it, which is what makes
         * the link rate load-bearing rather than decorative. */
        if (rb_strenc_m_vid(342012u, RB_STRENC_LINK_RATE_HBR2) !=
                m / 2u) {
            printf("FAIL strenc: the link rate does not scale m_vid\n");
            bad++;
        }
        /* The sink: 148500 kHz over HBR2. */
        if (rb_strenc_m_vid(148500u, RB_STRENC_LINK_RATE_HBR2) != 9011u) {
            printf("FAIL strenc: m_vid(148500, HBR2) = %u want 9011\n",
                   rb_strenc_m_vid(148500u, RB_STRENC_LINK_RATE_HBR2));
            bad++;
        }
        /* Refusals: 0 is never a valid M. */
        if (rb_strenc_m_vid(0u, RB_STRENC_LINK_RATE_HBR2) != 0u ||
            rb_strenc_m_vid(148500u, 0u) != 0u) {
            printf("FAIL strenc: m_vid did not refuse a zero input\n");
            bad++;
        }
        /* 64-bit: the numerator passes 2^32 well before the field does. */
        if (rb_strenc_m_vid(600000u, RB_STRENC_LINK_RATE_HBR2) != 36408u) {
            printf("FAIL strenc: m_vid overflowed 32 bits\n");
            bad++;
        }
        if (RB_STRENC_N_VID != 0x8000u) {
            printf("FAIL strenc: N_VID moved off 0x8000\n");
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
