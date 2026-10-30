/* animtest - does a CONTINUOUSLY ANIMATING on-screen window produce display
 * transactions, and is CoreVideo's display clock alive?
 *
 * Why this tool exists (2xe). Every lever found so far - typing (2wl),
 * AnyDesk (2x5), screencapture (2xa/2xd) - is an EXTERNAL agent poking
 * WindowServer. None of them is what a user means by "a smooth desktop".
 * The question nobody has asked is the plain one: when something on screen
 * changes by itself, continuously, does the chain run at the panel's rate?
 *
 * An idle desktop composites nothing on EVERY Mac - that is correct
 * behaviour, not a defect. So 0.09 sets/s at idle may be no defect at all,
 * and the whole frontier would then be about interaction, not about idle.
 * This separates the two for the first time.
 *
 * TWO measurements, deliberately in one process so they share a window:
 *
 *   1. CVDisplayLink callbacks/s - CoreVideo's own view of the panel clock.
 *      If this is not ~143.8 then userspace has no display clock, and
 *      nothing above it can schedule a frame. It is also the control for
 *      arm 2: a dead link makes a dead animation unreadable (rule 39's
 *      corollary - an instrument that needs a load must publish the load).
 *
 *   2. RBFBAmtrSets/s while a CoreAnimation animation runs forever in a
 *      visible window. The animation is driven by WindowServer itself from
 *      a CABasicAnimation, NOT by this process per frame, so it is exactly
 *      the shape of real UI motion and costs no IPC per frame.
 *
 * --link-only runs arm 1 with no window at all, which is the control for
 * arm 2 in the same boot: same process, same clock, no on-screen change.
 */
#import <AppKit/AppKit.h>
#import <QuartzCore/QuartzCore.h>
#import <CoreVideo/CoreVideo.h>
#include <stdatomic.h>
#include <mach/mach_time.h>
#include <math.h>

static atomic_ullong gLinkFires = 0;

/* 2xe: the vblank's REGULARITY, not just its rate. CVDisplayLink's callback
 * carries the hostTime CoreVideo believes the vblank happened at, and that
 * number comes from the framebuffer's vblank, so the spread of consecutive
 * deltas is a direct read on whether our thread_call is a CLOCK or merely a
 * timer that fires about that often. A real vblank interrupt is rock steady;
 * thread_call_enter_delayed is coalesced by the kernel timer subsystem and
 * is re-armed AFTER the handler runs, so it drifts by the handler's own
 * duration every single tick. The two are indistinguishable by rate alone,
 * which is why every earlier reading of "144 Hz" could not see this. */
#define kJitMax 40000
static double  gJit[kJitMax];
static unsigned gJitN = 0;
static uint64_t gLastHost = 0;
static double  gTbNs = 1.0;

static CVReturn linkCB(CVDisplayLinkRef l, const CVTimeStamp *now,
                       const CVTimeStamp *out, CVOptionFlags in,
                       CVOptionFlags *f, void *ctx)
{
    (void)l; (void)out; (void)in; (void)f; (void)ctx;
    atomic_fetch_add(&gLinkFires, 1);
    if (now && (now->flags & kCVTimeStampHostTimeValid)) {
        uint64_t h = now->hostTime;
        if (gLastHost && gJitN < kJitMax) {
            gJit[gJitN++] = (double)(h - gLastHost) * gTbNs / 1.0e6; /* ms */
        }
        gLastHost = h;
    }
    return kCVReturnSuccess;
}

/* One ioreg per sample. 2x7's key() ran a separate ioreg per counter and
 * produced two false "writes != sets" alarms; the counters must be read in
 * one pass or they are read seconds apart at up to 85/s. */
/* 2y6: the VBL period is READ, not assumed.
 *
 * This tool used to divide by a literal 143.83 to turn vblank counts into
 * seconds of panel time, and then used that as the DENOMINATOR for sets/s.
 * That was correct while 143.83 Hz was the only mode the kext had. The
 * moment boot 256 ran at 60 Hz it silently reported 11.94 s of panel time
 * for a 30.14 s window and "137.72 sets/s" for a true 54.58/s - a 2.5x
 * overstatement, in the direction of looking like a triumph. Nothing in the
 * output said anything was wrong.
 *
 * Rule 41 in a userspace tool rather than a kext: the kext publishes
 * RBFBVBLPeriodNs, so read it. If it is absent (an older framebuffer), say
 * so loudly and fall back - never silently. */
