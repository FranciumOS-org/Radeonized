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
#include <math.h>
#import <mach/mach_time.h>

static const char *kSrc =
    "#include <metal_stdlib>\n"
    "using namespace metal;\n"
    "kernel void fill(device uint *out [[buffer(0)]],\n"
    "                 uint i [[thread_position_in_grid]]) {\n"
    "    out[i] = i * 3u + 7u;\n"
    "}\n"
    /* 2ws: the CONTROL kernel. GPUStartTime/GPUEndTime are only worth
     * building on if they are real GPU timestamps rather than the instant the
     * driver noticed the stamp. This one burns a caller-chosen amount of GPU
     * time, so GPUEnd-GPUStart must scale with the loop count while
     * commit->GPUStart stays pinned to the tick. If instead both move
     * together, the pair is a detection time and the split reading is wrong. */
    "kernel void spin(device uint *out [[buffer(0)]],\n"
    "                 constant uint &loops [[buffer(1)]],\n"
    "                 uint i [[thread_position_in_grid]]) {\n"
    "    uint v = i * 3u + 7u;\n"
    "    uint acc = v;\n"
    "    for (uint k = 0; k < loops; k++) { acc = acc * 1664525u + 1013904223u; }\n"
    "    out[i] = (acc == 0xffffffffu) ? acc : v;\n"
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

/* 2ws: THE PHASE SWEEP - does retirement happen on a CLOCK or after a DELAY?
 *
 * 2wq wrote that the 167-181 ms cluster was "too tight for a uniform poll,
 * which would spread over [0,T] and average T/2". That reasoning is WRONG for
 * a SERIAL loop, and the error matters because it pointed away from the one
 * mechanism the data fits best.
 *
 * A serial loop PHASE-LOCKS to the retirement instant. The next buffer is
 * submitted microseconds after the previous one retired, i.e. microseconds
 * after a sweep - so it has just missed one and waits very nearly a FULL
 * period. wall ~= T every time, tightly clustered. The spread over [0,T]
 * applies only to a submission with random phase, and the bench has exactly
 * one of those: iteration 0, which read 97.9 ms and 104.6 ms in the two runs.
 *
 * So the discriminator is to BREAK the phase lock deliberately: wait d after
 * each completion before submitting the next.
 *
 *   periodic sweep, period T  ->  wall(d) ~= T - d  for d < T, then wraps to
 *                                 2T - d. A SAWTOOTH, and its wrap point IS T.
 *   fixed per-buffer delay D  ->  wall(d) ~= D for every d. FLAT.
 *   uniform random poll       ->  noisy, no structure in d.
 *
 * The three are not close together, so one run separates them. */
static void spin_until(double t) {
    /* Spin rather than nanosleep: the quantity being resolved IS a few ms, so
     * the sleep's own granularity would land inside the answer. */
    while (now_s() < t) { }
}

static int phase_on(id<MTLDevice> dev, int reps) {
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

    const int    steps = 25;          /* 0 .. 240 ms */
    const double stepms = 10.0;
    printf("  phase: %d delays x %d reps (wait d after each completion, then "
           "resubmit)\n", steps, reps);
    printf("    d(ms)   median wall(ms)   d+wall(ms)   min      max\n");
    fflush(stdout);

    double *w = malloc(reps * sizeof(double));
    if (!w) { printf("  FAIL: out of memory\n"); return 1; }
    double bestsum = 1e9, bestd = -1, firstsum = 0;
    for (int s = 0; s < steps; s++) {
        double d = s * stepms / 1e3;
        for (int r = 0; r < reps; r++) {
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
            w[r] = now_s() - t0;
            if (cb.error) { printf("  d=%.0f rep %d ERROR\n", d * 1e3, r); }
            spin_until(now_s() + d);   /* break the phase lock */
        }
        qsort(w, reps, sizeof(double), cmp_d);
        double med = w[reps / 2];
        printf("    %5.0f   %13.3f   %10.3f   %7.3f  %7.3f\n",
               d * 1e3, med * 1e3, (d + med) * 1e3, w[0] * 1e3, w[reps - 1] * 1e3);
        fflush(stdout);
        if (s == 0) { firstsum = d + med; }
        if (d + med < bestsum) { bestsum = d + med; bestd = d; }
    }

    /* The verdict is about SHAPE, so it is computed from the shape, not eyed:
     * on a periodic sweep d+wall stays ~constant at T until the wrap; on a
     * fixed delay d+wall climbs by exactly d. */
    printf("  d=0 gives d+wall = %.1f ms; the flattest point is d=%.0f ms at "
           "%.1f ms\n", firstsum * 1e3, bestd * 1e3, bestsum * 1e3);
    printf("  READ THE TABLE: d+wall roughly CONSTANT across d means a PERIODIC\n"
           "  sweep and that constant is the period. d+wall climbing with d\n"
           "  means a FIXED per-buffer delay and the wall column is flat.\n");

    uint32_t *p = (uint32_t *)buf.contents;
    int bad = 0;
    for (uint32_t i = 0; i < N; i++) { if (p[i] != i * 3u + 7u) { bad++; } }
    printf("  verify: %s (%d of %d wrong)\n", bad ? "FAIL" : "PASS", bad, N);
    free(w);
    return bad ? 1 : 0;
}

