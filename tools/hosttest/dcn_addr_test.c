/* SPDX-License-Identifier: MIT */
/* 3dcn: the DCN 3.0.2 -> 3.1.2 address translation.
 *
 * THE CONTROL IS AN INDEPENDENT GENERATOR. `tools/re/dcn_remap_gen.py` builds
 * the table from two pinned upstream headers; 660m-Metal's `dcnremap.hpp` was
 * built from the same headers by a different script, and the two agree on
 * **298 of 298 entries, with none present in only one of them**. The entries
 * pinned below are spot checks from that comparison - the full diff is the
 * generator's own `--check-against`, re-runnable at any time.
 *
 * The derivation is also checked against this board rather than only against
 * another project: `build/out/rbdump tests/data/rembrandt-680m-tufa15.bin`
 * prints the DMU bases as 0x12 0xc0 0x34c0 0x9000 0x2403c00, and base[3] =
 * 0x9000 is what makes the 47 registers that CHANGE BASE_IDX mappable. Taking
 * that number from 660m-Metal's table instead would have made the
 * cross-check circular for exactly the entries that needed it.
 */

#include "rb_dcn_addr.h"
#include <stdio.h>

static int fails;
#define A(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

int dcn_addr_tests(void);

int dcn_addr_tests(void)
{
    int moved;

    fails = 0;

    /* 1. The table the binary search runs on is sorted and duplicate-free.
     * The generator promises this; a promise is not evidence about the file
     * that got compiled in. */
    A(rb_dcn_addr_table_ok() == 1);
    A(RB_DCN_REMAP_COUNT == 298);

    /* 1b. And the sortedness predicate is exercised against tables that are
     * NOT sorted, which is the only way it has any coverage at all. The
     * rule-48 sweep established that: with the predicate reading the global
     * table, breaking its comparison fired nothing, because the real table is
     * sorted and no other input existed. These fixtures are that input. */
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
        /* Degenerate inputs refuse rather than claiming a valid table. */
        A(rb_dcn_addr_sorted(good, 0) == 0);
        A(rb_dcn_addr_sorted(0, 3) == 0);
        /* One entry is trivially sorted, and must not be refused - a check
         * that demanded two would reject a table with a single moved
         * register. */
        A(rb_dcn_addr_sorted(good, 1) == 1);
    }

    /* 2. Spot checks, all agreed by both generators.
     *
     * DOMAIN16_PG_CONFIG is the worked example that validates the whole
     * derivation: 3.0.2 offset 0x00a1 and 3.1.2 offset 0x0089, both BASE_IDX
     * 2, against the chip-read base 0x34c0 -> 0x3561 and 0x3549. */
    moved = -1;
    A(rb_dcn_addr_312(0x3561u, &moved) == 0x3549u);
    A(moved == 1);
    /* DCN_VM_FB_LOCATION_BASE: 0x0493 -> 0x0475 in segment 2. A write to the
     * wrong one of these points DCN's framebuffer aperture somewhere else,
     * which is the class of mistake this table exists to prevent. */
    A(rb_dcn_addr_312(0x3953u, &moved) == 0x3935u);
    A(moved == 1);
    /* DC_IP_REQUEST_CNTL - the gate DCN power-domain access goes through. */
    A(rb_dcn_addr_312(0x3572u, &moved) == 0x3553u);
    A(moved == 1);

    /* 3. A register that CHANGES BASE_IDX, which is the case the equal-bases
     * shortcut could not handle and base[3] = 0x9000 unlocked.
     * DC_PERFMON15_PERFCOUNTER_CNTL is 0x13d0 in BASE_IDX 2 on 3.0.2
     * (0x34c0 + 0x13d0 = 0x4890) and 0x08c7 in BASE_IDX **3** on 3.1.2
     * (0x9000 + 0x08c7 = 0x98c7). Each side resolved against its own base. */
    A(rb_dcn_addr_312(0x4890u, &moved) == 0x98c7u);
    A(moved == 1);
    A(0x34c0u + 0x13d0u == 0x4890u);
    A(0x9000u + 0x08c7u == 0x98c7u);

    /* 4. AN UNMOVED ADDRESS COMES BACK UNCHANGED, and says so. This is the
     * majority case by far - the table holds only the 298 that moved, out of
     * thousands - so getting it wrong would corrupt almost every access.
     * OTG0_CONTROL 0x1b41 and HUBP0_SURFACE_PITCH 0x077f are both registers
     * this project reads on hardware and neither is in the table. */
    moved = -1;
    A(rb_dcn_addr_312(0x1b41u, &moved) == 0x1b41u);
    A(moved == 0);
    A(rb_dcn_addr_312(0x077fu, &moved) == 0x077fu);
    A(moved == 0);
    A(rb_dcn_addr_312(0u, &moved) == 0u);
    A(moved == 0);
    A(rb_dcn_addr_312(0xffffffffu, &moved) == 0xffffffffu);
    A(moved == 0);

    /* 5. `moved` is optional and a NULL must not be written through. */
    A(rb_dcn_addr_312(0x3561u, 0) == 0x3549u);
    A(rb_dcn_addr_312(0x1b41u, 0) == 0x1b41u);

    /* 6. The search finds the FIRST and LAST entries, which a binary search
     * with an off-by-one on either bound would miss while still passing every
     * interior lookup above. */
    A(rb_dcn_addr_312(rb_dcn_remap_tbl[0].from, &moved)
      == rb_dcn_remap_tbl[0].to);
    A(moved == 1);
    A(rb_dcn_addr_312(rb_dcn_remap_tbl[RB_DCN_REMAP_COUNT - 1u].from, &moved)
      == rb_dcn_remap_tbl[RB_DCN_REMAP_COUNT - 1u].to);
    A(moved == 1);

    /* 7. EVERY entry is findable, and every `to` differs from its `from`.
     * The second half matters: an entry that maps to itself would be a
     * generator bug that this lookup could never reveal, since the unmoved
     * path returns the same value. */
    {
        uint32_t i;
        for (i = 0; i < (uint32_t)RB_DCN_REMAP_COUNT; i++) {
            int m = -1;
            A(rb_dcn_addr_312(rb_dcn_remap_tbl[i].from, &m)
              == rb_dcn_remap_tbl[i].to);
            A(m == 1);
            A(rb_dcn_remap_tbl[i].from != rb_dcn_remap_tbl[i].to);
        }
    }

    /* 8. Translation is not an involution and must not be applied twice. A
     * caller that remapped an already-remapped address would corrupt it, so
     * the property is pinned: at least one `to` is itself a `from` in the
     * table, which is exactly why double application is unsafe. */
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
        printf("dcn addr: 3.0.2 -> 3.1.2 translation, %u moved registers, "
               "sortedness, the BASE_IDX-change case, unmoved passthrough "
               "and the double-application hazard: passed\n",
               (unsigned)RB_DCN_REMAP_COUNT);
    }
    return fails;
}