static double gPeriodNs = 0.0;
/* 2y9: THE TIME BASE MUST COME FROM THE SAME SNAPSHOT AS THE COUNTERS.
 *
 * 2y7 reported our VBL delivering 57.00/s against a requested 60.00 and called
 * it a -5.0 % defect, then blamed macOS timer coalescing. Both wrong, and the
 * mechanism is rule 31 aimed at this tool: these counters are read from
 * PUBLISHED PROPERTIES, which are refreshed periodically and so are stale by
 * design. Dividing a stale-bracketed counter delta by a FRESH wall-clock delta
 * undercounts by however far behind the last publish was - and that error
 * scales with the publish interval in wall time, so it appeared at 60 Hz and
 * not at 143.83 Hz. The kext's own RBFBVBLModeMhz read 60033 against a
 * requested 60002, +0.05 %, with nothing wrong at all.
 *
 * So rates are now counter-delta over RBFBVBLUptimeNs-delta, both taken from
 * the SAME ioreg pass, and the staleness cancels instead of being charged to
 * the kext. Wall time is still printed, as the control: if the two disagree,
 * that is a finding about this tool and not about the framebuffer. */
static unsigned long long gUptimeNs = 0;

static double fbRateHz(void)
{
    return gPeriodNs > 0.0 ? 1e9 / gPeriodNs : 0.0;
}

static int fbsnap(unsigned long long *sets, unsigned long long *fires,
                  unsigned long long *writes)
{
    FILE *p = popen("ioreg -rl -c RBAMDFramebuffer 2>/dev/null | awk '"
                    "/\"RBFBAmtrSets\" = /     { s = $NF } "
                    "/\"RBFBVBLFires\" = /     { f = $NF } "
                    "/\"RBFBHubpWrites\" = /   { w = $NF } "
                    "/\"RBFBVBLPeriodNs\" = /  { p = $NF } "
                    "/\"RBFBVBLUptimeNs\" = /  { u = $NF } "
                    "END { if (s == \"\" || f == \"\") exit 1; "
                    "print s, f, w, (p == \"\" ? 0 : p), "
                    "(u == \"\" ? 0 : u) }'", "r");
    if (!p) { return 0; }
    unsigned long long a = 0, b = 0, c = 0, d = 0, e = 0;
    int n = fscanf(p, "%llu %llu %llu %llu %llu", &a, &b, &c, &d, &e);
    int st = pclose(p);
    if (n < 2 || st != 0) { return 0; }
    *sets = a; *fires = b; *writes = c;
    if (d > 0) { gPeriodNs = (double)d; }
    gUptimeNs = e;   /* the KEXT's own clock, same snapshot as the counters */
    return 1;
}

