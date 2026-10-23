/* SPDX-License-Identifier: MIT
 *
 * 2pb: CAN FRAMES ACTUALLY REACH THE SCREEN?
 *
 * 2pb's first instrument localised nothing cleanly: the GPU blit is fine
 * (2.6 GB/s VRAM->VRAM, control passed), but the readback number was a cached
 * memcpy rather than a bus transfer, and CGDisplayCreateImage carries
 * WindowServer IPC. Rule 16 - the instrument is a suspect - so measure the
 * path that is actually in question instead.
 *
 * This renders full-screen through a CAMetalLayer, which is the same present
 * path WindowServer composites through, and reports the achieved frame time
 * distribution. Twice:
 *
 *   displaySyncEnabled = NO   -> raw capacity of the present path, with the
 *                                refresh clock taken out of the question.
 *   displaySyncEnabled = YES  -> what a real application experiences.
 *
 * The reading:
 *   sync OFF near 144+ fps -> the present path is NOT the bottleneck, and the
 *                             5-second stalls are serialisation somewhere
 *                             else, not bandwidth.
 *   sync OFF far below     -> every frame is being COPIED to the fixed
 *                             scanout address instead of flipped to, and page
 *                             flipping is the fix.
 *   p99 >> p50             -> the stalls are spikes, which is what the user
 *                             describes, and the mean would have hidden them.
 *
 * Opens a black full-screen window for a few seconds, renders, and quits.
 * Writes nothing; touches no hardware directly.
 */
#import <Cocoa/Cocoa.h>
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>
#import <mach/mach_time.h>
#import <sys/resource.h>

static double g_npt = 0.0;
static double now_ms(void) {
    if (g_npt == 0.0) { mach_timebase_info_data_t tb; mach_timebase_info(&tb);
        g_npt = (double)tb.numer / (double)tb.denom; }
    return (double)mach_absolute_time() * g_npt / 1.0e6;
}

static int cmpd(const void *a, const void *b) {
    double x = *(const double *)a, y = *(const double *)b;
    return (x > y) - (x < y);
}

#define FRAMES 240

/* Pipelined: no per-frame wait, so the GPU can have several frames in flight.
 * If this is much faster than the serialised pass the problem is LATENCY; if
 * it is the same, the problem is BANDWIDTH and no amount of pipelining hides
 * it. That distinction decides the diagnosis, so it is measured, not argued. */
static void run_pipelined(CAMetalLayer *layer, id<MTLCommandQueue> q, BOOL sync,
                          const char *label) {
    layer.displaySyncEnabled = sync;
    const int N = 120;
    int got = 0;
    id<MTLCommandBuffer> last = nil;
    double deadline = now_ms() + 20000.0;
    double t0 = now_ms();
    for (int i = 0; i < N && now_ms() < deadline; i++) {
        id<CAMetalDrawable> d = [layer nextDrawable];
        if (!d) continue;
        MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
        rp.colorAttachments[0].texture = d.texture;
        rp.colorAttachments[0].loadAction = MTLLoadActionClear;
        rp.colorAttachments[0].storeAction = MTLStoreActionStore;
        rp.colorAttachments[0].clearColor =
            MTLClearColorMake((i & 15) / 15.0, 0.06, 0.02, 1.0);
        id<MTLCommandBuffer> cb = [q commandBuffer];
        id<MTLRenderCommandEncoder> e = [cb renderCommandEncoderWithDescriptor:rp];
        [e endEncoding];
        [cb presentDrawable:d];
        [cb commit];
        last = cb;
        got++;
    }
    if (last) [last waitUntilCompleted];
    double el = now_ms() - t0;
    /* CPU time vs wall time decides the mechanism. A CPU copy of 7.9 MB per
     * frame through an uncached mapping pegs a core, so cpu/wall approaches 1.
     * Waiting on a fence or a vblank that never comes burns no CPU, so
     * cpu/wall approaches 0. This is the difference between "page flipping is
     * the fix" and "something is serialising". */
    struct rusage ru; getrusage(RUSAGE_SELF, &ru);
    double cpu_ms = (double)ru.ru_utime.tv_sec * 1000.0 + ru.ru_utime.tv_usec / 1000.0
                  + (double)ru.ru_stime.tv_sec * 1000.0 + ru.ru_stime.tv_usec / 1000.0;
    static double prev_cpu = 0.0;
    double d_cpu = cpu_ms - prev_cpu; prev_cpu = cpu_ms;
    if (got < 10) { printf("  %-10s INCONCLUSIVE - %d frames\n", label, got); return; }
    printf("  %-10s %4d frames | %6.2f ms/frame (%6.1f fps) | total %.0f ms | "
           "our CPU %.0f ms = %.0f%% of wall  [pipelined]\n",
           label, got, el / got, 1000.0 * got / el, el, d_cpu, 100.0 * d_cpu / el);
}

