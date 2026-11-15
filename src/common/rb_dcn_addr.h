/* SPDX-License-Identifier: MIT */
#ifndef RB_DCN_ADDR_H
#define RB_DCN_ADDR_H

/* 3dcn-b: what to do with a register access Apple's dcn302 DAL makes on this
 * dcn312 chip - REDIRECT it, BLOCK it, or let it through.
 *
 * THE UNITS, AND A CORRECTION TO 3dcn. The first draft of this file keyed the
 * table on ABSOLUTE dword addresses (base + offset), because that is how
 * 660m-Metal publishes theirs and the two tables agreed 298 for 298. It is
 * the wrong key for OUR hook point, and this project had already measured
 * why. 2pn (boot 143) read four accesses through
 * `AmdRegisterAccess::hwReadReg32(uint)` - 0xd31, 0xde3, 0xde7, 0xde8 - and
 * resolved them 4 for 4 as BASE_IDX 2 RAW DWORD OFFSETS
 * (`CM0_CM_GAMUT_REMAP_C21_C22` and three `CM0_CM_SHAPER_RAMA_*`), and 0 for
 * 4 as absolute addresses in any base. That accessor takes ONE argument, so
 * there is no base in the call at all: the base is applied inside. An
 * absolute table cannot be applied where the argument is an offset, so the
 * generator now emits offsets restricted to one BASE_IDX, and absolute mode
 * survives only as the cross-check against their published table.
 *
 * Why the two projects can disagree about this: 660m-Metal runs 25G229 and
 * instantiates Apple's Navi **23** controller; we run 25G342 and Navi **21**.
 * Their `isDisplayReg()` accepts `[0xc0, 0x1260)` and `[0x34c0, 0x8000)`,
 * which is an absolute reading. Both can be true of different builds and
 * different controller classes, and ours is the one measured on this machine
 * - rule 41's shape applied to an ABI rather than to a constant.
 *
 * WHY THREE OUTCOMES AND NOT TWO. 2pm blocked every moved offset and 2po
 * measured the result: *"Apple's dcn302 DAL cannot bring up DCN 3.1.2 by
 * filtering writes. DCHUBBUB is why"* - 46 DCHUBBUB registers blocked, and
 * DCN cannot come up without its hub. Redirecting instead of blocking removes
 * exactly that: **all 46 of those DCHUBBUB offsets have a destination in
 * 3.1.2**, so the register the DAL meant to reach is reachable. What cannot
 * be redirected is a register that does not exist on this chip - the 11
 * `DOMAIN4..9_PG_*` offsets are the clearest case, since 3.1.2 has
 * `DOMAIN16..18` instead - and those must still be blocked, because the
 * offset lands on a REAL but DIFFERENT register here: `0x089` is
 * `DOMAIN4_PG_STATUS` there and `DOMAIN16_PG_CONFIG` here, so a status read
 * becomes a power-gate config write for the wrong domain.
 *
 * So, in order:
 *   1. the offset moved and its register exists here  -> REDIRECT  (198)
 *   2. no destination, and the offset means something else here -> BLOCK (29)
 *   3. otherwise -> PASS
 *
 * Of the 160 offsets `rb_dcn_deny.h` calls dangerous, 131 fall in case 1 and
 * 29 in case 2. A further 67 offsets are in case 1 without being dangerous:
 * they moved, but nothing real sits at the old offset here, so today they are
 * silent writes into a hole. Those are not harmless - they are writes the DAL
 * believes it made - and redirecting them is the point.
 *
 * WHAT THIS IS NOT. Not a write policy for THIS project's own code -
 * `rb_dcn_is_denied()` keeps that job and is untouched. Not a claim about
 * BASE_IDX 3: 47 registers leave base 2 between the generations and cannot be
 * redirected on a path that is never told which segment to use, and 2pn
 * already recorded base 3 (audio: AFMT, VPG, DIG) as uncovered. Not a field
 * remap either - a register can keep its address while its FIELDS move, which
 * this table cannot see and which 660m-Metal handles separately.
 */

#include <stdint.h>

