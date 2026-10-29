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
 * 2xc, FROM THE FIRST LIVE RUN: frames/s and RBFBAmtrSets/s came back as the
 * SAME NUMBER every sample (3.27/3.24, 3.64/3.34, 0.99/0.99). So WindowServer
 * composites exactly once per frame the consumer ASKS FOR, and the consumer is
 * the clock. CGDisplayStream only asks when something CHANGED, which is
 * circular here - nothing changes, so it does not ask, so nothing composites -
 * and it settles at about 1/s. `screencapture -v` reached 71.3/s because a
 * VIDEO RECORDING demands a frame on a fixed cadence whether or not the screen
 * changed. So the bridge needs an API that DEMANDS a rate, which is
 * ScreenCaptureKit's minimumFrameInterval - the one screencapture itself uses
 * on 15.x. SCK is the default here; --cgs keeps the CGDisplayStream path so the
 * two can be compared from ONE binary, i.e. under one TCC grant.
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

#import <Foundation/Foundation.h>
#import <ScreenCaptureKit/ScreenCaptureKit.h>
#import <CoreMedia/CoreMedia.h>
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

/* SCK sink: counts and discards. Nothing is retained, copied or written. */
@interface RBSink : NSObject <SCStreamOutput, SCStreamDelegate>
@end

@implementation RBSink
- (void)stream:(SCStream *)s didOutputSampleBuffer:(CMSampleBufferRef)sb
                                             ofType:(SCStreamOutputType)type
{
    (void)s;
    if (type != SCStreamOutputTypeScreen) { return; }
    /* SCK marks unchanged frames idle; both still cost a recomposite, which is
     * the whole point, so count them apart rather than dropping either. */
    CFArrayRef att = CMSampleBufferGetSampleAttachmentsArray(sb, false);
    long st = -1;
    if (att && CFArrayGetCount(att) > 0) {
        CFDictionaryRef d = CFArrayGetValueAtIndex(att, 0);
        CFNumberRef n = CFDictionaryGetValue(d, (__bridge CFStringRef)SCStreamFrameInfoStatus);
        if (n) { CFNumberGetValue(n, kCFNumberLongType, &st); }
    }
    if (st == SCFrameStatusIdle) { gIdle++; } else if (st == SCFrameStatusBlank) { gBlank++; }
    else { gComplete++; }
}
- (void)stream:(SCStream *)s didStopWithError:(NSError *)e
{
    (void)s;
    fprintf(stderr, "  *** SCK stream stopped: %s\n",
            e.localizedDescription.UTF8String ?: "(no reason)");
    gStopped++;
}
@end

/* Returns 0 on success. Keeps the stream alive in *out so main can stop it. */
static int sck_start(CGDirectDisplayID want, size_t w, size_t h, double fps,
                     SCStream **out, RBSink **sinkOut)
{
    __block SCDisplay *disp = nil;
    __block NSError *err = nil;
    dispatch_semaphore_t sem = dispatch_semaphore_create(0);

    [SCShareableContent getShareableContentWithCompletionHandler:
        ^(SCShareableContent *content, NSError *e) {
            err = e;
            for (SCDisplay *d in content.displays) {
                if (d.displayID == want) { disp = d; break; }
            }
            if (!disp && content.displays.count > 0) { disp = content.displays[0]; }
            dispatch_semaphore_signal(sem);
        }];
    if (dispatch_semaphore_wait(sem,
            dispatch_time(DISPATCH_TIME_NOW, 15ull * NSEC_PER_SEC)) != 0) {
        fprintf(stderr, "FAIL: SCShareableContent timed out after 15s.\n");
        return 1;
    }
    if (!disp) {
        fprintf(stderr,
            "FAIL: no shareable display (%s).\n"
            "  This is the Screen Recording permission. Grant it to the app or\n"
            "  terminal that launched this, then run it again.\n",
            err ? err.localizedDescription.UTF8String : "no error reported");
        return 1;
    }

    SCContentFilter *filter =
        [[SCContentFilter alloc] initWithDisplay:disp excludingWindows:@[]];
    SCStreamConfiguration *cfg = [[SCStreamConfiguration alloc] init];
    cfg.width = w; cfg.height = h;
    cfg.pixelFormat = 'BGRA';
    cfg.queueDepth = 6;
    cfg.showsCursor = YES;
    /* THE WHOLE POINT: demand a frame this often whether or not anything
     * changed. CGDisplayStream has no equivalent, which is why it stalls. */
    cfg.minimumFrameInterval = CMTimeMake(1, (int32_t)(fps + 0.5));

    RBSink *sink = [[RBSink alloc] init];
    SCStream *st = [[SCStream alloc] initWithFilter:filter
                                      configuration:cfg
                                           delegate:sink];
    NSError *addErr = nil;
    dispatch_queue_t q = dispatch_queue_create("rbgpu.sck", DISPATCH_QUEUE_SERIAL);
    if (![st addStreamOutput:sink type:SCStreamOutputTypeScreen
          sampleHandlerQueue:q error:&addErr]) {
        fprintf(stderr, "FAIL: addStreamOutput: %s\n",
                addErr.localizedDescription.UTF8String);
        return 1;
    }

    __block NSError *startErr = nil;
    dispatch_semaphore_t sem2 = dispatch_semaphore_create(0);
    [st startCaptureWithCompletionHandler:^(NSError *e) {
        startErr = e; dispatch_semaphore_signal(sem2);
    }];
    if (dispatch_semaphore_wait(sem2,
            dispatch_time(DISPATCH_TIME_NOW, 15ull * NSEC_PER_SEC)) != 0) {
        fprintf(stderr, "FAIL: startCapture timed out after 15s.\n");
        return 1;
    }
    if (startErr) {
        fprintf(stderr, "FAIL: startCapture: %s\n",
                startErr.localizedDescription.UTF8String);
        return 1;
    }
    *out = st; *sinkOut = sink;
    return 0;
}

