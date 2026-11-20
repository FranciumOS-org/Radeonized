/* SPDX-License-Identifier: MIT */
/* rb_hidpi.h - the one flag that makes macOS offer a HiDPI mode (issue #9).
 *
 * WHAT WAS WRONG. On a native 2560x1440 panel this framebuffer offered only
 * its 1x modes, so `CGDisplayCopyAllDisplayModes()` returned the eight native
 * rates and nothing else, Displays settings showed no "looks like" option, and
 * all UI rendered at 1x. The reporter of #9 (NE156QHM-NY4, 2560x1440 165 Hz
 * eDP, Ryzen 7735H / 680M) traced it to a single missing bit in
 * `getInformationForDisplayMode`: `kDisplayModeValidForHiResFlag`.
 *
 * WHY THE BIT IS ENOUGH, AND WHY THAT IS NOT A SCALER. With the flag set,
 * CoreDisplay derives a HiDPI mode that REUSES the existing `ioDisplayModeID`:
 * width/height become the nominal size halved (1280x720 on that panel) while
 * pixelWidth/pixelHeight stay the real raster (2560x1440). No new mode entry,
 * no new id, and - the part that matters - no scaling stage, because the
 * backing store CoreGraphics asks for is exactly the raster the display engine
 * is already scanning. That is why it renders crisp rather than corrupted.
 *
 * The reporter verified the bit survives into the kernel's own table:
 * `IOFBConfig -> IOFBModes[].DM` read back `flags = 0x00800003`, i.e.
 * VALID | SAFE | VALID_FOR_HI_RES, which is what CoreDisplay consumes.
 *
 * THE CONSTANT IS THE SDK'S, NOT A NUMBER WE CHOSE. `IOGraphicsTypes.h:178`
 * defines `kDisplayModeValidForHiResFlag = 0x00800000`, matching the reported
 * read-back exactly. This header does not redefine it; it uses it, and the
 * hosttest pins the value so a future SDK moving it is a failing test rather
 * than a silently wrong flag.
 *
 * WHY THIS IS OPT-IN AND NOT A DEFAULT. The derived tier is always the nominal
 * size halved, so what it is WORTH depends entirely on the panel:
 *
 *   2560x1440 -> 1280x720 logical   the reporter's case, useful
 *   2880x1800 -> 1440x900  logical  (#8's panel, untested here)
 *   1920x1080 ->  960x540  logical  the reference board: nobody wants this
 *
 * The reference machine - the only one this project can measure - is 1080p,
 * where the one derived tier is 960x540. So turning this on by default would
 * change what every existing user is offered, on the strength of a measurement
 * from a panel this project does not have, to add a mode that is useless on the
 * panel it does have. It is therefore a token, and the token is the user's
 * statement that their panel is big enough for it. No size threshold is
 * enforced, because any number here would be invented rather than measured.
 *
 * The reporter also observed that 2x is "noticeably less smooth than 1x" on
 * their machine (window drags, Mission Control), unprofiled. That is a second
 * reason not to impose it, and it is recorded rather than explained: a 2x
 * backing is four times the pixels per composite, so some cost is expected,
 * but nothing here measures how much of the jank that accounts for.
 *
 * WHAT THIS DOES NOT DO. It does not add intermediate "looks like" tiers.
 * #9 established why, by reading CoreDisplay: `IOFBScalerInfo` is an
 * `IODisplayScalerInformation`, exactly 0x30 bytes, and
 * `_IOFBInstallScaledResolution` reads exactly three dwords out of it
 * (+0x10 features, +0x14 maxHorizontalPixels, +0x18 maxVerticalPixels) with no
 * count and no loop - so the blob gates WHETHER scaled resolutions are offered
 * and cannot express a list of them. A tier between halved and native would
 * need a V2 timing whose scaled width/height exceed the native raster plus
 * `kIOScaleCanDownSamplePixels`, i.e. a real downsample stage in the display
 * pipe. Without one, that combination is the corrupted-scanout case, and this
 * header deliberately stops short of it.
 */
#ifndef RB_HIDPI_H
#define RB_HIDPI_H

/* The SDK's value, repeated here ONLY so the hosttest can compare the two and
 * fail if they ever diverge. Nothing uses this name to build a flag word. */
#define RB_HIDPI_EXPECTED_FLAG 0x00800000u

/* Add the HiRes bit to a mode's flags when the token is armed, and change
 * nothing at all when it is not. Taking the existing flags as input rather
 * than returning a flag word keeps VALID/SAFE/DEFAULT where they are decided
 * and makes "disarmed is byte-identical to before" a testable statement. */
static inline unsigned rb_hidpi_mode_flags(unsigned flags, int armed,
                                           unsigned hiResFlag) {
    return armed ? (flags | hiResFlag) : flags;
}

/* The logical size CoreDisplay will derive, for the diag keys only. This is
 * not a mode we publish and not a size we ask for - it is what the halving
 * above produces, published so a boot records what the user was offered
 * (rule 14). Odd rasters floor, which is what a shift does. */
static inline unsigned rb_hidpi_logical(unsigned nominal) {
    return nominal / 2u;
}

#endif /* RB_HIDPI_H */
