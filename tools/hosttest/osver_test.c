/* The OS support matrix (2xz). Like the default-on table, the matrix IS data,
 * so this checks the data and the one decision function that reads it.
 *
 * What these assertions are for: a row that claims an artifact it has not
 * measured is indistinguishable, from inside the kext, from a row that has -
 * and the consequence is a measured constant written on an OS it was never
 * measured on, which is rule 17. So the claims are pinned here, with the
 * evidence named, and a change to any of them has to come through this file. */
#include <stdio.h>
#include <string.h>

#include "rb_osver.h"
/* 2xz: the cave rows are checked AGAINST the matrix here, because they are
 * the one artifact whose claim bit and whose data live in two different
 * files. A row with no bit, or a bit with no row, is drift that nothing else
 * in the build would notice. */
#include "rbgpu_mtlcave.h"

#define CHECK(cond, what) do { \
    printf((cond) ? "ok   %s\n" : "FAIL %s\n", (what)); \
    failures += !(cond); \
} while (0)

int osver_tests(void);

int osver_tests(void)
{
    int failures = 0;
    int i, j;

    /* --- the range ---------------------------------------------------- */

    CHECK(RB_OS_MIN_DARWIN == RB_OS_VENTURA,
          "the floor is Ventura - the user's ask, and the oldest IOGraphics shape in scope");
    CHECK(!rb_os_in_range(RB_OS_VENTURA - 1),
          "Monterey and earlier are out of range");
    CHECK(rb_os_in_range(RB_OS_VENTURA) && rb_os_in_range(RB_OS_SONOMA) &&
          rb_os_in_range(RB_OS_SEQUOIA) && rb_os_in_range(RB_OS_TAHOE),
          "Ventura through Tahoe are all in range");
    /* The ceiling is on the CLAIM, not on loading: an OS newer than this tree
     * knows must still load, because a boot that refuses teaches nothing. */
    CHECK(rb_os_in_range(RB_OS_MAX_DARWIN + 1) &&
          rb_os_row_for(RB_OS_MAX_DARWIN + 1) == 0 &&
          rb_os_derived_mask(RB_OS_MAX_DARWIN + 1) == 0u,
          "a Darwin newer than this tree loads, has no row, and claims nothing");
    CHECK(rb_os_derived_mask(RB_OS_VENTURA - 1) == 0u,
          "an out-of-range OS claims nothing either");

    /* --- the rows are well formed ------------------------------------- */

    for (i = 0; i < RB_OS_ROW_COUNT; i++) {
        CHECK(rb_os_rows[i].name != 0 && rb_os_rows[i].name[0] != '\0',
              "every row has a name to print (rule 14: silence is not a result)");
        CHECK(rb_os_rows[i].note != 0 && rb_os_rows[i].note[0] != '\0',
              "every row says what its claim rests on");
        CHECK((rb_os_rows[i].derived & ~(unsigned)RB_OSD_ALL) == 0u,
              "no row claims an artifact bit this tree does not define");
        CHECK(rb_os_in_range(rb_os_rows[i].darwin),
              "no row is below the declared floor");
        for (j = i + 1; j < RB_OS_ROW_COUNT; j++) {
            CHECK(rb_os_rows[i].darwin != rb_os_rows[j].darwin,
                  "no two rows share a Darwin major (the lookup takes the first)");
        }
        CHECK(rb_os_row_for(rb_os_rows[i].darwin) == &rb_os_rows[i],
              "the lookup finds each row at its own key");
    }
    CHECK(RB_OS_ROW_COUNT == 4,
          "four rows: Ventura, Sonoma, Sequoia, Tahoe");

    /* --- the claims themselves --------------------------------------- */

    /* The two that have never been booted claim nothing. This is the
     * assertion that keeps the honest answer honest: adding a bit here
     * without a boot behind it has to break a named test. */
    CHECK(rb_os_derived_mask(RB_OS_VENTURA) == 0u,
          "Ventura claims nothing - no boot of this project has happened there");
    CHECK(rb_os_derived_mask(RB_OS_SONOMA) == 0u,
          "Sonoma claims nothing - no boot of this project has happened there");
    CHECK(!rb_os_derived(RB_OS_VENTURA, RB_OSD_KEXTOFF) &&
          !rb_os_derived(RB_OS_VENTURA, RB_OSD_MTLCAVE) &&
          !rb_os_derived(RB_OS_SONOMA, RB_OSD_KEXTOFF) &&
          !rb_os_derived(RB_OS_SONOMA, RB_OSD_MTLCAVE),
          "so neither answers yes to any artifact");

    /* Sequoia: where everything was derived, and the cave row that boot 231
     * ran. No USB table - 2xv was derived on Tahoe. */
    CHECK(rb_os_derived(RB_OS_SEQUOIA, RB_OSD_KEXTOFF),
          "Sequoia has the object offsets - they were all derived there");
    CHECK(rb_os_derived(RB_OS_SEQUOIA, RB_OSD_MTLCAVE),
          "Sequoia has a cave row - the one boot 231 ran");
    CHECK(!rb_os_derived(RB_OS_SEQUOIA, RB_OSD_XHCI),
          "Sequoia has no USB interrupter table - 2xv was derived on Tahoe");

    /* Tahoe: the current machine. Everything. */
    CHECK(rb_os_derived(RB_OS_TAHOE, RB_OSD_ALL),
          "Tahoe has all three - it is the machine boot 247 ran on");

    /* --- the all-or-nothing rule ------------------------------------- */

    /* A caller that needs two measured things and gets one is in the same
     * position as one that got neither, so the answer is AND and not OR. */
    CHECK(!rb_os_derived(RB_OS_SEQUOIA, RB_OSD_KEXTOFF | RB_OSD_XHCI),
          "a mask naming one present and one absent artifact answers no");
    CHECK(rb_os_derived(RB_OS_SEQUOIA, RB_OSD_KEXTOFF | RB_OSD_MTLCAVE),
          "...and answers yes only when every named artifact is present");
    CHECK(rb_os_derived(RB_OS_VENTURA, 0u),
          "an empty mask is vacuously satisfied - a caller that needs nothing measured proceeds");

    /* --- names, which are the log line ------------------------------- */

    CHECK(strcmp(rb_os_name(RB_OS_TAHOE), "Tahoe") == 0 &&
          strcmp(rb_os_name(RB_OS_VENTURA), "Ventura") == 0,
          "a known OS prints its name");
    CHECK(rb_os_name(RB_OS_MAX_DARWIN + 1) != 0 &&
          strcmp(rb_os_name(RB_OS_MAX_DARWIN + 1), "newer-than-this-tree") == 0,
          "a newer OS prints that it is newer, not nothing");
    CHECK(strcmp(rb_os_name(RB_OS_VENTURA - 1), "out-of-range") == 0,
          "an older OS prints that it is out of range");

    /* --- the cave rows against the matrix ---------------------------- */

    /* The invariant: a row exists for a Darwin major IF AND ONLY IF that OS's
     * matrix entry claims RB_OSD_MTLCAVE. Either half alone is a lie - a row
     * without the bit is data nothing will reach, and a bit without a row
     * makes `rb_os_derived` promise a patch that cannot be placed. */
    for (i = 0; i < RB_OS_ROW_COUNT; i++) {
        const int d = rb_os_rows[i].darwin;
        const int claims = rb_os_derived(d, RB_OSD_MTLCAVE);
        const int has = rbgpu_mtlcave_row(d) != 0;

        CHECK(claims == has,
              "each OS claims RB_OSD_MTLCAVE exactly when a cave row exists for it");
    }
    CHECK(rbgpu_mtlcave_row(RB_OS_VENTURA) == 0 &&
          rbgpu_mtlcave_row(RB_OS_SONOMA) == 0,
          "Ventura and Sonoma have no cave row - stated twice, in the matrix and in the table");
    CHECK(rbgpu_mtlcave_row(RB_OS_SEQUOIA) != 0 &&
          rbgpu_mtlcave_row(RB_OS_TAHOE) != 0,
          "Sequoia's row survived 2xt instead of being replaced, and Tahoe has its own");
    CHECK(rbgpu_mtlcave_row(RB_OS_MAX_DARWIN + 1) == 0,
          "the lookup is exact, never nearest-neighbour - a wrong build's addresses are worth nothing");

    for (i = 0; i < RB_MTLCAVE_ROW_COUNT; i++) {
        const struct rb_mtlcave_row *r = &rb_mtlcave_rows[i];

        /* The read buffer in rbgpu_mtlk_place() is sized from MAXLEN, not from
         * a row's own array, so a longer pattern must fail HERE and not
         * silently truncate a compare there. */
        CHECK(r->cave_len <= RBGPU_MTL_MAXLEN && r->fn_len <= RBGPU_MTL_MAXLEN,
              "every pattern fits the fixed read buffer (RBGPU_MTL_MAXLEN)");
        CHECK(r->cave_len == r->cave_anchor + r->cave_reserved,
              "cave_len is exactly the anchor plus the reserved run it covers");
        CHECK(r->cave_used > 0 && r->cave_used <= r->cave_reserved,
              "the cave code fits the zero run it replaces");
        /* The find pattern's reserved tail is the all-zero run the cave goes
         * into. If it is not zero, the row was derived against something that
         * was not a zero run and the write would clobber real data. */
        {
            int nonzero = 0;
            for (j = (int)r->cave_anchor; j < (int)r->cave_len; j++) {
                nonzero += r->cave_find[j] != 0;
            }
            CHECK(nonzero == 0,
                  "the reserved area of every find pattern is all zero - it is a zero run, not live data");
        }
        /* The anchor is what makes an all-zero cave identifiable at all, so a
         * replacement that changed it would move the match. */
        CHECK(memcmp(r->cave_find, r->cave_repl, r->cave_anchor) == 0,
              "the replacement leaves the anchor bytes alone - it is context, not payload");
        CHECK(memcmp(r->cave_find, r->cave_repl, r->cave_len) != 0,
              "...and does change something past it");
        /* find and repl must differ, or a verify after the write cannot tell
         * a landed write from an untouched page (2lq in byte form). */
        CHECK(memcmp(r->fn_find, r->fn_repl, r->fn_len) != 0,
              "the detour's replacement differs from its find - otherwise the verify proves nothing");
        /* The detour is a jmp, so the first byte changes and the rest of the
         * prologue is left as context. */
        CHECK(r->fn_repl[0] == 0xE9,
              "the detour starts with a rel32 jmp");
        /* 312: a multisampled render target must keep Apple's own swizzle
         * mode. The guard is `cmp qword [rbx+0x30], 2 ; jae OUT`. Without it
         * the remap turns 64KB_R_X into 64KB_D, AddrLib answers
         * ADDR_INVALIDPARAMS and every MSAA colour texture is nil. Only the
         * Tahoe row was regenerated (Sequoia has no machine to derive it on),
         * so only that row is required to carry it. */
        if (r->darwin == 25) {
            static const unsigned char guard[] = { 0x48, 0x83, 0x7B, 0x30, 0x02, 0x73 };
            int found = 0;
            for (j = 0; j + (int)sizeof(guard) <= (int)r->cave_len; j++) {
                found += memcmp(r->cave_repl + j, guard, sizeof(guard)) == 0;
            }
            CHECK(found == 1,
                  "the Tahoe cave skips the remap for sampleCount >= 2 - MSAA colour keeps its _X mode (312)");
        }
        CHECK(r->cave_vm > r->text_vm && r->fn_vm > r->text_vm,
              "both addresses are inside the image, above its __TEXT base");
        CHECK(r->cave_vm >= r->cave_anchor,
              "the anchor does not underflow the cave address");
        CHECK(r->os != 0 && r->os[0] != '\0' && rb_os_row_for(r->darwin) != 0,
              "every cave row names an OS the matrix knows");
        for (j = i + 1; j < RB_MTLCAVE_ROW_COUNT; j++) {
            CHECK(rb_mtlcave_rows[i].darwin != rb_mtlcave_rows[j].darwin,
                  "no two cave rows share a Darwin major");
            /* 2xt's whole lesson: the rows must be DIFFERENT data. Two rows
             * with the same addresses would mean one was copied rather than
             * derived. */
            CHECK(rb_mtlcave_rows[i].fn_vm != rb_mtlcave_rows[j].fn_vm &&
                  rb_mtlcave_rows[i].cave_vm != rb_mtlcave_rows[j].cave_vm,
                  "no two cave rows share addresses - a copied row is not a derived one");
        }
    }

    return failures;
}
