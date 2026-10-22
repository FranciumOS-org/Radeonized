// iosurftex - back the textures Apple's driver lays out wrongly with IOSurfaces.
//
// 2mn established the law: a CPU replace() into a driver-allocated texture is
// corrupted iff the surface is in a 64 KB tiled mode (align 65536) AND its
// footprint exceeds its pixel bytes. 2mo established that the identical
// replace(), at the identical geometry and storage mode, is clean 0/12 when the
// texture is backed by an IOSurface instead - because an IOSurface's layout is
// agreed between the userspace driver and the kernel by construction, while the
// driver's own padded 64 KB layout is not.
//
// Apple's Metal driver cannot be patched on macOS 15 (2lq: the bundle on disk is
// a stub, the code lives only in the dyld shared cache), and no kernel lever
// reaches client texture layout (2mo). So this interposes in the client process
// instead: MTLCreateSystemDefaultDevice is wrapped, the returned device's class
// has -newTextureWithDescriptor: swizzled, and any descriptor the law predicts
// will fail is served from an IOSurface. Everything else falls through
// untouched, and any failure at all falls through untouched.
//
//   clang -dynamiclib -O2 -fno-objc-arc -o build/out/iosurftex.dylib \
//         tools/iosurftex.m -framework Metal -framework IOSurface -framework Foundation
//
// ARC MUST BE OFF (2mr). Metal declares MTLCreateSystemDefaultDevice and
// MTLCopyAllDevices NS_RETURNS_RETAINED, and -newTextureWithDescriptor: is in
// the `new` family, so all three hand the caller a +1 reference. Built with
// -fobjc-arc, ARC inserted a release on the way out of each replacement while
// the caller still owned its reference, and every application that enumerates
// Metal devices died in objc_release at the next autorelease-pool pop - the assistant,
// Terminal and Mail alike, whether the interposer was enabled or not. An
// interposer must pass ownership straight through and touch no retain count.
// tools/ownership-test.m is the regression test.
//   DYLD_INSERT_LIBRARIES=build/out/iosurftex.dylib <program>
//
// RBGPU_IOSURFTEX=0    off
// RBGPU_IOSURFTEX_LOG=1  one line per decision, to stderr

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <IOSurface/IOSurface.h>
#import <objc/runtime.h>
#import <stdatomic.h>
#import <os/lock.h>

static int gLog = 0, gAnyUsage = 0;
// RBGPU_IOSURFTEX=0 is authoritative off - it used to be overridden by having
// logging on, which made "off" not off. RBGPU_IOSURFTEX_INERT=1 loads and
// interposes but never swizzles, to separate "loading this dylib breaks the
// app" from "swizzling the device breaks the app".
static int gEnabled = 1, gInert = 0, gTrace = 0;
static IMP gOrigNewTextureIOSurface;
// An app launched by launchd has no terminal, so stderr goes nowhere useful.
// RBGPU_IOSURFTEX_LOGFILE names a file every process appends to instead.
static FILE *gLogFile;
static atomic_long gSwapped, gFellThrough;
// Why a descriptor the law flagged was declined - so that a run which fixes
// nothing still says what the application actually asked for.
static atomic_long gDeclUsage, gDeclFormat, gDeclStruct, gLawClean;
static IMP gOrigNewTexture;

// 2mu: the decisive question. Our law is about a CPU upload - replaceRegion -
// into a driver-allocated tiled texture. the assistant's glyph atlas IS in the failing
// class and IS being swapped, and its text did not change, so either the atlas
// is not filled by CPU at all (a GPU render pass or blit, both of which were
// correct in every test) or the text comes from elsewhere. Count the CPU
// uploads and say which textures get them.
static atomic_long gReplaceCalls;
static IMP gOrigReplace;

// 2mv: an inventory of every texture the application creates, deduplicated by
// descriptor, so a run says what the app actually asks for instead of only what
// we chose to act on. Swapping ONE 1024x128 atlas changed nothing on screen,
// and a ten-second sample of startup is not evidence about the page that backs
// the text being read.
#define TRACE_MAX 512
struct traceRow {
    unsigned long w, h, fmt, usage, type, mips, storage, align;
    double ratio;
    int iosurf;          // created over an IOSurface by the app itself
    int flagged;         // the law predicts corruption
    int swapped;         // we served it
    unsigned long count;
};
static struct traceRow gTraceRows[TRACE_MAX];
static int gTraceN;
static os_unfair_lock gTraceLock = OS_UNFAIR_LOCK_INIT;
static void traceRecord(id<MTLDevice> dev, MTLTextureDescriptor *d,
                        int iosurf, int flagged, int swapped);