int main(int argc, char **argv)
{
    double secs = 0.0;              /* 0 = run until killed */
    double every = 10.0;
    int quiet = 0;
    int useCgs = 0;                 /* default: SCK, which can demand a rate */
    double fps = 144.0;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--secs") && i + 1 < argc) {
            secs = atof(argv[++i]);
        } else if (!strcmp(argv[i], "--every") && i + 1 < argc) {
            every = atof(argv[++i]);
            if (every < 1.0) { every = 1.0; }
        } else if (!strcmp(argv[i], "--fps") && i + 1 < argc) {
            fps = atof(argv[++i]);
            if (fps < 1.0 || fps > 240.0) { fps = 144.0; }
        } else if (!strcmp(argv[i], "--cgs")) {
            useCgs = 1;
        } else if (!strcmp(argv[i], "--quiet")) {
            quiet = 1;
        } else {
            fprintf(stderr,
                "usage: %s [--secs N] [--every N] [--fps N] [--cgs] [--quiet]\n"
                "  Holds a capture stream on the main display and discards every\n"
                "  frame, so WindowServer keeps compositing (2xa). Reports frames/s\n"
                "  and RBFBAmtrSets/s together.\n"
                "    default  ScreenCaptureKit, demanding --fps frames/s (144)\n"
                "    --cgs    CGDisplayStream instead: it only asks on CHANGE, so\n"
                "             on an idle desktop it stalls at about 1/s (2xc)\n",
                argv[0]);
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

    void *s = NULL;
    RBStopFn Stop = NULL;
    SCStream *sck = nil;
    RBSink *sink = nil;

    if (useCgs) {
        /* The SDK marks every CGDisplayStream entry point unavailable, so the
         * calls will not compile; the symbols are live at runtime (checked with
         * CGMainDisplayID as a positive control and a made-up name as a
         * negative one). Kept only as the A/B against SCK. */
        void *cg = dlopen("/System/Library/Frameworks/CoreGraphics.framework/"
                          "CoreGraphics", RTLD_LAZY);
        if (!cg) { fprintf(stderr, "FAIL: dlopen CoreGraphics: %s\n", dlerror()); return 1; }
        RBCreateFn Create = (RBCreateFn)dlsym(cg, "CGDisplayStreamCreateWithDispatchQueue");
        RBStartFn  Start  = (RBStartFn) dlsym(cg, "CGDisplayStreamStart");
        Stop = (RBStopFn) dlsym(cg, "CGDisplayStreamStop");
        if (!Create || !Start || !Stop) {
            fprintf(stderr, "FAIL: CGDisplayStream symbols missing; use the default SCK mode.\n");
            return 1;
        }
        dispatch_queue_t q =
            dispatch_queue_create("rbgpu.dstream", DISPATCH_QUEUE_SERIAL);
        s = Create((uint32_t)d, w, h, 'BGRA', NULL, q,
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
                "  Screen Recording permission, for whatever launched this.\n");
            return 1;
        }
        int32_t e = Start(s);
        if (e != 0) { fprintf(stderr, "FAIL: CGDisplayStreamStart = %d\n", (int)e); return 1; }
        printf("  mode: CGDisplayStream (asks only on change)\n");
    } else {
        if (sck_start(d, w, h, fps, &sck, &sink) != 0) { return 1; }
        printf("  mode: ScreenCaptureKit, demanding %.0f frames/s\n", fps);
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

    if (sck) {
        dispatch_semaphore_t sem = dispatch_semaphore_create(0);
        [sck stopCaptureWithCompletionHandler:^(NSError *e) {
            (void)e; dispatch_semaphore_signal(sem);
        }];
        dispatch_semaphore_wait(sem,
            dispatch_time(DISPATCH_TIME_NOW, 5ull * NSEC_PER_SEC));
    } else if (s && Stop) {
        Stop(s);
    }
    printf("dstream: stopped after %.0fs, %llu frames\n",
           now_s() - t0, (unsigned long long)gComplete);
    return 0;
}