/* 2ws: THE LATTICE FIT - is the clock FREE-RUNNING, and what is its period to
 * a tenth of a millisecond?
 *
 * The phase sweep established that retirement is periodic. Two things it does
 * not settle. (1) Whether the clock runs on its own or is armed per
 * submission - a sawtooth is consistent with either if the arming is itself
 * aligned to something. (2) The period to better than about half a
 * millisecond, which matters because the candidates are close together:
 * 25 panel vblanks is 173.76 ms, a 6 Hz timer is 166.67 ms, and a 5.75 Hz
 * timer would be a number no one would write down.
 *
 * So: submit with pseudo-random delays spread over more than two periods,
 * record the ABSOLUTE completion instant of each, and ask whether every one
 * of them lands on a single lattice k*T + phi. Scanning T and minimising the
 * circular spread of (t mod T) does both jobs at once - a free-running clock
 * gives one sharp minimum with a residual of microseconds over a run many
 * seconds long, and a per-submission arming gives no coherent T at all.
 *
 * The random delays are the point: a fixed delay would alias, and a serial
 * loop phase-locks, which is exactly how 2wq was misled. */
static int lattice_on(id<MTLDevice> dev, int samples) {
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

    double *t = malloc(samples * sizeof(double));
    if (!t) { printf("  FAIL: out of memory\n"); return 1; }
    printf("  lattice: %d completions at pseudo-random phase\n", samples);
    fflush(stdout);
    unsigned seed = 12345;
    for (int i = 0; i < samples; i++) {
        id<MTLCommandBuffer> cb = [q commandBuffer];
        id<MTLComputeCommandEncoder> enc = [cb computeCommandEncoder];
        [enc setComputePipelineState:ps];
        [enc setBuffer:buf offset:0 atIndex:0];
        [enc dispatchThreads:MTLSizeMake(N, 1, 1)
       threadsPerThreadgroup:MTLSizeMake(64, 1, 1)];
        [enc endEncoding];
        [cb commit];
        [cb waitUntilCompleted];
        /* 2ws: ABSOLUTE mach time, not relative. mach_absolute_time shares one
         * epoch across every process on the machine, so the fitted phase of
         * two concurrent runs can be compared directly - which is what
         * separates a system-wide kernel clock from a per-process one. */
        t[i] = now_s();                  /* the completion instant, absolute */
        if (cb.error) { printf("  sample %d ERROR\n", i); }
        /* 0..320 ms, deterministic so a rerun is comparable. */
        seed = seed * 1103515245u + 12345u;
        spin_until(now_s() + ((seed >> 16) % 3200) / 1e4);
    }
    double span = t[samples - 1] - t[0];

    /* Scan T. The figure of merit is the circular standard deviation of the
     * phases, which is scale-free and does not care where phi lands. */
    double bestT = 0, bestR = -1;
    for (double T = 0.150; T <= 0.200; T += 0.000002) {
        double cs = 0, sn = 0;
        for (int i = 0; i < samples; i++) {
            double ph = fmod(t[i], T) / T * 2.0 * M_PI;
            cs += cos(ph); sn += sin(ph);
        }
        double R = sqrt(cs * cs + sn * sn) / samples;   /* 1 = perfectly locked */
        if (R > bestR) { bestR = R; bestT = T; }
    }
    /* Residual spread in ms, from the concentration R. */
    double sigma = bestR > 0 && bestR < 1 ? sqrt(-2.0 * log(bestR)) * bestT / (2.0 * M_PI) : 0;
    printf("  span %.2f s over %d samples -> %.1f periods\n",
           span, samples, bestT > 0 ? span / bestT : 0);
    /* The lattice PHASE, in the shared mach epoch. */
    double cs = 0, sn = 0;
    for (int i = 0; i < samples; i++) {
        double ph = fmod(t[i], bestT) / bestT * 2.0 * M_PI;
        cs += cos(ph); sn += sin(ph);
    }
    double phi = atan2(sn, cs);
    if (phi < 0) { phi += 2.0 * M_PI; }
    printf("  BEST T = %.4f ms   lock R = %.4f   residual sigma = %.3f ms\n",
           bestT * 1e3, bestR, sigma * 1e3);
    printf("  LATTICE PHASE phi = %.3f ms of %.3f ms (shared mach epoch)\n",
           phi / (2.0 * M_PI) * bestT * 1e3, bestT * 1e3);
    printf("  a free-running clock gives R near 1 over that many periods;\n"
           "  per-submission arming gives no coherent T (R stays low).\n");
    /* The named candidates, scored against the same data. */
    struct { const char *name; double T; } cand[] = {
        { "25 x panel vblank (143.88 Hz)", 25.0 / 143.88 },
        { "25 x 144 Hz",                   25.0 / 144.0 },
        { "6 Hz",                          1.0 / 6.0 },
        { "5.75 Hz",                       1.0 / 5.75 },
        { "1/6 s + 4%",                    0.166667 * 1.04 },
    };
    printf("    %-32s %10s %8s\n", "candidate", "T(ms)", "lock R");
    for (unsigned k = 0; k < sizeof(cand) / sizeof(cand[0]); k++) {
        double cs = 0, sn = 0;
        for (int i = 0; i < samples; i++) {
            double ph = fmod(t[i], cand[k].T) / cand[k].T * 2.0 * M_PI;
            cs += cos(ph); sn += sin(ph);
        }
        printf("    %-32s %10.4f %8.4f\n", cand[k].name, cand[k].T * 1e3,
               sqrt(cs * cs + sn * sn) / samples);
    }

    uint32_t *p = (uint32_t *)buf.contents;
    int bad = 0;
    for (uint32_t i = 0; i < N; i++) { if (p[i] != i * 3u + 7u) { bad++; } }
    printf("  verify: %s (%d of %d wrong)\n", bad ? "FAIL" : "PASS", bad, N);
    free(t);
    return bad ? 1 : 0;
}