#define LOG(fmt, ...) do { \
    if (gLog) { \
        FILE *f = gLogFile ? gLogFile : stderr; \
        fprintf(f, "iosurftex: " fmt "\n", ##__VA_ARGS__); \
        fflush(f); \
    } \
} while (0)

// Only formats whose IOSurface equivalent is unambiguous. Anything else falls
// through: a wrong bytesPerElement would corrupt worse than the bug does.
static bool fourccFor(MTLPixelFormat f, uint32_t *cc, size_t *bpe)
{
    switch (f) {
    case MTLPixelFormatBGRA8Unorm:
    case MTLPixelFormatBGRA8Unorm_sRGB: *cc = 'BGRA'; *bpe = 4; return true;
    case MTLPixelFormatRGBA8Unorm:
    case MTLPixelFormatRGBA8Unorm_sRGB: *cc = 'RGBA'; *bpe = 4; return true;
    case MTLPixelFormatR8Unorm:         *cc = 'L008'; *bpe = 1; return true;
    default: return false;
    }
}

// The 2mn law, asked of the driver itself rather than reimplemented.
static bool lawPredictsFailure(id<MTLDevice> dev, MTLTextureDescriptor *d, size_t bpe)
{
    MTLSizeAndAlign sa = [dev heapTextureSizeAndAlignWithDescriptor:d];
    size_t pixels = d.width * d.height * bpe;
    return sa.align == 65536 && sa.size > pixels;
}

static bool structureOk(MTLTextureDescriptor *d)
{
    return d.textureType == MTLTextureType2D
        && d.mipmapLevelCount == 1
        && d.arrayLength == 1
        && d.sampleCount == 1
        && d.depth == 1;
}

// Sampled and render-target textures are served; an IOSurface backs both, and
// the assistant's glyph atlas is RenderTarget|ShaderRead (2ms). ShaderWrite,
// PixelFormatView and ShaderAtomic are not served: they carry layout
// constraints of their own and none of them is a glyph atlas.
// RBGPU_IOSURFTEX_ANYUSAGE=1 lifts the restriction entirely.
#define IOSURFTEX_OK_USAGE (MTLTextureUsageShaderRead | MTLTextureUsageRenderTarget)
static bool usageOk(MTLTextureDescriptor *d)
{
    return gAnyUsage || (d.usage & ~(MTLTextureUsage)IOSURFTEX_OK_USAGE) == 0;
}

typedef void (*ReplaceIMP)(id, SEL, MTLRegion, NSUInteger, NSUInteger,
                           const void *, NSUInteger, NSUInteger);

static void swizzledReplace(id self, SEL _cmd, MTLRegion region, NSUInteger level,
                            NSUInteger slice, const void *bytes, NSUInteger bpr,
                            NSUInteger bpi)
{
    long n = atomic_fetch_add(&gReplaceCalls, 1) + 1;
    if (gLog) {
        id<MTLTexture> t = (id<MTLTexture>)self;
        LOG("CPU UPLOAD #%ld into %lux%lu fmt=%lu  region %lux%lu at (%lu,%lu)",
            n, (unsigned long)t.width, (unsigned long)t.height,
            (unsigned long)t.pixelFormat,
            (unsigned long)region.size.width, (unsigned long)region.size.height,
            (unsigned long)region.origin.x, (unsigned long)region.origin.y);
    }
    ((ReplaceIMP)gOrigReplace)(self, _cmd, region, level, slice, bytes, bpr, bpi);
}

// The 4-argument selector is a SEPARATE implementation, not a forwarder: with
// only the 6-argument one swizzled, lawtest's 192 uploads were counted as zero.
typedef void (*Replace4IMP)(id, SEL, MTLRegion, NSUInteger, const void *, NSUInteger);
static IMP gOrigReplace4;

static void swizzledReplace4(id self, SEL _cmd, MTLRegion region, NSUInteger level,
                             const void *bytes, NSUInteger bpr)
{
    long n = atomic_fetch_add(&gReplaceCalls, 1) + 1;
    if (gLog) {
        id<MTLTexture> t = (id<MTLTexture>)self;
        LOG("CPU UPLOAD #%ld into %lux%lu fmt=%lu  region %lux%lu at (%lu,%lu)",
            n, (unsigned long)t.width, (unsigned long)t.height,
            (unsigned long)t.pixelFormat,
            (unsigned long)region.size.width, (unsigned long)region.size.height,
            (unsigned long)region.origin.x, (unsigned long)region.origin.y);
    }
    ((Replace4IMP)gOrigReplace4)(self, _cmd, region, level, bytes, bpr);
}

