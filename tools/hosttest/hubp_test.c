/* SPDX-License-Identifier: MIT */
#include "rb_hubp.h"
#include "rb_regs.h"
#include <stdio.h>
#include <string.h>

/* 2wh: the guards on the first write this project makes to the display
 * engine. Everything the kext decides is decided by rb_hubp.h, so everything
 * it decides is run here first. The measured numbers are boot 133's pipe
 * (2os) and boot 194's transaction (2wg): if a decode is wrong, these stop
 * agreeing with each other. */

static uint32_t reg_from_defs(const char *name)
{
    for (unsigned i = 0; i < RB_NREG_DEFS; i++) {
        if (!strcmp(rb_reg_defs[i].name, name)) { return rb_reg_defs[i].reg; }
    }
    return 0;
}

int hubp_tests(void)
{
    int bad = 0;
#define A(x) do { if (!(x)) { printf("  FAIL HUBP:%d: %s\n", __LINE__, #x); bad++; } } while (0)

    /* 1. The offset table agrees with rb_regs.h, which was written from the
     * same Linux header by a different hand at a different time. Two tables,
     * one source: a typo in either one shows up here. */
    A(rb_hubp_dw(0, RB_HUBP_REG_ADDR)    == reg_from_defs("HUBP0_PRIMARY_ADDR"));
    A(rb_hubp_dw(0, RB_HUBP_REG_ADDR_HI) == reg_from_defs("HUBP0_PRIMARY_ADDR_HI"));
    A(rb_hubp_dw(1, RB_HUBP_REG_ADDR)    == reg_from_defs("HUBP1_PRIMARY_ADDR"));
    A(rb_hubp_dw(2, RB_HUBP_REG_ADDR)    == reg_from_defs("HUBP2_PRIMARY_ADDR"));
    A(rb_hubp_dw(3, RB_HUBP_REG_ADDR)    == reg_from_defs("HUBP3_PRIMARY_ADDR"));
    A(rb_hubp_dw(0, RB_HUBP_REG_PITCH)   == reg_from_defs("HUBP0_SURFACE_PITCH"));
    A(rb_hubp_dw(0, RB_HUBP_REG_CONFIG)  == reg_from_defs("HUBP0_SURFACE_CONFIG"));
    A(rb_hubp_dw(1, RB_HUBP_REG_CONFIG)  == reg_from_defs("HUBP1_SURFACE_CONFIG"));
    A(rb_hubp_dw(3, RB_HUBP_REG_CONFIG)  == reg_from_defs("HUBP3_SURFACE_CONFIG"));

    /* 2. The stride is uniform. Asserted, never used to compute an offset -
     * the table came from the header and stays authoritative. */
    for (unsigned i = 1; i < RB_HUBP_COUNT; i++) {
        for (unsigned r = 0; r < RB_HUBP_REG_COUNT; r++) {
            A(rb_hubp_dw(i, (enum rb_hubp_reg)r) ==
              rb_hubp_dw(0, (enum rb_hubp_reg)r) + i * RB_HUBP_STRIDE);
        }
    }

    /* 2b. The tiling pair this build reads so a scrambled image is
     * distinguishable from a wrong address. Checked against the Linux header
     * values directly, since rb_regs.h does not carry them. */
    A(rb_hubp_dw(0, RB_HUBP_REG_ADDR_CFG) == 0x05e6);
    A(rb_hubp_dw(0, RB_HUBP_REG_TILING)   == 0x05e7);
    A(rb_hubp_dw(1, RB_HUBP_REG_TILING)   == 0x06c3);

    /* 2c. 2xj's flip-control pair, read-only. Stated here directly from
     * dcn_3_1_2_offset.h (regHUBPREQ<n>_DCSURF_FLIP_CONTROL / _CONTROL2),
     * which is a SECOND independent statement of the same four numbers - the
     * stride loop above already checks them a third way, and would fail if
     * any one of them were off the 0xdc lattice. rb_regs.h does not carry
     * them, so there is no third table to cross-check against. */
    A(rb_hubp_dw(0, RB_HUBP_REG_FLIP_CTL)  == 0x061b);
    A(rb_hubp_dw(0, RB_HUBP_REG_FLIP_CTL2) == 0x061c);
    A(rb_hubp_dw(1, RB_HUBP_REG_FLIP_CTL)  == 0x06f7);
    A(rb_hubp_dw(2, RB_HUBP_REG_FLIP_CTL)  == 0x07d3);
    A(rb_hubp_dw(3, RB_HUBP_REG_FLIP_CTL)  == 0x08af);
    A(rb_hubp_dw(3, RB_HUBP_REG_FLIP_CTL2) == 0x08b0);
    /* And they must sit just past SURFACE_CONTROL (0x061a, rb_regs.h), which
     * is the family cross-check: a flip-control register that did not land
     * adjacent to it would mean we are reading a different block. */
    A(rb_hubp_dw(0, RB_HUBP_REG_FLIP_CTL) ==
      reg_from_defs("HUBP0_SURFACE_CONTROL") + 1);

    /* 2d. 2xk's flip-pending predicate, and the field masks it rests on,
     * from dcn_3_1_2_sh_mask.h. The LIVE value boot 216 read is the control:
     * 0x04100000 must decode to PENDING_DELAY = 65 with every other field
     * zero and NO leftover bits - if the masks were wrong, bits would be left
     * over or FLIP_TYPE would not read 0. */
    A(RB_HUBP_FLIP_UPDATE_LOCK   == 0x00000001u);
    A(RB_HUBP_FLIP_TYPE          == 0x00000002u);
    A(RB_HUBP_FLIP_PENDING       == 0x00000100u);
    A(RB_HUBP_FLIP_PENDING_DELAY == 0x3ff00000u);
    {
        const uint32_t live = 0x04100000u;   /* boot 216, HUBP0, measured */
        const uint32_t known = RB_HUBP_FLIP_UPDATE_LOCK | RB_HUBP_FLIP_TYPE |
                               0x000000f0u | RB_HUBP_FLIP_PENDING |
                               0x00000200u | 0x00001000u | 0x00010000u |
                               0x00020000u | 0x00040000u |
                               RB_HUBP_FLIP_PENDING_DELAY;
        A((live & ~known) == 0u);                               /* no leftover */
        A((live & RB_HUBP_FLIP_TYPE) == 0u);                    /* vsync'd */
        A((live & RB_HUBP_FLIP_UPDATE_LOCK) == 0u);             /* not locked */
        A(((live & RB_HUBP_FLIP_PENDING_DELAY) >> 20) == 65u);  /* the one set field */
        A(rb_hubp_flip_pending(live) == 0);
    }
    A(rb_hubp_flip_pending(0x00000100u) == 1);
    A(rb_hubp_flip_pending(0x00000000u) == 0);
    A(rb_hubp_flip_pending(0xfffffeffu) == 0);   /* every OTHER bit set */

    /* 2e (2xm). The defer decision. Boot 217 measured 593 hits in 33 877
     * checks - 1.75 % of flips programmed over an armed, unlatched one - so
     * this is the mechanism behind the tearing, counted before it was acted
     * on (2xk was the instrument, this is the fix). */
    A(RB_HUBP_FLIP2_PENDING_MIN_TIME == 0x000000ffu);
    A(RB_HUBP_FLIP2_GSL_ENABLE       == 0x00000100u);
    A(RB_HUBP_FLIP2_TRIPLE_BUFFER    == 0x00000400u);
    {
        /* The two live values, both measured: boot 216 and boot 217 agree. */
        const uint32_t ctl2_live = 0x00000040u;  /* PENDING_MIN_TIME 64 only */
        A((ctl2_live & RB_HUBP_FLIP2_TRIPLE_BUFFER) == 0u);
        A((ctl2_live & RB_HUBP_FLIP2_GSL_ENABLE) == 0u);
        A((ctl2_live & RB_HUBP_FLIP2_PENDING_MIN_TIME) == 64u);
        A((ctl2_live & ~(uint32_t)(RB_HUBP_FLIP2_PENDING_MIN_TIME |
                                   RB_HUBP_FLIP2_GSL_ENABLE |
                                   RB_HUBP_FLIP2_TRIPLE_BUFFER)) == 0u);

        /* Idle: nothing pending, so write. This is 98.25 % of boot 217. */
        A(rb_hubp_flip_would_discard(0x04100000u, ctl2_live) == 0);
        /* Pending with one slot: deferring is the whole point. */
        A(rb_hubp_flip_would_discard(0x04100100u, ctl2_live) == 1);
        /* The guard: pending but triple-buffered discards nothing, so write.
         * Mutating it away makes this assertion fail, which is the only
         * reason the guard can be trusted on a machine that does not set
         * the bit (rule 39's shape - the configuration lacks the symptom). */
        A(rb_hubp_flip_would_discard(0x04100100u,
                                     ctl2_live | RB_HUBP_FLIP2_TRIPLE_BUFFER) == 0);
        A(rb_hubp_flip_would_discard(0x04100000u,
                                     ctl2_live | RB_HUBP_FLIP2_TRIPLE_BUFFER) == 0);
        /* PENDING is the ONLY bit that defers: every other bit set, pending
         * clear, must still write - otherwise a field we have not identified
         * could silently stall every flip. */
        A(rb_hubp_flip_would_discard(0xfffffeffu, 0u) == 0);
        A(rb_hubp_flip_would_discard(0x00000100u, 0u) == 1);
    }

    /* 2f (2xn). The NotReady hold, and above all that it is BOUNDED. */
    {
        const uint32_t idle = 0x04100000u;   /* measured, not pending */
        const uint32_t pend = 0x04100100u;   /* same, PENDING set */

        /* Not pending: always ready, deadline irrelevant - including a
         * deadline far in the future, which is the case that would otherwise
         * hold forever. */
        A(rb_hubp_hold_notready(idle, 0u, 0u) == 0);
        A(rb_hubp_hold_notready(idle, 0u, ~0ull) == 0);
        A(rb_hubp_hold_notready(idle, 100u, 200u) == 0);

        /* Pending and inside the deadline: hold. */
        A(rb_hubp_hold_notready(pend, 100u, 200u) == 1);

        /* THE SAFETY PROPERTY: pending but past the deadline answers ready,
         * so a pending bit that never clears cannot wedge the display. */
        A(rb_hubp_hold_notready(pend, 200u, 200u) == 0);   /* == is expired */
        A(rb_hubp_hold_notready(pend, 201u, 200u) == 0);
        A(rb_hubp_hold_notready(pend, ~0ull, 200u) == 0);
        /* A deadline never set (0) is expired for every clock value, so an
         * arm that forgets to set one fails SAFE, not open. */
        A(rb_hubp_hold_notready(pend, 0u, 0u) == 0);
        A(rb_hubp_hold_notready(pend, 1u, 0u) == 0);

        /* Only PENDING holds: every other bit set with PENDING clear must
         * answer ready, or an unidentified field could stall every frame. */
        A(rb_hubp_hold_notready(0xfffffeffu, 0u, ~0ull) == 0);
        /* ...and triple buffering does NOT exempt it here (unlike
         * rb_hubp_flip_would_discard): our address is still unlatched. */
        A(rb_hubp_hold_notready(pend | RB_HUBP_FLIP2_TRIPLE_BUFFER,
                                100u, 200u) == 1);
    }

    /* 3. Out-of-range asks answer 0, not a wild offset. */
    A(rb_hubp_dw(RB_HUBP_COUNT, RB_HUBP_REG_ADDR) == 0);
    A(rb_hubp_dw(0, RB_HUBP_REG_COUNT) == 0);

    /* 4. The live pipe 2os measured is accepted, and each field on its own is
     * load-bearing: move any one and the state gate refuses, with a code that
     * says which. A single `return RB_HUBP_OK` would fail all three. */
    A(rb_hubp_check_state(RB_HUBP_EXPECT_PITCH, RB_HUBP_EXPECT_CONFIG,
                          RB_HUBP_EXPECT_ADDR_HI) == RB_HUBP_OK);
    A(rb_hubp_check_state(0x0780, RB_HUBP_EXPECT_CONFIG,
                          RB_HUBP_EXPECT_ADDR_HI) == RB_HUBP_REFUSE_PITCH);
    A(rb_hubp_check_state(RB_HUBP_EXPECT_PITCH, 0x0a,
                          RB_HUBP_EXPECT_ADDR_HI) == RB_HUBP_REFUSE_CONFIG);
    A(rb_hubp_check_state(RB_HUBP_EXPECT_PITCH, RB_HUBP_EXPECT_CONFIG,
                          0x000000f5) == RB_HUBP_REFUSE_STATE_HI);
    /* The pitch register is pitch-1, so 1920 itself is the WRONG value and
     * must be refused - the trap 2os's annotation ("1920 px" for 0x77f)
     * sets for anyone reading the journal too quickly. */
    A(rb_hubp_check_state(1920, RB_HUBP_EXPECT_CONFIG,
                          RB_HUBP_EXPECT_ADDR_HI) == RB_HUBP_REFUSE_PITCH);

    /* 5. Boot 194's measured surface address is accepted, with one frame. */
    const uint64_t frame = 1920ULL * 1080ULL * 4ULL;   /* 0x7e9000 */
    A(frame == 8294400);
    A(rb_hubp_check_addr(0xf41002a000ULL, RB_HUBP_EXPECT_ADDR_HI, frame) == RB_HUBP_OK);
    /* And the address the pipe already holds - writing it back is a no-op,
     * which is the restore path, so it must be legal. */
    A(rb_hubp_check_addr(RB_MC_CARVEOUT_BASE, RB_HUBP_EXPECT_ADDR_HI, frame) == RB_HUBP_OK);

    /* 6. Every refusal the address gate owes, each reachable on its own. */
    A(rb_hubp_check_addr(0, RB_HUBP_EXPECT_ADDR_HI, frame) == RB_HUBP_REFUSE_ZERO);
    /* Below the carve-out: a GPUVM address, which is the realistic wrong
     * input - Apple's GPUVM space is 0xFF_BF.., above, and a system-physical
     * one (0x7_8000_0000) is below. Both must be refused. */
    A(rb_hubp_check_addr(0x780000000ULL, RB_HUBP_EXPECT_ADDR_HI, frame) == RB_HUBP_REFUSE_RANGE);
    A(rb_hubp_check_addr(0xffbf00000000ULL, RB_HUBP_EXPECT_ADDR_HI, frame) == RB_HUBP_REFUSE_RANGE);
    /* Exactly one past the top. */
    A(rb_hubp_check_addr(RB_MC_CARVEOUT_BASE + RB_MC_CARVEOUT_SPAN,
                         RB_HUBP_EXPECT_ADDR_HI, 0) == RB_HUBP_REFUSE_RANGE);
    /* The last legal 256-byte slot is in range on its own but cannot hold a
     * frame - the check that stops a fetch running off the end of VRAM. */
    A(rb_hubp_check_addr(RB_MC_CARVEOUT_BASE + RB_MC_CARVEOUT_SPAN - 0x100,
                         RB_HUBP_EXPECT_ADDR_HI, 0) == RB_HUBP_OK);
    A(rb_hubp_check_addr(RB_MC_CARVEOUT_BASE + RB_MC_CARVEOUT_SPAN - 0x100,
                         RB_HUBP_EXPECT_ADDR_HI, frame) == RB_HUBP_REFUSE_FRAME);
    /* Misalignment, at the finest granularity that matters. */
    A(rb_hubp_check_addr(0xf41002a000ULL + 1, RB_HUBP_EXPECT_ADDR_HI, frame) == RB_HUBP_REFUSE_ALIGN);
    A(rb_hubp_check_addr(0xf41002a0ffULL, RB_HUBP_EXPECT_ADDR_HI, frame) == RB_HUBP_REFUSE_ALIGN);
    A(rb_hubp_check_addr(0xf41002a100ULL, RB_HUBP_EXPECT_ADDR_HI, frame) == RB_HUBP_OK);
    /* A HIGH dword that would move. In-carve-out addresses all share 0xf4, so
     * this fires only when the pipe's own HIGH is something else - i.e. when
     * the state gate was passed with a stale reading. Defence in depth. */
    A(rb_hubp_check_addr(0xf41002a000ULL, 0x000000f5, frame) == RB_HUBP_REFUSE_HI_MOVED);

    /* 7. No verdict collides with OK, so a caller testing `!= RB_HUBP_OK`
     * cannot miss a refusal. */
    A(RB_HUBP_OK == 0);
    A(RB_HUBP_REFUSE_INDEX != RB_HUBP_OK && RB_HUBP_REFUSE_FRAME != RB_HUBP_OK);

    /* 8. 2wm: the write gate. The measured flag word is 0x75 (2we), which
     * carries bit 0x1, so the gated form must accept it - a regression here
     * would silently stop the display that 2wi got working. */
    A(rb_hubp_should_write(0x75, 0) == 1);
    A(rb_hubp_should_write(0x75, 1) == 1);
    /* A colour-matrix-only transaction (bit 0x10, bit 0x1 clear) is exactly
     * the case 2wh refuses and -rbgpufbflipall deliberately accepts. The two
     * must DIFFER, or the boot-arg cannot answer anything. */
    A(rb_hubp_should_write(0x10, 0) == 0);
    A(rb_hubp_should_write(0x10, 1) == 1);
    /* An empty flag word is still refused when gated, still written when not:
     * the override is the caller's choice, not a function of the flags. */
    A(rb_hubp_should_write(0, 0) == 0);
    A(rb_hubp_should_write(0, 1) == 1);
    /* And the gate reads bit 0x1 ONLY - not "any bit set", which would accept
     * 0x10 and make the whole experiment a no-op. */
    A(rb_hubp_should_write(RB_HUBP_FLAG_PLANE, 0) == 1);
    A(RB_HUBP_FLAG_PLANE == 0x1ull);

#undef A
    printf("hubp: offsets cross-checked against rb_regs.h, pipe-state, "
           "address guards and the 2wm write gate%s\n",
           bad ? " FAILED" : ": passed");
    return bad;
}
