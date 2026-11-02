/* SPDX-License-Identifier: MIT */
/* 2A0: read the frame-time budget, and bracket a measurement window.
 *
 * WHY THIS IS NOT AN EIGHTH DIAL. Seven experiments on the ~73 sets/s ceiling
 * had one shape - move something, read `sets/s`, compare - and all seven
 * returned 70-75 (2xn, 2zb, 2zc, 2zd, 2zg, 2zz). That shape can only say
 * whether a guess moved the number. This prints a DECOMPOSITION of the
 * interval between two transactions into named pieces that sum back to it:
 * our work inside setAttribute('amtr'), our 'amtr' GETs, our vblank handler,
 * and the remainder in which Apple's accelerator had the frame and did not
 * call us.
 *
 * THE SELF-CHECK IS THE POINT. `rate from budget` below is sets/s computed
 * from timings; tools/paired.sh computes sets/s from a counter delta over a
 * measured window. Two independent routes to one number, so a budget that is
 * wrong says so instead of being read as a finding - which is the guard
 * 2zb's "194 Hz panel" did not have (rule 26).
 *
 * `reset` needs root, because IORegistryEntrySetCFProperties on a driver
 * does; reading does not.
 */
#import <Foundation/Foundation.h>
#import <IOKit/IOKitLib.h>

static io_service_t findFB(void)
{
    return IOServiceGetMatchingService(kIOMainPortDefault,
                                       IOServiceMatching("RBAMDFramebuffer"));
}

static long rd(io_service_t fb, const char *key)
{
    CFTypeRef v = IORegistryEntryCreateCFProperty(
        fb, (__bridge CFStringRef)[NSString stringWithUTF8String:key],
        kCFAllocatorDefault, 0);
    long out = -1;
    if (v) {
        if (CFGetTypeID(v) == CFNumberGetTypeID()) {
            long long t = 0;
            CFNumberGetValue((CFNumberRef)v, kCFNumberLongLongType, &t);
            out = (long)t;
        } else if (CFGetTypeID(v) == CFBooleanGetTypeID()) {
            out = CFBooleanGetValue((CFBooleanRef)v) ? 1 : 0;
        }
        CFRelease(v);
    }
    return out;
}

static void us(const char *label, long ns, long n, long maxns, long drop)
{
    printf("  %-22s %8.3f ms   n %-7ld max %8.3f ms   dropped %ld\n",
           label, ns / 1000000.0, n, maxns / 1000000.0, drop);
}

