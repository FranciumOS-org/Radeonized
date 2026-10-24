// skyshaders (2qb): can this GPU build what CompositorMetal::new_compositor
// needs?  If it cannot, SkyLight composites the desktop on the CPU.
//
// 2qa closed every other branch of the compositor decision and left exactly
// one: `CompositorMetal::new_compositor(accelId)` (SkyLight, unslid
// 0x7ff807f0045e) returns NULL unless all of
// ctx->0x90 / 0xa8->vt[0x10]() / 0x110 / 0x118 / 0x120 / 0x128 are non-NULL,
// and then `WSGetCompositorMetal` returns NULL rather than the SW compositor.
// The fail path has NO log string, and boot 149's 922 SkyLight log lines
// contain no Metal failure - so the thing that breaks does not report.
//
// The selectors that function uses were decoded from its own
// RIP-relative slots (slide-info v2 pointers -> Obj-C selrefs), and the
// CFStrings with them:
//
//   mainBundle / pathForResource:ofType: / fileURLWithPath:isDirectory:
//   newLibraryWithURL:error:          @"SkyLightShaders.air64" @"metallib"
//   newSamplerStateWithDescriptor:    setNormalizedCoordinates:/Min/Mag/Mip
//   newDepthStencilStateWithDescriptor:
//   newSharedEvent / setLabel:        @"SkyLight Renderer"
//   setSubmissionQueue: / setCompletionQueue: / setGPUPriority:
//   supportPriorityBand / compilerPropagatesThreadPriority:
//   maxTextureWidth2D / maxTextureHeight2D / recommendedMaxWorkingSetSize
//   acceleratorPort / registryID / isRemovable / peerGroupID / name
//   rangeOfString:                   @"Intel" @"NVIDIA"
//
// So this tool rebuilds each of those with PUBLIC Metal APIs on this machine's
// own device.  Nothing internal is called and no address is hand-computed:
// 2qa established that none of the SkyLight compositor symbols are exported,
// and calling them at `unslid + slide` with a hand-built ABI would prove
// less than this does.
//
// WHAT THIS CAN AND CANNOT SHOW (state it before running, not after):
//   - a FAILURE here is strong: these are the same public calls on the same
//     device, and Metal hands back an NSError that SkyLight discards.
//   - a SUCCESS here does NOT prove WindowServer succeeds.  WindowServer is a
//     different process with different entitlements and ordering, and 2pz was
//     burned once already by measuring a client process and reading it as
//     WindowServer's answer.
//
// Exit: 0 everything built, 1 something failed (the answer), 3 inconclusive.

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <IOKit/IOKitLib.h>

static int g_fail = 0;

static void ok(const char *what, id obj, NSError *err) {
    if (obj) { printf("  OK    %-34s %s\n", what, "built"); return; }
    g_fail++;
    printf("  FAIL  %-34s %s\n", what,
           err ? [[err localizedDescription] UTF8String] : "nil, no NSError");
    if (err && [err userInfo].count) {
        for (NSString *k in [err userInfo]) {
            NSString *v = [[[err userInfo] objectForKey:k] description];
            // the Metal compiler log lands here; it is the whole point
            printf("          %s = %s\n", [k UTF8String],
                   [v length] > 900 ? [[v substringToIndex:900] UTF8String]
                                    : [v UTF8String]);
        }
    }
}

// Build a vertex descriptor from the function's OWN declared attributes.
// Without this, newRenderPipelineStateWithDescriptor: fails with "Vertex
// function has input attributes but no vertex descriptor was set" - which is
// the TOOL's error, not the GPU's, and the first version of this section
// reported it as a failure to link (rule 16: the instrument is a suspect).
static MTLVertexDescriptor *vdesc_for(id<MTLFunction> v) {
    if (v.vertexAttributes.count == 0) return nil;
    MTLVertexDescriptor *vd = [MTLVertexDescriptor new];
    NSUInteger off = 0;
    for (MTLVertexAttribute *a in v.vertexAttributes) {
        if (!a.active) continue;
        MTLVertexFormat f; NSUInteger sz;
        switch (a.attributeType) {
            case MTLDataTypeFloat:  f = MTLVertexFormatFloat;  sz = 4;  break;
            case MTLDataTypeFloat2: f = MTLVertexFormatFloat2; sz = 8;  break;
            case MTLDataTypeFloat3: f = MTLVertexFormatFloat3; sz = 12; break;
            case MTLDataTypeFloat4: f = MTLVertexFormatFloat4; sz = 16; break;
            case MTLDataTypeInt:
            case MTLDataTypeUInt:   f = MTLVertexFormatUInt;   sz = 4;  break;
            case MTLDataTypeInt2:
            case MTLDataTypeUInt2:  f = MTLVertexFormatUInt2;  sz = 8;  break;
            case MTLDataTypeHalf2:  f = MTLVertexFormatHalf2;  sz = 4;  break;
            case MTLDataTypeHalf4:  f = MTLVertexFormatHalf4;  sz = 8;  break;
            default:                f = MTLVertexFormatFloat4; sz = 16; break;
        }
        vd.attributes[a.attributeIndex].format = f;
        vd.attributes[a.attributeIndex].offset = off;
        vd.attributes[a.attributeIndex].bufferIndex = 0;
        off += sz;
    }
    if (off == 0) return nil;
    vd.layouts[0].stride = off;
    vd.layouts[0].stepFunction = MTLVertexStepFunctionPerVertex;
    return vd;
}

