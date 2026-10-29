/* 2x2: does the compositor have a FRAME CLOCK?
 *
 * Boot 204 settled that the display-pipe retirement path works and the supply
 * is still keystroke-gated (837x between idle and a typing burst). The one
 * structural difference left between the configuration that composites
 * continuously and the one that does not is WHICH IOFramebuffer serves the
 * panel - same WindowServer, same CoreDisplay.
 *
 * A compositor paces itself on the display's vblank, which userspace gets
 * through CVDisplayLink -> CoreDisplay -> IOFramebuffer's vblank timestamps
 * (getTimeOfVBL / getVBLTime, throttled by updateVBL / deferredVBLDisable).
 * If that clock does not tick on our framebuffer, WindowServer has no cadence
 * and can only draw when an event forces it - which is exactly "frames while
 * typing, nothing when idle, and a ~10 s floor".
 *
 * This measures the clock directly and takes no position on the cause. Run it
 * in BOTH configurations; the CPU-framebuffer boot is the control, and it must
 * read ~143.8 Hz there or the instrument is wrong (rule 16) and nothing it
 * says about the armed boot means anything.
 *
 * Reads only. No kext, no arming, no reboot.
 */
#import <Foundation/Foundation.h>
#import <CoreVideo/CoreVideo.h>
#import <CoreGraphics/CoreGraphics.h>
#include <mach/mach_time.h>
#include <stdatomic.h>

static atomic_ullong g_ticks = 0;
static atomic_ullong g_firstNs = 0, g_lastNs = 0;

static double now_s(void) {
    static mach_timebase_info_data_t tb;
    if (!tb.denom) mach_timebase_info(&tb);
    return (double)mach_absolute_time() * tb.numer / tb.denom / 1e9;
}

static CVReturn cb(CVDisplayLinkRef dl, const CVTimeStamp *inNow,
                   const CVTimeStamp *inOut, CVOptionFlags f, CVOptionFlags *fo,
                   void *ctx) {
    (void)dl; (void)inNow; (void)inOut; (void)f; (void)fo; (void)ctx;
    unsigned long long n = (unsigned long long)(now_s() * 1e9);
    unsigned long long expected = 0;
    atomic_compare_exchange_strong(&g_firstNs, &expected, n);
    atomic_store(&g_lastNs, n);
    atomic_fetch_add(&g_ticks, 1);
    return kCVReturnSuccess;
}

int main(int argc, char **argv) {
    @autoreleasepool {
        double secs = (argc > 1) ? atof(argv[1]) : 5.0;
        if (secs < 1 || secs > 120) secs = 5.0;

        CGDirectDisplayID d = CGMainDisplayID();
        printf("main display id      : 0x%x\n", d);
        CGDisplayModeRef m = CGDisplayCopyDisplayMode(d);
        if (m) {
            printf("mode                 : %zux%zu @ %.2f Hz\n",
                   CGDisplayModeGetPixelWidth(m), CGDisplayModeGetPixelHeight(m),
                   CGDisplayModeGetRefreshRate(m));
            CGDisplayModeRelease(m);
        } else {
            printf("mode                 : (CGDisplayCopyDisplayMode returned NULL)\n");
        }

        CVDisplayLinkRef dl = NULL;
        CVReturn r = CVDisplayLinkCreateWithCGDisplay(d, &dl);
        if (r != kCVReturnSuccess || !dl) {
            printf("RESULT: CVDisplayLinkCreateWithCGDisplay FAILED (%d)\n", r);
            printf("        no display link exists -> the compositor has no clock source here\n");
            return 2;
        }
        /* The nominal period is what the framebuffer ADVERTISES; the measured
         * callback rate is what it DELIVERS. They differ exactly when the
         * timestamps are published but the interrupt never arrives, which is
         * the case this tool exists to distinguish. */
        CVTime nom = CVDisplayLinkGetNominalOutputVideoRefreshPeriod(dl);
        if (nom.flags & kCVTimeIsIndefinite) {
            printf("nominal period       : INDEFINITE\n");
        } else {
            printf("nominal period       : %lld/%d = %.3f ms -> %.2f Hz\n",
                   nom.timeValue, nom.timeScale,
                   1000.0 * nom.timeValue / nom.timeScale,
                   (double)nom.timeScale / nom.timeValue);
        }

        CVDisplayLinkSetOutputCallback(dl, cb, NULL);
        double t0 = now_s();
        r = CVDisplayLinkStart(dl);
        if (r != kCVReturnSuccess) {
            printf("RESULT: CVDisplayLinkStart FAILED (%d)\n", r);
            return 3;
        }
        printf("counting for %.1f s ...\n", secs);
        [[NSRunLoop currentRunLoop] runUntilDate:
            [NSDate dateWithTimeIntervalSinceNow:secs]];
        CVDisplayLinkStop(dl);
        double el = now_s() - t0;

        unsigned long long n = atomic_load(&g_ticks);
        printf("\ncallbacks            : %llu in %.3f s\n", n, el);
        printf("MEASURED RATE        : %.2f Hz\n", n / el);
        if (n >= 2) {
            double span = (atomic_load(&g_lastNs) - atomic_load(&g_firstNs)) / 1e9;
            printf("first->last span     : %.3f s over %llu intervals -> %.3f ms each\n",
                   span, n - 1, 1000.0 * span / (n - 1));
        }
        /* Rule 14: say which outcome this is, so a zero is a reading. */
        if (n == 0) {
            printf("\nVERDICT: the display link produced NO callbacks.\n"
                   "         WindowServer has no frame cadence from this framebuffer.\n");
        } else if (n / el < 20.0) {
            printf("\nVERDICT: the clock ticks but FAR below the panel rate.\n");
        } else {
            printf("\nVERDICT: the clock is running at the panel rate.\n");
        }
        return 0;
    }
}
