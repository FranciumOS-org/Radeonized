/* 305: the stream write list, in its two phases.
 *
 * THE SPLIT IS THE SUBJECT OF THIS FILE. Upstream has two functions -
 * dp_set_stream_attribute and dp_unblank - and LINK TRAINING HAPPENS BETWEEN
 * THEM. 305 shipped one nineteen-write list ending in DP_VID_STREAM_ENABLE 1,
 * and boot 477 paid for it: with the stream enabled, dptrain refuses with
 * RB_DPTX_W_STREAM_LIVE (66). So the load-bearing check here is not that the
 * writes are right - it is that the ATTRIBUTE phase CANNOT enable the stream.
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "rb_strprog.h"

int strprog_tests(void);

/* Checks that must hold of EVERY write in EVERY phase. */
static unsigned strprog_check_common(const struct rb_strprog_plan *p,
                                     const char *tag)
{
    const uint32_t lo = RB_STRENC_TYPEC_INST * RB_STRENC_INST_STRIDE;
    unsigned bad = 0;
    uint32_t i;

    if (p->n == 0u) {
        printf("FAIL strprog/%s: an accepted build produced no writes\n", tag);
        return 1;
    }
    for (i = 0; i < p->n; i++) {
        const struct rb_strprog_write *w = &p->w[i];
        const uint32_t rel = w->dw - lo;

        /* Every write must land inside instance 2's own block, and NONE may
         * touch instance 0's. The panel-by-name rule as an address range. */
        if (w->dw < lo) {
            printf("FAIL strprog/%s: write %u at 0x%x is below instance 2's "
                   "block - it could be the panel's\n", tag, i, w->dw);
            bad++;
        }
        if (rel != RB_STRENC_DW_MSA_PARAM1 &&
            rel != RB_STRENC_DW_VID_STREAM_CNTL &&
            rel != RB_STRENC_DW_DIG_FE_CNTL &&
            rel != RB_STRENC_DW_STEER_FIFO &&
            rel != RB_STRENC_DW_PIXEL_FORMAT &&
            rel != RB_STRENC_DW_MSA_COLORIM &&
            rel != RB_STRENC_DW_MSA_MISC &&
            rel != RB_STRENC_DW_MSA_PARAM2 &&
            rel != RB_STRENC_DW_MSA_PARAM3 &&
            rel != RB_STRENC_DW_MSA_PARAM4 &&
            rel != RB_STRENC_DW_SEC_FRAMING4 &&
            rel != RB_STRENC_DW_VID_TIMING &&
            rel != RB_STRENC_DW_VID_N &&
            rel != RB_STRENC_DW_VID_M) {
            printf("FAIL strprog/%s: write %u targets 0x%x, which is not a "
                   "register this file is allowed to touch\n", tag, i, w->dw);
            bad++;
        }
        if (w->mask == 0u) {
            printf("FAIL strprog/%s: write %u has an empty mask\n", tag, i);
            bad++;
        }
        if ((w->value & ~w->mask) != 0u) {
            printf("FAIL strprog/%s: write %u has value bits outside its "
                   "mask\n", tag, i);
            bad++;
        }
        if (w->what == 0 || w->what[0] == 0) {
            printf("FAIL strprog/%s: write %u has no description - every path "
                   "out must say something\n", tag, i);
            bad++;
        }
        /* DIS_DEFER is RETIRED. Nothing may write it, however tempting
         * DIG2's 0-against-2 looks: dcn20_stream_encoder.c:541 says the
         * register has no effect on the enable transition. */
        if (rel == RB_STRENC_DW_VID_STREAM_CNTL && (w->mask & 0x300u)) {
            printf("FAIL strprog/%s: write %u touches "
                   "DP_VID_STREAM_DIS_DEFER, which is retired\n", tag, i);
            bad++;
        }
    }

    /* The returned wait points must be inside the list and point at the
     * stream-control register, whichever phase put them there. */
    /* THE BOUND FIRST, then the value. Reading w[idx] to find out whether
     * idx is in range is the check doing the thing it exists to prevent -
     * and the break-it sweep is where that came out. */
    if (p->wait_off_idx >= 0) {
        if ((uint32_t)p->wait_off_idx >= p->n) {
            printf("FAIL strprog/%s: wait_off_idx %d is outside a %u-write "
                   "list\n", tag, p->wait_off_idx, p->n);
            bad++;
        } else {
            const struct rb_strprog_write *w = &p->w[p->wait_off_idx];
            if ((w->dw - lo) != RB_STRENC_DW_VID_STREAM_CNTL ||
                (w->value & RB_STRPROG_M_STREAM_ENABLE) != 0u) {
                printf("FAIL strprog/%s: wait_off_idx does not point at an "
                       "ENABLE-0 write\n", tag);
                bad++;
            }
        }
    }
    if (p->wait_on_idx >= 0) {
        if ((uint32_t)p->wait_on_idx >= p->n) {
            printf("FAIL strprog/%s: wait_on_idx %d is outside a %u-write "
                   "list\n", tag, p->wait_on_idx, p->n);
            bad++;
        } else {
            const struct rb_strprog_write *w = &p->w[p->wait_on_idx];
            if ((w->dw - lo) != RB_STRENC_DW_VID_STREAM_CNTL ||
                (w->value & RB_STRPROG_M_STREAM_ENABLE) == 0u) {
                printf("FAIL strprog/%s: wait_on_idx does not point at an "
                       "ENABLE-1 write\n", tag);
                bad++;
            }
        }
    }
    return bad;
}

