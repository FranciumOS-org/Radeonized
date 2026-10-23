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
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

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

int main(int argc, const char **argv) {
    @autoreleasepool {
        bool wantDispatch = false;
        for (int i = 1; i < argc; i++) {
            if (!strcmp(argv[i], "--dispatch")) { wantDispatch = true; }
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
        }
        if (!wantDispatch) {
            printf("\n(enumerate only; pass --dispatch to actually run a kernel)\n");
        }
        return rc;
    }
}
