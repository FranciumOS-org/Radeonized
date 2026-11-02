/* SPDX-License-Identifier: MIT */
/* 2C0: the RDPCSTX PHY read - DP alt mode, lane power and the reference
 * clock. The thing that makes it an instrument rather than five numbers is
 * that the panel is scanning while it runs, so one of the five PHYs MUST be
 * active; a sweep where none is has not read the PHY blocks at all.
 *
 * And the thing it must NOT do is claim an identification it cannot make:
 * this register has no is_usb bit, so DISABLE clear is ambiguous between
 * "alt mode active" and "not a Type-C PHY", and the enum says so.
 */
#include <stdio.h>
#include <stdint.h>
#include "rb_dpalt.h"

int dpalt_tests(void);

int dpalt_tests(void)
{
    int bad = 0;

    /* The offsets, against docs/linux-ref/dpcs_4_2_0_offset.h read directly:
     *   regRDPCSTX0_RDPCSTX_PHY_CNTL6 0x2946
     *   regRDPCSTX1_                  0x2a1e
     *   regRDPCSTX2_                  0x2af6
     *   regRDPCSTX3_                  0x2bce
     *   regRDPCSTX4_                  0x2ca6
     * Pinned per instance, NOT recomputed from base + i * stride, because a
     * formula checked against itself proves nothing - and the stride is the
     * part most likely to be wrong, since dpcs_3_1_4_offset.h exists upstream
     * and would have given different numbers (rule 46; the version comes from
     * dcn31_resource.c:86's include line, not from the DCN version). */
    {
        static const uint32_t want[5] =
            { 0x2946, 0x2a1e, 0x2af6, 0x2bce, 0x2ca6 };
        for (unsigned i = 0; i < 5; i++) {
            if (rb_dpalt_dw(i) != want[i]) {
                printf("FAIL dpalt: dw[%u] = 0x%x want 0x%x\n",
                       i, rb_dpalt_dw(i), want[i]);
                bad++;
            }
        }
        if (rb_dpalt_dw(5) != RB_DPALT_DW_INVALID ||
            rb_dpalt_dw(99) != RB_DPALT_DW_INVALID) {
            printf("FAIL dpalt: out-of-range instance returned an offset\n");
            bad++;
        }
        /* The aperture bound must cover the LAST register or the first read
         * of RDPCSTX4 is outside what hubpSetup() checked. 0x2ca6 is well
         * above the 0x1fcb the AUX block needed, so this is the value that
         * now sets the bound. */
        if (rb_dpalt_dw_max() != 0x2ca6) {
            printf("FAIL dpalt: dw_max 0x%x want 0x2ca6\n", rb_dpalt_dw_max());
            bad++;
        }
    }

    /* The three DP alt fields, at the bit positions dpcs_4_2_0_sh_mask.h
     * gives: DP4 0x00010000, DISABLE 0x00020000, DISABLE_ACK 0x00040000.
     * Each checked in isolation so a swapped pair cannot pass. */
    {
        if (!rb_dpalt_dp4(0x00010000u) || rb_dpalt_dp4(0x00060000u)) {
            printf("FAIL dpalt: DP4 is not bit 16\n"); bad++;
        }
        if (!rb_dpalt_disabled(0x00020000u) || rb_dpalt_disabled(0x00050000u)) {
            printf("FAIL dpalt: DISABLE is not bit 17\n"); bad++;
        }
        if (!rb_dpalt_disable_acked(0x00040000u) ||
            rb_dpalt_disable_acked(0x00030000u)) {
            printf("FAIL dpalt: DISABLE_ACK is not bit 18\n"); bad++;
        }
        if (!rb_dpalt_ref_clk_en(0x00080000u) ||
            rb_dpalt_ref_clk_en(0x00100000u)) {
            printf("FAIL dpalt: REF_CLK_EN is not bit 19\n"); bad++;
        }
        if (!rb_dpalt_ref_clk_req(0x00100000u) ||
            rb_dpalt_ref_clk_req(0x00080000u)) {
            printf("FAIL dpalt: REF_CLK_REQ is not bit 20\n"); bad++;
        }
    }

    /* The four transmitters: PSTATE at 0x3 << (tx*4), MPLL_EN at 0x4 << same.
     * The interleaved layout is the part a reader gets wrong, so every lane
     * is checked rather than lane 0 and a shift. */
    {
        /* TX0 PSTATE 2, TX1 PSTATE 1, TX2 PSTATE 3, TX3 PSTATE 0 */
        const uint32_t w = 0x00000132u;
        static const uint32_t want_ps[4] = { 2, 3, 1, 0 };
        for (unsigned tx = 0; tx < 4; tx++) {
            if (rb_dpalt_tx_pstate(w, tx) != want_ps[tx]) {
                printf("FAIL dpalt: tx%u pstate %u want %u\n",
                       tx, rb_dpalt_tx_pstate(w, tx), want_ps[tx]);
                bad++;
            }
        }
        if (rb_dpalt_tx_pstate(w, 4) != 0u) {
            printf("FAIL dpalt: out-of-range tx returned a pstate\n"); bad++;
        }

        /* MPLL_EN on TX0 and TX1 only - the two-lane shape DP alt mode keeps,
         * and what 2BB's reading of 2 lanes from the sink's DPCD predicts the
         * chip should show if that port is the one in alt mode. */
        if (rb_dpalt_lanes_on(0x00000044u) != 2u) {
            printf("FAIL dpalt: lanes_on(TX0|TX1) = %u want 2\n",
                   rb_dpalt_lanes_on(0x00000044u));
            bad++;
        }
        if (rb_dpalt_lanes_on(0x00004444u) != 4u) {
            printf("FAIL dpalt: lanes_on(all four) = %u want 4\n",
                   rb_dpalt_lanes_on(0x00004444u));
            bad++;
        }
        if (rb_dpalt_lanes_on(0u) != 0u) {
            printf("FAIL dpalt: lanes_on(0) != 0\n"); bad++;
        }
        /* A PSTATE-only word must not count as a lane: the two fields are
         * adjacent and this is where a wrong mask shows up. */
        if (rb_dpalt_lanes_on(0x00003333u) != 0u) {
            printf("FAIL dpalt: PSTATE bits counted as enabled MPLLs\n");
            bad++;
        }
        if (rb_dpalt_tx_mpll_en(0x00000044u, 4)) {
            printf("FAIL dpalt: out-of-range tx reported an enabled MPLL\n");
            bad++;
        }
    }

    /* The per-instance verdict, including the member that exists only to
     * refuse an identification this register cannot make. */
    {
        if (rb_dpalt_verdict(0x00060000u) != RB_DPALT_ALT_DISABLED) {
            printf("FAIL dpalt: DISABLE+ACK is not ALT_DISABLED\n"); bad++;
        }
        if (rb_dpalt_verdict(0x00020000u) != RB_DPALT_ALT_SETTLING) {
            printf("FAIL dpalt: DISABLE without ACK is not SETTLING\n"); bad++;
        }
        /* The important one. DISABLE clear must NOT read as "alt mode is
         * active on a Type-C port": with no is_usb bit, a PHY wired to
         * nothing reads identically. */
        if (rb_dpalt_verdict(0u) != RB_DPALT_ALT_AMBIGUOUS) {
            printf("FAIL dpalt: DISABLE clear is not AMBIGUOUS\n"); bad++;
        }
        if (rb_dpalt_verdict(0x00044444u) != RB_DPALT_ALT_AMBIGUOUS) {
            printf("FAIL dpalt: ACK set without DISABLE is not AMBIGUOUS\n");
            bad++;
        }
    }

    /* phy_active: the reference clock OR any enabled MPLL. Either alone is
     * enough, because the control needs to fire on a PHY that is up in any
     * form, not only on one in a particular state. */
    {
        if (!rb_dpalt_phy_active(RB_DPALT_REF_CLK_EN)) {
            printf("FAIL dpalt: ref clk alone is not active\n"); bad++;
        }
        if (!rb_dpalt_phy_active(0x00000004u)) {
            printf("FAIL dpalt: an enabled MPLL alone is not active\n"); bad++;
        }
        if (rb_dpalt_phy_active(0u)) {
            printf("FAIL dpalt: an empty word reads as active\n"); bad++;
        }
        /* DP alt bits alone must not make a PHY look active - they say
         * nothing about whether it is clocked. */
        if (rb_dpalt_phy_active(0x00060000u)) {
            printf("FAIL dpalt: DP alt bits alone read as active\n"); bad++;
        }
    }

    /* THE SWEEP CONTROL. The panel scans while this runs, so at least one of
     * the five must be active; none means the five words are not the five PHY
     * blocks. Boot 273 read a correctly located ring as all zeros, so this
     * failure mode is measured, not hypothetical. */
    {
        static const uint32_t none[5]  = { 0, 0, 0, 0, 0 };
        static const uint32_t one[5]   = { 0, 0, RB_DPALT_REF_CLK_EN, 0, 0 };
        if (rb_dpalt_sweep_verdict(none, 5) != RB_DPALT_SWEEP_NO_ACTIVE) {
            printf("FAIL dpalt: an all-dark sweep was accepted\n"); bad++;
        }
        if (rb_dpalt_sweep_verdict(one, 5) != RB_DPALT_SWEEP_OK) {
            printf("FAIL dpalt: one active PHY did not pass the control\n");
            bad++;
        }
        if (rb_dpalt_sweep_verdict(0, 5) != RB_DPALT_SWEEP_BAD_ARGS ||
            rb_dpalt_sweep_verdict(one, 0) != RB_DPALT_SWEEP_BAD_ARGS ||
            rb_dpalt_sweep_verdict(one, 99) != RB_DPALT_SWEEP_BAD_ARGS) {
            printf("FAIL dpalt: bad arguments were not refused\n"); bad++;
        }

        /* DELIBERATELY NOT A REFUSAL: five identical words. With no cable
         * attached that is the expected reading, and refusing it would make
         * the instrument blind in the regime it exists to measure - 2A9's
         * defect, where turning the hold off turned the measurement off. */
        static const uint32_t uniform[5] = {
            RB_DPALT_REF_CLK_EN, RB_DPALT_REF_CLK_EN, RB_DPALT_REF_CLK_EN,
            RB_DPALT_REF_CLK_EN, RB_DPALT_REF_CLK_EN
        };
        if (rb_dpalt_sweep_verdict(uniform, 5) != RB_DPALT_SWEEP_OK) {
            printf("FAIL dpalt: a uniform sweep was refused\n"); bad++;
        }

        /* Zero must mean NOT_SAMPLED, because the kext's words are zero
         * before the first sample and a verdict field that defaults to a
         * passing control is 2A3's defect from the other side. Pinned as a
         * value, not just as a name, since renumbering the enum is exactly
         * what would silently break it. */
        if ((int)RB_DPALT_SWEEP_NOT_SAMPLED != 0) {
            printf("FAIL dpalt: NOT_SAMPLED is not the zero value\n"); bad++;
        }
        if ((int)RB_DPALT_SWEEP_OK == 0) {
            printf("FAIL dpalt: SWEEP_OK is zero, so an unsampled field "
                   "would read as a passing control\n");
            bad++;
        }
    }

    /* The two masks the caller publishes. active_mask locates the panel's own
     * PHY, which narrows the Type-C candidate by elimination (2B9's use of
     * AUX0 as the reference for AUX2); not_disabled_mask is the quantity
     * whose CHANGE across a cable event does the identification (2B4's
     * method). Bit order is checked against a hand-built word so an
     * off-by-one in the shift cannot pass. */
    {
        static const uint32_t w[5] = {
            RB_DPALT_REF_CLK_EN,              /* 0: active, alt not disabled */
            RB_DPALT_DISABLE,                 /* 1: dark,   alt disabled     */
            0x00000004u,                      /* 2: active, alt not disabled */
            RB_DPALT_DISABLE | RB_DPALT_DISABLE_ACK, /* 3: dark, disabled    */
            0u                                /* 4: dark,   alt not disabled */
        };
        if (rb_dpalt_active_mask(w, 5) != 0x05u) {
            printf("FAIL dpalt: active_mask 0x%x want 0x05\n",
                   rb_dpalt_active_mask(w, 5));
            bad++;
        }
        if (rb_dpalt_not_disabled_mask(w, 5) != 0x15u) {
            printf("FAIL dpalt: not_disabled_mask 0x%x want 0x15\n",
                   rb_dpalt_not_disabled_mask(w, 5));
            bad++;
        }
        if (rb_dpalt_active_mask(0, 5) != 0u ||
            rb_dpalt_not_disabled_mask(0, 5) != 0u) {
            printf("FAIL dpalt: a NULL array produced a mask\n"); bad++;
        }
        /* An over-long count must be clamped, not read past the array. */
        if (rb_dpalt_active_mask(w, 99) != 0x05u) {
            printf("FAIL dpalt: count was not clamped\n"); bad++;
        }
    }

    return bad;
}