#include "rb_dcn_remap.h"
#include "rb_dcn_deny.h"

#define RB_DCN_ACT_PASS     0u  /* same register here; let it through      */
#define RB_DCN_ACT_REDIRECT 1u  /* moved; *out holds the 3.1.2 offset      */
#define RB_DCN_ACT_BLOCK    2u  /* no destination and the offset is unsafe */

/* The BASE_IDX every offset here is understood in. An access known to be in
 * another segment must not be passed to these functions. */
#define RB_DCN_ADDR_BASE_IDX RB_DCN_DENY_BASE_IDX

/* The 3.1.2 offset for a 3.0.2 one, or the input unchanged if it did not
 * move. `moved` distinguishes the two, so a caller cannot read "not in the
 * table" as "maps to itself" (rule 14). */
static inline uint32_t rb_dcn_off_312(uint32_t off302, int *moved)
{
    uint32_t lo = 0, hi = (uint32_t)RB_DCN_REMAP_COUNT;

    while (lo < hi) {
        const uint32_t mid = lo + ((hi - lo) / 2u);
        const uint32_t key = rb_dcn_remap_tbl[mid].from;
        if (key == off302) {
            if (moved) { *moved = 1; }
            return rb_dcn_remap_tbl[mid].to;
        }
        if (key < off302) { lo = mid + 1u; } else { hi = mid; }
    }
    if (moved) { *moved = 0; }
    return off302;
}

/* 1 if `off` is one of the offsets rb_dcn_deny.h calls dangerous: nothing of
 * that name exists in 3.1.2, but a DIFFERENT real register sits at the
 * offset. Kept separate from the table lookup so the two questions stay
 * distinguishable in the caller and in a trace. */
static inline int rb_dcn_off_dangerous(uint32_t off)
{
    uint32_t i;

    for (i = 0; i < (uint32_t)RB_DCN_DENY_COUNT; i++) {
        if (rb_dcn_deny[i] == off) { return 1; }
    }
    return 0;
}

/* The decision for one access. `out`, when given, receives the offset to use
 * and is written on EVERY outcome including BLOCK - where it is the unchanged
 * input, so a trace can report what was refused rather than only that
 * something was (rule 14). */
static inline uint32_t rb_dcn_dal_action(uint32_t off302, uint32_t *out)
{
    int moved = 0;
    const uint32_t to = rb_dcn_off_312(off302, &moved);

    if (moved) {
        if (out) { *out = to; }
        return RB_DCN_ACT_REDIRECT;
    }
    if (out) { *out = off302; }
    return rb_dcn_off_dangerous(off302) ? RB_DCN_ACT_BLOCK : RB_DCN_ACT_PASS;
}

/* 1 if `tbl` is sorted by `from` with no duplicates, i.e. if a binary search
 * is valid on it.
 *
 * IT TAKES THE TABLE RATHER THAN READING THE GLOBAL ONE, and that is the
 * whole point of the signature. The first draft checked `rb_dcn_remap_tbl`
 * directly, and the rule-48 sweep caught it: breaking the comparison FIRED
 * NOTHING, because the only table it could ever see is the real one, the real
 * one is sorted, and so it returned 1 either way. The check had no coverage -
 * the same "the term carries no information" defect this project has now
 * written into its own guards four times. With the table as a parameter the
 * hosttest can feed it an unsorted fixture and a duplicated one, and breaking
 * the comparison fails a test. */
static inline int rb_dcn_addr_sorted(const struct rb_dcn_remap *tbl,
                                     uint32_t count)
{
    uint32_t i;

    if (!tbl || count == 0u) { return 0; }
    for (i = 1u; i < count; i++) {
        if (tbl[i].from <= tbl[i - 1u].from) {
            return 0;
        }
    }
    return 1;
}

/* The same question asked of the table that actually got compiled in. */
static inline int rb_dcn_addr_table_ok(void)
{
    return rb_dcn_addr_sorted(rb_dcn_remap_tbl, (uint32_t)RB_DCN_REMAP_COUNT);
}

#endif /* RB_DCN_ADDR_H */
