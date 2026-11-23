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
#import <CoreVideo/CoreVideo.h>
#import <VideoToolbox/VideoToolbox.h>
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


/* ------------------------------------------------------------------ 3ftp
 * --probe: is an SCK frame CPU-readable, and does a realtime H.264 encoder
 * take it?
 *
 * FaceTime's screen share is two processes: replayd holds the SCK/SLSHWCapture
 * stream (it links ScreenCaptureKit and names com.apple.FaceTime and
 * avconferenced), and avconferenced encodes with VCPRealtimeEncoder. A sibling
 * session measured that encode failing - dataBufferPtr 0x0 on the first frame,
 * then BufMgtCompCachePic err -8969 - and proposed that SCK's buffers have no
 * CPU-readable base address on this machine.
 *
 * That hypothesis has a control it was not tested against: screencapture -v
 * produced correct SOFTWARE-encoded H.264 of the whole screen here. So this
 * mode asks the two questions separately, in one run, each with its own
 * control:
 *
 *   1. the BUFFER - lock an SCK frame and read its base address, against a
 *      CVPixelBuffer this tool allocates itself with IOSurface backing, same
 *      size and format, filled with a known pattern. If ours maps and SCK's
 *      does not, the buffer is the variable. If both map, it is not.
 *   2. the ENCODER - push both through the SAME VTCompressionSession in
 *      realtime mode. A session that takes our buffer and refuses SCK's blames
 *      the buffer; one that refuses both blames the encoder; one that takes
 *      both says the fault is in neither and lives in FaceTime's own pipeline.
 *
 * It also creates a hardware-REQUIRED session first, because "no hardware
 * H.264 encode" is the finding everything else here is downstream of, and a
 * reading is worth more than an inference (rule 14: the refusal is a code).
 * Nothing is written to disk; encoded bytes are counted and dropped.
 */

static unsigned gPixFmt = 'BGRA';
static int      gProbeWant;          /* frames still to probe, 0 = off */
static int      gProbeRan;           /* frames actually probed */
static VTCompressionSessionRef gEncSw;
static int      gEncSwErr = -999999; /* -999999 = not attempted yet */
static int      gEncHwErr = -999999;
static int      gEncHwTried;
static volatile int gCbCalls, gCbOkay;
static volatile long gCbBytes;

static void probe_cb(void *outRef, void *srcRef, OSStatus st,
                     VTEncodeInfoFlags flags, CMSampleBufferRef sb)
{
    (void)outRef; (void)srcRef; (void)flags;
    gCbCalls++;
    if (st == noErr && sb) {
        gCbOkay++;
        CMBlockBufferRef bb = CMSampleBufferGetDataBuffer(sb);
        if (bb) { gCbBytes += (long)CMBlockBufferGetDataLength(bb); }
    }
}

/* Builds the two sessions once, at the first probed frame, so they are sized
 * from the real buffer rather than from what we asked SCK for. */
static void probe_encoders(size_t w, size_t h)
{
    if (gEncSwErr != -999999) { return; }

    /* Hardware first, and REQUIRED, so the answer is a code and not a guess
     * about which encoder got picked. */
    const void *hwk[1] = { kVTVideoEncoderSpecification_RequireHardwareAcceleratedVideoEncoder };
    const void *hwv[1] = { kCFBooleanTrue };
    CFDictionaryRef hwSpec = CFDictionaryCreate(NULL, hwk, hwv, 1,
                                 &kCFTypeDictionaryKeyCallBacks,
                                 &kCFTypeDictionaryValueCallBacks);
    VTCompressionSessionRef hw = NULL;
    gEncHwErr = (int)VTCompressionSessionCreate(NULL, (int32_t)w, (int32_t)h,
                        kCMVideoCodecType_H264, hwSpec, NULL, NULL,
                        probe_cb, NULL, &hw);
    gEncHwTried = 1;
    if (hw) { VTCompressionSessionInvalidate(hw); CFRelease(hw); }
    CFRelease(hwSpec);

    /* Then the one FaceTime falls back to: software, realtime. */
    const void *swk[1] = { kVTVideoEncoderSpecification_EnableHardwareAcceleratedVideoEncoder };
    const void *swv[1] = { kCFBooleanFalse };
    CFDictionaryRef swSpec = CFDictionaryCreate(NULL, swk, swv, 1,
                                 &kCFTypeDictionaryKeyCallBacks,
                                 &kCFTypeDictionaryValueCallBacks);
    gEncSwErr = (int)VTCompressionSessionCreate(NULL, (int32_t)w, (int32_t)h,
                        kCMVideoCodecType_H264, swSpec, NULL, NULL,
                        probe_cb, NULL, &gEncSw);
    CFRelease(swSpec);
    if (gEncSw) {
        VTSessionSetProperty(gEncSw, kVTCompressionPropertyKey_RealTime, kCFBooleanTrue);
        VTSessionSetProperty(gEncSw, kVTCompressionPropertyKey_AllowFrameReordering, kCFBooleanFalse);
        VTSessionSetProperty(gEncSw, kVTCompressionPropertyKey_ProfileLevel,
                             kVTProfileLevel_H264_Main_AutoLevel);
    }
}

