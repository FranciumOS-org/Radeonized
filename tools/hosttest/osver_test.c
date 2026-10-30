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

    return failures;
}
