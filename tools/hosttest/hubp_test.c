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

#undef A
    printf("hubp: offsets cross-checked against rb_regs.h, pipe-state and "
           "address guards%s\n", bad ? " FAILED" : ": passed");
    return bad;
}
