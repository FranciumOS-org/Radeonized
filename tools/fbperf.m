/* SPDX-License-Identifier: MIT
 *
 * 2pb: WHERE DOES THE TIME GO? The user's symptoms do not fit a 30 Hz clock:
 * a 5-second freeze while a minimise blur runs, 30 s to change a wallpaper,
 * typing that drops words. Those are THROUGHPUT symptoms, and every one of
 * them is an operation that moves or reads back a whole screen.
 *
 * So measure the three paths separately and let them disagree:
 *
 *   A. GPU-side blit  - texture -> texture, never leaves VRAM. The CONTROL.
 *                       If this is fast, the GPU and its memory are fine.
 *   B. GPU -> CPU readback - texture -> malloc'd bytes. What a blur or a
 *                       backdrop filter needs.
 *   C. Screen capture - CGDisplayCreateImage of the whole display. This is
 *                       the real compositor path: it reads the live scanout
 *                       surface, which on IONDRVFramebuffer is ONE 8 MB
 *                       buffer at a fixed address.
 *
 * A fast and C slow localises the problem to the scanout path - which would
 * mean the composited output has to be COPIED to a fixed address every frame
 * because nothing flips HUBP0_PRIMARY_SURFACE_ADDRESS. That is a different
 * defect from the 30 Hz one and needs a different fix.
 *
 * Read-only. Allocates and frees; writes nothing to hardware.
 */
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <CoreGraphics/CoreGraphics.h>
#import <mach/mach_time.h>

#import <dlfcn.h>

/* CGDisplayCreateImage is marked obsoleted in the 15.0 SDK but is still
 * EXPORTED and still works. Taken by dlsym rather than redeclared, because
 * the header's availability attribute wins over a local prototype. Nothing
 * in ScreenCaptureKit is a drop-in synchronous equivalent, and this is the
 * measurement that matters: it reads the live scanout surface. */
typedef CGImageRef (*t_cgdci)(CGDirectDisplayID);

static double g_ns_per_tick = 0.0;
static double now_ms(void) {
    if (g_ns_per_tick == 0.0) {
        mach_timebase_info_data_t tb; mach_timebase_info(&tb);
        g_ns_per_tick = (double)tb.numer / (double)tb.denom;
    }
    return (double)mach_absolute_time() * g_ns_per_tick / 1.0e6;
}

#define W 1920
#define H 1080
#define BYTES ((size_t)W * H * 4)

int main(void) {
    @autoreleasepool {
        id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
        if (!dev) { printf("no Metal device\n"); return 1; }
        printf("device: %s\n", dev.name.UTF8String);
        printf("recommendedMaxWorkingSet: %llu MB\n",
               (unsigned long long)(dev.recommendedMaxWorkingSetSize >> 20));
        printf("one screen = %zu bytes (%.1f MB)\n\n", BYTES, BYTES / 1048576.0);

        id<MTLCommandQueue> q = [dev newCommandQueue];
        MTLTextureDescriptor *td =
            [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                              width:W height:H mipmapped:NO];
        td.usage = MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite |
                   MTLTextureUsageRenderTarget;
        td.storageMode = MTLStorageModePrivate;
        id<MTLTexture> src = [dev newTextureWithDescriptor:td];
        id<MTLTexture> dst = [dev newTextureWithDescriptor:td];
        if (!src || !dst) { printf("texture alloc failed\n"); return 1; }

        /* ---- A. GPU-side blit. The control. ---- */
        const int NA = 60;
        double t0 = now_ms();
        for (int i = 0; i < NA; i++) {
            id<MTLCommandBuffer> cb = [q commandBuffer];
            id<MTLBlitCommandEncoder> be = [cb blitCommandEncoder];
            [be copyFromTexture:src sourceSlice:0 sourceLevel:0
                   sourceOrigin:MTLOriginMake(0,0,0) sourceSize:MTLSizeMake(W,H,1)
                      toTexture:dst destinationSlice:0 destinationLevel:0
              destinationOrigin:MTLOriginMake(0,0,0)];
            [be endEncoding];
            [cb commit];
            if (i == NA - 1) [cb waitUntilCompleted];
        }
        double a = (now_ms() - t0) / NA;
        printf("A. GPU blit  (VRAM->VRAM) : %7.2f ms/screen  %8.1f MB/s   [control]\n",
               a, BYTES / 1048576.0 / (a / 1000.0));

        /* ---- B. GPU -> CPU readback ---- */
        td.storageMode = MTLStorageModeShared;
        td.usage = MTLTextureUsageShaderRead;
        id<MTLTexture> shared = [dev newTextureWithDescriptor:td];
        void *buf = malloc(BYTES);
        const int NB = 20;
        t0 = now_ms();
        for (int i = 0; i < NB; i++) {
            [shared getBytes:buf bytesPerRow:W*4
                  fromRegion:MTLRegionMake2D(0,0,W,H) mipmapLevel:0];
        }
        double b = (now_ms() - t0) / NB;
        printf("B. readback   (GPU->CPU)  : %7.2f ms/screen  %8.1f MB/s\n",
               b, BYTES / 1048576.0 / (b / 1000.0));
        free(buf);

        /* ---- C. the real compositor path ---- */
        t_cgdci cgdci = (t_cgdci)dlsym(RTLD_DEFAULT, "CGDisplayCreateImage");
        if (!cgdci) { printf("C. screen cap: CGDisplayCreateImage unavailable\n"); return 0; }
        CGDirectDisplayID disp = CGMainDisplayID();
        const int NC = 10;
        double worst = 0, total = 0;
        for (int i = 0; i < NC; i++) {
            double s = now_ms();
            CGImageRef img = cgdci(disp);
            double e = now_ms() - s;
            if (img) CFRelease(img);
            total += e;
            if (e > worst) worst = e;
        }
        double c = total / NC;
        printf("C. screen cap (scanout)   : %7.2f ms/screen  %8.1f MB/s   "
               "(worst %.2f ms)\n", c, BYTES / 1048576.0 / (c / 1000.0), worst);

        printf("\n--- reading ---\n");
        printf("A fast + C slow  -> the SCANOUT path is the bottleneck: the\n"
               "                    composited frame is being copied to a fixed\n"
               "                    address instead of flipped to. Needs page\n"
               "                    flipping (HUBP0_PRIMARY_SURFACE_ADDRESS).\n");
        printf("A slow           -> not the framebuffer at all; the GPU or its\n"
               "                    memory path is the problem.\n");
        printf("all fast         -> throughput is NOT the defect and the stalls\n"
               "                    are somewhere else (serialisation, not bandwidth).\n");
        printf("\nfor scale: 60 fps of one screen = %.0f MB/s; a 5-pass blur\n"
               "reads ~5x that.\n", BYTES / 1048576.0 * 60);
    }
    return 0;
}
