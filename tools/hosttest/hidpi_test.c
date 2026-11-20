/* #9: the HiRes mode flag.
 *
 * What this file can and cannot establish. It can pin the constant against the
 * SDK's own header, pin that a disarmed build emits byte-identical flags, and
 * pin the halving. It CANNOT establish that macOS then offers the derived mode
 * - that happened on the reporter's 2560x1440 panel, which this project does
 * not have, and the only evidence for it is their read-back of
 * flags = 0x00800003 out of IOFBModes[].DM plus a working Displays pane
 * (rule 45: the deliverable is something a human observes). So the assertions
 * below are about OUR half of the contract only, and the comment in
 * rb_hidpi.h says which half that is.
 */
#include <stdio.h>
#include <IOKit/graphics/IOGraphicsTypes.h>
#include "rb_hidpi.h"

static int fails;
#define ck(c, msg) do { if (!(c)) { printf("FAIL hidpi: %s\n", (msg)); fails++; } } while (0)

int hidpi_tests(void);
int hidpi_tests(void)
{
    fails = 0;

    /* THE CONSTANT IS THE SDK'S. If a future SDK moves the bit, this is a
     * failing test rather than a framebuffer quietly setting the wrong flag.
     * Both halves matter: the name must exist and the value must be the one
     * the reporter read back out of the kernel's table. */
    ck(kDisplayModeValidForHiResFlag == RB_HIDPI_EXPECTED_FLAG,
       "kDisplayModeValidForHiResFlag is 0x00800000 as reported in #9");
    ck(RB_HIDPI_EXPECTED_FLAG == 0x00800000u,
       "the expected value is stated as a literal, not derived from the SDK "
       "- otherwise the check above compares the SDK with itself");

    /* DISARMED IS BYTE-IDENTICAL. This is the load-bearing one: the token is
     * opt-in precisely so that every existing user's offer is unchanged, and
     * that claim is only as good as this assertion. Tested over the whole
     * input space of interest rather than one value. */
    {
        unsigned ins[] = { 0u, 1u, 3u,
                           (unsigned)(kDisplayModeValidFlag |
                                      kDisplayModeSafeFlag),
                           (unsigned)(kDisplayModeValidFlag |
                                      kDisplayModeSafeFlag |
                                      kDisplayModeDefaultFlag),
                           0x00800003u,            /* already set            */
                           0xffffffffu };
        for (unsigned i = 0; i < sizeof ins / sizeof ins[0]; i++) {
            ck(rb_hidpi_mode_flags(ins[i], 0, RB_HIDPI_EXPECTED_FLAG)
                   == ins[i],
               "disarmed changes NOTHING, for any input flags");
            ck(rb_hidpi_mode_flags(ins[i], 1, RB_HIDPI_EXPECTED_FLAG)
                   == (ins[i] | RB_HIDPI_EXPECTED_FLAG),
               "armed adds the bit and touches no other bit");
        }
    }

    /* The ONE bit. A mask that happened to be wider would still pass an
     * "is the bit set" check, so the statement is equality with the OR, and
     * separately that exactly one bit differs. */
    {
        unsigned base = (unsigned)(kDisplayModeValidFlag |
                                   kDisplayModeSafeFlag);
        unsigned got  = rb_hidpi_mode_flags(base, 1, RB_HIDPI_EXPECTED_FLAG);
        unsigned diff = got ^ base;
        ck(diff == RB_HIDPI_EXPECTED_FLAG,
           "exactly the HiRes bit differs");
        ck(got == 0x00800003u,
           "the word is 0x00800003, which is what #9 read back from "
           "IOFBConfig -> IOFBModes[].DM on the working machine");
    }

    /* ALREADY-SET IS IDEMPOTENT, so a second caller cannot corrupt the word.
     * Not hypothetical: five query methods share modeKnown() and more than
     * one path can fill an info struct. */
    ck(rb_hidpi_mode_flags(0x00800003u, 1, RB_HIDPI_EXPECTED_FLAG)
           == 0x00800003u,
       "arming twice is the same word");

    /* THE DERIVED SIZE, including why the reference board does not want it.
     * These are the three panels named in the issues; the 1080p row is the
     * reason the token exists at all. */
    ck(rb_hidpi_logical(2560u) == 1280u && rb_hidpi_logical(1440u) == 720u,
       "#9's panel derives 1280x720 logical");
    ck(rb_hidpi_logical(2880u) == 1440u && rb_hidpi_logical(1800u) == 900u,
       "#8's panel derives 1440x900 logical");
    ck(rb_hidpi_logical(1920u) == 960u && rb_hidpi_logical(1080u) == 540u,
       "THE REFERENCE BOARD derives 960x540 - the measured reason this is "
       "opt-in rather than a default");
    /* Odd and degenerate inputs: floor, and no division by anything. */
    ck(rb_hidpi_logical(1u) == 0u, "1 floors to 0");
    ck(rb_hidpi_logical(1081u) == 540u, "an odd raster floors");
    ck(rb_hidpi_logical(0u) == 0u, "0 is 0, not a trap");

    if (fails == 0) {
        printf("hidpi: ok\n");
    }
    return fails;
}