int main(int argc, char **argv)
{
    io_service_t fb = findFB();
    if (!fb) {
        fprintf(stderr, "no RBAMDFramebuffer - is the framebuffer armed?\n");
        return 1;
    }

    if (argc >= 2) {
        const char *cmd = argv[1];
        long want = -1;
        if (!strcmp(cmd, "on") || !strcmp(cmd, "reset")) { want = 1; }
        else if (!strcmp(cmd, "off")) { want = 0; }
        else {
            fprintf(stderr, "usage: ftbudget [reset|on|off]\n"
                            "  reset/on  arm the budget and ZERO the window\n"
                            "  off       the control for rule 16 - the two\n"
                            "            mach_absolute_time() reads per\n"
                            "            transaction stop happening\n");
            IOObjectRelease(fb);
            return 2;
        }
        NSDictionary *d = @{ @"RBFBFtOn": @(want) };
        kern_return_t kr = IORegistryEntrySetCFProperties(
            fb, (__bridge CFDictionaryRef)d);
        printf("RBFBFtOn(%ld) -> 0x%x%s\n", want, kr,
               kr == KERN_SUCCESS ? " (OK, window zeroed)" : "");
        IOObjectRelease(fb);
        return kr == KERN_SUCCESS ? 0 : 1;
    }

    /* Rule 15/39: say which configuration this reading comes from before any
     * number of it is interpreted. A budget taken with the instrument off, or
     * at 60 Hz, or with no animation running, is a different fact. */
    const long on = rd(fb, "RBFBFtOn");
    printf("RBFBFtOn %ld   resets %ld   mode %ld mHz / %ld lines   "
           "vbl period %.3f ms\n",
           on, rd(fb, "RBFBFtResets"), rd(fb, "RBFBModeCurMhz"),
           rd(fb, "RBFBModeCurLines"),
           rd(fb, "RBFBVBLPeriodNs") / 1000000.0);
    if (on != 1) {
        printf("\nthe instrument is OFF, so every number below is from "
               "before it was turned off.\nrun `sudo ftbudget reset` and a "
               "window, then read again.\n");
    }

    const long work = rd(fb, "RBFBFtWorkNs");
    const long gap  = rd(fb, "RBFBFtGapNs");
    const long get  = rd(fb, "RBFBFtGetNs");
    const long vbl  = rd(fb, "RBFBFtVblNs");
    const long iv   = rd(fb, "RBFBFtIntervalNs");

    printf("\nper transaction:\n");
    us("work (setAttribute)", work, rd(fb, "RBFBFtWorkN"),
       rd(fb, "RBFBFtWorkMax"), rd(fb, "RBFBFtWorkDrop"));
    us("gap (between)", gap, rd(fb, "RBFBFtGapN"),
       rd(fb, "RBFBFtGapMax"), rd(fb, "RBFBFtGapDrop"));
    us("  of which: amtr GET", get, rd(fb, "RBFBFtGetN"),
       rd(fb, "RBFBFtGetMax"), 0);
    us("  of which: vblank", vbl, rd(fb, "RBFBFtVblN"),
       rd(fb, "RBFBFtVblMax"), 0);

    printf("\n  interval             %8.3f ms\n", iv / 1000000.0);
    printf("  ours                 %8.3f ms  (%.1f %%)\n",
           rd(fb, "RBFBFtOursNs") / 1000000.0,
           rd(fb, "RBFBFtOursPm") / 10.0);
    printf("  upstream             %8.3f ms  (%.1f %%)   <- Apple had the "
           "frame and did not call us\n",
           rd(fb, "RBFBFtUpstreamNs") / 1000000.0,
           rd(fb, "RBFBFtUpstreamPm") / 10.0);

    /* The kIOReturnNotReady hold is OURS but its cost is spent inside Apple's
     * retry loop, so it lands in `upstream`. Saying so here is the difference
     * between a budget and a confidently mislabelled number (rule 17). */
    const long held = rd(fb, "RBFBAmtrHeld"), gets = rd(fb, "RBFBAmtrGets");
    if (gets > 0) {
        printf("  amtr held/gets       %ld / %ld  (%.1f %%) - the 2xo hold is "
               "ours,\n                       but Apple spends the WAIT, so "
               "it is inside `upstream`\n", held, gets,
               100.0 * (double)held / (double)gets);
    }

    /* 2A1/2A2: the one comparison that can attribute the hold, and the
     * premise that makes the attribution valid.
     *
     * 2A1 printed "the hold does NOT delay Apple measurably" whenever the
     * retry/ready RATIO was under 2.0x. That threshold was invented, and boot
     * 284's 1.58x in fact owned ~21 % of the interval - because the ratio is
     * the wrong statistic. What costs frame rate is the per-hold PENALTY times
     * the number of holds per transaction. The ratio is printed for context
     * and no longer decides anything. */
    const long rtry = rd(fb, "RBFBFtRetryNs"), rtryN = rd(fb, "RBFBFtRetryN");
    const long poll = rd(fb, "RBFBFtPollNs"), pollN = rd(fb, "RBFBFtPollN");
    printf("\nhow fast Apple asks again (gap between 'amtr' GETs):\n");
    printf("  after we HELD        %8.3f ms   n %ld\n",
           rtry / 1000000.0, rtryN);
    printf("  after we said ready  %8.3f ms   n %ld   <- the control\n",
           poll / 1000000.0, pollN);

    if (rtryN < 30 || pollN < 30) {
        printf("  NOT A COMPARISON: one bucket has under 30 samples, so a\n"
               "  difference between the means is not resolvable (rule 18).\n");
    } else {
        const long tile = rd(fb, "RBFBFtGetsTilePm");
        printf("  GET gaps account for %.1f %% of the interval", tile / 10.0);
        if (tile < 900 || tile > 1100) {
            printf("  <- PREMISE FAILED\n");
            printf("  The GET gaps do NOT account for the interval, so the\n"
                   "  hold's share CANNOT be attributed from them. Something\n"
                   "  else happens between transactions, or the counts come\n"
                   "  from different windows. No share is printed.\n");
        } else {
            printf("  (so the attribution below is valid)\n");
            const long pen = rd(fb, "RBFBFtHoldPenaltyNs");
            const long hpt = rd(fb, "RBFBFtHoldsPerTxnPm");
            const long shr = rd(fb, "RBFBFtHoldSharePm");
            printf("  penalty per hold     %8.3f ms   x %.2f holds/txn\n",
                   pen / 1000000.0, hpt / 1000.0);
            printf("  SO THE 2xo HOLD OWNS %.1f %% of the interval",
                   shr / 10.0);
            if (shr >= 50) {
                const double iv = rd(fb, "RBFBFtIntervalNs") / 1000000.0;
                const double without = iv - (pen / 1000000.0) * (hpt / 1000.0);
                printf("  -> ~%.1f /s without it\n",
                       without > 0.0 ? 1000.0 / without : 0.0);
                printf("  2xo recorded this hold costing 27 %% of the frame\n"
                       "  rate when it was introduced, so that is an\n"
                       "  independent corroboration, not a new finding.\n"
                       "  IT IS A TRADE: the hold is what stopped the\n"
                       "  compositor drawing into the live scanout. Turning it\n"
                       "  off brings 2xo's flicker back, which is the thing to\n"
                       "  look at while measuring - run tools/holdaba.sh.\n");
            } else {
                printf("\n  Under 5 %%: the hold is not where the frame rate\n"
                       "  goes, and `upstream` is Apple's own pacing.\n");
            }
        }
    }

    printf("\n  rate from budget     %8.3f /s\n",
           rd(fb, "RBFBFtRateMhz") / 1000.0);
    printf("  CROSS-CHECK: tools/paired.sh computes this from a counter delta\n"
           "  over a measured window. If the two disagree by more than a few\n"
           "  per cent the budget is wrong and nothing above is a finding.\n");

    IOObjectRelease(fb);
    return 0;
}
