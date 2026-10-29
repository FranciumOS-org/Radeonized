/* 2xb - hold a display stream open so WindowServer keeps compositing.
 *
 * 2xa measured the lever: an active screen capture takes RBFBAmtrSets from
 * 0.05-0.08 to 71.30 sets/s, reversibly, with two controls - the same number
 * a connected AnyDesk session gives. WindowServer names the mechanism itself,
 * WSDisplayStreamNeedsRecomposite (2x9): a display stream needs a frame, so it
 * recomposites.
 *
 * So this is a BRIDGE, not a fix. It makes the panel usable today while the
 * real question - what makes WindowServer believe the panel itself needs no
 * frame - stays open. Say that plainly rather than shipping it as a driver.
 *
 * Why CGDisplayStream and not ScreenCaptureKit: 2x9 read AnyDesk's own link
 * table and _CGDisplayStreamCreateWithDispatchQueue RESOLVES on 15.8.1, so the
 * "obsoleted in 15.0" notice that made 2x5 rewrite framepump to SCK was not a
 * reason to avoid it - and that rewrite is what revoked framepump's TCC grant,
 * since permission is keyed to binary identity. One API, one binary, one grant.
 *
 * It DISCARDS every frame. screencapture -v proves the lever but writes about
 * 450 KB/s to disk; nothing here keeps a byte.
 *
 * AND IT REPORTS ITS OWN EFFECT. framepump cost three exchanges because
 * "didn't work" meant TCC-denied, then ran-with-zero-frames, then TCC-denied
 * again, and only the tool's own output could have told them apart (rule 14,
 * and rule 39's corollary: an instrument that needs its own load must publish
 * the load beside the result). So this prints frames/s AND the framebuffer's
 * transaction rate side by side, and names the failure when frames stop.
 */

#include <CoreGraphics/CoreGraphics.h>
#include <IOSurface/IOSurfaceRef.h>
#include <dispatch/dispatch.h>
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* The SDK marks every CGDisplayStream entry point `unavailable: obsoleted in
 * macOS 15.0`, so -Wno-deprecated-declarations does not get us past it - the
 * calls simply will not compile. But the symbols are LIVE in the dylib: all
 * four resolve through dlopen/dlsym on this machine, with CGMainDisplayID as a
 * positive control and a made-up name as a negative one (rule 26, after a
 * first check that linked no CoreGraphics at all and so searched nothing).
 * AnyDesk requires the same symbols and runs here. So bind them by hand. */
typedef void (^RBStreamHandler)(int32_t status, uint64_t displayTime,
                                IOSurfaceRef frameSurface, void *updateRef);
typedef void *(*RBCreateFn)(uint32_t display, size_t w, size_t h,
                            int32_t pixelFormat, CFDictionaryRef properties,
                            dispatch_queue_t queue, RBStreamHandler handler);
typedef int32_t (*RBStartFn)(void *stream);
typedef int32_t (*RBStopFn)(void *stream);

enum { RBFrameComplete = 0, RBFrameIdle = 1, RBFrameBlank = 2, RBStopped = 3 };

static volatile uint64_t gComplete, gIdle, gBlank, gStopped;

static double now_s(void)
{
    return (double)clock_gettime_nsec_np(CLOCK_MONOTONIC) / 1e9;
}

/* One ioreg per sample, value taken as the last field. 2xa: computing a byte
 * offset instead dropped the leading digit of every counter and produced
 * negative deltas. */
static long fbkey(const char *key)
{
    char cmd[320];
    snprintf(cmd, sizeof cmd,
             "ioreg -rl -c RBAMDFramebuffer 2>/dev/null | "
             "awk '/\"%s\" = /{v=$NF} END{if(v==\"\")exit 1; print v}'", key);
    FILE *f = popen(cmd, "r");
    if (!f) { return -1; }
    long v = -1;
    if (fscanf(f, "%ld", &v) != 1) { v = -1; }
    pclose(f);
    return v;
}

