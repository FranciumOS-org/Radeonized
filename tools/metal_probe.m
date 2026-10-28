// 2hc: does the 680M actually EXECUTE Metal work, or merely advertise it?
//
// 2hb: the armed boot reported `Metal: Supported` from system_profiler, and
// the armed registry shows an AMDAccelSharedUserClient whose API property
// reads "Metal". Neither establishes that a single instruction ran on the
// GPU - a capability string is a claim, and a user client attaching is a
// client attaching. This runs work and CHECKS THE ANSWER.
//
// Two stages, because they fail differently and the second can hang:
//   --enumerate (default)  list every MTLDevice and its properties. Touches
//                          no queues and dispatches nothing.
//   --dispatch             also run a compute kernel that writes a known
//                          function of each index, then VERIFY every element
//                          on the CPU. A kernel that runs but computes
//                          nothing would otherwise look like success.
//
// Built with CommandLineTools clang; the shader is compiled at runtime via
// newLibraryWithSource, so no Xcode metal compiler is needed.
//
// 2wo: --bench [N]. Boot 197 measured GFX channel 52 submitting 2.40 stamps/s
// and retiring 2.09, with a backlog growing 10 -> 118, WHILE the ring read
// r == w at all three samples with no VM faults - so the chip executes
// everything immediately and the ~2/s is the driver learning that work
// finished. This decomposes one command buffer into the three intervals that
// tell those apart, which no score from a benchmark app can:
//
//   GPU      GPUEndTime - GPUStartTime           what the chip actually took
//   driver   kernelEndTime - kernelStartTime     what the driver scheduled
//   wall     commit -> waitUntilCompleted        what the caller waited for
//
// If GPU is microseconds and wall is hundreds of milliseconds, the defect is
// the completion path and it is proven on the RENDERING side with no display
// involvement at all. If wall tracks GPU, the ~2/s is not a GPU-work ceiling
// and the display scheduling is where to look next.
//
// It also runs a PIPELINED pass - N buffers committed back to back, waiting
// only for the last - because a per-buffer notification cost and a bandwidth
// ceiling look identical when every buffer is waited on individually.
//
// Needs no GUI and writes nothing to the screen that matters: the caller
// redirects it to a file. Every iteration is flushed, so a hang still leaves
// the iterations that completed (rule 10).
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <mach/mach_time.h>

static const char *kSrc =
    "#include <metal_stdlib>\n"
    "using namespace metal;\n"
    "kernel void fill(device uint *out [[buffer(0)]],\n"
    "                 uint i [[thread_position_in_grid]]) {\n"
    "    out[i] = i * 3u + 7u;\n"
    "}\n";

#define N 4096

static int dispatch_on(id<MTLDevice> dev) {
    NSError *err = nil;
    printf("  dispatch: creating queue\n"); fflush(stdout);
    id<MTLCommandQueue> q = [dev newCommandQueue];
    if (!q) { printf("  FAIL: no command queue\n"); return 1; }

    printf("  dispatch: compiling shader\n"); fflush(stdout);
    id<MTLLibrary> lib = [dev newLibraryWithSource:@(kSrc) options:nil error:&err];
    if (!lib) { printf("  FAIL: shader compile: %s\n", err.description.UTF8String); return 1; }
    id<MTLFunction> fn = [lib newFunctionWithName:@"fill"];
    if (!fn) { printf("  FAIL: no function 'fill'\n"); return 1; }
    id<MTLComputePipelineState> ps = [dev newComputePipelineStateWithFunction:fn error:&err];
    if (!ps) { printf("  FAIL: pipeline: %s\n", err.description.UTF8String); return 1; }

    // Prefilled with a sentinel, so "the kernel never ran" and "the kernel
    // wrote zeros" cannot look the same (absence is not a result).
    id<MTLBuffer> buf = [dev newBufferWithLength:N * sizeof(uint32_t)
                                         options:MTLResourceStorageModeShared];
    if (!buf) { printf("  FAIL: no buffer\n"); return 1; }
    uint32_t *p = (uint32_t *)buf.contents;
    for (int i = 0; i < N; i++) { p[i] = 0xDEADBEEFu; }

    printf("  dispatch: encoding %d threads\n", N); fflush(stdout);
    id<MTLCommandBuffer> cb = [q commandBuffer];
    id<MTLComputeCommandEncoder> enc = [cb computeCommandEncoder];
    [enc setComputePipelineState:ps];
    [enc setBuffer:buf offset:0 atIndex:0];
    [enc dispatchThreads:MTLSizeMake(N, 1, 1)
   threadsPerThreadgroup:MTLSizeMake(64, 1, 1)];
    [enc endEncoding];

    printf("  dispatch: committing - IF THE GPU HANGS, IT HANGS HERE\n");
    fflush(stdout);
    [cb commit];
    [cb waitUntilCompleted];
    printf("  dispatch: command buffer status = %ld\n", (long)cb.status);
    if (cb.error) { printf("  FAIL: %s\n", cb.error.description.UTF8String); return 1; }

    int bad = 0, untouched = 0;
    for (uint32_t i = 0; i < N; i++) {
        uint32_t want = i * 3u + 7u;
        if (p[i] == 0xDEADBEEFu) { untouched++; }
        else if (p[i] != want) {
            if (bad < 4) { printf("  MISMATCH at %u: got 0x%08X want 0x%08X\n", i, p[i], want); }
            bad++;
        }
    }
    if (untouched == N) { printf("  FAIL: every element still the sentinel - NOTHING ran\n"); return 1; }
    if (untouched)      { printf("  FAIL: %d of %d elements untouched\n", untouched, N); return 1; }
    if (bad)            { printf("  FAIL: %d of %d elements wrong\n", bad, N); return 1; }
    printf("  PASS: all %d elements correct - the GPU executed the kernel\n", N);
    return 0;
}

