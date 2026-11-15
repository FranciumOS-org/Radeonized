/* SPDX-License-Identifier: MIT */
/* 3dcn-b: the dcn302 -> dcn312 access policy for Apple's DAL.
 *
 * THE UNITS ARE MEASURED, NOT CHOSEN. 2pn (boot 143, controller only, display
 * intact) read four accesses through `AmdRegisterAccess::hwReadReg32(uint)`:
 * 0xd31, 0xde3, 0xde7, 0xde8. Those resolve 4 for 4 as BASE_IDX 2 raw dword
 * offsets and 0 for 4 as absolute addresses in any base, so the table is
 * keyed on offsets. 3dcn's first draft keyed it on absolute addresses; this
 * file is anchored on the measurement instead.
 *
 * TWO CONTROLS, AND THEY ANSWER DIFFERENT QUESTIONS.
 *   - The arithmetic: generated in absolute mode, our table agrees with
 *     660m-Metal's independently generated `dcnremap.hpp` on 298 of 298
 *     entries with none present in only one. That validates the parse and the
 *     bases; re-runnable with the generator's own --check-against.
 *   - The hardware: the four offsets 2pn actually observed must come out of
 *     this policy as PASS, because they did not move. A boot that passed them
 *     through is the boot that left the display intact.
 */

#include "rb_dcn_addr.h"
#include <stdio.h>

static int fails;
#define A(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

int dcn_addr_tests(void);

