// surfdump.c (2ni) — what properties does a CoreAnimation backing store carry
// that an IOSurface allocated by hand does not?
//
// 2nh: the first backing store a process allocates is often right and every
// later one is wrong, while plain IOSurfaces allocated here are right every
// time. 2ni then ruled out an address or pressure threshold - pre-allocating
// 16 IOSurfaces or 256 MB of memory before the window left its rate unchanged
// (3/8 baseline, 2/8, 4/8).
//
// For an IOSurface-backed texture the tiling is not chosen by
// amdMtl_HWL_DetermineSwizzleMode - which is why patching that in-process did
// nothing (2nd) - it comes from values stored ON the surface. So read them.
//
// IOSurfaceLookup resolves a surface id to a surface this process may use.
// Walking the id space finds the ones CoreAnimation made for this session's
// windows, and their dictionaries can be compared against one made here.
//
// build: make surfdump
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <IOSurface/IOSurface.h>
#include <CoreFoundation/CoreFoundation.h>

static void printValue(const char *key, CFTypeRef v)
{
    if (!v) return;
    CFTypeID t = CFGetTypeID(v);
    if (t == CFNumberGetTypeID()) {
        long long n = 0;
        CFNumberGetValue((CFNumberRef)v, kCFNumberLongLongType, &n);
        printf("      %-28s %lld (0x%llx)\n", key, n, (unsigned long long)n);
    } else if (t == CFBooleanGetTypeID()) {
        printf("      %-28s %s\n", key, CFBooleanGetValue((CFBooleanRef)v) ? "true" : "false");
    } else {
        char buf[128];
        CFStringRef d = CFCopyDescription(v);
        if (CFStringGetCString(d, buf, sizeof(buf), kCFStringEncodingUTF8))
            printf("      %-28s %s\n", key, buf);
        CFRelease(d);
    }
}

// Keys worth asking for by name. IOSurface's dictionary is not enumerable
// through the public API, so the interesting ones are queried explicitly -
// and anything the driver stores for tiling would be among them.
static const char *kKeys[] = {
    "IOSurfaceAllocSize", "IOSurfaceWidth", "IOSurfaceHeight",
    "IOSurfaceBytesPerRow", "IOSurfaceBytesPerElement", "IOSurfacePixelFormat",
    "IOSurfaceOffset", "IOSurfaceElementWidth", "IOSurfaceElementHeight",
    "IOSurfacePlaneCount", "IOSurfaceCacheMode", "IOSurfaceIsGlobal",
    "IOSurfacePurgeWhenNotInUse", "IOSurfaceNonPurgeable",
    // AMD / accelerator tiling hints seen on this stack
    "IOSurfaceTileMode", "IOSurfaceSwizzle", "IOSurfaceTiling",
    "IOAccelTileMode", "IOAccelSwizzleMode", "IOSurfaceYCbCrMatrix",
    "IOSurfaceColorSpace", "IOSurfaceContentHeadroom",
};

int main(int argc, char **argv)
{
    int want_w = argc > 1 ? atoi(argv[1]) : 0;
    int want_h = argc > 2 ? atoi(argv[2]) : 0;
    int maxid  = argc > 3 ? atoi(argv[3]) : 20000;

    printf("walking IOSurface ids 1..%d", maxid);
    if (want_w) printf(", reporting only %dx%d", want_w, want_h);
    printf("\n\n");

    int found = 0, resolved = 0;
    for (int id = 1; id <= maxid; id++) {
        IOSurfaceRef s = IOSurfaceLookup((IOSurfaceID)id);
        if (!s) continue;
        resolved++;
        size_t w = IOSurfaceGetWidth(s), h = IOSurfaceGetHeight(s);
        if (want_w && !(w == (size_t)want_w && h == (size_t)want_h)) { CFRelease(s); continue; }
        found++;
        printf("  surface id %d  %zux%zu  bpr=%zu  alloc=%zu\n",
               id, w, h, IOSurfaceGetBytesPerRow(s), IOSurfaceGetAllocSize(s));
        for (size_t k = 0; k < sizeof(kKeys)/sizeof(kKeys[0]); k++) {
            CFStringRef key = CFStringCreateWithCString(NULL, kKeys[k], kCFStringEncodingUTF8);
            CFTypeRef v = IOSurfaceCopyValue(s, key);
            if (v) { printValue(kKeys[k], v); CFRelease(v); }
            CFRelease(key);
        }
        CFRelease(s);
        if (found >= 12) break;
    }
    // Rule 14: every path says something.
    printf("\n%d ids resolved, %d matched\n", resolved, found);
    if (resolved == 0)
        printf("none resolved - IOSurfaceLookup only reaches surfaces this process may use,\n"
               "so CoreAnimation's own backing stores may not be reachable this way.\n");
    return 0;
}