/* Wall-clock seconds, from the monotonic clock rather than NSDate. */
static double now_s(void) {
    static mach_timebase_info_data_t tb;
    if (tb.denom == 0) { mach_timebase_info(&tb); }
    return (double)mach_absolute_time() * tb.numer / tb.denom / 1e9;
}

static int cmp_d(const void *a, const void *b) {
    double x = *(const double *)a, y = *(const double *)b;
    return (x > y) - (x < y);
}

static int bench_on(id<MTLDevice> dev, int iters) {
    NSError *err = nil;
    id<MTLCommandQueue> q = [dev newCommandQueue];
    id<MTLLibrary> lib = [dev newLibraryWithSource:@(kSrc) options:nil error:&err];
    if (!q || !lib) { printf("  FAIL: no queue or library\n"); return 1; }
    id<MTLComputePipelineState> ps =
        [dev newComputePipelineStateWithFunction:[lib newFunctionWithName:@"fill"]
                                           error:&err];
    id<MTLBuffer> buf = [dev newBufferWithLength:N * sizeof(uint32_t)
                                         options:MTLResourceStorageModeShared];
    if (!ps || !buf) { printf("  FAIL: no pipeline or buffer\n"); return 1; }

    /* The dispatch is deliberately TINY - 4096 threads of three arithmetic
     * ops. Anything the chip takes longer than microseconds on is overhead,
     * which is the whole point: this measures latency, not throughput. */
    double *wall = calloc(iters, sizeof(double));
    double *gpu  = calloc(iters, sizeof(double));
    double *drv  = calloc(iters, sizeof(double));
    if (!wall || !gpu || !drv) { printf("  FAIL: out of memory\n"); return 1; }

    printf("  bench: %d serial command buffers (commit, wait, repeat)\n", iters);
    fflush(stdout);
    for (int i = 0; i < iters; i++) {
        id<MTLCommandBuffer> cb = [q commandBuffer];
        id<MTLComputeCommandEncoder> enc = [cb computeCommandEncoder];
        [enc setComputePipelineState:ps];
        [enc setBuffer:buf offset:0 atIndex:0];
        [enc dispatchThreads:MTLSizeMake(N, 1, 1)
       threadsPerThreadgroup:MTLSizeMake(64, 1, 1)];
        [enc endEncoding];
        double t0 = now_s();
        [cb commit];
        [cb waitUntilCompleted];
        wall[i] = now_s() - t0;
        gpu[i]  = cb.GPUEndTime    - cb.GPUStartTime;
        drv[i]  = cb.kernelEndTime - cb.kernelStartTime;
        if (cb.error) {
            printf("  iter %d ERROR: %s\n", i, cb.error.description.UTF8String);
        }
        /* Flushed every iteration so a hang keeps what already ran. */
        printf("    %3d  wall %9.3f ms   gpu %9.3f ms   driver %9.3f ms\n",
               i, wall[i] * 1e3, gpu[i] * 1e3, drv[i] * 1e3);
        fflush(stdout);
    }

    double *sw = malloc(iters * sizeof(double));
    double *sg = malloc(iters * sizeof(double));
    if (!sw || !sg) { printf("  FAIL: out of memory\n"); return 1; }
    memcpy(sw, wall, iters * sizeof(double));
    memcpy(sg, gpu,  iters * sizeof(double));
    qsort(sw, iters, sizeof(double), cmp_d);
    qsort(sg, iters, sizeof(double), cmp_d);
    double medw = sw[iters / 2], medg = sg[iters / 2];
    printf("  SERIAL median: wall %.3f ms  gpu %.3f ms  -> %.1f buffers/s\n",
           medw * 1e3, medg * 1e3, medw > 0 ? 1.0 / medw : 0.0);
    printf("  SERIAL range : wall %.3f .. %.3f ms\n", sw[0] * 1e3, sw[iters - 1] * 1e3);
    if (medg > 0) {
        printf("  OVERHEAD RATIO: wall / gpu = %.1fx\n", medw / medg);
    }

    /* Pipelined: commit all of them, wait only for the last. A per-buffer
     * notification cost disappears here; a bandwidth ceiling does not. */
    printf("  bench: %d pipelined command buffers (commit all, wait for last)\n",
           iters);
    fflush(stdout);
    id<MTLCommandBuffer> last = nil;
    double p0 = now_s();
    for (int i = 0; i < iters; i++) {
        id<MTLCommandBuffer> cb = [q commandBuffer];
        id<MTLComputeCommandEncoder> enc = [cb computeCommandEncoder];
        [enc setComputePipelineState:ps];
        [enc setBuffer:buf offset:0 atIndex:0];
        [enc dispatchThreads:MTLSizeMake(N, 1, 1)
       threadsPerThreadgroup:MTLSizeMake(64, 1, 1)];
        [enc endEncoding];
        [cb commit];
        last = cb;
    }
    [last waitUntilCompleted];
    double ptot = now_s() - p0;
    printf("  PIPELINED: %d buffers in %.3f s -> %.1f buffers/s\n",
           iters, ptot, ptot > 0 ? iters / ptot : 0.0);
    if (medw > 0) {
        printf("  PIPELINE SPEEDUP vs serial: %.1fx\n", (iters / ptot) / (1.0 / medw));
    }

    /* The verify from --dispatch still applies: a fast wrong answer is not a
     * result (rule 14). */
    uint32_t *p = (uint32_t *)buf.contents;
    int bad = 0;
    for (uint32_t i = 0; i < N; i++) {
        if (p[i] != i * 3u + 7u) { bad++; }
    }
    printf("  verify: %s (%d of %d wrong)\n", bad ? "FAIL" : "PASS", bad, N);
    free(wall); free(gpu); free(drv); free(sw); free(sg);
    return bad ? 1 : 0;
}