static void installReplaceCounter(void)
{
    Class tc = objc_getClass("GFXAAMD_MtlTexture");
    if (!tc) { LOG("no GFXAAMD_MtlTexture class - cannot count CPU uploads"); return; }

    SEL s6 = @selector(replaceRegion:mipmapLevel:slice:withBytes:bytesPerRow:bytesPerImage:);
    Method m6 = class_getInstanceMethod(tc, s6);
    if (m6) {
        gOrigReplace = method_getImplementation(m6);
        method_setImplementation(m6, (IMP)swizzledReplace);
    }
    SEL s4 = @selector(replaceRegion:mipmapLevel:withBytes:bytesPerRow:);
    Method m4 = class_getInstanceMethod(tc, s4);
    if (m4) {
        gOrigReplace4 = method_getImplementation(m4);
        method_setImplementation(m4, (IMP)swizzledReplace4);
    }
    LOG("counting CPU uploads on GFXAAMD_MtlTexture (6-arg %s, 4-arg %s)",
        m6 ? "yes" : "no", m4 ? "yes" : "no");
}

static id swizzledNewTexture(id self, SEL _cmd, MTLTextureDescriptor *desc)
{
    id (*orig)(id, SEL, MTLTextureDescriptor *) = (void *)gOrigNewTexture;
    uint32_t cc; size_t bpe;
    int flagged = 0;

    if (!desc) goto fallthrough;

    // The law is evaluated first, with a nominal 4 bytes per element when the
    // format is one we do not serve, so that declines can be counted against
    // the descriptors that would actually have been corrupted.
    {
        bool known = fourccFor(desc.pixelFormat, &cc, &bpe);
        if (!lawPredictsFailure((id<MTLDevice>)self, desc, known ? bpe : 4)) {
            atomic_fetch_add(&gLawClean, 1);
            goto fallthrough;
        }
        flagged = 1;
        if (!structureOk(desc)) {
            atomic_fetch_add(&gDeclStruct, 1);
            if (gLog >= 2) LOG("DECLINED(structure) %lux%lu type=%lu mips=%lu",
                (unsigned long)desc.width, (unsigned long)desc.height,
                (unsigned long)desc.textureType, (unsigned long)desc.mipmapLevelCount);
            goto fallthrough;
        }
        if (!usageOk(desc)) {
            atomic_fetch_add(&gDeclUsage, 1);
            if (gLog >= 2) LOG("DECLINED(usage) %lux%lu usage=0x%lx fmt=%lu",
                (unsigned long)desc.width, (unsigned long)desc.height,
                (unsigned long)desc.usage, (unsigned long)desc.pixelFormat);
            goto fallthrough;
        }
        if (!known) {
            atomic_fetch_add(&gDeclFormat, 1);
            if (gLog >= 2) LOG("DECLINED(format) %lux%lu fmt=%lu usage=0x%lx",
                (unsigned long)desc.width, (unsigned long)desc.height,
                (unsigned long)desc.pixelFormat, (unsigned long)desc.usage);
            goto fallthrough;
        }
    }

    {
        // bytesPerRow must be set explicitly. Left to IOSurface's own default a
        // 1000-wide surface hung the probe during 2mo, so it is pinned to the
        // 256-byte alignment the blit engine wants.
        size_t bpr = (desc.width * bpe + 255) & ~(size_t)255;
        IOSurfaceRef surf = NULL;
        // The literals are autoreleased, and this can run on a thread with no
        // pool of its own, so it gets one. The texture is created after it
        // drains, since it is +1 and must not be autoreleased here.
        @autoreleasepool {
            surf = IOSurfaceCreate((CFDictionaryRef)@{
                (NSString *)kIOSurfaceWidth:           @(desc.width),
                (NSString *)kIOSurfaceHeight:          @(desc.height),
                (NSString *)kIOSurfaceBytesPerElement: @(bpe),
                (NSString *)kIOSurfaceBytesPerRow:     @(bpr),
                (NSString *)kIOSurfacePixelFormat:     @(cc),
            });
        }
        if (!surf) { LOG("IOSurfaceCreate failed for %lux%lu", (unsigned long)desc.width,
                         (unsigned long)desc.height); goto fallthrough; }

        // `new` family: +1, returned to the caller as its own reference.
        id tex = [(id<MTLDevice>)self newTextureWithDescriptor:desc iosurface:surf plane:0];
        CFRelease(surf);                       // the texture holds its own reference
        if (!tex) { LOG("newTextureWithDescriptor:iosurface: failed for %lux%lu",
                        (unsigned long)desc.width, (unsigned long)desc.height); goto fallthrough; }

        long n = atomic_fetch_add(&gSwapped, 1) + 1;
        traceRecord((id<MTLDevice>)self, desc, 0, 1, 1);
        LOG("swapped %lux%lu fmt=%lu bpr=%zu  (total %ld)", (unsigned long)desc.width,
            (unsigned long)desc.height, (unsigned long)desc.pixelFormat, bpr, n);
        return tex;
    }

fallthrough:
    atomic_fetch_add(&gFellThrough, 1);
    traceRecord((id<MTLDevice>)self, desc, 0, flagged, 0);
    return orig(self, _cmd, desc);
}