int main(int argc, char **argv)
{
    double secs = 0.0;              /* 0 = run until killed */
    double every = 10.0;
    int quiet = 0;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--secs") && i + 1 < argc) {
            secs = atof(argv[++i]);
        } else if (!strcmp(argv[i], "--every") && i + 1 < argc) {
            every = atof(argv[++i]);
            if (every < 1.0) { every = 1.0; }
        } else if (!strcmp(argv[i], "--quiet")) {
            quiet = 1;
        } else {
            fprintf(stderr,
                "usage: %s [--secs N] [--every N] [--quiet]\n"
                "  Holds a CGDisplayStream on the main display and discards every\n"
                "  frame, so WindowServer keeps compositing (2xa). Reports frames/s\n"
                "  and RBFBAmtrSets/s together.\n", argv[0]);
            return 2;
        }
    }

    CGDirectDisplayID d = CGMainDisplayID();
    size_t w = CGDisplayPixelsWide(d), h = CGDisplayPixelsHigh(d);
    if (w == 0 || h == 0) {
        fprintf(stderr, "FAIL: main display reports %zux%zu\n", w, h);
        return 1;
    }
    printf("dstream: display 0x%x, %zux%zu, discarding every frame\n",
           (unsigned)d, w, h);

    void *cg = dlopen("/System/Library/Frameworks/CoreGraphics.framework/"
                      "CoreGraphics", RTLD_LAZY);
    if (!cg) {
        fprintf(stderr, "FAIL: dlopen CoreGraphics: %s\n", dlerror());
        return 1;
    }
    RBCreateFn Create = (RBCreateFn)dlsym(cg, "CGDisplayStreamCreateWithDispatchQueue");
    RBStartFn  Start  = (RBStartFn) dlsym(cg, "CGDisplayStreamStart");
    RBStopFn   Stop   = (RBStopFn)  dlsym(cg, "CGDisplayStreamStop");
    if (!Create || !Start || !Stop) {
        fprintf(stderr,
            "FAIL: CGDisplayStream symbols missing from this CoreGraphics\n"
            "  (Create %p Start %p Stop %p). The API has finally been removed;\n"
            "  this needs rewriting against ScreenCaptureKit.\n",
            (void *)Create, (void *)Start, (void *)Stop);
        return 1;
    }

    dispatch_queue_t q =
        dispatch_queue_create("rbgpu.dstream", DISPATCH_QUEUE_SERIAL);

    /* No kCGDisplayStreamMinimumFrameTime: a frame-time floor would throttle
     * the very recomposites this exists to cause. */
    void *s = Create(
        (uint32_t)d, w, h, 'BGRA', NULL, q,
        ^(int32_t status, uint64_t t, IOSurfaceRef surf, void *upd) {
            (void)t; (void)surf; (void)upd;
            switch (status) {
            case RBFrameComplete: gComplete++; break;
            case RBFrameIdle:     gIdle++;     break;
            case RBFrameBlank:    gBlank++;    break;
            case RBStopped:       gStopped++;  break;
            }
        });

    if (!s) {
        fprintf(stderr,
            "FAIL: CGDisplayStreamCreateWithDispatchQueue returned NULL.\n"
            "  Almost certainly Screen Recording permission. Grant it to this\n"
            "  binary (or to the terminal running it) in System Settings ->\n"
            "  Privacy & Security -> Screen Recording, then run it again.\n"
            "  Do NOT rebuild after granting: permission is keyed to binary\n"
            "  identity and a rebuild revokes it (2x5).\n");
        return 1;
    }

    int32_t e = Start(s);
    if (e != 0) {
        fprintf(stderr, "FAIL: CGDisplayStreamStart = %d\n", (int)e);
        return 1;
    }

    long s0 = fbkey("RBFBAmtrSets"), f0 = fbkey("RBFBVBLFires");
    int haveFb = (s0 >= 0 && f0 >= 0);
    if (!haveFb && !quiet) {
        printf("  note: RBAMDFramebuffer is not publishing - frames/s only\n");
    }
    printf("  stream started%s\n", secs > 0 ? "" : " (ctrl-C or kill to stop)");
    fflush(stdout);

    double t0 = now_s(), tp = t0;
    uint64_t cp = 0;
    int warned = 0;

    for (;;) {
        usleep(250000);
        double t = now_s();
        if (secs > 0 && t - t0 >= secs) { break; }
        if (t - tp < every) { continue; }

        uint64_t c = gComplete;
        double fps = (double)(c - cp) / (t - tp);

        char rate[96] = "";
        if (haveFb) {
            long s1 = fbkey("RBFBAmtrSets"), f1 = fbkey("RBFBVBLFires");
            if (s1 >= s0 && f1 > f0) {
                /* The panel's own vblank counter is the clock (2wj): 143.83 Hz. */
                double sec = (double)(f1 - f0) / 143.83;
                snprintf(rate, sizeof rate, "   fb %6.2f sets/s",
                         sec > 0 ? (double)(s1 - s0) / sec : 0.0);
            } else if (s1 >= 0 && f1 >= 0) {
                snprintf(rate, sizeof rate, "   fb counters reset (new boot?)");
            }
            s0 = s1; f0 = f1;
        }

        printf("  +%6.0fs  frames %8llu (%7.2f/s)  idle %llu blank %llu%s\n",
               t - t0, (unsigned long long)c, fps,
               (unsigned long long)gIdle, (unsigned long long)gBlank, rate);
        fflush(stdout);

        /* A stream that creates fine and then delivers nothing is the exact
         * shape of framepump's failure. Name it instead of printing a quiet
         * zero forever. */
        if (!warned && c == 0 && t - t0 > 20.0) {
            printf("  *** 0 frames after %.0fs - the stream exists but delivers\n"
                   "      nothing. Check Screen Recording permission. The stream\n"
                   "      MAY still be enough to force recomposites, so read the\n"
                   "      fb rate above, not the frame count (2x5).\n", t - t0);
            fflush(stdout);
            warned = 1;
        }
        if (gStopped) {
            printf("  *** stream reported Stopped - giving up\n");
            break;
        }
        tp = t; cp = c;
    }

    Stop(s);
    printf("dstream: stopped after %.0fs, %llu frames\n",
           now_s() - t0, (unsigned long long)gComplete);
    return 0;
}
