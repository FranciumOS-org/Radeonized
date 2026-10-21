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
//   clang -dynamiclib -O2 -fobjc-arc -o build/out/iosurftex.dylib \
//         tools/iosurftex.m -framework Metal -framework IOSurface -framework Foundation
//   DYLD_INSERT_LIBRARIES=build/out/iosurftex.dylib <program>
//
// RBGPU_IOSURFTEX=0    off
// RBGPU_IOSURFTEX_LOG=1  one line per decision, to stderr

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <IOSurface/IOSurface.h>
#import <objc/runtime.h>
#import <stdatomic.h>

static int gLog = 0;
static atomic_long gSwapped, gFellThrough;
static IMP gOrigNewTexture;

#define LOG(fmt, ...) do { if (gLog) fprintf(stderr, "iosurftex: " fmt "\n", ##__VA_ARGS__); } while (0)

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

static bool qualifies(MTLTextureDescriptor *d)
{
    return d.textureType == MTLTextureType2D
        && d.mipmapLevelCount == 1
        && d.arrayLength == 1
        && d.sampleCount == 1
        && d.depth == 1
        // An IOSurface-backed texture cannot be a render target or be written by
        // a shader on this driver; those paths are not the CPU-upload path the
        // law is about anyway.
        && (d.usage & ~(MTLTextureUsageShaderRead)) == 0;
}

static id swizzledNewTexture(id self, SEL _cmd, MTLTextureDescriptor *desc)
{
    id (*orig)(id, SEL, MTLTextureDescriptor *) = (void *)gOrigNewTexture;
    uint32_t cc; size_t bpe;

    if (!desc || !qualifies(desc) || !fourccFor(desc.pixelFormat, &cc, &bpe)) goto fallthrough;
    if (!lawPredictsFailure((id<MTLDevice>)self, desc, bpe)) goto fallthrough;

    {
        // bytesPerRow must be set explicitly. Left to IOSurface's own default a
        // 1000-wide surface hung the probe during 2mo, so it is pinned to the
        // 256-byte alignment the blit engine wants.
        size_t bpr = (desc.width * bpe + 255) & ~(size_t)255;
        IOSurfaceRef surf = IOSurfaceCreate((__bridge CFDictionaryRef)@{
            (__bridge NSString *)kIOSurfaceWidth:           @(desc.width),
            (__bridge NSString *)kIOSurfaceHeight:          @(desc.height),
            (__bridge NSString *)kIOSurfaceBytesPerElement: @(bpe),
            (__bridge NSString *)kIOSurfaceBytesPerRow:     @(bpr),
            (__bridge NSString *)kIOSurfacePixelFormat:     @(cc),
        });
        if (!surf) { LOG("IOSurfaceCreate failed for %lux%lu", (unsigned long)desc.width,
                         (unsigned long)desc.height); goto fallthrough; }

        id tex = [(id<MTLDevice>)self newTextureWithDescriptor:desc iosurface:surf plane:0];
        CFRelease(surf);
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

static void installOnce(id<MTLDevice> dev)
{
    static atomic_flag done = ATOMIC_FLAG_INIT;
    if (!dev || atomic_flag_test_and_set(&done)) return;

    Class cls = object_getClass(dev);
    Method m = class_getInstanceMethod(cls, @selector(newTextureWithDescriptor:));
    if (!m) { LOG("no -newTextureWithDescriptor: on %s", class_getName(cls)); return; }

    gOrigNewTexture = method_getImplementation(m);
    method_setImplementation(m, (IMP)swizzledNewTexture);
    LOG("installed on %s (device \"%s\")", class_getName(cls), dev.name.UTF8String);
}

// Interposed, not swizzled, so the driver bundle is certainly loaded by the time
// the class is looked up.
static id<MTLDevice> my_MTLCreateSystemDefaultDevice(void)
{
    id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
    if (gLog || getenv("RBGPU_IOSURFTEX") == NULL || atoi(getenv("RBGPU_IOSURFTEX")) != 0)
        installOnce(dev);
    return dev;
}

static NSArray<id<MTLDevice>> *my_MTLCopyAllDevices(void)
{
    NSArray<id<MTLDevice>> *a = MTLCopyAllDevices();
    if (a.count) installOnce(a[0]);
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
    gLog = l && atoi(l) != 0;
    LOG("loaded");
}