int main(int argc, const char **argv) {
    @autoreleasepool {
        bool wantDispatch = false;
        bool wantBench = false;
        int iters = 20;
        for (int i = 1; i < argc; i++) {
            if (!strcmp(argv[i], "--dispatch")) { wantDispatch = true; }
            else if (!strcmp(argv[i], "--bench")) {
                wantBench = true;
                /* Bounded: this submits real GPU work on a machine whose
                 * channels already time out, so an accidental huge N is not
                 * something to find out about afterwards. */
                if (i + 1 < argc && argv[i + 1][0] >= '0' && argv[i + 1][0] <= '9') {
                    int n = atoi(argv[++i]);
                    if (n >= 1 && n <= 1000) { iters = n; }
                    else { printf("--bench N must be 1..1000; using %d\n", iters); }
                }
            }
        }
        NSArray<id<MTLDevice>> *devs = MTLCopyAllDevices();
        printf("MTLCopyAllDevices: %lu device(s)\n", (unsigned long)devs.count);
        if (devs.count == 0) {
            printf("no Metal devices - nothing to test\n");
            return 2;
        }
        int rc = 0;
        for (id<MTLDevice> d in devs) {
            printf("\ndevice: %s\n", d.name.UTF8String);
            printf("  registryID      = 0x%llx\n", d.registryID);
            printf("  lowPower        = %d\n", (int)d.isLowPower);
            printf("  removable       = %d\n", (int)d.isRemovable);
            printf("  headless        = %d\n", (int)d.isHeadless);
            printf("  unifiedMemory   = %d\n", (int)d.hasUnifiedMemory);
            printf("  maxThreadsPerTG = %lu\n", (unsigned long)d.maxThreadsPerThreadgroup.width);
            printf("  recommendedMax  = %llu MB\n", d.recommendedMaxWorkingSetSize >> 20);

            /* 2or: which GPU family does Metal think this is? System Profiler
             * prints "Metal 3" from MTLCopyAllDevices + MTLGPUFamily (the
             * strings spdisplays_metal3 / spdisplays_metalfamily /
             * spdisplays_mtlgpufamilysupport are in SPDisplaysReporter), so
             * "Metal: Supported" rather than "Metal 3" is a question about
             * this list, not about the display pipe. */
            struct { const char *name; MTLGPUFamily fam; } fams[] = {
                { "Metal3",    MTLGPUFamilyMetal3 },
                { "Apple7",    MTLGPUFamilyApple7 },
                { "Mac2",      MTLGPUFamilyMac2 },
                { "Common3",   MTLGPUFamilyCommon3 },
                { "Common2",   MTLGPUFamilyCommon2 },
                { "Common1",   MTLGPUFamilyCommon1 },
            };
            printf("  GPU families    =");
            for (size_t i = 0; i < sizeof(fams) / sizeof(fams[0]); i++) {
                if ([d respondsToSelector:@selector(supportsFamily:)] &&
                    [d supportsFamily:fams[i].fam]) {
                    printf(" %s", fams[i].name);
                }
            }
            printf("\n");
            if (@available(macOS 13.0, *)) {
                printf("  argumentBuffers = tier %ld\n", (long)d.argumentBuffersSupport);
            }
            if (wantDispatch) { rc |= dispatch_on(d); }
            if (wantBench)    { rc |= bench_on(d, iters); }
        }
        if (!wantDispatch && !wantBench) {
            printf("\n(enumerate only; --dispatch runs a kernel, "
                   "--bench [N] times one)\n");
        }
        return rc;
    }
}