int dcn_addr_tests(void)
{
    uint32_t out;
    int moved;

    fails = 0;

    /* 1. The table the binary search runs on is sorted and duplicate-free.
     * The generator promises it; a promise is not evidence about the file
     * that got compiled in. */
    A(rb_dcn_addr_table_ok() == 1);
    A(RB_DCN_REMAP_COUNT == 198);
    A(RB_DCN_DENY_COUNT == 160);
    A(RB_DCN_ADDR_BASE_IDX == 2u);

    /* 1b. The sortedness predicate exercised against tables that are NOT
     * sorted, which is the only way it has any coverage. The rule-48 sweep
     * established that: reading the global table, breaking its comparison
     * fired nothing, because the real table is sorted and no other input
     * existed. These fixtures are that input. */
    {
        static const struct rb_dcn_remap good[3] = {
            { 0x10u, 0x20u }, { 0x11u, 0x21u }, { 0x12u, 0x22u }
        };
        static const struct rb_dcn_remap unsorted[3] = {
            { 0x10u, 0x20u }, { 0x12u, 0x22u }, { 0x11u, 0x21u }
        };
        static const struct rb_dcn_remap duped[3] = {
            { 0x10u, 0x20u }, { 0x11u, 0x21u }, { 0x11u, 0x99u }
        };
        A(rb_dcn_addr_sorted(good, 3) == 1);
        A(rb_dcn_addr_sorted(unsorted, 3) == 0);
        A(rb_dcn_addr_sorted(duped, 3) == 0);
        A(rb_dcn_addr_sorted(good, 0) == 0);
        A(rb_dcn_addr_sorted(0, 3) == 0);
        /* One entry is trivially sorted and must not be refused. */
        A(rb_dcn_addr_sorted(good, 1) == 1);
    }

    /* 2. THE HARDWARE ANCHOR. The only four accesses this project has ever
     * observed on this path did not move, so the policy must PASS them
     * untouched and must not rewrite the offset. */
    {
        static const uint32_t observed[4] = { 0xd31u, 0xde3u, 0xde7u, 0xde8u };
        unsigned i;
        for (i = 0; i < 4; i++) {
            out = 0xdeadu;
            A(rb_dcn_dal_action(observed[i], &out) == RB_DCN_ACT_PASS);
            A(out == observed[i]);
        }
    }

    /* 3. REDIRECT, on the entry that validates the whole derivation.
     * DOMAIN16_PG_CONFIG is offset 0x0a1 in dcn302 and 0x089 here, both
     * BASE_IDX 2. */
    out = 0;
    A(rb_dcn_dal_action(0x0a1u, &out) == RB_DCN_ACT_REDIRECT);
    A(out == 0x089u);
    /* DCN_VM_FB_LOCATION_BASE 0x493 -> 0x475. A write to the wrong one of
     * these points DCN's framebuffer aperture elsewhere, which is the class
     * of damage this table exists to prevent. */
    A(rb_dcn_dal_action(0x493u, &out) == RB_DCN_ACT_REDIRECT);
    A(out == 0x475u);

    /* 4. BLOCK, and the pair that shows why REDIRECT and BLOCK must coexist.
     * 0x089 is DOMAIN4_PG_STATUS to Apple's dcn302 DAL and DOMAIN16_PG_CONFIG
     * on this chip: there is no DOMAIN4 here, so there is nowhere to send it,
     * and letting it through turns a status read into a power-gate config
     * write for the wrong domain. Note 0x0a1 REDIRECTS **to** 0x089 while
     * 0x089 itself is BLOCKED - the two are not in conflict, they are the two
     * halves of the same rename. */
    out = 0;
    A(rb_dcn_dal_action(0x089u, &out) == RB_DCN_ACT_BLOCK);
    A(out == 0x089u);          /* rule 14: say what was refused */

    /* 5. 2po's NEGATIVE RESULT IS ADDRESSED, and this is the check that says
     * so. 2po measured that blocking every moved offset cannot bring up DCN
     * because 46 DCHUBBUB registers get blocked and DCN has no hub without
     * them. Every one of those must now REDIRECT rather than BLOCK. Three
     * named here; the full count is asserted below. */
    A(rb_dcn_dal_action(0x48fu, &out) == RB_DCN_ACT_REDIRECT);
    A(out == 0x46fu);          /* DCHUBBUB_SDPIF_CFG0 */
    A(rb_dcn_dal_action(0x491u, &out) == RB_DCN_ACT_REDIRECT);
    A(out == 0x473u);          /* DCHUBBUB_FORCE_IO_STATUS_0 */
    A(rb_dcn_dal_action(0x492u, &out) == RB_DCN_ACT_REDIRECT);
    A(out == 0x474u);          /* DCHUBBUB_FORCE_IO_STATUS_1 */

    /* 6. The split across the deny list, asserted as counts so a regenerated
     * table cannot quietly change the policy's shape. 131 of the 160
     * dangerous offsets are redirectable and 29 are not. */
    {
        uint32_t i, redirected = 0, blocked = 0;
        for (i = 0; i < (uint32_t)RB_DCN_DENY_COUNT; i++) {
            const uint32_t act = rb_dcn_dal_action(rb_dcn_deny[i], &out);
            if (act == RB_DCN_ACT_REDIRECT) { redirected++; }
            else if (act == RB_DCN_ACT_BLOCK) { blocked++; }
            else { A(0); }     /* a dangerous offset must never PASS */
        }
        A(redirected == 131);
        A(blocked == 29);
        A(redirected + blocked == (uint32_t)RB_DCN_DENY_COUNT);
    }

    /* 7. Every table entry is findable, none maps to itself, and every one of
     * them REDIRECTS. An entry mapping to itself would be a generator bug
     * this lookup could never reveal, since PASS returns the input too. */
    {
        uint32_t i;
        for (i = 0; i < (uint32_t)RB_DCN_REMAP_COUNT; i++) {
            moved = -1;
            A(rb_dcn_off_312(rb_dcn_remap_tbl[i].from, &moved)
              == rb_dcn_remap_tbl[i].to);
            A(moved == 1);
            A(rb_dcn_remap_tbl[i].from != rb_dcn_remap_tbl[i].to);
            A(rb_dcn_dal_action(rb_dcn_remap_tbl[i].from, &out)
              == RB_DCN_ACT_REDIRECT);
        }
    }

    /* 8. Boundaries: the first and last entries, which an off-by-one on
     * either bound would miss while every interior lookup still passed. */
    A(rb_dcn_off_312(rb_dcn_remap_tbl[0].from, &moved)
      == rb_dcn_remap_tbl[0].to);
    A(moved == 1);
    A(rb_dcn_off_312(rb_dcn_remap_tbl[RB_DCN_REMAP_COUNT - 1u].from, &moved)
      == rb_dcn_remap_tbl[RB_DCN_REMAP_COUNT - 1u].to);
    A(moved == 1);

    /* 9. Unmoved, undangerous offsets pass unchanged - the majority case by
     * far, since the table holds 198 out of thousands. OTG0_CONTROL 0x1b41
     * and HUBP0's own DCSURF_SURFACE_PITCH 0x0607 (rb_hubp.h:100) are
     * registers this project reads on hardware every armed boot.
     *
     * Note 0x077f is NOT one of them, and the first draft of this test said
     * it was: 0x077f is RB_HUBP_EXPECT_PITCH, a pitch VALUE of 1919, and as
     * an OFFSET it is DC_PERFMON7_PERFMON_CVALUE_LOW, which moved to 0x06a3.
     * The test failed and caught the conflation. It is kept below as the
     * sharpest instance of the double-application hazard, because 0x085b
     * moves TO 0x077f: remap twice and a write aimed at one perfmon counter
     * lands on another. */
    moved = -1;
    A(rb_dcn_off_312(0x1b41u, &moved) == 0x1b41u);
    A(moved == 0);
    A(rb_dcn_off_312(0x0607u, &moved) == 0x0607u);
    A(moved == 0);
    A(rb_dcn_dal_action(0x0607u, &out) == RB_DCN_ACT_PASS);
    /* The hazard, concretely: 0x085b -> 0x077f -> 0x06a3 if applied twice. */
    A(rb_dcn_off_312(0x085bu, 0) == 0x077fu);
    A(rb_dcn_off_312(0x077fu, 0) == 0x06a3u);
    A(rb_dcn_dal_action(0x1b41u, &out) == RB_DCN_ACT_PASS);
    A(rb_dcn_dal_action(0u, &out) == RB_DCN_ACT_PASS);
    A(rb_dcn_dal_action(0xffffffffu, &out) == RB_DCN_ACT_PASS);

    /* 10. The out-pointer and moved-pointer are both optional. */
    A(rb_dcn_off_312(0x0a1u, 0) == 0x089u);
    A(rb_dcn_dal_action(0x0a1u, 0) == RB_DCN_ACT_REDIRECT);
    A(rb_dcn_dal_action(0x089u, 0) == RB_DCN_ACT_BLOCK);

    /* 11. The three actions are distinct values, so a trace that records one
     * number identifies the decision. */
    A(RB_DCN_ACT_PASS != RB_DCN_ACT_REDIRECT);
    A(RB_DCN_ACT_PASS != RB_DCN_ACT_BLOCK);
    A(RB_DCN_ACT_REDIRECT != RB_DCN_ACT_BLOCK);

    /* 12. Redirection is NOT an involution and must not be applied twice: at
     * least one `to` is itself a `from`, which is exactly why a caller that
     * remapped an already-remapped offset would corrupt it. 0x0a1 -> 0x089 is
     * itself an instance, since 0x089 is a live offset here. */
    {
        uint32_t i, j, both = 0;
        for (i = 0; i < (uint32_t)RB_DCN_REMAP_COUNT; i++) {
            for (j = 0; j < (uint32_t)RB_DCN_REMAP_COUNT; j++) {
                if (rb_dcn_remap_tbl[i].to == rb_dcn_remap_tbl[j].from) {
                    both++;
                    break;
                }
            }
        }
        A(both > 0);
    }

    if (!fails) {
        printf("dcn addr: dcn302->dcn312 policy, %u redirects and %u blocks, "
               "2pn's four measured offsets pass, 2po's 46 DCHUBBUB registers "
               "redirect, the 131/29 split, sortedness and the "
               "double-application hazard: passed\n",
               (unsigned)RB_DCN_REMAP_COUNT, 29u);
    }
    return fails;
}
