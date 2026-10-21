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

static int gLog = 0, gAnyUsage = 0;
// RBGPU_IOSURFTEX=0 is authoritative off - it used to be overridden by having
// logging on, which made "off" not off. RBGPU_IOSURFTEX_INERT=1 loads and
// interposes but never swizzles, to separate "loading this dylib breaks the
// app" from "swizzling the device breaks the app".
static int gEnabled = 1, gInert = 0;
// An app launched by launchd has no terminal, so stderr goes nowhere useful.
// RBGPU_IOSURFTEX_LOGFILE names a file every process appends to instead.
static FILE *gLogFile;
static atomic_long gSwapped, gFellThrough;
// Why a descriptor the law flagged was declined - so that a run which fixes
// nothing still says what the application actually asked for.
static atomic_long gDeclUsage, gDeclFormat, gDeclStruct, gLawClean;
static IMP gOrigNewTexture;

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

// An IOSurface-backed texture is served for a read-only sampled texture, which
// is what a glyph atlas is. RBGPU_IOSURFTEX_ANYUSAGE=1 lifts this if a real
// application turns out to ask for more.
static bool usageOk(MTLTextureDescriptor *d)
{
    return gAnyUsage || (d.usage & ~(MTLTextureUsageShaderRead)) == 0;
}

static id swizzledNewTexture(id self, SEL _cmd, MTLTextureDescriptor *desc)
{
    id (*orig)(id, SEL, MTLTextureDescriptor *) = (void *)gOrigNewTexture;
    uint32_t cc; size_t bpe;

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
        LOG("swapped %lux%lu fmt=%lu bpr=%zu  (total %ld)", (unsigned long)desc.width,
            (unsigned long)desc.height, (unsigned long)desc.pixelFormat, bpr, n);
        return tex;
    }

fallthrough:
    atomic_fetch_add(&gFellThrough, 1);
    return orig(self, _cmd, desc);
}

// Printed once per process at exit, so a single run answers whether this
// application's textures come through here at all.
static void report(void)
{
    FILE *f = gLogFile ? gLogFile : stderr;
    fprintf(f, "iosurftex: pid %d summary: swapped=%ld  declined(usage)=%ld "
            "declined(format)=%ld declined(structure)=%ld  law-clean=%ld  fellthrough=%ld\n",
            getpid(), atomic_load(&gSwapped), atomic_load(&gDeclUsage),
            atomic_load(&gDeclFormat), atomic_load(&gDeclStruct),
            atomic_load(&gLawClean), atomic_load(&gFellThrough));
    fflush(f);
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
    const char *lf = getenv("RBGPU_IOSURFTEX_LOGFILE");
    if (lf && *lf) {
        gLogFile = fopen(lf, "a");       // every process appends; each line carries its pid
        if (gLogFile && !gLog) gLog = 1; // naming a file is asking for logging
    }
    LOG("loaded in pid %d (%s)%s%s%s", getpid(), getprogname(),
        gEnabled ? "" : " DISABLED", gInert ? " INERT" : "",
        gAnyUsage ? " anyusage" : "");
}
