/* SPDX-License-Identifier: MIT */
/* 2FC: the DIO per-link encoder-type mux.
 *
 * What makes this decoder worth testing is not the arithmetic - the stride is
 * 1 - but the AMBIGUITY it must preserve. `ENCODER_TYPE_DIG` is zero, so a
 * zero word is indistinguishable from an unprogrammed register, a gated
 * aperture and a wrong address. A decoder that reported zero as "muxed to
 * DIG, confirmed" would turn the one uninterpretable reading into a passing
 * control, which is the defect 2C0 caught in its own sweep enum before it
 * shipped. So the cases below pin the name `DIG_OR_UNSET`, pin
 * NOT_SAMPLED == 0, and pin that a uniform sweep is reported as UNIFORM and
 * never as a confirmation.
 */
#include <stdio.h>
#include <stdint.h>
#include "rb_dio.h"

int dio_tests(void);

int dio_tests(void)
{
    int bad = 0;

    /* The offsets, against docs/linux-ref/dcn_3_1_2_offset.h:11737-11747 read
     * directly:
     *   regDIO_LINKA_CNTL 0x1f04   regDIO_LINKD_CNTL 0x1f07
     *   regDIO_LINKB_CNTL 0x1f05   regDIO_LINKE_CNTL 0x1f08
     *   regDIO_LINKC_CNTL 0x1f06   regDIO_LINKF_CNTL 0x1f09
     * Pinned per instance, not recomputed from base + i * stride: a formula
     * checked against itself proves nothing, and 2FA found three different
     * strides inside one function. */
    {
        static const uint32_t want[RB_DIO_COUNT] =
            { 0x1f04, 0x1f05, 0x1f06, 0x1f07, 0x1f08, 0x1f09 };
        unsigned i;
        for (i = 0; i < RB_DIO_COUNT; i++) {
            if (rb_dio_dw(i) != want[i]) {
                printf("FAIL dio: dw[%u] = 0x%x want 0x%x\n",
                       i, rb_dio_dw(i), want[i]);
                bad++;
            }
        }
        if (rb_dio_dw_max() != 0x1f09u) {
            printf("FAIL dio: dw_max = 0x%x want 0x1f09\n", rb_dio_dw_max());
            bad++;
        }
        /* Out of range must refuse, not wrap onto a neighbouring register.
         * LINKF is the last; 0x1f0a is DIO_CLK_CNTL territory. */
        if (rb_dio_dw(RB_DIO_COUNT) != RB_DIO_DW_INVALID) {
            printf("FAIL dio: dw(%u) = 0x%x want INVALID\n",
                   RB_DIO_COUNT, rb_dio_dw(RB_DIO_COUNT));
            bad++;
        }
    }

    /* Each field in isolation, so a swapped pair cannot pass. The three masks
     * do not overlap and each test word sets exactly one of them. */
    {
        if (rb_dio_enc_type(0x00000002u) != 2u ||
            rb_dio_hpo_hdmi_sel(0x00000002u) != 0u ||
            rb_dio_hpo_dp_sel(0x00000002u) != 0u) {
            printf("FAIL dio: ENC_TYPE_SEL alone decoded wrong\n");
            bad++;
        }
        if (rb_dio_hpo_hdmi_sel(0x00000050u) != 5u ||
            rb_dio_enc_type(0x00000050u) != 0u ||
            rb_dio_hpo_dp_sel(0x00000050u) != 0u) {
            printf("FAIL dio: HPO_HDMI_ENC_SEL alone decoded wrong\n");
            bad++;
        }
        if (rb_dio_hpo_dp_sel(0x00000300u) != 3u ||
            rb_dio_enc_type(0x00000300u) != 0u ||
            rb_dio_hpo_hdmi_sel(0x00000300u) != 0u) {
            printf("FAIL dio: HPO_DP_ENC_SEL alone decoded wrong\n");
            bad++;
        }
        /* Bits above 0x700 belong to no field we decode and must not leak
         * into one. */
        if (rb_dio_enc_type(0xfffff800u) != 0u ||
            rb_dio_hpo_hdmi_sel(0xfffff800u) != 0u ||
            rb_dio_hpo_dp_sel(0xfffff800u) != 0u) {
            printf("FAIL dio: undecoded bits leaked into a field\n");
            bad++;
        }
    }

    /* THE AMBIGUITY. Zero is DIG and zero is also unset, and the verdict's
     * name has to carry that or the reading is over-claimed. */
    {
        if (rb_dio_sel(0x00000000u) != RB_DIO_SEL_DIG_OR_UNSET) {
            printf("FAIL dio: 0 is not DIG_OR_UNSET\n");
            bad++;
        }
        if (rb_dio_sel(0x00000001u) != RB_DIO_SEL_HDMI_FRL) {
            printf("FAIL dio: 1 is not HDMI_FRL\n");
            bad++;
        }
        if (rb_dio_sel(0x00000002u) != RB_DIO_SEL_DP_128B132B) {
            printf("FAIL dio: 2 is not DP_128B132B\n");
            bad++;
        }
        /* 3 is not in upstream's enum. It must come back as its own code, not
         * silently as DIG, because it would mean the word is not this
         * register at all (rule 14). */
        if (rb_dio_sel(0x00000003u) != RB_DIO_SEL_RESERVED) {
            printf("FAIL dio: 3 is not RESERVED\n");
            bad++;
        }
        /* An unsampled field must not read as any of the four. */
        if (RB_DIO_SEL_NOT_SAMPLED != 0) {
            printf("FAIL dio: NOT_SAMPLED is not 0\n");
            bad++;
        }
        if (rb_dio_sel(0x00000000u) == RB_DIO_SEL_NOT_SAMPLED) {
            printf("FAIL dio: a sampled zero reads as NOT_SAMPLED\n");
            bad++;
        }
    }

    /* The nonzero mask: the only thing that proves these six words are the
     * six registers rather than six reads of a dead aperture. */
    {
        uint32_t w[RB_DIO_COUNT] = { 0, 0, 0, 0, 0, 0 };
        if (rb_dio_nonzero_mask(w, RB_DIO_COUNT) != 0u) {
            printf("FAIL dio: all-zero gave a nonzero mask\n");
            bad++;
        }
        w[2] = 0x00000002u;            /* LINKC on 128b/132b */
        if (rb_dio_nonzero_mask(w, RB_DIO_COUNT) != 0x04u) {
            printf("FAIL dio: mask = 0x%x want 0x04\n",
                   rb_dio_nonzero_mask(w, RB_DIO_COUNT));
            bad++;
        }
        /* A bit OUTSIDE the three fields must not set the mask: it would make
         * a reserved high bit look like a programmed mux. */
        w[2] = 0u;
        w[5] = 0x00001000u;
        if (rb_dio_nonzero_mask(w, RB_DIO_COUNT) != 0u) {
            printf("FAIL dio: an undecoded bit set the nonzero mask\n");
            bad++;
        }
        if (rb_dio_nonzero_mask(0, RB_DIO_COUNT) != 0u) {
            printf("FAIL dio: NULL did not give 0\n");
            bad++;
        }
    }

    /* The sweep. UNIFORM is deliberately NOT an error - the expected outcome
     * of this whole read is six identical zeros - and DIFFERS is the only
     * verdict that carries information. Both are pinned so a later edit
     * cannot quietly make uniformity a pass or a failure. */
    {
        uint32_t w[RB_DIO_COUNT] = { 0, 0, 0, 0, 0, 0 };
        if (rb_dio_sweep_verdict(w, RB_DIO_COUNT) != RB_DIO_SWEEP_UNIFORM) {
            printf("FAIL dio: all-zero sweep is not UNIFORM\n");
            bad++;
        }
        w[2] = 0x00000002u;
        if (rb_dio_sweep_verdict(w, RB_DIO_COUNT) != RB_DIO_SWEEP_DIFFERS) {
            printf("FAIL dio: one differing instance is not DIFFERS\n");
            bad++;
        }
        /* A difference OUTSIDE the three fields is not a difference in the
         * mux, so it must still read UNIFORM.
         *
         * THE BIT GOES ON INSTANCE 0, and this is not a free choice. The
         * verdict compares every instance against `w[0]`, so a case that puts
         * the stray bit on w[3] leaves w[0] at zero and passes whether or not
         * `w[0]` is masked - rule 48's "a test that passes for the wrong
         * reason is not coverage", found by the break-it run: removing the
         * mask from the `first =` line fired NOTHING. Instance 0 is the one
         * word the comparison treats differently, so it is the one the case
         * has to use. The second bit on w[3] keeps the other side covered. */
        w[2] = 0u;
        w[0] = 0x80000000u;
        w[3] = 0x40000000u;
        if (rb_dio_sweep_verdict(w, RB_DIO_COUNT) != RB_DIO_SWEEP_UNIFORM) {
            printf("FAIL dio: an undecoded bit made the sweep DIFFER\n");
            bad++;
        }
        /* And with the stray bit on instance 0, a real field difference
         * elsewhere must still come back DIFFERS - otherwise the case above
         * could pass by making every verdict UNIFORM. */
        w[4] = 0x00000001u;
        if (rb_dio_sweep_verdict(w, RB_DIO_COUNT) != RB_DIO_SWEEP_DIFFERS) {
            printf("FAIL dio: a field difference past a stray bit is not "
                   "DIFFERS\n");
            bad++;
        }
        w[0] = 0u; w[3] = 0u; w[4] = 0u;
        if (rb_dio_sweep_verdict(0, RB_DIO_COUNT) != RB_DIO_SWEEP_BAD_ARGS ||
            rb_dio_sweep_verdict(w, 0) != RB_DIO_SWEEP_BAD_ARGS ||
            rb_dio_sweep_verdict(w, RB_DIO_COUNT + 1u) !=
                RB_DIO_SWEEP_BAD_ARGS) {
            printf("FAIL dio: bad args not refused\n");
            bad++;
        }
        if (RB_DIO_SWEEP_NOT_SAMPLED != 0 || RB_DIO_SWEEP_DIFFERS == 0) {
            printf("FAIL dio: sweep NOT_SAMPLED/DIFFERS numbering\n");
            bad++;
        }
    }

    if (!bad) { printf("ok dio: %u links, offsets and the zero ambiguity\n",
                       RB_DIO_COUNT); }
    return bad;
}
