/* 2x5: the cheapest possible frame consumer.
 *
 * Boot 208 measured it: with AnyDesk running, display-pipe transactions went
 * 0.08/s -> 83.4/s with no typing at all, and RBFBHubpWrites tracked them
 * exactly (2 rejects in the whole boot). So the kernel chain was always
 * correct, and the defect is that WindowServer composites LAZILY - with
 * nothing consuming frames it skips them, and a screen-capture client forces
 * it to produce every one.
 *
 * AnyDesk proves the mechanism but pays for encoding and networking. This
 * holds the same lever with nothing attached: a ScreenCaptureKit stream
 * scaled down as far as it will go, whose output handler does nothing. The
 * frames get PRODUCED - which is the entire point - and then dropped.
 *
 * CGDisplayStream was obsoleted in macOS 15.0, so this is SCK.
 *
 * This is a WORKAROUND and is labelled as one. The proper fix is whatever
 * makes WindowServer treat the panel itself as a consumer. Until that is
 * found, this is the difference between a usable desktop and a frozen one.
 *
 * Needs Screen Recording permission, as any capture client does - AnyDesk
 * included. Granted once in System Settings > Privacy & Security.
 */
#import <Foundation/Foundation.h>
#import <ScreenCaptureKit/ScreenCaptureKit.h>
#import <CoreMedia/CoreMedia.h>
#include <stdatomic.h>

static atomic_ullong g_frames = 0;
static size_t gW = 0, gH = 0;   /* 0 = the display's native size */

@interface RBPump : NSObject <SCStreamOutput, SCStreamDelegate>
@end

@implementation RBPump
- (void)stream:(SCStream *)stream
didOutputSampleBuffer:(CMSampleBufferRef)sampleBuffer
        ofType:(SCStreamOutputType)type {
    (void)stream; (void)sampleBuffer;
    /* Deliberately empty. Producing the frame is the product; looking at it
     * is not. Touching the sample buffer here would only add cost. */
    if (type == SCStreamOutputTypeScreen) {
        atomic_fetch_add(&g_frames, 1);
    }
}
- (void)stream:(SCStream *)stream didStopWithError:(NSError *)error {
    fprintf(stderr, "framepump: stream stopped: %s\n",
            error.localizedDescription.UTF8String ?: "(no description)");
    exit(4);
}
@end

int main(int argc, char **argv) {
    @autoreleasepool {
        double report = (argc > 1) ? atof(argv[1]) : 10.0;
        if (argc > 3) { gW = (size_t)atol(argv[2]); gH = (size_t)atol(argv[3]); }
        __block SCStream *stream = nil;
        RBPump *pump = [RBPump new];
        dispatch_queue_t q = dispatch_queue_create("rbgpu.framepump",
                                                   DISPATCH_QUEUE_SERIAL);
        dispatch_semaphore_t ready = dispatch_semaphore_create(0);
        __block BOOL ok = NO;

        [SCShareableContent getShareableContentWithCompletionHandler:
            ^(SCShareableContent *content, NSError *err) {
            if (err || content.displays.count == 0) {
                fprintf(stderr,
                    "framepump: no shareable content (%s).\n"
                    "  This is almost always Screen Recording permission:\n"
                    "  System Settings > Privacy & Security > Screen Recording.\n"
                    "  AnyDesk needs the same permission.\n",
                    err.localizedDescription.UTF8String ?: "no displays");
                dispatch_semaphore_signal(ready);
                return;
            }
            SCDisplay *disp = content.displays.firstObject;
            SCContentFilter *filter =
                [[SCContentFilter alloc] initWithDisplay:disp excludingWindows:@[]];
            SCStreamConfiguration *cfg = [SCStreamConfiguration new];
            /* 2x5 correction: 64x36 was wrong twice over. Measured with
             * AnyDesk up and transactions running at 65/s, a 64x36 stream was
             * handed ZERO frames - SCK delivers only on a change it can see,
             * and at that size almost any real change downscales to identical
             * pixels. So it could neither observe the desktop nor be trusted
             * to drive it. AnyDesk captures at native size; match it, and let
             * the caller scale down to probe where the threshold is. */
            size_t w = gW ? gW : (size_t)disp.width;
            size_t h = gH ? gH : (size_t)disp.height;
            cfg.width = w;
            cfg.height = h;
            cfg.minimumFrameInterval = CMTimeMake(1, 144);   /* ask for 144 Hz */
            cfg.showsCursor = NO;
            cfg.queueDepth = 3;
            NSError *e = nil;
            stream = [[SCStream alloc] initWithFilter:filter
                                        configuration:cfg
                                             delegate:pump];
            if (![stream addStreamOutput:pump
                                    type:SCStreamOutputTypeScreen
                      sampleHandlerQueue:q
                                   error:&e]) {
                fprintf(stderr, "framepump: addStreamOutput failed: %s\n",
                        e.localizedDescription.UTF8String ?: "?");
                dispatch_semaphore_signal(ready);
                return;
            }
            [stream startCaptureWithCompletionHandler:^(NSError *se) {
                if (se) {
                    fprintf(stderr, "framepump: startCapture failed: %s\n",
                            se.localizedDescription.UTF8String ?: "?");
                } else {
                    ok = YES;
                }
                dispatch_semaphore_signal(ready);
            }];
        }];

        dispatch_semaphore_wait(ready, DISPATCH_TIME_FOREVER);
        if (!ok) { return 2; }

        printf("framepump: stream running (%zux%zu). Ctrl-C to stop.\n",
               gW ? gW : 0, gH ? gH : 0);
        printf("           the DESKTOP should now animate on its own.\n");
        fflush(stdout);

        unsigned long long lastN = 0;
        NSDate *lastT = [NSDate date];
        for (;;) {
            [[NSRunLoop currentRunLoop]
                runUntilDate:[NSDate dateWithTimeIntervalSinceNow:report]];
            NSDate *nowT = [NSDate date];
            unsigned long long n = atomic_load(&g_frames);
            double el = [nowT timeIntervalSinceDate:lastT];
            printf("  frames %8llu   %6.1f/s\n", n, (double)(n - lastN) / el);
            fflush(stdout);
            lastN = n; lastT = nowT;
        }
    }
}