int main(int argc, char **argv)
{
    double secs = 30.0, every = 10.0;
    int window = 1;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--secs") && i + 1 < argc)  { secs  = atof(argv[++i]); }
        else if (!strcmp(argv[i], "--every") && i + 1 < argc) { every = atof(argv[++i]); }
        else if (!strcmp(argv[i], "--link-only")) { window = 0; }
        else {
            fprintf(stderr, "usage: animtest [--secs N] [--every N] [--link-only]\n");
            return 2;
        }
    }

    mach_timebase_info_data_t tb;
    mach_timebase_info(&tb);
    gTbNs = (double)tb.numer / (double)tb.denom;

    @autoreleasepool {
        NSApplication *app = [NSApplication sharedApplication];
        [app setActivationPolicy:NSApplicationActivationPolicyAccessory];

        NSWindow *win = nil;
        if (window) {
            NSRect r = NSMakeRect(60, 60, 360, 220);
            win = [[NSWindow alloc] initWithContentRect:r
                        styleMask:NSWindowStyleMaskTitled|NSWindowStyleMaskClosable
                          backing:NSBackingStoreBuffered defer:NO];
            [win setTitle:@"rbgpu animtest"];
            [win setLevel:NSFloatingWindowLevel];

            NSView *v = [win contentView];
            [v setWantsLayer:YES];
            CALayer *root = [v layer];
            root.backgroundColor = CGColorCreateGenericRGB(0.07, 0.07, 0.09, 1.0);

            CALayer *dot = [CALayer layer];
            dot.frame = CGRectMake(0, 80, 56, 56);
            dot.cornerRadius = 28;
            dot.backgroundColor = CGColorCreateGenericRGB(0.95, 0.35, 0.15, 1.0);
            [root addSublayer:dot];

            /* WindowServer runs this; we do nothing per frame. A repeating,
             * autoreversing position animation is the plainest continuous
             * on-screen change there is. */
            CABasicAnimation *move = [CABasicAnimation animationWithKeyPath:@"position.x"];
            move.fromValue   = @(28);
            move.toValue     = @(332);
            move.duration    = 0.8;
            move.repeatCount = HUGE_VALF;
            move.autoreverses = YES;
            move.timingFunction =
                [CAMediaTimingFunction functionWithName:kCAMediaTimingFunctionLinear];
            [dot addAnimation:move forKey:@"move"];

            CABasicAnimation *fade = [CABasicAnimation animationWithKeyPath:@"opacity"];
            fade.fromValue   = @(1.0);
            fade.toValue     = @(0.25);
            fade.duration    = 0.37;
            fade.repeatCount = HUGE_VALF;
            fade.autoreverses = YES;
            [root addAnimation:fade forKey:@"fade"];

            [win orderFrontRegardless];
        }

        CVDisplayLinkRef link = NULL;
        CGDirectDisplayID disp = CGMainDisplayID();
        CVReturn cv = CVDisplayLinkCreateWithCGDisplay(disp, &link);
        if (cv != kCVReturnSuccess || !link) {
            fprintf(stderr, "CVDisplayLinkCreateWithCGDisplay failed: %d\n", cv);
            return 1;
        }
        CVDisplayLinkSetOutputCallback(link, linkCB, NULL);
        cv = CVDisplayLinkStart(link);
        if (cv != kCVReturnSuccess) {
            fprintf(stderr, "CVDisplayLinkStart failed: %d\n", cv);
            return 1;
        }

        /* The link's own idea of the period, independent of our counting. */
        CVTime nom = CVDisplayLinkGetNominalOutputVideoRefreshPeriod(link);
        double nomHz = 0.0;
        if (!(nom.flags & kCVTimeIsIndefinite) && nom.timeValue) {
            nomHz = (double)nom.timeScale / (double)nom.timeValue;
        }

        printf("mode: %s\n", window ? "animating window + display link"
                                    : "display link only (no window)");
        printf("nominal refresh: %.2f Hz%s\n", nomHz,
               (nom.flags & kCVTimeIsIndefinite) ? " (INDEFINITE)" : "");
        fflush(stdout);

        unsigned long long s0 = 0, f0 = 0, w0 = 0;
        int haveFB = fbsnap(&s0, &f0, &w0);
        /* 2y9: latch the kext's own clock from the SAME snapshot. */
        const unsigned long long u0 = gUptimeNs;
        if (!haveFB) {
            printf("RBAMDFramebuffer: ABSENT - fb columns will read n/a\n");
        }
        unsigned long long l0 = atomic_load(&gLinkFires);
        double t0 = CFAbsoluteTimeGetCurrent();

        __block double lastT = t0;
        __block unsigned long long lastL = l0, lastS = s0, lastF = f0;
        __block int done = 0;

        dispatch_source_t tick =
            dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0,
                                   dispatch_get_main_queue());
        dispatch_source_set_timer(tick,
            dispatch_time(DISPATCH_TIME_NOW, (int64_t)(every * NSEC_PER_SEC)),
            (uint64_t)(every * NSEC_PER_SEC), NSEC_PER_SEC / 10);
        dispatch_source_set_event_handler(tick, ^{
            double t = CFAbsoluteTimeGetCurrent();
            unsigned long long l = atomic_load(&gLinkFires);
            unsigned long long s = 0, f = 0, w = 0;
            int ok = fbsnap(&s, &f, &w);
            double dt = t - lastT;
            printf("+%4.0fs  link %6.2f/s", t - t0, (double)(l - lastL) / dt);
            if (ok && haveFB) {
                /* sets/s against WALL time, which is the rate a human sees.
                 * The delivery fraction - sets per vblank - is a SEPARATE
                 * number, because dividing by panel time conflates "we fed
                 * every scanout" with "the panel scans slowly". */
                unsigned long long dv = f - lastF;
                printf("   sets %7.2f/s  vbl %6.2f/s  fed %5.1f%%",
                       (double)(s - lastS) / dt,
                       (double)dv / dt,
                       dv ? (double)(s - lastS) / (double)dv * 100.0 : 0.0);
            } else {
                printf("   sets      n/a");
            }
            printf("\n");
            fflush(stdout);
            lastT = t; lastL = l; lastS = s; lastF = f;
            if (t - t0 >= secs) {
                done = 1;
                [NSApp stop:nil];
                /* stop: only takes effect on the next event; post one. */
                [NSApp postEvent:[NSEvent otherEventWithType:NSEventTypeApplicationDefined
                                   location:NSZeroPoint modifierFlags:0
                                  timestamp:0 windowNumber:0 context:nil
                                    subtype:0 data1:0 data2:0] atStart:YES];
            }
        });
        dispatch_resume(tick);

        [app run];
        (void)done;

        CVDisplayLinkStop(link);
        unsigned long long l1 = atomic_load(&gLinkFires);
        unsigned long long s1 = 0, f1 = 0, w1 = 0;
        int ok = fbsnap(&s1, &f1, &w1);
        double t1 = CFAbsoluteTimeGetCurrent();
        printf("\n--- totals over %.2f s wall ---\n", t1 - t0);
        printf("display link : %llu fires = %.2f/s\n", l1 - l0,
               (double)(l1 - l0) / (t1 - t0));
        if (ok && haveFB) {
            const double hz = fbRateHz();
            const double wall = t1 - t0;
            /* The kext's own elapsed time between the two snapshots. */
            const double kdt = (gUptimeNs > u0) ? (double)(gUptimeNs - u0) / 1e9
                                                : 0.0;
            const double base = kdt > 0.0 ? kdt : wall;
            if (kdt > 0.0) {
                printf("time base    : kext %.2f s vs wall %.2f s (%+.2f %%)%s\n",
                       kdt, wall, (kdt / wall - 1.0) * 100.0,
                       (kdt / wall < 0.97 || kdt / wall > 1.03)
                           ? "   <-- published counters are STALE; "
                             "rates below use the kext clock"
                           : "");
            } else {
                printf("time base    : wall only "
                       "(RBFBVBLUptimeNs not published)\n");
            }
            if (hz <= 0.0) {
                printf("fb vbl period: NOT PUBLISHED by this framebuffer - "
                       "delivery fractions below are omitted rather than "
                       "computed from a guessed rate\n");
            }
            printf("fb vbl fires : %llu = %.2f/s", f1 - f0,
                   (double)(f1 - f0) / base);
            if (hz > 0.0) {
                printf("   (asked %.2f/s, %+.1f%%)", hz,
                       ((double)(f1 - f0) / base / hz - 1.0) * 100.0);
            }
            printf("\n");
            printf("amtr sets    : %llu = %.2f/s", s1 - s0,
                   (double)(s1 - s0) / base);
            if (f1 - f0) {
                printf("   = %.1f%% of scanouts fed",
                       (double)(s1 - s0) / (double)(f1 - f0) * 100.0);
            }
            printf("\n");
            printf("hubp writes  : %llu%s\n", w1 - w0,
                   (w1 - w0) == (s1 - s0) ? " (tracks sets exactly)"
                                          : "  <-- DIVERGES from sets");
        }
        if (gJitN >= 8) {
            double mn = 1e30, mx = -1e30, sum = 0.0;
            for (unsigned i = 0; i < gJitN; i++) {
                if (gJit[i] < mn) { mn = gJit[i]; }
                if (gJit[i] > mx) { mx = gJit[i]; }
                sum += gJit[i];
            }
            double mean = sum / gJitN, var = 0.0;
            for (unsigned i = 0; i < gJitN; i++) {
                double d = gJit[i] - mean;
                var += d * d;
            }
            var /= gJitN;
            printf("\n--- vblank regularity, %u intervals (CVTimeStamp hostTime) ---\n",
                   gJitN);
            printf("mean   %8.4f ms  = %7.3f Hz\n", mean, 1000.0 / mean);
            printf("stddev %8.4f ms  (%.1f%% of mean)\n", sqrt(var),
                   100.0 * sqrt(var) / mean);
            printf("min    %8.4f ms   max %8.4f ms   spread %.4f ms\n",
                   mn, mx, mx - mn);
            /* 2y6: the target is whatever mode is CURRENT, not the 143.83 Hz
             * the panel had when this histogram was written. A jitter plot
             * against the wrong target reads as a defect. */
            const double thz = fbRateHz();
            if (thz > 0.0) {
                printf("target %8.4f ms  = %7.3f Hz  (RBFBVBLPeriodNs, live)\n",
                       gPeriodNs / 1e6, thz);
            } else {
                printf("target        ?  (RBFBVBLPeriodNs not published)\n");
            }
            /* A histogram, because a mean and a sigma cannot tell a steady
             * clock with a few long stalls from a uniformly sloppy one, and
             * those two have different fixes. */
            static const double edge[] = {6.5, 7.0, 7.5, 8.0, 8.5, 9.0,
                                          10.0, 12.0, 16.0, 1e30};
            static const char *lbl[] = {"<6.5", "6.5-7", "7-7.5", "7.5-8",
                                        "8-8.5", "8.5-9", "9-10", "10-12",
                                        "12-16", ">16"};
            unsigned bin[10] = {0};
            for (unsigned i = 0; i < gJitN; i++) {
                for (unsigned b = 0; b < 10; b++) {
                    if (gJit[i] < edge[b]) { bin[b]++; break; }
                }
            }
            printf("histogram (ms):\n");
            for (unsigned b = 0; b < 10; b++) {
                if (!bin[b]) { continue; }
                printf("  %-7s %6u  %5.1f%%  ", lbl[b], bin[b],
                       100.0 * bin[b] / gJitN);
                unsigned n = (unsigned)(60.0 * bin[b] / gJitN);
                for (unsigned k = 0; k < n; k++) { putchar('#'); }
                printf("\n");
            }
        } else {
            printf("\nvblank regularity: only %u intervals - NOT a result\n", gJitN);
        }
        CVDisplayLinkRelease(link);
    }
    return 0;
}
