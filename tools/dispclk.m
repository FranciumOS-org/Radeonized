/* SPDX-License-Identifier: MIT */
/*
 * dispclk (2ot): what frame clock does macOS actually have for this display?
 *
 * This is the ORACLE for the vblank step of the display plan. The panel scans
 * out at 143.833 Hz (2os, measured from OTG0's frame counter), but the boot
 * framebuffer publishes no refresh clock at all (IOFB0Hz = Yes), so what
 * paces every animation on the machine is whatever CoreVideo falls back to.
 *
 * Run it before and after publishing a framebuffer with a real vblank. On
 * boot 133 it reported:
 *
 *   CGDisplayModeGetRefreshRate = 0.000 Hz
 *   nominal refresh period: 33333333/1000000000 = 30.000 Hz
 *   display link fired 60 times, 1966.8 ms span = 30.00 Hz
 *
 * 30 Hz against a 144 Hz panel - roughly one frame in five. That number, not
 * the cosmetic "7 MB", is the choppiness.
 *
 * CVDisplayLink is deprecated and that is fine: it is the lowest-level
 * read-only way to ask what the system's frame clock is, and it needs no
 * window, no drawable and no GPU work.
 */
#import <Foundation/Foundation.h>
#import <CoreGraphics/CoreGraphics.h>
#import <CoreVideo/CoreVideo.h>
#include <mach/mach_time.h>

static volatile uint64_t g_n = 0, g_first = 0, g_last = 0;

static CVReturn cb(CVDisplayLinkRef l, const CVTimeStamp *now, const CVTimeStamp *out,
                   CVOptionFlags fi, CVOptionFlags *fo, void *ctx) {
    uint64_t t = mach_absolute_time();
    if (!g_first) g_first = t;
    g_last = t; g_n++;
    return kCVReturnSuccess;
}

int main(void) {
    CGDirectDisplayID d = CGMainDisplayID();
    CGDisplayModeRef m = CGDisplayCopyDisplayMode(d);
    printf("CGDisplay %u: %zux%zu, CGDisplayModeGetRefreshRate = %.3f Hz\n",
           d, CGDisplayModeGetWidth(m), CGDisplayModeGetHeight(m),
           CGDisplayModeGetRefreshRate(m));
    CFRelease(m);

    CVDisplayLinkRef link = NULL;
    if (CVDisplayLinkCreateWithCGDisplay(d, &link) != kCVReturnSuccess || !link) {
        printf("CVDisplayLinkCreateWithCGDisplay FAILED - no display link for this display\n");
        return 1;
    }
    CVTime nominal = CVDisplayLinkGetNominalOutputVideoRefreshPeriod(link);
    if (nominal.flags & kCVTimeIsIndefinite) {
        printf("nominal refresh period: INDEFINITE - macOS has no refresh clock here\n");
    } else {
        printf("nominal refresh period: %lld/%d = %.3f Hz\n",
               nominal.timeValue, nominal.timeScale,
               (double)nominal.timeScale / (double)nominal.timeValue);
    }
    printf("actual latency: %.3f ms\n", CVDisplayLinkGetActualOutputVideoRefreshPeriod(link) * 1000.0);

    CVDisplayLinkSetOutputCallback(link, cb, NULL);
    CVDisplayLinkStart(link);
    printf("\nmeasuring the callback rate for 2.0 s ...\n");
    [NSThread sleepForTimeInterval:2.0];
    CVDisplayLinkStop(link);

    mach_timebase_info_data_t tb; mach_timebase_info(&tb);
    double ns = (double)(g_last - g_first) * tb.numer / tb.denom;
    if (g_n > 1 && ns > 0) {
        printf("display link fired %llu times, %.1f ms span = %.2f Hz\n",
               g_n, ns / 1e6, (g_n - 1) * 1e9 / ns);
    } else {
        printf("display link fired %llu times - NO usable vsync callback\n", g_n);
    }
    CVDisplayLinkRelease(link);
    return 0;
}
