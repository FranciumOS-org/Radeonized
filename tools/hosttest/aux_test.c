/* SPDX-License-Identifier: MIT */
/* 2B8: the DP AUX engine survey. Read-only - there is no writer to test, and
 * that is deliberate (see rb_aux.h). What these cases guard is the offset
 * arithmetic, the aperture bound, and the refusal that keeps "this engine is
 * not configured" apart from "there is nothing to compare it against".
 */
#include <stdio.h>
#include <stdint.h>
#include "rb_aux.h"

int aux_tests(void);

int aux_tests(void)
{
    int bad = 0;

    /* Offsets pinned per instance against dcn_3_1_2_offset.h read directly,
     * not recomputed by the formula under test (rule 26 for arithmetic). */
    {
        static const uint32_t want_ctl[5] =
            { 0x1f50, 0x1f6c, 0x1f88, 0x1fa4, 0x1fc0 };
        static const uint32_t want_data[5] =
            { 0x1f56, 0x1f72, 0x1f8e, 0x1faa, 0x1fc6 };
        for (unsigned i = 0; i < 5; i++) {
            if (rb_aux_dw(i, RB_AUX_REG_CONTROL) != want_ctl[i]) {
                printf("FAIL aux: CONTROL[%u] = 0x%x want 0x%x\n", i,
                       rb_aux_dw(i, RB_AUX_REG_CONTROL), want_ctl[i]); bad++;
            }
            if (rb_aux_dw(i, RB_AUX_REG_SW_DATA) != want_data[i]) {
                printf("FAIL aux: SW_DATA[%u] = 0x%x want 0x%x\n", i,
                       rb_aux_dw(i, RB_AUX_REG_SW_DATA), want_data[i]); bad++;
            }
        }
        /* The three registers that are not on the CONTROL+n pattern, so a
         * transcription slip in any one of them shows up here. */
        if (rb_aux_dw(0, RB_AUX_REG_SW_STATUS) != 0x1f54 ||
            rb_aux_dw(0, RB_AUX_REG_DPHY_RX_CTL1) != 0x1f5b ||
            rb_aux_dw(2, RB_AUX_REG_DPHY_RX_CTL1) != 0x1f93 ||
            rb_aux_dw(2, RB_AUX_REG_ARB_CONTROL) != 0x1f8a) {
            printf("FAIL aux: a non-contiguous register is misplaced\n");
            bad++;
        }
        if (rb_aux_dw(5, RB_AUX_REG_CONTROL) != RB_AUX_DW_INVALID ||
            rb_aux_dw(0, RB_AUX_REG_COUNT) != RB_AUX_DW_INVALID) {
            printf("FAIL aux: out-of-range did not refuse\n"); bad++;
        }
        /* The bound must cover the LAST register of the LAST instance, or the
         * first read is outside what hubpSetup() checked. */
        if (rb_aux_dw_max() != 0x1fcb) {
            printf("FAIL aux: dw_max 0x%x want 0x1fcb\n", rb_aux_dw_max());
            bad++;
        }
        for (unsigned i = 0; i < RB_AUX_COUNT; i++) {
            for (unsigned r = 0; r < RB_AUX_REG_COUNT; r++) {
                if (rb_aux_dw(i, (enum rb_aux_reg)r) > rb_aux_dw_max()) {
                    printf("FAIL aux: inst %u reg %u is above dw_max\n", i, r);
                    bad++;
                }
            }
        }
    }

    /* AUX_EN is bit 0, and the mask goes through it. */
    {
        if (rb_aux_enabled(1u) != 1 || rb_aux_enabled(2u) != 0) {
            printf("FAIL aux: AUX_EN is not bit 0\n"); bad++;
        }
        uint32_t ctl[5] = { 1u, 0u, 1u, 0u, 1u };
        if (rb_aux_enabled_mask(ctl, 5) != 0x15u) {
            printf("FAIL aux: enabled_mask 0x%x want 0x15\n",
                   rb_aux_enabled_mask(ctl, 5)); bad++;
        }
        if (rb_aux_enabled_mask(ctl, 99u) != 0x15u ||
            rb_aux_enabled_mask(0, 5) != 0) {
            printf("FAIL aux: clamp or NULL wrong\n"); bad++;
        }
    }

    /* THE CASE THIS HEADER EXISTS FOR. The comparison is against the engine
     * the firmware actually drives, and with no reference there is no verdict
     * - "does not match" must not be returned for "nothing to match". */
    {
        const uint32_t ref_ctl = RB_AUX_EN, ref_dphy = 0x00103f21u;
        if (rb_aux_matches_ref(RB_AUX_EN, 0x00103f21u, ref_ctl, ref_dphy) != 1) {
            printf("FAIL aux: an identical engine did not match\n"); bad++;
        }
        if (rb_aux_matches_ref(RB_AUX_EN, 0u, ref_ctl, ref_dphy) != 0) {
            printf("FAIL aux: a zero DPHY matched a configured one\n"); bad++;
        }
        /* Enabled but differently timed is NOT a match - that is the whole
         * discrimination, and without this case the function could be a
         * constant 1 for any enabled engine. */
        if (rb_aux_matches_ref(RB_AUX_EN, 0x00103f22u, ref_ctl, ref_dphy) != 0) {
            printf("FAIL aux: a different DPHY word matched\n"); bad++;
        }
        /* Not enabled -> no match, whatever the DPHY says. */
        if (rb_aux_matches_ref(0u, 0x00103f21u, ref_ctl, ref_dphy) != 0) {
            printf("FAIL aux: a disabled engine matched\n"); bad++;
        }
        /* NO REFERENCE -> 0, even when the two words are identical. A
         * reference that is itself unconfigured is not a reference, and
         * returning 1 here would report "configured like the panel's engine"
         * off two engines that are both dark (rule 14). */
        if (rb_aux_matches_ref(RB_AUX_EN, 0x11u, 0u, 0x11u) != 0) {
            printf("FAIL aux: matched against an unconfigured reference\n");
            bad++;
        }
    }

    printf("aux: 2B8's DCN 3.1.2 offsets (DCE110 engine, rule 46 checked in "
           "dcn31_resource.c), the bound, and the reference comparison%s\n",
           bad ? " FAILED" : ": passed");
    return bad;
}