/* The in-run control: our own IOSurface-backed buffer, same geometry and
 * format, with a pattern written through the base address we are asking SCK
 * for. If this one cannot be written, the probe itself is broken (rule 16) and
 * says so rather than blaming SCK. */
static CVPixelBufferRef probe_control_buffer(size_t w, size_t h, OSType fmt)
{
    const void *k[1] = { kCVPixelBufferIOSurfacePropertiesKey };
    CFDictionaryRef empty = CFDictionaryCreate(NULL, NULL, NULL, 0,
                                &kCFTypeDictionaryKeyCallBacks,
                                &kCFTypeDictionaryValueCallBacks);
    const void *v[1] = { empty };
    CFDictionaryRef attrs = CFDictionaryCreate(NULL, k, v, 1,
                                &kCFTypeDictionaryKeyCallBacks,
                                &kCFTypeDictionaryValueCallBacks);
    CVPixelBufferRef pb = NULL;
    CVReturn cr = CVPixelBufferCreate(NULL, w, h, fmt, attrs, &pb);
    CFRelease(attrs); CFRelease(empty);
    if (cr != kCVReturnSuccess || !pb) { return NULL; }

    if (CVPixelBufferLockBaseAddress(pb, 0) == kCVReturnSuccess) {
        uint8_t *p = CVPixelBufferGetBaseAddress(pb);
        size_t bpr = CVPixelBufferGetBytesPerRow(pb);
        if (p) { for (size_t y = 0; y < h; y++) { memset(p + y * bpr, 0x40 + (int)(y & 0x3f), bpr); } }
        CVPixelBufferUnlockBaseAddress(pb, 0);
    }
    return pb;
}

/* Counts non-zero bytes in a few scattered rows. A frame that locks fine and
 * is all zeroes is black, which is a different finding from one that will not
 * map at all - and black is the symptom being chased. */
static void probe_content(const uint8_t *p, size_t bpr, size_t w, size_t h,
                          long *nonzero, long *looked)
{
    *nonzero = 0; *looked = 0;
    if (!p) { return; }
    for (size_t i = 0; i < 16; i++) {
        size_t y = (h * i) / 16;
        const uint8_t *row = p + y * bpr;
        size_t n = w * 4 < bpr ? w * 4 : bpr;
        for (size_t x = 0; x < n; x += 7) { (*looked)++; if (row[x]) { (*nonzero)++; } }
    }
}