// Traced only, never swapped: a texture the application has already put over
// its own IOSurface is in the class every measurement found clean, and it is
// worth knowing how much of the app's work arrives this way.
static id swizzledNewTextureIOSurface(id self, SEL _cmd, MTLTextureDescriptor *desc,
                                      IOSurfaceRef surf, NSUInteger plane)
{
    id (*orig)(id, SEL, MTLTextureDescriptor *, IOSurfaceRef, NSUInteger) =
        (void *)gOrigNewTextureIOSurface;
    traceRecord((id<MTLDevice>)self, desc, 1, 0, 0);
    return orig(self, _cmd, desc, surf, plane);
}

static void traceRecord(id<MTLDevice> dev, MTLTextureDescriptor *d,
                        int iosurf, int flagged, int swapped)
{
    if (!gTrace || !d) return;
    MTLSizeAndAlign sa = [dev heapTextureSizeAndAlignWithDescriptor:d];
    unsigned long px = d.width * d.height;   // ratio against pixels, bpe folded in by size
    os_unfair_lock_lock(&gTraceLock);
    for (int i = 0; i < gTraceN; i++) {
        struct traceRow *r = &gTraceRows[i];
        if (r->w == d.width && r->h == d.height && r->fmt == (unsigned long)d.pixelFormat
            && r->usage == (unsigned long)d.usage && r->type == (unsigned long)d.textureType
            && r->mips == d.mipmapLevelCount && r->storage == (unsigned long)d.storageMode
            && r->iosurf == iosurf) {
            r->count++; r->swapped += swapped;
            os_unfair_lock_unlock(&gTraceLock);
            return;
        }
    }
    if (gTraceN < TRACE_MAX) {
        struct traceRow *r = &gTraceRows[gTraceN++];
        r->w = d.width; r->h = d.height; r->fmt = (unsigned long)d.pixelFormat;
        r->usage = (unsigned long)d.usage; r->type = (unsigned long)d.textureType;
        r->mips = d.mipmapLevelCount; r->storage = (unsigned long)d.storageMode;
        r->align = sa.align; r->ratio = px ? (double)sa.size / (double)px : 0;
        r->iosurf = iosurf; r->flagged = flagged; r->swapped = swapped; r->count = 1;
        os_unfair_lock_unlock(&gTraceLock);
        LOG("NEW %lux%lu fmt=%lu usage=0x%lx type=%lu mips=%lu storage=%lu "
            "align=%lu size/px=%.2f%s%s", r->w, r->h, r->fmt, r->usage, r->type,
            r->mips, r->storage, r->align, r->ratio,
            iosurf ? " APP-IOSURFACE" : "", flagged ? " FLAGGED" : "");
        return;
    }
    os_unfair_lock_unlock(&gTraceLock);
}