/* 2ws: --busy, the keep-the-GPU-working load generator.
 *
 * It exists for one question. The 176 ms is a tick that releases a whole
 * batch at once, with the GPU idle in between. Two mechanisms fit that and
 * they have opposite consequences:
 *
 *   an idle/attach cycle - the queue is only swapped onto the hardware
 *     periodically, so a client that keeps work in flight CONTINUOUSLY stays
 *     swapped in and should see its latency collapse. That would be a route
 *     to 144 Hz with no kernel change at all.
 *   a hard scheduling quantum - every release waits for the next tick no
 *     matter how busy the queue is, and the latency does not move.
 *
 * So run this in one process and --bench in another, and compare the second
 * one's median against the 176 ms it reads on an idle machine. */
static int busy_on(id<MTLDevice> dev, double secs) {
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
    printf("  busy: keeping work in flight for %.1f s\n", secs);
    fflush(stdout);
    double t0 = now_s();
    unsigned long long n = 0;
    /* A bounded window of outstanding buffers: enough that the queue is never
     * empty, few enough that this does not grow without limit. */
    while (now_s() - t0 < secs) {
        @autoreleasepool {
            id<MTLCommandBuffer> last = nil;
            for (int i = 0; i < 8; i++) {
                id<MTLCommandBuffer> cb = [q commandBuffer];
                id<MTLComputeCommandEncoder> enc = [cb computeCommandEncoder];
                [enc setComputePipelineState:ps];
                [enc setBuffer:buf offset:0 atIndex:0];
                [enc dispatchThreads:MTLSizeMake(N, 1, 1)
               threadsPerThreadgroup:MTLSizeMake(64, 1, 1)];
                [enc endEncoding];
                [cb commit];
                last = cb;
                n++;
            }
            [last waitUntilCompleted];
        }
    }
    double el = now_s() - t0;
    printf("  busy: %llu buffers in %.2f s -> %.1f/s\n", n, el, n / el);
    return 0;
}

