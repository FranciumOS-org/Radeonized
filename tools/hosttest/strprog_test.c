/* 305: the stream write list. */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "rb_strprog.h"

int strprog_tests(void);

int strprog_tests(void)
{
    unsigned bad = 0;
    struct rb_strprog_write w[RB_STRPROG_MAX_WRITES];
    struct rb_strenc_msa msa;
    uint32_t n = 0;
    int rc;

    /* Boot 474's own derived words for the panel; the values do not matter to
     * the list's shape, only that they are carried through unmodified. */
    msa.param1 = 0x083b046au;
    msa.param2 = 0x007f0014u;
    msa.param3 = 0x003c8005u;
    msa.param4 = 0x07800438u;

    /* ---- the happy path: DIG2 fed by OTG1 ---- */
    rc = rb_strprog_build(RB_STRENC_TYPEC_INST, 1u, &msa, 0u,
                          (int)RB_STRENC_DERIVE_OK, 9011u,
                          w, RB_STRPROG_MAX_WRITES, &n);
    if (rc != RB_STRPROG_OK) {
        printf("FAIL strprog: the licensed build was refused (%d)\n", rc);
        bad++;
    } else {
        /* Every write must land inside instance 2's own block, and NONE may
         * touch instance 0's. That is the panel-by-name rule expressed as an
         * address range rather than as a comment. */
        const uint32_t lo = RB_STRENC_TYPEC_INST * RB_STRENC_INST_STRIDE;
        for (uint32_t i = 0; i < n; i++) {
            const uint32_t rel = w[i].dw - lo;
            if (w[i].dw < lo) {
                printf("FAIL strprog: write %u at 0x%x is below instance 2's "
                       "block - it could be the panel's\n", i, w[i].dw);
                bad++;
            }
            if (rel == RB_STRENC_DW_MSA_PARAM1 ||
                rel == RB_STRENC_DW_VID_STREAM_CNTL ||
                rel == RB_STRENC_DW_DIG_FE_CNTL ||
                rel == RB_STRENC_DW_STEER_FIFO ||
                rel == RB_STRENC_DW_PIXEL_FORMAT ||
                rel == RB_STRENC_DW_MSA_COLORIM ||
                rel == RB_STRENC_DW_MSA_MISC ||
                rel == RB_STRENC_DW_MSA_PARAM2 ||
                rel == RB_STRENC_DW_MSA_PARAM3 ||
                rel == RB_STRENC_DW_MSA_PARAM4 ||
                rel == RB_STRENC_DW_SEC_FRAMING4 ||
                rel == RB_STRENC_DW_VID_TIMING ||
                rel == RB_STRENC_DW_VID_N ||
                rel == RB_STRENC_DW_VID_M) {
                /* fine */
            } else {
                printf("FAIL strprog: write %u targets 0x%x, which is not a "
                       "register this file is allowed to touch\n", i, w[i].dw);
                bad++;
            }
            if (w[i].mask == 0u) {
                printf("FAIL strprog: write %u has an empty mask\n", i);
                bad++;
            }
            if ((w[i].value & ~w[i].mask) != 0u) {
                printf("FAIL strprog: write %u has value bits outside its "
                       "mask\n", i);
                bad++;
            }
            if (w[i].what == 0 || w[i].what[0] == 0) {
                printf("FAIL strprog: write %u has no description - every "
                       "path out must say something\n", i);
                bad++;
            }
        }

        /* THE TWO HARDCODED WAIT INDEXES MUST POINT AT WHAT THEY CLAIM.
         * A constant naming a position in a list someone will later reorder
         * is exactly the kind that drifts silently. */
        if (n <= RB_STRPROG_WAIT_ON_IDX) {
            printf("FAIL strprog: WAIT_ON_IDX %u is past the end of a "
                   "%u-write list\n", RB_STRPROG_WAIT_ON_IDX, n);
            bad++;
        } else {
            const uint32_t offc = w[RB_STRPROG_WAIT_OFF_IDX].dw - lo;
            const uint32_t onc  = w[RB_STRPROG_WAIT_ON_IDX].dw - lo;
            if (offc != RB_STRENC_DW_VID_STREAM_CNTL ||
                w[RB_STRPROG_WAIT_OFF_IDX].value != 0u) {
                printf("FAIL strprog: WAIT_OFF_IDX does not point at the "
                       "ENABLE-0 write\n");
                bad++;
            }
            if (onc != RB_STRENC_DW_VID_STREAM_CNTL ||
                w[RB_STRPROG_WAIT_ON_IDX].value == 0u) {
                printf("FAIL strprog: WAIT_ON_IDX does not point at the "
                       "ENABLE-1 write\n");
                bad++;
            }
            if (RB_STRPROG_WAIT_ON_IDX != n - 1u) {
                printf("FAIL strprog: the ENABLE-1 write is not last\n");
                bad++;
            }
        }

        /* The OTG connect must come FIRST: a stream configured before its
         * front end is pointed at a pipe is configured against the wrong
         * raster, and index 0 is the cheapest place to say so. */
        if ((w[0].dw - lo) != RB_STRENC_DW_DIG_FE_CNTL ||
            w[0].mask != RB_STRPROG_M_DIG_SOURCE_SEL ||
            w[0].value != 1u) {
            printf("FAIL strprog: the OTG connect is not the first write, or "
                   "does not carry tg_inst as a plain index\n");
            bad++;
        }

        /* The four MSA words must be carried through byte for byte, and as
         * REG_SET (whole), because upstream zeroes those registers. */
        {
            unsigned seen = 0;
            for (uint32_t i = 0; i < n; i++) {
                const uint32_t rel = w[i].dw - lo;
                if (rel == RB_STRENC_DW_MSA_PARAM1) {
                    seen++;
                    if (w[i].value != msa.param1 || !w[i].whole) {
                        printf("FAIL strprog: PARAM1 not carried whole\n");
                        bad++;
                    }
                }
                if (rel == RB_STRENC_DW_MSA_PARAM3 && w[i].value != msa.param3) {
                    printf("FAIL strprog: PARAM3 was modified in transit\n");
                    bad++;
                }
            }
            if (seen != 1u) {
                printf("FAIL strprog: PARAM1 written %u times, want once\n",
                       seen);
                bad++;
            }
        }

        /* m_vid must reach DP_VID_M, and N must be 0x8000. */
        {
            unsigned okm = 0, okn = 0;
            for (uint32_t i = 0; i < n; i++) {
                const uint32_t rel = w[i].dw - lo;
                if (rel == RB_STRENC_DW_VID_M && w[i].value == 9011u) { okm++; }
                if (rel == RB_STRENC_DW_VID_N &&
                    w[i].value == RB_STRENC_N_VID) { okn++; }
            }
            if (okm != 1u || okn != 1u) {
                printf("FAIL strprog: m_vid/n_vid did not reach their "
                       "registers (%u/%u)\n", okm, okn);
                bad++;
            }
        }

        /* GEN_EN must be dropped BEFORE M/N go in and raised AFTER - the
         * order is the whole reason M is primed rather than programmed. */
        {
            int idx_gen_off = -1, idx_m = -1, idx_gen_on = -1;
            for (uint32_t i = 0; i < n; i++) {
                const uint32_t rel = w[i].dw - lo;
                if (rel == RB_STRENC_DW_VID_TIMING) {
                    if ((w[i].value & RB_STRPROG_M_VID_M_N_GEN_EN) == 0u &&
                        idx_gen_off < 0) { idx_gen_off = (int)i; }
                    else if (w[i].value & RB_STRPROG_M_VID_M_N_GEN_EN) {
                        idx_gen_on = (int)i;
                    }
                }
                if (rel == RB_STRENC_DW_VID_M) { idx_m = (int)i; }
            }
            if (idx_gen_off < 0 || idx_m < 0 || idx_gen_on < 0 ||
                !(idx_gen_off < idx_m && idx_m < idx_gen_on)) {
                printf("FAIL strprog: GEN_EN/M ordering is wrong "
                       "(%d, %d, %d)\n", idx_gen_off, idx_m, idx_gen_on);
                bad++;
            }
        }

        /* The steer FIFO must be pulsed 1 then 0, in that order, and must
         * END at 0 - that is what clears DIG2's held reset. */
        {
            int first = -1, last = -1;
            for (uint32_t i = 0; i < n; i++) {
                if ((w[i].dw - lo) != RB_STRENC_DW_STEER_FIFO) { continue; }
                if (first < 0) { first = (int)i; }
                last = (int)i;
            }
            if (first < 0 || first == last) {
                printf("FAIL strprog: the steer FIFO was not pulsed\n");
                bad++;
            } else if (w[first].value == 0u || w[last].value != 0u) {
                printf("FAIL strprog: the FIFO pulse does not go 1 then 0 - "
                       "DIG2's held reset would survive it\n");
                bad++;
            } else if (w[first].delay_us == 0u) {
                printf("FAIL strprog: the FIFO reset has no settling delay\n");
                bad++;
            }
        }

        /* DIS_DEFER is RETIRED. Nothing may write it, however tempting
         * DIG2's 0-against-2 looks: upstream says the register has no effect
         * on the enable transition. */
        for (uint32_t i = 0; i < n; i++) {
            if ((w[i].dw - lo) == RB_STRENC_DW_VID_STREAM_CNTL &&
                (w[i].mask & 0x300u)) {
                printf("FAIL strprog: a write touches DP_VID_STREAM_DIS_DEFER,"
                       " which is retired\n");
                bad++;
            }
        }
    }

    /* ---- every refusal, and each its own code ---- */
    {
        uint32_t m = 0;
        if (rb_strprog_build(RB_STRENC_PANEL_INST, 1u, &msa, 0u,
                (int)RB_STRENC_DERIVE_OK, 9011u, w, RB_STRPROG_MAX_WRITES, &m)
                != RB_STRPROG_E_PANEL || m != 0u) {
            printf("FAIL strprog: DIG0 was not refused BY NAME\n");
            bad++;
        }
        if (rb_strprog_build(RB_STRENC_COUNT, 1u, &msa, 0u,
                (int)RB_STRENC_DERIVE_OK, 9011u, w, RB_STRPROG_MAX_WRITES, &m)
                != RB_STRPROG_E_INST) {
            printf("FAIL strprog: an out-of-range instance was accepted\n");
            bad++;
        }
        if (rb_strprog_build(2u, 0u, &msa, 0u, (int)RB_STRENC_DERIVE_OK,
                9011u, w, RB_STRPROG_MAX_WRITES, &m) != RB_STRPROG_E_OTG0) {
            printf("FAIL strprog: OTG0 was not refused BY NAME\n");
            bad++;
        }
        if (rb_strprog_build(2u, 8u, &msa, 0u, (int)RB_STRENC_DERIVE_OK,
                9011u, w, RB_STRPROG_MAX_WRITES, &m) != RB_STRPROG_E_OTG) {
            printf("FAIL strprog: a tg_inst outside the field was accepted\n");
            bad++;
        }
        /* THE LICENCE. A non-zero mismatch means the derivation did not
         * reproduce the panel, and nothing may be written on it. */
        if (rb_strprog_build(2u, 1u, &msa, 4u, (int)RB_STRENC_DERIVE_OK,
                9011u, w, RB_STRPROG_MAX_WRITES, &m) != RB_STRPROG_E_MSA ||
            m != 0u) {
            printf("FAIL strprog: a PARAM3 mismatch did not refuse the "
                   "write\n");
            bad++;
        }
        if (rb_strprog_build(2u, 1u, &msa, RB_STRENC_MSA_MISMATCH_ARGS,
                (int)RB_STRENC_DERIVE_OK, 9011u, w, RB_STRPROG_MAX_WRITES, &m)
                != RB_STRPROG_E_MSA) {
            printf("FAIL strprog: 'could not derive' did not refuse the "
                   "write\n");
            bad++;
        }
        if (rb_strprog_build(2u, 1u, &msa, 0u,
                (int)RB_STRENC_DERIVE_TOTAL_ZERO, 9011u, w,
                RB_STRPROG_MAX_WRITES, &m) != RB_STRPROG_E_DERIVE) {
            printf("FAIL strprog: a target pipe with no raster was "
                   "accepted\n");
            bad++;
        }
        if (rb_strprog_build(2u, 1u, &msa, 0u, (int)RB_STRENC_DERIVE_OK,
                0u, w, RB_STRPROG_MAX_WRITES, &m) != RB_STRPROG_E_MVID) {
            printf("FAIL strprog: m_vid 0 was accepted\n");
            bad++;
        }
        if (rb_strprog_build(2u, 1u, 0, 0u, (int)RB_STRENC_DERIVE_OK, 9011u,
                w, RB_STRPROG_MAX_WRITES, &m) != RB_STRPROG_E_ARGS ||
            rb_strprog_build(2u, 1u, &msa, 0u, (int)RB_STRENC_DERIVE_OK,
                9011u, w, RB_STRPROG_MAX_WRITES - 1u, &m)
                != RB_STRPROG_E_ARGS) {
            printf("FAIL strprog: NULL/short-buffer not refused\n");
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
    if (RB_STRPROG_E_OTG >= RB_STRPROG_W_FIRST ||
        RB_STRPROG_W_NOT_ARMED < RB_STRPROG_W_FIRST ||
        RB_STRPROG_W_OK != RB_STRPROG_OK) {
        printf("FAIL strprog: the builder and caller refusal ranges collide\n");
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
     * point of reading bit 16 instead of bit 0. */
    if (rb_strprog_verdict(0x00000001u) == RB_STRPROG_V_RUNNING) {
        printf("FAIL strprog: a request for a stream read as a stream\n");
        bad++;
    }

    if (!bad) {
        printf("ok strprog: %u writes, the OTG connect first, the MSA licence "
               "and the ENABLE/STATUS split\n", n);
    }
    return bad;
}