static void traceReport(void)
{
    if (!gTrace) return;
    FILE *f = gLogFile ? gLogFile : stderr;
    os_unfair_lock_lock(&gTraceLock);
    fprintf(f, "iosurftex: pid %d inventory, %d distinct descriptors:\n", getpid(), gTraceN);
    for (int i = 0; i < gTraceN; i++) {
        struct traceRow *r = &gTraceRows[i];
        fprintf(f, "iosurftex:   n=%-5lu %4lux%-5lu fmt=%-4lu usage=0x%-2lx type=%lu mips=%lu "
                "store=%lu align=%-6lu size/px=%.2f%s%s swapped=%lu\n",
                r->count, r->w, r->h, r->fmt, r->usage, r->type, r->mips, r->storage,
                r->align, r->ratio, r->iosurf ? " APP-IOSURFACE" : "",
                r->flagged ? " FLAGGED" : "", (unsigned long)r->swapped);
    }
    os_unfair_lock_unlock(&gTraceLock);
    fflush(f);
}

// Printed once per process at exit, so a single run answers whether this
// application's textures come through here at all.
static void report(void)
{
    FILE *f = gLogFile ? gLogFile : stderr;
    fprintf(f, "iosurftex: pid %d summary: swapped=%ld  declined(usage)=%ld "
            "declined(format)=%ld declined(structure)=%ld  law-clean=%ld  fellthrough=%ld "
            "cpu-uploads=%ld\n",
            getpid(), atomic_load(&gSwapped), atomic_load(&gDeclUsage),
            atomic_load(&gDeclFormat), atomic_load(&gDeclStruct),
            atomic_load(&gLawClean), atomic_load(&gFellThrough),
            atomic_load(&gReplaceCalls));
    fflush(f);
    traceReport();
}

static void installOnce(id<MTLDevice> dev)
{
    static atomic_flag done = ATOMIC_FLAG_INIT;
    if (!dev || atomic_flag_test_and_set(&done)) return;

    Class cls = object_getClass(dev);
    Method m = class_getInstanceMethod(cls, @selector(newTextureWithDescriptor:));
    if (!m) { LOG("no -newTextureWithDescriptor: on %s", class_getName(cls)); return; }

    gOrigNewTexture = method_getImplementation(m);
    method_setImplementation(m, (IMP)swizzledNewTexture);
    installReplaceCounter();
    atexit(report);
    LOG("pid %d installed on %s (device \"%s\")", getpid(), class_getName(cls),
        dev.name.UTF8String);
}

// Interposed, not swizzled, so the driver bundle is certainly loaded by the time
// the class is looked up.
__attribute__((ns_returns_retained))
static id<MTLDevice> my_MTLCreateSystemDefaultDevice(void)
{
    id<MTLDevice> dev = MTLCreateSystemDefaultDevice();   // +1, passed straight on
    if (gEnabled && !gInert) installOnce(dev);
    return dev;
}

__attribute__((ns_returns_retained))
static NSArray<id<MTLDevice>> *my_MTLCopyAllDevices(void)
{
    NSArray<id<MTLDevice>> *a = MTLCopyAllDevices();      // +1, passed straight on
    if (gEnabled && !gInert && a.count) installOnce(a[0]);
    return a;
}

__attribute__((used, section("__DATA,__interpose")))
static const struct { const void *replacement, *replacee; } gInterposers[] = {
    { (const void *)&my_MTLCreateSystemDefaultDevice, (const void *)&MTLCreateSystemDefaultDevice },
    { (const void *)&my_MTLCopyAllDevices,            (const void *)&MTLCopyAllDevices },
};

__attribute__((constructor)) static void init(void)
{
    const char *l = getenv("RBGPU_IOSURFTEX_LOG");
    gLog = l ? atoi(l) : 0;
    const char *a = getenv("RBGPU_IOSURFTEX_ANYUSAGE");
    gAnyUsage = a && atoi(a) != 0;
    const char *en = getenv("RBGPU_IOSURFTEX");
    gEnabled = !en || atoi(en) != 0;
    const char *in = getenv("RBGPU_IOSURFTEX_INERT");
    gInert = in && atoi(in) != 0;
    const char *tr = getenv("RBGPU_IOSURFTEX_TRACE");
    gTrace = tr && atoi(tr) != 0;
    const char *lf = getenv("RBGPU_IOSURFTEX_LOGFILE");
    if (lf && *lf) {
        gLogFile = fopen(lf, "a");       // every process appends; each line carries its pid
        if (gLogFile && !gLog) gLog = 1; // naming a file is asking for logging
    }
    LOG("loaded in pid %d (%s)%s%s%s%s", getpid(), getprogname(),
        gEnabled ? "" : " DISABLED", gInert ? " INERT" : "",
        gTrace ? " TRACE" : "", gAnyUsage ? " anyusage" : "");
}