static int bench_on(id<MTLDevice> dev, int iters, uint32_t heavy) {
    NSError *err = nil;
    id<MTLCommandQueue> q = [dev newCommandQueue];
    id<MTLLibrary> lib = [dev newLibraryWithSource:@(kSrc) options:nil error:&err];
    if (!q || !lib) { printf("  FAIL: no queue or library\n"); return 1; }
    id<MTLComputePipelineState> ps =
        [dev newComputePipelineStateWithFunction:
                 [lib newFunctionWithName:heavy ? @"spin" : @"fill"] error:&err];
    id<MTLBuffer> buf = [dev newBufferWithLength:N * sizeof(uint32_t)
                                         options:MTLResourceStorageModeShared];
    if (!ps || !buf) { printf("  FAIL: no pipeline or buffer\n"); return 1; }
    if (heavy) { printf("  bench: HEAVY kernel, %u loops per thread\n", heavy); }

    /* The dispatch is deliberately TINY - 4096 threads of three arithmetic
     * ops. Anything the chip takes longer than microseconds on is overhead,
     * which is the whole point: this measures latency, not throughput. */
    /* 2wr: the discriminator. The completion HANDLER and the WAIT are two
     * different ways to learn the same fact, so timing both from the same
     * commit separates "the notification is late" from "the wait primitive is
     * late". If the handler fires in microseconds and only waitUntilCompleted
     * returns at 172 ms, the defect is the wait; if both are late together,
     * it is the notification and the interrupt path is where to look. */
    double *hand = calloc(iters, sizeof(double));
    double *wall = calloc(iters, sizeof(double));
    double *gpu  = calloc(iters, sizeof(double));
    double *drv  = calloc(iters, sizeof(double));
    double *ksta = calloc(iters, sizeof(double));
    double *gsta = calloc(iters, sizeof(double));
    double *gend = calloc(iters, sizeof(double));
    if (!wall || !gpu || !drv || !ksta || !gsta || !gend) {
        printf("  FAIL: out of memory\n"); return 1;
    }

    printf("  bench: %d serial command buffers (commit, wait, repeat)\n", iters);
    fflush(stdout);
    for (int i = 0; i < iters; i++) {
        id<MTLCommandBuffer> cb = [q commandBuffer];
        id<MTLComputeCommandEncoder> enc = [cb computeCommandEncoder];
        [enc setComputePipelineState:ps];
        [enc setBuffer:buf offset:0 atIndex:0];
        if (heavy) { [enc setBytes:&heavy length:sizeof(heavy) atIndex:1]; }
        [enc dispatchThreads:MTLSizeMake(N, 1, 1)
       threadsPerThreadgroup:MTLSizeMake(64, 1, 1)];
        [enc endEncoding];
        __block double tHandler = 0;
        double t0 = now_s();
        [cb addCompletedHandler:^(id<MTLCommandBuffer> b) {
            (void)b;
            tHandler = now_s();
        }];
        [cb commit];
        [cb waitUntilCompleted];
        wall[i] = now_s() - t0;
        hand[i] = tHandler > 0 ? tHandler - t0 : -1.0;
        gpu[i]  = cb.GPUEndTime    - cb.GPUStartTime;
        drv[i]  = cb.kernelEndTime - cb.kernelStartTime;
        /* 2ws: the SPLIT. GPUStartTime, kernelStartTime and mach_absolute_time
         * share one host timebase, so subtracting the commit instant says
         * WHERE the latency sits - before the work starts or after it ends.
         * 2wr timed the completion handler against waitUntilCompleted and
         * found them 9 us apart, but BOTH of those are downstream of
         * submission, so that test could not see a submit delay at all. The
         * sample showed Metal submitting from a dispatch source, which is
         * per-process - matching a per-process clock phase - so this is the
         * measurement that decides it. */
        ksta[i] = cb.kernelStartTime - t0;
        gsta[i] = cb.GPUStartTime    - t0;
        gend[i] = cb.GPUEndTime      - t0;
        if (cb.error) {
            printf("  iter %d ERROR: %s\n", i, cb.error.description.UTF8String);
        }
        /* Flushed every iteration so a hang keeps what already ran. */
        printf("    %3d  wall %9.3f | kstart %9.3f  gstart %9.3f  gend %9.3f "
               "| gpu %7.3f  drv %7.3f  hand %9.3f\n",
               i, wall[i] * 1e3, ksta[i] * 1e3, gsta[i] * 1e3, gend[i] * 1e3,
               gpu[i] * 1e3, drv[i] * 1e3, hand[i] * 1e3);
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
    {
        double *sh = malloc(iters * sizeof(double));
        if (sh) {
            memcpy(sh, hand, iters * sizeof(double));
            qsort(sh, iters, sizeof(double), cmp_d);
            double medh = sh[iters / 2];
            printf("  HANDLER median: %.3f ms   (wait - handler = %.3f ms)\n",
                   medh * 1e3, (medw - medh) * 1e3);
            printf("  VERDICT: %s\n",
                   medh < 0 ? "no handler fired - inconclusive" :
                   (medh < medw / 10.0)
                       ? "handler is PROMPT, the WAIT is late -> wait primitive"
                       : "handler and wait are late TOGETHER -> notification");
            free(sh);
        }
    }
    if (medg > 0) {
        printf("  OVERHEAD RATIO: wall / gpu = %.1fx\n", medw / medg);
    }
    {
        /* Medians of the split, over iterations 1.. so the warm-up is out. */
        int n = iters > 1 ? iters - 1 : iters;
        int off = iters > 1 ? 1 : 0;
        double *a = malloc(n * sizeof(double));
        double *b = malloc(n * sizeof(double));
        if (a && b) {
            memcpy(a, gsta + off, n * sizeof(double));
            memcpy(b, gend + off, n * sizeof(double));
            qsort(a, n, sizeof(double), cmp_d);
            qsort(b, n, sizeof(double), cmp_d);
            double mgs = a[n / 2], mge = b[n / 2];
            printf("  SPLIT median: commit -> GPUstart %.3f ms, GPUend -> "
                   "return %.3f ms\n", mgs * 1e3, (medw - mge) * 1e3);
            printf("  SPLIT VERDICT: %s\n",
                   mgs > medw / 2.0
                       ? "the wait is BEFORE the work -> SUBMISSION is late"
                       : ((medw - mge) > medw / 2.0
                          ? "the wait is AFTER the work -> COMPLETION is late"
                          : "neither half dominates - read the table"));
        }
        free(a); free(b);
    }

    /* Pipelined: commit all of them, wait only for the last. A per-buffer
     * notification cost disappears here; a bandwidth ceiling does not. */
    printf("  bench: %d pipelined command buffers (commit all, wait for last)\n",
           iters);
    fflush(stdout);
    /* 2ws: keep every buffer so its GPUStartTime can be read afterwards. If
     * all of them start at the same instant, one periodic kick released the
     * whole batch and the 176 ms is a TICK. If they start staggered by their
     * own execution times, the GPU was running continuously and the 176 ms is
     * a one-off WAKE-UP. The serial test cannot tell these apart because it
     * only ever has one buffer outstanding. */
    NSMutableArray *all = [NSMutableArray arrayWithCapacity:(NSUInteger)iters];
    id<MTLCommandBuffer> last = nil;
    double p0 = now_s();
    for (int i = 0; i < iters; i++) {
        id<MTLCommandBuffer> cb = [q commandBuffer];
        id<MTLComputeCommandEncoder> enc = [cb computeCommandEncoder];
        [enc setComputePipelineState:ps];
        [enc setBuffer:buf offset:0 atIndex:0];
        if (heavy) { [enc setBytes:&heavy length:sizeof(heavy) atIndex:1]; }
        [enc dispatchThreads:MTLSizeMake(N, 1, 1)
       threadsPerThreadgroup:MTLSizeMake(64, 1, 1)];
        [enc endEncoding];
        [cb commit];
        [all addObject:cb];
        last = cb;
    }
    [last waitUntilCompleted];
    double ptot = now_s() - p0;
    {
        int shown = iters < 16 ? iters : 16;
        printf("    pipelined GPUstart/GPUend relative to first commit (ms):\n");
        for (int i = 0; i < shown; i++) {
            id<MTLCommandBuffer> cb = all[(NSUInteger)i];
            printf("      %3d  gstart %9.3f  gend %9.3f\n",
                   i, (cb.GPUStartTime - p0) * 1e3, (cb.GPUEndTime - p0) * 1e3);
        }
        double g0 = ((id<MTLCommandBuffer>)all[0]).GPUStartTime;
        double gl = ((id<MTLCommandBuffer>)all[(NSUInteger)(iters - 1)]).GPUStartTime;
        printf("    first->last GPUstart spread = %.3f ms over %d buffers\n",
               (gl - g0) * 1e3, iters);
    }
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
    free(hand); free(wall); free(gpu); free(drv); free(sw); free(sg);
    free(ksta); free(gsta); free(gend);
    return bad ? 1 : 0;
}

int main(int argc, const char **argv) {
    @autoreleasepool {
        bool wantDispatch = false;
        bool wantBench = false;
        bool wantPhase = false;
        bool wantLattice = false;
        bool wantBusy = false;
        double busySecs = 20;
        uint32_t heavy = 0;
        int samples = 120;
        int iters = 20;
        int reps = 7;
        for (int i = 1; i < argc; i++) {
            if (!strcmp(argv[i], "--dispatch")) { wantDispatch = true; }
            else if (!strcmp(argv[i], "--heavy")) {
                if (i + 1 < argc && argv[i + 1][0] >= '0' && argv[i + 1][0] <= '9') {
                    long n = atol(argv[++i]);
                    if (n >= 1 && n <= 10000000) { heavy = (uint32_t)n; }
                    else { printf("--heavy N must be 1..10000000\n"); }
                }
            }
            else if (!strcmp(argv[i], "--busy")) {
                wantBusy = true;
                if (i + 1 < argc && argv[i + 1][0] >= '0' && argv[i + 1][0] <= '9') {
                    int n = atoi(argv[++i]);
                    if (n >= 1 && n <= 120) { busySecs = n; }
                    else { printf("--busy N must be 1..120 s; using %.0f\n", busySecs); }
                }
            }
            else if (!strcmp(argv[i], "--lattice")) {
                wantLattice = true;
                if (i + 1 < argc && argv[i + 1][0] >= '0' && argv[i + 1][0] <= '9') {
                    int n = atoi(argv[++i]);
                    if (n >= 8 && n <= 2000) { samples = n; }
                    else { printf("--lattice N must be 8..2000; using %d\n", samples); }
                }
            }
            else if (!strcmp(argv[i], "--phase")) {
                wantPhase = true;
                if (i + 1 < argc && argv[i + 1][0] >= '0' && argv[i + 1][0] <= '9') {
                    int n = atoi(argv[++i]);
                    if (n >= 1 && n <= 100) { reps = n; }
                    else { printf("--phase N must be 1..100; using %d\n", reps); }
                }
            }
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
            if (wantBench)    { rc |= bench_on(d, iters, heavy); }
            if (wantPhase)    { rc |= phase_on(d, reps); }
            if (wantLattice)  { rc |= lattice_on(d, samples); }
            if (wantBusy)     { rc |= busy_on(d, busySecs); }
        }
        if (!wantDispatch && !wantBench) {
            printf("\n(enumerate only; --dispatch runs a kernel, "
                   "--bench [N] times one, --phase [N] sweeps the\n"
                   "    retirement phase)\n");
        }
        return rc;
    }
}