static void probe_frame(CMSampleBufferRef sb, long frameStatus)
{
    gProbeRan++;
    int idx = gProbeRan;
    printf("\n  --- probe frame %d (SCFrameStatus %ld) ---\n", idx, frameStatus);

    CVImageBufferRef ib = CMSampleBufferGetImageBuffer(sb);
    if (!ib) {
        printf("    image buffer:      NULL  <-- no CVPixelBuffer on this sample\n");
        fflush(stdout);
        return;
    }
    CVPixelBufferRef pb = (CVPixelBufferRef)ib;
    size_t w = CVPixelBufferGetWidth(pb), h = CVPixelBufferGetHeight(pb);
    OSType fmt = CVPixelBufferGetPixelFormatType(pb);
    IOSurfaceRef surf = CVPixelBufferGetIOSurface(pb);
    size_t planes = CVPixelBufferGetPlaneCount(pb);
    char f4[5] = { (char)(fmt >> 24), (char)(fmt >> 16), (char)(fmt >> 8), (char)fmt, 0 };
    for (int i = 0; i < 4; i++) { if (f4[i] < 32 || f4[i] > 126) { f4[i] = '.'; } }

    printf("    geometry:          %zux%zu  format '%s' (0x%08x)  planes %zu\n",
           w, h, f4, (unsigned)fmt, planes);
    printf("    IOSurface:         %s\n", surf ? "present" : "NULL");

    CVReturn lk = CVPixelBufferLockBaseAddress(pb, kCVPixelBufferLock_ReadOnly);
    const uint8_t *base = NULL;
    int allBlack = 0;
    size_t bpr = 0;
    if (lk == kCVReturnSuccess) {
        base = CVPixelBufferGetBaseAddress(pb);
        bpr = CVPixelBufferGetBytesPerRow(pb);
    }
    printf("    lock(ReadOnly):    %d %s\n", (int)lk,
           lk == kCVReturnSuccess ? "(success)" : "<-- LOCK REFUSED");
    printf("    base address:      %p %s\n", (const void *)base,
           base ? "" : " <-- NULL, which is the reported FaceTime symptom");
    printf("    bytes per row:     %zu\n", bpr);

    for (size_t pl = 0; pl < planes; pl++) {
        void *pb0 = lk == kCVReturnSuccess
                        ? CVPixelBufferGetBaseAddressOfPlane(pb, pl) : NULL;
        printf("    plane %zu base:     %p  bpr %zu %s\n", pl, pb0,
               lk == kCVReturnSuccess ? CVPixelBufferGetBytesPerRowOfPlane(pb, pl) : 0,
               pb0 ? "" : "<-- NULL");
    }
    if (base) {
        long nz = 0, looked = 0;
        probe_content(base, bpr, w, h, &nz, &looked);
        printf("    content:           %ld of %ld sampled bytes non-zero  %s\n",
               nz, looked, (allBlack = (nz == 0)) ? "<-- ALL BLACK" : "(has picture)");
    }
    if (lk == kCVReturnSuccess) {
        CVPixelBufferUnlockBaseAddress(pb, kCVPixelBufferLock_ReadOnly);
    }

    probe_encoders(w, h);
    if (idx == 1) {
        printf("    H.264 session, hardware REQUIRED: err %d %s\n", gEncHwErr,
               gEncHwErr == 0 ? "(created - hardware encode EXISTS)"
                              : "<-- no hardware H.264 encoder on this machine");
        printf("    H.264 session, software forced:   err %d %s\n", gEncSwErr,
               gEncSwErr == 0 ? "(created)" : "<-- could not create either");
    }

    if (!gEncSw) { fflush(stdout); return; }

    /* The control goes through the same session, immediately before the real
     * frame, so the two differ in the buffer and in nothing else. */
    CVPixelBufferRef ctl = probe_control_buffer(w, h, fmt);
    int c0 = gCbOkay;
    OSStatus ce = -1;
    if (ctl) {
        ce = VTCompressionSessionEncodeFrame(gEncSw, ctl,
                CMTimeMake(idx * 2, 60), kCMTimeInvalid, NULL, NULL, NULL);
        VTCompressionSessionCompleteFrames(gEncSw, kCMTimeInvalid);
    }
    int cOut = gCbOkay - c0;
    printf("    encode OUR buffer: %s  encodeFrame err %d, %d frame(s) out\n",
           ctl ? "control allocated" : "CONTROL ALLOCATION FAILED",
           (int)ce, cOut);

    int s0 = gCbOkay;
    OSStatus se = VTCompressionSessionEncodeFrame(gEncSw, pb,
                      CMTimeMake(idx * 2 + 1, 60), kCMTimeInvalid, NULL, NULL, NULL);
    VTCompressionSessionCompleteFrames(gEncSw, kCMTimeInvalid);
    int sOut = gCbOkay - s0;
    printf("    encode SCK buffer: encodeFrame err %d, %d frame(s) out\n",
           (int)se, sOut);

    if (ctl) { CVPixelBufferRelease(ctl); }

    if (idx == 1) {
        const char *verdict;
        /* The break-it sweep caught this: a NULL base address printed beside
         * ">>> both encoded" is a contradiction a reader has to resolve by
         * hand. The buffer reading is the one being chased, so it outranks the
         * encode result, and an all-black frame that encodes fine is still the
         * symptom. */
        if (lk != kCVReturnSuccess || !base) {
            verdict = "the SCK buffer would NOT MAP - this is the reported "
                      "symptom, and the encode lines below it say nothing about it";
        } else if (allBlack) {
            verdict = "the SCK buffer maps but is ALL BLACK - capture, not encode";
        } else if (!ctl || (ce != 0 && se != 0 && cOut == 0 && sOut == 0)) {
            verdict = "BOTH refused - the encoder is the variable, not the buffer";
        } else if (cOut > 0 && sOut == 0) {
            verdict = "ours encoded, SCK's did not - THE BUFFER IS THE VARIABLE";
        } else if (cOut > 0 && sOut > 0) {
            verdict = "both encoded - neither the buffer nor the software encoder "
                      "is broken here";
        } else {
            verdict = "mixed - read the three lines above, do not take this one";
        }
        printf("    >>> %s\n", verdict);
    }
    fflush(stdout);
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
    /* 3ftp: probe only COMPLETE frames - an idle frame carries no new surface,
     * so a null base address on one would say nothing. */
    if (gProbeWant > 0 && st != SCFrameStatusIdle) { gProbeWant--; probe_frame(sb, st); }
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
    cfg.pixelFormat = (OSType)gPixFmt;
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
        } else if (!strcmp(argv[i], "--probe") && i + 1 < argc) {
            gProbeWant = atoi(argv[++i]);
            if (gProbeWant < 1) { gProbeWant = 1; }
            if (gProbeWant > 32) { gProbeWant = 32; }
        } else if (!strcmp(argv[i], "--fmt") && i + 1 < argc) {
            /* 3gvb: FaceTime's encoder asks for biplanar YCbCr, not BGRA, and
             * a buffer that maps in one format says nothing about the other -
             * different plane count, different allocation path. */
            const char *f = argv[++i];
            if (strlen(f) == 4) {
                gPixFmt = ((unsigned)f[0] << 24) | ((unsigned)f[1] << 16) |
                          ((unsigned)f[2] << 8) | (unsigned)f[3];
            }
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
                "             on an idle desktop it stalls at about 1/s (2xc)\n"
                "    --probe N  (3ftp) lock the first N complete frames, read the\n"
                "             base address, and push each through a realtime H.264\n"
                "             encoder beside a buffer this tool allocates itself.\n"
                "             Answers whether FaceTime's screen share fails on the\n"
                "             BUFFER or on the ENCODER. Implies --cgs off.\n",
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