static void run_pass(CAMetalLayer *layer, id<MTLCommandQueue> q, BOOL sync,
                     const char *label) {
    layer.displaySyncEnabled = sync;
    double *ft = calloc(FRAMES, sizeof(double));
    int got = 0;
    double t_prev = now_ms();
    double deadline = t_prev + 15000.0;      /* watchdog: never hang */

    for (int i = 0; i < FRAMES && now_ms() < deadline; i++) {
        id<CAMetalDrawable> d = [layer nextDrawable];
        if (!d) continue;
        MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
        rp.colorAttachments[0].texture = d.texture;
        rp.colorAttachments[0].loadAction = MTLLoadActionClear;
        rp.colorAttachments[0].storeAction = MTLStoreActionStore;
        /* A full clear writes every pixel, so this is a whole screen of
         * traffic per frame and not an empty submit. */
        rp.colorAttachments[0].clearColor =
            MTLClearColorMake((i & 15) / 15.0, 0.02, 0.06, 1.0);
        id<MTLCommandBuffer> cb = [q commandBuffer];
        id<MTLRenderCommandEncoder> e = [cb renderCommandEncoderWithDescriptor:rp];
        [e endEncoding];
        [cb presentDrawable:d];
        [cb commit];
        [cb waitUntilCompleted];           /* measure the WHOLE path, honestly */
        double t = now_ms();
        if (i > 4) ft[got++] = t - t_prev;  /* drop warm-up */
        t_prev = t;
    }

    if (got < 10) {
        printf("  %-10s INCONCLUSIVE - only %d frames completed in 15 s\n", label, got);
        free(ft); return;
    }
    double sum = 0; for (int i = 0; i < got; i++) sum += ft[i];
    double mean = sum / got;
    qsort(ft, (size_t)got, sizeof(double), cmpd);
    double p50 = ft[got / 2], p99 = ft[(int)(got * 0.99)], mx = ft[got - 1];
    printf("  %-10s %4d frames | mean %6.2f ms (%6.1f fps) | p50 %6.2f | "
           "p99 %7.2f | max %7.2f ms\n",
           label, got, mean, 1000.0 / mean, p50, p99, mx);
    free(ft);
}

int main(void) {
    @autoreleasepool {
        [NSApplication sharedApplication];
        [NSApp setActivationPolicy:NSApplicationActivationPolicyAccessory];

        id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
        if (!dev) { printf("no Metal device\n"); return 1; }
        id<MTLCommandQueue> q = [dev newCommandQueue];

        NSRect f = [NSScreen mainScreen].frame;
        printf("device: %s\nscreen: %.0f x %.0f (backing scale %.1f)\n\n",
               dev.name.UTF8String, f.size.width, f.size.height,
               [NSScreen mainScreen].backingScaleFactor);

        NSWindow *w = [[NSWindow alloc]
            initWithContentRect:f styleMask:NSWindowStyleMaskBorderless
                        backing:NSBackingStoreBuffered defer:NO];
        w.level = NSNormalWindowLevel;
        w.opaque = YES;
        w.backgroundColor = [NSColor blackColor];

        CAMetalLayer *layer = [CAMetalLayer layer];
        layer.device = dev;
        layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
        layer.framebufferOnly = YES;
        layer.frame = f;
        CGFloat s = [NSScreen mainScreen].backingScaleFactor;
        layer.drawableSize = CGSizeMake(f.size.width * s, f.size.height * s);
        w.contentView.layer = layer;
        w.contentView.wantsLayer = YES;
        [w makeKeyAndOrderFront:nil];

        /* Let the window actually map before timing anything. */
        for (int i = 0; i < 30; i++)
            [[NSRunLoop currentRunLoop] runUntilDate:
                [NSDate dateWithTimeIntervalSinceNow:0.01]];

        printf("full-screen present, %d frames per pass, waitUntilCompleted each frame:\n",
               FRAMES);
        run_pass(layer, q, NO,  "sync OFF");
        run_pass(layer, q, YES, "sync ON");
        run_pipelined(layer, q, NO,  "pipe OFF");
        run_pipelined(layer, q, YES, "pipe ON");

        [w orderOut:nil];
        printf("\none screen = 7.9 MB; 144 fps of that = 1139 MB/s, 60 fps = 475 MB/s.\n"
               "The GPU blit control measured 2649 MB/s, so the GPU can feed it.\n");
    }
    return 0;
}