// Metal ABORTS (__assert_rtn inside validateWithDevice) on a descriptor it
// considers malformed, and logs the reason as '<private>'. That kills the
// whole process, so a render-pipeline probe must not share a process with the
// rest of the tool: `--pair <vertex> <fragment> <bgra|rgba16f>` tests exactly
// one pair and exits, and the caller reads the exit status. 0 built, 1 refused
// with an NSError, 2 Metal aborted (which is a statement about the DESCRIPTOR,
// not about this GPU), 3 a name was absent.
static int pair_mode(const char *vn, const char *fn_, const char *fmt) {
    NSArray<id<MTLDevice>> *devs = MTLCopyAllDevices();
    if (devs.count == 0) { printf("no device\n"); return 3; }
    id<MTLDevice> dev = devs[0];
    NSError *err = nil;
    id<MTLLibrary> lib = [dev newLibraryWithURL:[NSURL fileURLWithPath:
        @"/System/Library/PrivateFrameworks/SkyLight.framework/Resources/"
         "SkyLightShaders.air64.metallib" isDirectory:NO] error:&err];
    if (!lib) { printf("no library\n"); return 3; }
    id<MTLFunction> v  = [lib newFunctionWithName:
        [NSString stringWithUTF8String:vn]];
    id<MTLFunction> fr = [lib newFunctionWithName:
        [NSString stringWithUTF8String:fn_]];
    if (!v || !fr) { printf("absent\n"); return 3; }
    MTLRenderPipelineDescriptor *rd = [MTLRenderPipelineDescriptor new];
    rd.vertexFunction = v;
    rd.fragmentFunction = fr;
    MTLPixelFormat pf = strcmp(fmt, "rgba16f") == 0
        ? MTLPixelFormatRGBA16Float : MTLPixelFormatBGRA8Unorm;
    // a fragment shader that writes several targets needs them all declared;
    // only attachment 0 was set before, which is one way to earn that assert
    for (int i = 0; i < 8; i++) rd.colorAttachments[i].pixelFormat = pf;
    rd.vertexDescriptor = vdesc_for(v);
    NSError *e = nil;
    id ps = [dev newRenderPipelineStateWithDescriptor:rd error:&e];
    if (ps) { printf("built\n"); return 0; }
    printf("refused: %s\n", e ? [[e localizedDescription] UTF8String]
                               : "nil, no NSError");
    return 1;
}

