/* SPDX-License-Identifier: MIT */
/* 2B3: the HPD read. The chip's own answer to "is a display attached to that
 * connector" - and the one thing that makes it an instrument rather than a
 * number is that DC_HPD_EN gates the sense bit, so "sense 0" with the block
 * disabled is an ABSENCE and not a result (rule 14).
 */
#include <stdio.h>
#include <stdint.h>
#include "rb_hpd.h"

int hpd_tests(void);

int hpd_tests(void)
{
    int bad = 0;

    /* The offsets, against docs/linux-ref/dcn_3_1_2_offset.h read directly:
     *   regHPD0_DC_HPD_INT_STATUS 0x1f14, regHPD1_ 0x1f1c -> stride 8
     *   regHPD0_DC_HPD_CONTROL    0x1f16
     * Pinned per instance rather than as a formula, because a formula tested
     * against itself proves nothing (rule 26's shape for arithmetic). */
    {
        static const uint32_t want_stat[5] =
            { 0x1f14, 0x1f1c, 0x1f24, 0x1f2c, 0x1f34 };
        static const uint32_t want_ctl[5] =
            { 0x1f16, 0x1f1e, 0x1f26, 0x1f2e, 0x1f36 };
        for (unsigned i = 0; i < 5; i++) {
            if (rb_hpd_dw_int_status(i) != want_stat[i]) {
                printf("FAIL hpd: INT_STATUS[%u] = 0x%x want 0x%x\n",
                       i, rb_hpd_dw_int_status(i), want_stat[i]); bad++;
            }
            if (rb_hpd_dw_control(i) != want_ctl[i]) {
                printf("FAIL hpd: CONTROL[%u] = 0x%x want 0x%x\n",
                       i, rb_hpd_dw_control(i), want_ctl[i]); bad++;
            }
        }
        /* Out of range refuses with a value that is not a usable offset, so a
         * caller that ignores the bound reads nothing meaningful instead of
         * reading some other block (rule 19). */
        if (rb_hpd_dw_int_status(5) != RB_HPD_DW_INVALID ||
            rb_hpd_dw_control(99) != RB_HPD_DW_INVALID) {
            printf("FAIL hpd: out-of-range instance returned an offset\n");
            bad++;
        }
        /* The aperture bound has to cover the LAST register, or the first HPD
         * read is outside what hubpSetup() checked. */
        if (rb_hpd_dw_max() != 0x1f36) {
            printf("FAIL hpd: dw_max 0x%x want 0x1f36\n", rb_hpd_dw_max());
            bad++;
        }
        if (rb_hpd_dw_max() < rb_hpd_dw_control(RB_HPD_COUNT - 1u)) {
            printf("FAIL hpd: dw_max below the highest register\n"); bad++;
        }
    }

    /* The fields, from dcn_3_1_2_sh_mask.h: SENSE bit 1, SENSE_DELAYED bit 4,
     * EN bit 28. */
    {
        if (rb_hpd_sense(0x2u) != 1 || rb_hpd_sense(0x1u) != 0) {
            printf("FAIL hpd: SENSE is not bit 1\n"); bad++;
        }
        if (rb_hpd_sense_delayed(0x10u) != 1 ||
            rb_hpd_sense_delayed(0x8u) != 0) {
            printf("FAIL hpd: SENSE_DELAYED is not bit 4\n"); bad++;
        }
        if (rb_hpd_enabled(0x10000000u) != 1 ||
            rb_hpd_enabled(0x08000000u) != 0) {
            printf("FAIL hpd: EN is not bit 28\n"); bad++;
        }
    }

    /* THE CASE THIS HEADER EXISTS FOR. A disabled block with the sense bit
     * clear must NOT read as "nothing is plugged in" - it reads as "this
     * instrument is off", which is a different fact and the one 2zw cost six
     * milestones by conflating one level out. */
    {
        if (rb_hpd_verdict(0x0u, 0x0u) != RB_HPD_DISABLED) {
            printf("FAIL hpd: EN clear did not refuse\n"); bad++;
        }
        /* Even with the sense bit SET, a disabled block says nothing. */
        if (rb_hpd_verdict(0x12u, 0x0u) != RB_HPD_DISABLED) {
            printf("FAIL hpd: EN clear reported a sink\n"); bad++;
        }
        if (rb_hpd_verdict(0x0u, RB_HPD_EN) != RB_HPD_NO_SINK) {
            printf("FAIL hpd: enabled + no sense should be NO_SINK\n"); bad++;
        }
        if (rb_hpd_verdict(RB_HPD_SENSE_DELAYED, RB_HPD_EN) != RB_HPD_SINK) {
            printf("FAIL hpd: debounced sense should be SINK\n"); bad++;
        }
        /* Raw sense without the debounce is a transient, not a sink: Linux
         * believes SENSE_DELAYED, so reporting SINK off the raw bit would
         * make a bouncing connector look like a display. */
        if (rb_hpd_verdict(RB_HPD_SENSE, RB_HPD_EN) != RB_HPD_SETTLING) {
            printf("FAIL hpd: raw sense alone should be SETTLING\n"); bad++;
        }
    }

    /* The masks, which are what gets published - one scalar for five blocks so
     * a before/after difference is one XOR. */
    {
        uint32_t stat[5] = { 0, RB_HPD_SENSE_DELAYED, 0,
                             RB_HPD_SENSE_DELAYED, RB_HPD_SENSE_DELAYED };
        uint32_t ctl[5]  = { RB_HPD_EN, RB_HPD_EN, RB_HPD_EN, 0, RB_HPD_EN };
        /* instance 1 and 4 are sinks; 3 has the sense bit but EN clear, so it
         * must NOT appear - that is the whole point of the mask going through
         * rb_hpd_verdict() rather than testing the sense bit directly. */
        const uint32_t sink = rb_hpd_sink_mask(stat, ctl, 5);
        if (sink != ((1u << 1) | (1u << 4))) {
            printf("FAIL hpd: sink_mask 0x%x want 0x12\n", sink); bad++;
        }
        const uint32_t en = rb_hpd_enabled_mask(ctl, 5);
        if (en != 0x17u) {   /* 0,1,2,4 */
            printf("FAIL hpd: enabled_mask 0x%x want 0x17\n", en); bad++;
        }
        /* A count above the array is clamped, and a NULL is 0 rather than a
         * dereference - this runs in the kernel. */
        if (rb_hpd_sink_mask(stat, ctl, 99u) != sink) {
            printf("FAIL hpd: count above RB_HPD_COUNT not clamped\n"); bad++;
        }
        if (rb_hpd_sink_mask(0, ctl, 5) != 0 ||
            rb_hpd_enabled_mask(0, 5) != 0) {
            printf("FAIL hpd: NULL did not return 0\n"); bad++;
        }
        /* And the usable reading is the DIFFERENCE: cable out, cable in, XOR
         * names the instance. One sample identifies nothing (rule 18). */
        uint32_t in[5] = { 0, 0, RB_HPD_SENSE_DELAYED, 0, 0 };
        uint32_t allen[5] = { RB_HPD_EN, RB_HPD_EN, RB_HPD_EN,
                              RB_HPD_EN, RB_HPD_EN };
        const uint32_t before = rb_hpd_sink_mask(stat, allen, 5);
        const uint32_t after  = rb_hpd_sink_mask(in, allen, 5);
        if ((before ^ after) == 0u) {
            printf("FAIL hpd: a changed cable state gave no difference\n");
            bad++;
        }
    }

    printf("hpd: 2B3's DCN 3.1.2 offsets, the EN gate that keeps "
           "\"no sink\" apart from \"instrument off\", and the masks%s\n",
           bad ? " FAILED" : ": passed");
    return bad;
}