int strprog_tests(void)
{
    const uint32_t lo = RB_STRENC_TYPEC_INST * RB_STRENC_INST_STRIDE;
    unsigned bad = 0;
    struct rb_strprog_plan att, unb;
    struct rb_strenc_msa msa;
    uint32_t i;
    int rc;

    /* Boot 474's own derived words for the panel; the values do not matter to
     * the list's shape, only that they are carried through unmodified. */
    msa.param1 = 0x083b046au;
    msa.param2 = 0x007f0014u;
    msa.param3 = 0x003c8005u;
    msa.param4 = 0x07800438u;

    rc = rb_strprog_build(RB_STRENC_TYPEC_INST, 1u, &msa, 0u,
                          (int)RB_STRENC_DERIVE_OK, 9011u,
                          RB_STRPROG_PHASE_ATTRIBUTE, &att);
    if (rc != RB_STRPROG_OK) {
        printf("FAIL strprog: the licensed ATTRIBUTE build was refused (%d)\n",
               rc);
        return ++bad;
    }
    rc = rb_strprog_build(RB_STRENC_TYPEC_INST, 1u, &msa, 0u,
                          (int)RB_STRENC_DERIVE_OK, 9011u,
                          RB_STRPROG_PHASE_UNBLANK, &unb);
    if (rc != RB_STRPROG_OK) {
        printf("FAIL strprog: the licensed UNBLANK build was refused (%d)\n",
               rc);
        return ++bad;
    }

    bad += strprog_check_common(&att, "attribute");
    bad += strprog_check_common(&unb, "unblank");

    /* ================================================================
     * THE GUARD BOOT 477 BOUGHT: the ATTRIBUTE phase must not, by any
     * write, leave DP_VID_STREAM_ENABLE set. If it does, dptrain refuses
     * 66 and the chain locks out its own next step.
     * ================================================================ */
    for (i = 0; i < att.n; i++) {
        if ((att.w[i].dw - lo) == RB_STRENC_DW_VID_STREAM_CNTL &&
            (att.w[i].value & RB_STRPROG_M_STREAM_ENABLE)) {
            printf("FAIL strprog: the ATTRIBUTE phase ENABLES the stream at "
                   "write %u - the link can no longer be trained (boot 477, "
                   "dptrain refusal 66)\n", i);
            bad++;
        }
    }
    /* And it must END by writing it to 0 explicitly: leaving it alone is not
     * the same, because a previous run (or boot 477's own single list) may
     * have left it set. */
    if (att.n == 0u ||
        (att.w[att.n - 1u].dw - lo) != RB_STRENC_DW_VID_STREAM_CNTL ||
        (att.w[att.n - 1u].value & RB_STRPROG_M_STREAM_ENABLE) != 0u ||
        att.wait_off_idx != (int)(att.n - 1u) ||
        att.wait_on_idx != -1) {
        printf("FAIL strprog: the ATTRIBUTE phase does not end by disabling "
               "the stream and waiting on it (off %d on %d of %u)\n",
               att.wait_off_idx, att.wait_on_idx, att.n);
        bad++;
    }

    /* The UNBLANK phase is the half that turns it on: ENABLE 1 last, with
     * the STATUS read pointed at it, and ENABLE 0 + wait first, as upstream
     * does before it touches the FIFO. */
    if (unb.n == 0u ||
        (unb.w[unb.n - 1u].dw - lo) != RB_STRENC_DW_VID_STREAM_CNTL ||
        (unb.w[unb.n - 1u].value & RB_STRPROG_M_STREAM_ENABLE) == 0u ||
        unb.wait_on_idx != (int)(unb.n - 1u) ||
        unb.wait_off_idx != 0) {
        printf("FAIL strprog: the UNBLANK phase does not bracket the stream "
               "(off %d on %d of %u)\n",
               unb.wait_off_idx, unb.wait_on_idx, unb.n);
        bad++;
    }

    /* ---- what belongs to which phase ---- */
    /* The OTG connect must come FIRST and belongs to ATTRIBUTE only: a
     * stream configured before its front end is pointed at a pipe is
     * configured against the wrong raster. */
    if ((att.w[0].dw - lo) != RB_STRENC_DW_DIG_FE_CNTL ||
        att.w[0].mask != RB_STRPROG_M_DIG_SOURCE_SEL ||
        att.w[0].value != 1u) {
        printf("FAIL strprog: the OTG connect is not the first ATTRIBUTE "
               "write, or does not carry tg_inst as a plain index\n");
        bad++;
    }
    for (i = 0; i < unb.n; i++) {
        if ((unb.w[i].dw - lo) == RB_STRENC_DW_DIG_FE_CNTL &&
            (unb.w[i].mask & RB_STRPROG_M_DIG_SOURCE_SEL)) {
            printf("FAIL strprog: UNBLANK re-selects the OTG source - the "
                   "connect belongs to the configure phase\n");
            bad++;
        }
    }
    /* The MSA words belong to ATTRIBUTE, and must be carried byte for byte
     * as REG_SET (whole), because upstream zeroes those registers. */
    {
        unsigned seen1 = 0;
        for (i = 0; i < att.n; i++) {
            const uint32_t rel = att.w[i].dw - lo;
            if (rel == RB_STRENC_DW_MSA_PARAM1) {
                seen1++;
                if (att.w[i].value != msa.param1 || !att.w[i].whole) {
                    printf("FAIL strprog: PARAM1 not carried whole\n");
                    bad++;
                }
            }
            if (rel == RB_STRENC_DW_MSA_PARAM3 &&
                att.w[i].value != msa.param3) {
                printf("FAIL strprog: PARAM3 was modified in transit\n");
                bad++;
            }
        }
        if (seen1 != 1u) {
            printf("FAIL strprog: PARAM1 written %u times in ATTRIBUTE, want "
                   "once\n", seen1);
            bad++;
        }
        for (i = 0; i < unb.n; i++) {
            const uint32_t rel = unb.w[i].dw - lo;
            if (rel == RB_STRENC_DW_MSA_PARAM1 ||
                rel == RB_STRENC_DW_MSA_PARAM2 ||
                rel == RB_STRENC_DW_MSA_PARAM3 ||
                rel == RB_STRENC_DW_MSA_PARAM4) {
                printf("FAIL strprog: UNBLANK rewrites an MSA word - the "
                       "attribute phase owns those\n");
                bad++;
            }
        }
    }
    /* M/N and the FIFO pulse belong to UNBLANK, and must be absent from
     * ATTRIBUTE: priming M before training would prime it against a link
     * that is not up. */
    for (i = 0; i < att.n; i++) {
        const uint32_t rel = att.w[i].dw - lo;
        if (rel == RB_STRENC_DW_VID_M || rel == RB_STRENC_DW_VID_N ||
            rel == RB_STRENC_DW_VID_TIMING ||
            rel == RB_STRENC_DW_STEER_FIFO) {
            printf("FAIL strprog: ATTRIBUTE write %u is an UNBLANK step "
                   "(0x%x)\n", i, rel);
            bad++;
        }
    }
    {
        unsigned okm = 0, okn = 0;
        for (i = 0; i < unb.n; i++) {
            const uint32_t rel = unb.w[i].dw - lo;
            if (rel == RB_STRENC_DW_VID_M && unb.w[i].value == 9011u) { okm++; }
            if (rel == RB_STRENC_DW_VID_N &&
                unb.w[i].value == RB_STRENC_N_VID) { okn++; }
        }
        if (okm != 1u || okn != 1u) {
            printf("FAIL strprog: m_vid/n_vid did not reach their registers "
                   "in UNBLANK (%u/%u)\n", okm, okn);
            bad++;
        }
    }
    /* GEN_EN must be dropped BEFORE M/N go in and raised AFTER - the order
     * is the whole reason M is primed rather than programmed. */
    {
        int idx_gen_off = -1, idx_m = -1, idx_gen_on = -1;
        for (i = 0; i < unb.n; i++) {
            const uint32_t rel = unb.w[i].dw - lo;
            if (rel == RB_STRENC_DW_VID_TIMING) {
                if ((unb.w[i].value & RB_STRPROG_M_VID_M_N_GEN_EN) == 0u &&
                    idx_gen_off < 0) { idx_gen_off = (int)i; }
                else if (unb.w[i].value & RB_STRPROG_M_VID_M_N_GEN_EN) {
                    idx_gen_on = (int)i;
                }
            }
            if (rel == RB_STRENC_DW_VID_M) { idx_m = (int)i; }
        }
        if (idx_gen_off < 0 || idx_m < 0 || idx_gen_on < 0 ||
            !(idx_gen_off < idx_m && idx_m < idx_gen_on)) {
            printf("FAIL strprog: GEN_EN/M ordering is wrong (%d, %d, %d)\n",
                   idx_gen_off, idx_m, idx_gen_on);
            bad++;
        }
    }
    /* The steer FIFO must be pulsed 1 then 0, in that order, and must END at
     * 0 - that is what clears DIG2's held reset, by construction. */
    {
        int first = -1, last = -1;
        for (i = 0; i < unb.n; i++) {
            if ((unb.w[i].dw - lo) != RB_STRENC_DW_STEER_FIFO) { continue; }
            if (first < 0) { first = (int)i; }
            last = (int)i;
        }
        if (first < 0 || first == last) {
            printf("FAIL strprog: the steer FIFO was not pulsed\n");
            bad++;
        } else if (unb.w[first].value == 0u || unb.w[last].value != 0u) {
            printf("FAIL strprog: the FIFO pulse does not go 1 then 0 - "
                   "DIG2's held reset would survive it\n");
            bad++;
        } else if (unb.w[first].delay_us == 0u) {
            printf("FAIL strprog: the FIFO reset has no settling delay\n");
            bad++;
        }
    }
    /* The FIFO must be reset only AFTER the stream has been taken down - that
     * is why upstream waits on STATUS before touching it. */
    {
        int fifo = -1;
        for (i = 0; i < unb.n; i++) {
            if ((unb.w[i].dw - lo) == RB_STRENC_DW_STEER_FIFO) {
                fifo = (int)i; break;
            }
        }
        if (fifo >= 0 && fifo < unb.wait_off_idx) {
            printf("FAIL strprog: the FIFO is reset before the stream is "
                   "disabled and waited on\n");
            bad++;
        }
    }

    /* ---- every refusal, and each its own code ---- */
    {
        struct rb_strprog_plan p;
        if (rb_strprog_build(RB_STRENC_PANEL_INST, 1u, &msa, 0u,
                (int)RB_STRENC_DERIVE_OK, 9011u,
                RB_STRPROG_PHASE_ATTRIBUTE, &p) != RB_STRPROG_E_PANEL ||
            p.n != 0u) {
            printf("FAIL strprog: DIG0 was not refused BY NAME\n");
            bad++;
        }
        /* A refused build must leave NO wait points either: a caller reading
         * them off a refused plan would wait on a write that is not there. */
        if (p.wait_off_idx != -1 || p.wait_on_idx != -1) {
            printf("FAIL strprog: a refused build left its wait points set\n");
            bad++;
        }
        if (rb_strprog_build(RB_STRENC_COUNT, 1u, &msa, 0u,
                (int)RB_STRENC_DERIVE_OK, 9011u,
                RB_STRPROG_PHASE_ATTRIBUTE, &p) != RB_STRPROG_E_INST) {
            printf("FAIL strprog: an out-of-range instance was accepted\n");
            bad++;
        }
        if (rb_strprog_build(2u, 0u, &msa, 0u, (int)RB_STRENC_DERIVE_OK,
                9011u, RB_STRPROG_PHASE_ATTRIBUTE, &p) != RB_STRPROG_E_OTG0) {
            printf("FAIL strprog: OTG0 was not refused BY NAME\n");
            bad++;
        }
        /* OTG0 must be refused in the UNBLANK half too: a refusal that only
         * guards one entry point is one entry point from not existing. */
        if (rb_strprog_build(2u, 0u, &msa, 0u, (int)RB_STRENC_DERIVE_OK,
                9011u, RB_STRPROG_PHASE_UNBLANK, &p) != RB_STRPROG_E_OTG0 ||
            rb_strprog_build(RB_STRENC_PANEL_INST, 1u, &msa, 0u,
                (int)RB_STRENC_DERIVE_OK, 9011u,
                RB_STRPROG_PHASE_UNBLANK, &p) != RB_STRPROG_E_PANEL) {
            printf("FAIL strprog: the panel is not refused in BOTH halves\n");
            bad++;
        }
        if (rb_strprog_build(2u, 8u, &msa, 0u, (int)RB_STRENC_DERIVE_OK,
                9011u, RB_STRPROG_PHASE_ATTRIBUTE, &p) != RB_STRPROG_E_OTG) {
            printf("FAIL strprog: a tg_inst outside the field was accepted\n");
            bad++;
        }
        /* An unknown phase is its own refusal, not a silent fall-through to
         * whichever half the enum happens to put first. */
        if (rb_strprog_build(2u, 1u, &msa, 0u, (int)RB_STRENC_DERIVE_OK,
                9011u, (enum rb_strprog_phase)7, &p) != RB_STRPROG_E_PHASE ||
            p.n != 0u) {
            printf("FAIL strprog: an unknown phase was not refused\n");
            bad++;
        }
        /* THE LICENCE, in both halves. A non-zero mismatch means the
         * derivation did not reproduce the panel. */
        if (rb_strprog_build(2u, 1u, &msa, 4u, (int)RB_STRENC_DERIVE_OK,
                9011u, RB_STRPROG_PHASE_ATTRIBUTE, &p) != RB_STRPROG_E_MSA ||
            p.n != 0u ||
            rb_strprog_build(2u, 1u, &msa, 4u, (int)RB_STRENC_DERIVE_OK,
                9011u, RB_STRPROG_PHASE_UNBLANK, &p) != RB_STRPROG_E_MSA) {
            printf("FAIL strprog: a PARAM3 mismatch did not refuse the "
                   "write\n");
            bad++;
        }
        if (rb_strprog_build(2u, 1u, &msa, RB_STRENC_MSA_MISMATCH_ARGS,
                (int)RB_STRENC_DERIVE_OK, 9011u,
                RB_STRPROG_PHASE_ATTRIBUTE, &p) != RB_STRPROG_E_MSA) {
            printf("FAIL strprog: 'could not derive' did not refuse the "
                   "write\n");
            bad++;
        }
        if (rb_strprog_build(2u, 1u, &msa, 0u,
                (int)RB_STRENC_DERIVE_TOTAL_ZERO, 9011u,
                RB_STRPROG_PHASE_ATTRIBUTE, &p) != RB_STRPROG_E_DERIVE) {
            printf("FAIL strprog: a target pipe with no raster was "
                   "accepted\n");
            bad++;
        }
        /* m_vid is only used by UNBLANK, but it is refused in BOTH halves on
         * purpose: a configure that succeeded on a bad m_vid would make the
         * unblank's refusal the first the operator hears of it, after the
         * link has been trained. */
        if (rb_strprog_build(2u, 1u, &msa, 0u, (int)RB_STRENC_DERIVE_OK,
                0u, RB_STRPROG_PHASE_ATTRIBUTE, &p) != RB_STRPROG_E_MVID ||
            rb_strprog_build(2u, 1u, &msa, 0u, (int)RB_STRENC_DERIVE_OK,
                0u, RB_STRPROG_PHASE_UNBLANK, &p) != RB_STRPROG_E_MVID) {
            printf("FAIL strprog: m_vid 0 was accepted\n");
            bad++;
        }
        if (rb_strprog_build(2u, 1u, 0, 0u, (int)RB_STRENC_DERIVE_OK, 9011u,
                RB_STRPROG_PHASE_ATTRIBUTE, &p) != RB_STRPROG_E_ARGS ||
            rb_strprog_build(2u, 1u, &msa, 0u, (int)RB_STRENC_DERIVE_OK,
                9011u, RB_STRPROG_PHASE_ATTRIBUTE, 0) != RB_STRPROG_E_ARGS) {
            printf("FAIL strprog: NULL arguments not refused\n");
            bad++;
        }
        if (RB_STRPROG_OK != 0) {
            printf("FAIL strprog: OK is not zero\n");
            bad++;
        }
    }

    /* The two refusal spaces must not overlap: a published field carries
     * either, and "not armed" reading as "the panel was refused" is rule
     * 14's defect. */
    if (RB_STRPROG_E_PHASE >= RB_STRPROG_W_FIRST ||
        RB_STRPROG_W_NOT_ARMED < RB_STRPROG_W_FIRST ||
        RB_STRPROG_W_OK != RB_STRPROG_OK) {
        printf("FAIL strprog: the builder and caller refusal ranges collide\n");
        bad++;
    }
    /* Both phases must fit the plan's own array - the builder's overflow
     * refusal is the backstop, but a phase that needs more than the array
     * holds is a design error, not a runtime one. */
    if (att.n > RB_STRPROG_MAX_WRITES || unb.n > RB_STRPROG_MAX_WRITES) {
        printf("FAIL strprog: a phase overflows the plan array\n");
        bad++;
    }

    /* ---- the field helper ---- */
    if (rb_strprog_shift(RB_STRPROG_M_COMPONENT_DEPTH) != 24u ||
        rb_strprog_shift(RB_STRPROG_M_DIG_START) != 10u ||
        rb_strprog_shift(RB_STRPROG_M_PIXEL_ENCODING) != 0u) {
        printf("FAIL strprog: a mask shift is wrong\n");
        bad++;
    }
    if (rb_strprog_field(RB_STRPROG_M_COMPONENT_DEPTH, 1u) != 0x01000000u) {
        printf("FAIL strprog: 8bpc does not land where the panel has it\n");
        bad++;
    }
    /* A value too wide for its field must be truncated INTO the field, never
     * allowed to spill into the neighbours. */
    if (rb_strprog_field(RB_STRPROG_M_PIXEL_ENCODING, 0xffu)
            != RB_STRPROG_M_PIXEL_ENCODING) {
        printf("FAIL strprog: an over-wide field value spilled\n");
        bad++;
    }

    /* ---- the ENABLE/STATUS verdict, the witness since 2EB ---- */
    if (rb_strprog_verdict(0x00010001u) != RB_STRPROG_V_RUNNING ||
        rb_strprog_verdict(0x00000001u) != RB_STRPROG_V_NO_STATUS ||
        rb_strprog_verdict(0x00010000u) != RB_STRPROG_V_NOT_ENABLED ||
        rb_strprog_verdict(0u) != RB_STRPROG_V_NOT_ENABLED) {
        printf("FAIL strprog: the ENABLE/STATUS verdict collapsed\n");
        bad++;
    }
    /* ENABLE alone must NOT read as a running stream - that is the whole
     * point of reading bit 16 instead of bit 0, and boot 477's answer. */
    if (rb_strprog_verdict(0x00000001u) == RB_STRPROG_V_RUNNING) {
        printf("FAIL strprog: a request for a stream read as a stream\n");
        bad++;
    }

    if (!bad) {
        printf("ok strprog: %u configure writes then %u unblank writes, the "
               "configure half CANNOT enable the stream, and the link can be "
               "trained between them\n", att.n, unb.n);
    }
    return bad;
}