int main(int argc, char **argv) {
    if (argc == 5 && strcmp(argv[1], "--pair") == 0) {
        @autoreleasepool { return pair_mode(argv[2], argv[3], argv[4]); }
    }
    @autoreleasepool {
        printf("== preconditions (rule 26: a search is an instrument too)\n");
        io_iterator_t it = 0;
        unsigned nacc = 0;
        if (IOServiceGetMatchingServices(kIOMainPortDefault,
                IOServiceMatching("IOAccelerator"), &it) == KERN_SUCCESS) {
            io_object_t o;
            while ((o = IOIteratorNext(it))) { nacc++; IOObjectRelease(o); }
            IOObjectRelease(it);
        }
        printf("IOAccelerator services         : %u\n", nacc);
        if (nacc == 0) {
            printf("INCONCLUSIVE: no accelerator in this boot, so a Metal "
                   "failure here would say nothing about compositing.\n");
            return 3;
        }

        NSArray<id<MTLDevice>> *devs = MTLCopyAllDevices();
        printf("MTLCopyAllDevices()            : %lu\n",
               (unsigned long)devs.count);
        if (devs.count == 0) {
            printf("INCONCLUSIVE: no Metal device.\n");
            return 3;
        }
        id<MTLDevice> dev = devs[0];
        printf("device                         : %s\n", [dev.name UTF8String]);
        printf("  lowPower=%d removable=%d headless=%d\n",
               (int)dev.isLowPower, (int)dev.isRemovable, (int)dev.isHeadless);
        // new_compositor does rangeOfString: on the name for these two
        printf("  name contains Intel=%d NVIDIA=%d  (new_compositor's own "
               "special cases; ours is neither)\n",
               [dev.name rangeOfString:@"Intel"].location  != NSNotFound,
               [dev.name rangeOfString:@"NVIDIA"].location != NSNotFound);
        printf("  recommendedMaxWorkingSetSize : %llu MB\n",
               dev.recommendedMaxWorkingSetSize >> 20);
        printf("  registryID                   : 0x%llx\n", dev.registryID);

        printf("\n== 1. the shader library new_compositor loads\n");
        NSBundle *b = [NSBundle bundleWithPath:
            @"/System/Library/PrivateFrameworks/SkyLight.framework"];
        NSString *p = [b pathForResource:@"SkyLightShaders.air64"
                                  ofType:@"metallib"];
        if (!p) {   // rule 14: say something on every path
            p = @"/System/Library/PrivateFrameworks/SkyLight.framework"
                 "/Resources/SkyLightShaders.air64.metallib";
            printf("  (pathForResource: failed; using the literal path)\n");
        }
        printf("  %s\n", [p UTF8String]);
        NSError *err = nil;
        id<MTLLibrary> lib = [dev newLibraryWithURL:
            [NSURL fileURLWithPath:p isDirectory:NO] error:&err];
        ok("newLibraryWithURL:error:", lib, err);
        if (lib)
            printf("  OK    %-34s %lu functions\n", "library contents",
                   (unsigned long)[lib functionNames].count);

        printf("\n== 2. the context objects it then builds\n");
        MTLSamplerDescriptor *sd = [MTLSamplerDescriptor new];
        sd.normalizedCoordinates = YES;
        sd.minFilter = MTLSamplerMinMagFilterLinear;
        sd.magFilter = MTLSamplerMinMagFilterLinear;
        sd.mipFilter = MTLSamplerMipFilterLinear;
        sd.maxAnisotropy = 1;
        ok("newSamplerStateWithDescriptor:",
           [dev newSamplerStateWithDescriptor:sd], nil);

        MTLDepthStencilDescriptor *dd = [MTLDepthStencilDescriptor new];
        ok("newDepthStencilStateWithDescriptor:",
           [dev newDepthStencilStateWithDescriptor:dd], nil);

        ok("newSharedEvent", [dev newSharedEvent], nil);

        id<MTLCommandQueue> q = [dev newCommandQueue];
        ok("newCommandQueue", q, nil);
        if (q) { q.label = @"SkyLight Renderer"; }

        printf("\n== 3. the library's functions, by type\n");
        NSMutableArray *verts = [NSMutableArray new];
        NSMutableArray *frags = [NSMutableArray new];
        NSMutableArray *kerns = [NSMutableArray new];
        if (lib) {
            for (NSString *n in [[lib functionNames]
                    sortedArrayUsingSelector:@selector(compare:)]) {
                id<MTLFunction> fn = [lib newFunctionWithName:n];
                if (!fn) { printf("  (no function object for %s)\n",
                                  [n UTF8String]); continue; }
                const char *t = "other";
                if (fn.functionType == MTLFunctionTypeVertex) {
                    t = "vertex";   [verts addObject:fn];
                } else if (fn.functionType == MTLFunctionTypeFragment) {
                    t = "fragment"; [frags addObject:fn];
                } else if (fn.functionType == MTLFunctionTypeKernel) {
                    t = "kernel";   [kerns addObject:fn];
                }
                printf("  %-9s %s\n", t, [n UTF8String]);
            }
            printf("  -> %lu vertex, %lu fragment, %lu kernel\n",
                   (unsigned long)verts.count, (unsigned long)frags.count,
                   (unsigned long)kerns.count);
        }

        printf("\n== 4. COMPUTE pipelines (the backend compiler for kernels)\n");
        if (!lib) { printf("  SKIPPED: no library.  (Not a pass.)\n"); }
        else if (kerns.count == 0) { printf("  none in this library\n"); }
        else {
            int built = 0;
            for (id<MTLFunction> fn in kerns) {
                NSError *e2 = nil;
                if ([dev newComputePipelineStateWithFunction:fn error:&e2]) built++;
                else {
                    g_fail++;
                    printf("  FAIL  %-28s %s\n", [fn.name UTF8String],
                           e2 ? [[e2 localizedDescription] UTF8String] : "nil");
                }
            }
            printf("  %d built of %lu\n", built, (unsigned long)kerns.count);
        }

        printf("\n== 5. RENDER pipelines: NOT TESTED HERE, and that is deliberate\n");
        printf("  `new_compositor` builds no render pipeline in the code 2qa\n"
               "  read - its six null checks are the library, the sampler\n"
               "  state, the depth-stencil state, the shared event and the\n"
               "  queues, all of which are section 1-2 above. Render\n"
               "  pipelines are built later, per shader, by whoever draws.\n"
               "  Metal also ABORTS on a descriptor it dislikes, so a probe\n"
               "  cannot share this process: use\n"
               "    tools/skyshaders_pairs.sh\n"
               "  which runs `skyshaders --pair V F fmt` once per pair.\n");

        printf("\n== 6. the PRIVATE device/queue APIs new_compositor uses\n");
        // Decoded from its own selrefs: supportPriorityBand, peerGroupID,
        // maxTextureWidth2D, maxTextureHeight2D, acceleratorPort,
        // compilerPropagatesThreadPriority:, and on a queue descriptor
        // setSubmissionQueue:/setCompletionQueue:/setGPUPriority:.
        // These are the parts a userspace oracle CAN still reach, and the
        // ones that do not depend on shader compilation - which section 1-2
        // just showed is not the problem.
        const char *sels[] = { "supportPriorityBand", "peerGroupID",
                               "maxTextureWidth2D", "maxTextureHeight2D",
                               "acceleratorPort", "registryID" };
        for (unsigned i = 0; i < sizeof(sels)/sizeof(sels[0]); i++) {
            SEL sel = sel_getUid(sels[i]);
            if (![dev respondsToSelector:sel]) {
                printf("  ABSENT %-34s device does not respond\n", sels[i]);
                continue;
            }
            NSMethodSignature *sig =
                [(id)dev methodSignatureForSelector:sel];
            if (!sig || sig.numberOfArguments != 2) {
                printf("  ?      %-34s unexpected signature\n", sels[i]);
                continue;
            }
            NSInvocation *inv = [NSInvocation invocationWithMethodSignature:sig];
            inv.target = dev; inv.selector = sel;
            @try { [inv invoke]; }
            @catch (NSException *ex) {
                printf("  THREW  %-34s %s\n", sels[i],
                       [[ex reason] UTF8String]);
                continue;
            }
            const char *rt = sig.methodReturnType;
            unsigned long long u = 0;
            if (rt[0] == 'B' || rt[0] == 'c') {
                char b = 0; [inv getReturnValue:&b]; u = (unsigned char)b;
                printf("  %-6s %-34s %s\n", u ? "YES" : "NO", sels[i],
                       u ? "" : "<-- a NO here is a real candidate");
            } else {
                [inv getReturnValue:&u];
                printf("  value  %-34s %llu (0x%llx)\n", sels[i], u, u);
            }
        }

        printf("\n  the priority-band queue new_compositor configures:\n");
        MTLCommandQueueDescriptor *qd = [MTLCommandQueueDescriptor new];
        int qfail = 0;
        const char *qsels[] = { "setSubmissionQueue:", "setCompletionQueue:",
                                "setGPUPriority:" };
        for (unsigned qi = 0; qi < sizeof(qsels)/sizeof(qsels[0]); qi++) {
            const char *qs = qsels[qi];
            SEL sel = sel_getUid(qs);
            printf("    %-26s descriptor responds: %s\n", qs,
                   [qd respondsToSelector:sel] ? "yes" : "NO");
            if (![qd respondsToSelector:sel]) qfail++;
        }
        id<MTLCommandQueue> q2 = nil;
        @try { q2 = [dev newCommandQueueWithDescriptor:qd]; }
        @catch (NSException *ex) {
            printf("    newCommandQueueWithDescriptor: THREW %s\n",
                   [[ex reason] UTF8String]);
        }
        printf("    newCommandQueueWithDescriptor: %s\n",
               q2 ? "built" : "NIL <-- candidate");
        if (!q2) g_fail++;

        printf("\n== verdict\n");
        if (g_fail == 0) {
            printf("Everything new_compositor needs BUILDS in this process.\n"
                   "That does NOT clear it: WindowServer is a different "
                   "process and\nthis tool cannot see its ordering or "
                   "entitlements.  It does mean the\nfailure is not a plain "
                   "'these shaders will not compile on gfx1035'.\n");
            return 0;
        }
        printf("%d of the objects new_compositor requires FAILED to build on "
               "this\ndevice.  That is the silent NULL - SkyLight discards "
               "exactly this error.\n", g_fail);
        return 1;
    }
}
