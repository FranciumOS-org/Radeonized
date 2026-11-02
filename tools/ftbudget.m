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
#include "rb_frametime.h"
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
            printf("  the hold owns        %8.1f %% of the interval\n",
                   shr / 10.0);
            {
                const long vblp = rd(fb, "RBFBVBLPeriodNs");
                const long iv = rd(fb, "RBFBFtIntervalNs");
                const uint64_t freei = rb_ft_hold_free_interval_ns(
                    (uint64_t)(iv > 0 ? iv : 0), (uint64_t)(pen > 0 ? pen : 0),
                    (uint64_t)(hpt > 0 ? hpt : 0),
                    (uint64_t)(vblp > 0 ? vblp : 0));
                if (freei == 0) {
                    printf("  NO RATE IS PREDICTED FROM THIS. Subtracting the\n"
                           "  share implies a rate at or above the panel's own\n"
                           "  vblank rate, which is impossible - the compositor\n"
                           "  cannot be handed more frames than there are\n"
                           "  vblanks. The share is a true share of THIS\n"
                           "  interval; it is not the effect of removing the\n"
                           "  hold, because removing it changes the GET count\n"
                           "  and the feed rate too (2A4/2A8). Measure that\n"
                           "  with tools/holdaba.sh, never by subtraction.\n");
                } else {
                    printf("  implied without it   %8.1f /s   (and %.2f /s is\n"
                           "  the panel's own ceiling, so this is at least\n"
                           "  possible - it is still not a measurement)\n",
                           1000000000.0 / (double)freei,
                           1000000000.0 / (double)vblp);
                }
            }
        }
    }

    /* 2A7: split a hold into the hardware wait and Apple's phase. */
    {
        const long lw = rd(fb, "RBFBFtLatchWaitNs");
        const long lwN = rd(fb, "RBFBFtLatchWaitN");
        const long asks = rd(fb, "RBFBFtHoldAsksPm");
        const long vbl = rd(fb, "RBFBVBLPeriodNs");
        if (lwN >= 30 && vbl > 0) {
            const double vbls = (double)lw / (double)vbl;
            printf("\nwhat one hold actually costs:\n");
            printf("  latch wait           %8.3f ms  = %.2f vblanks   n %ld\n",
                   lw / 1000000.0, vbls, lwN);
            printf("  asks to get through  %8.2f\n", asks / 1000.0);
            /* The flip is vsync-latched (RB_HUBP_FLIP_TYPE 0), so one vblank
             * of any hold is hardware time no answer of ours can skip.
             * Anything beyond one vblank is a round trip that arrived after
             * the latch had already happened. */
            if (vbls > 1.35) {
                printf("  Beyond one vblank: %.2f vblanks of this is Apple\n"
                       "  asking again AFTER the flip had already latched.\n"
                       "  One vblank is hardware we cannot skip; the rest is\n"
                       "  phase, and phase is the only part worth attacking.\n",
                       vbls - 1.0);
            } else {
                printf("  At or near one vblank: the hold pays the HARDWARE,\n"
                       "  not Apple's polling phase. The flip cannot latch\n"
                       "  sooner, so there is nothing to recover and the\n"
                       "  ~36 percent is the real price of the 2xo fix.\n");
            }
        } else if (lwN > 0) {
            printf("\nwhat one hold actually costs: only %ld closed runs, "
                   "under 30 - not resolvable (rule 18).\n", lwN);
        }
    }

    /* 2A9: is there a middle setting between 91 and 141 sets/s?
     *
     * 2A8 read the latch wait as a MEAN and closed the frame-rate line on it.
     * A mean of one vblank cannot distinguish "every flip needs the whole
     * period" from "most resolve at once and a tail is exposed", and only the
     * second has a partial hold in it - so this prints the DISTRIBUTION.
     *
     * The run is keyed on SURFACE_FLIP_PENDING rather than on our own answer,
     * so it is filed with the hold on AND with it off. Which matters: the
     * flicker only happens with the hold off, and a phase distribution
     * measured in the other regime is not evidence about it (rule 39). The
     * tool therefore says which regime the reading came from, every time.
     */
    {
        const long pw   = rd(fb, "RBFBFtPendWaitNs");
        const long pn   = rd(fb, "RBFBFtPendHistN");
        const long earl = rd(fb, "RBFBFtPendEarlyPm");
        const long asks = rd(fb, "RBFBFtPendAsksPm");
        const long vbl  = rd(fb, "RBFBVBLPeriodNs");
        const long lw   = rd(fb, "RBFBFtLatchWaitNs");
        const long lwN  = rd(fb, "RBFBFtLatchWaitN");
        /* rd() returns 1/0 for a bool and -1 when the key is absent, and the
         * three must stay distinct: reading "absent" as OFF would label a
         * reading with the wrong regime, which is the one thing this block
         * must not do. */
        const long heldv = rd(fb, "RBFBAmtrNotReady");
        const bool holdOn = (heldv == 1);
        const bool holdKnown = (heldv == 0 || heldv == 1);

        if (pn >= 30 && vbl > 0) {
            printf("\nthe exposure window, as a DISTRIBUTION (2A9):\n");
            if (!holdKnown) {
                printf("  hold is UNKNOWN - RBFBAmtrNotReady is not published,\n"
                       "  so this histogram cannot be attributed to a regime\n"
                       "  and must not be read as evidence either way.\n");
            } else {
                printf("  hold is %s  -  %s\n",
                       holdOn ? "ON " : "OFF",
                       holdOn ? "this is the CONTROL regime; the flicker does"
                                " not happen here"
                              : "this is the regime the flicker happens in");
            }
            printf("  pending wait         %8.3f ms  = %.2f vblanks   n %ld\n",
                   pw / 1000000.0, (double)pw / (double)vbl, pn);
            printf("  asks per run         %8.2f\n", asks / 1000.0);
            for (unsigned i = 0; i < 4; i++) {
                char key[24];
                snprintf(key, sizeof(key), "RBFBFtPendQ%u", i);
                const long c = rd(fb, key);
                printf("  [%.2f-%.2f vblanks)  %8ld  %5.1f %%\n",
                       i * 0.25, (i + 1) * 0.25, c, 100.0 * c / pn);
            }
            const long over = rd(fb, "RBFBFtPendQ4");
            printf("  [>= 1.00 vblanks  )  %8ld  %5.1f %%\n",
                   over, 100.0 * over / pn);

            /* The cross-check, free: with the hold on this run is the same
             * physical interval ftLatchWait measures, so the two must agree.
             * A disagreement means one of them is bracketing the wrong thing
             * and neither number means anything (rule 26). */
            if (holdOn && lwN >= 30 && lw > 0) {
                const double disagree = 100.0 * (double)(pw - lw) / (double)lw;
                printf("  cross-check vs 2A7 latch wait: %+.1f %%", disagree);
                if (disagree > 5.0 || disagree < -5.0) {
                    printf("   REFUSED - over 5 %%,\n"
                           "  so one of the two brackets the wrong interval "
                           "and neither is usable.\n");
                } else {
                    printf("   (agrees)\n");
                }
            }

            printf("\n");
            if (earl < 50) {
                printf("  NO MIDDLE SETTING. %.1f %% of flips resolve inside\n"
                       "  the vblank they were submitted in, so essentially\n"
                       "  every flip needs the whole period. There is no\n"
                       "  subset that is safer than the rest to let through,\n"
                       "  and no rate throttle helps: the hazard is PER FLIP,\n"
                       "  so feeding 91 %% instead of 98 %% buys 7 %% less\n"
                       "  flicker for 7 %% less frame rate. 2A8 stands and\n"
                       "  the choice really is 91-or-141.\n", earl / 10.0);
            } else {
                printf("  A MIDDLE SETTING MAY EXIST: %.1f %% of flips\n"
                       "  resolve inside their own vblank. A hold that\n"
                       "  engaged only on the rest would let that fraction\n"
                       "  run unheld. This is a measured SHARE, not a\n"
                       "  predicted rate - 2A8 is what extrapolating a rate\n"
                       "  from a share costs. What a partial hold delivers\n"
                       "  is an A/B and nothing else.\n", earl / 10.0);
            }
            if (holdOn) {
                printf("  Read this again with the hold OFF before acting on\n"
                       "  it: `holdset 0` changes the feed rate from ~63 %% to\n"
                       "  ~98 %% of vblanks, which is a regime change, and\n"
                       "  this histogram describes the regime it was taken in.\n");
            }
        } else if (pn > 0) {
            printf("\nthe exposure window: only %ld closed pending runs, "
                   "under 30 - not resolvable (rule 18).\n", pn);
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
