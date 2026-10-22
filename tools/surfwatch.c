// surfwatch.c (2nj) — what does CoreAnimation ask for when it makes a layer
// backing store, and how does it differ from a surface made by hand?
//
// surfdump could not answer this: IOSurfaceLookup reaches only surfaces the
// calling process may use, and CoreAnimation's backing stores are not global,
// so 0 of 20000 ids resolved. But the creation happens IN THIS PROCESS, so it
// can be watched on the way through.
//
// This interposes IOSurfaceCreate and prints the properties dictionary of
// every surface the process makes, in order. The probe allocates its own
// surfaces with known properties and they are always clean; CoreAnimation's
// backing stores are not. Both appear here, in creation order, and the first
// backing store differs from the later ones in whether it ends up wrong
// (2nh), so the ordering is part of the evidence.
//
// Per-process injection through DYLD_INSERT_LIBRARIES (rule 21), never a
// launchd environment.
//
// build: make surfwatch
// use:   DYLD_INSERT_LIBRARIES=build/out/surfwatch.dylib build/out/screenprobe ...
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <IOSurface/IOSurface.h>
#include <CoreFoundation/CoreFoundation.h>

static int gSeq = 0;

static void dumpDict(CFDictionaryRef d)
{
    if (!d) { fprintf(stderr, "        (null properties)\n"); return; }
    CFIndex n = CFDictionaryGetCount(d);
    const void **keys = malloc(sizeof(void *) * n);
    const void **vals = malloc(sizeof(void *) * n);
    CFDictionaryGetKeysAndValues(d, keys, vals);
    for (CFIndex i = 0; i < n; i++) {
        char kb[96] = {0}, vb[160] = {0};
        CFStringRef ks = CFCopyDescription(keys[i]);
        CFStringRef vs = CFCopyDescription(vals[i]);
        CFStringGetCString(ks, kb, sizeof(kb), kCFStringEncodingUTF8);
        CFStringGetCString(vs, vb, sizeof(vb), kCFStringEncodingUTF8);
        fprintf(stderr, "        %-34s %s\n", kb, vb);
        CFRelease(ks); CFRelease(vs);
    }
    free(keys); free(vals);
}

static IOSurfaceRef my_IOSurfaceCreate(CFDictionaryRef properties)
{
    IOSurfaceRef s = IOSurfaceCreate(properties);
    int seq = ++gSeq;
    fprintf(stderr, "surfwatch: #%d IOSurfaceCreate -> %s", seq, s ? "ok" : "FAILED");
    if (s) {
        fprintf(stderr, "  id=%u %zux%zu bpr=%zu alloc=%zu",
                IOSurfaceGetID(s), IOSurfaceGetWidth(s), IOSurfaceGetHeight(s),
                IOSurfaceGetBytesPerRow(s), IOSurfaceGetAllocSize(s));
    }
    fprintf(stderr, "\n");
    dumpDict(properties);
    return s;
}

// A surface the WindowServer allocated arrives in this process through a mach
// port rather than being created here. Catching that tells us who the
// allocator really is - and the counts above (0, 1, 0, 1 creates for 1, 2, 4
// and 6 windows) already say it is not this process.
static IOSurfaceRef my_IOSurfaceLookupFromMachPort(mach_port_t port)
{
    IOSurfaceRef s = IOSurfaceLookupFromMachPort(port);
    int seq = ++gSeq;
    fprintf(stderr, "surfwatch: #%d IOSurfaceLookupFromMachPort(%u) -> %s", seq,
            (unsigned)port, s ? "ok" : "FAILED");
    if (s) {
        fprintf(stderr, "  id=%u %zux%zu bpr=%zu alloc=%zu",
                IOSurfaceGetID(s), IOSurfaceGetWidth(s), IOSurfaceGetHeight(s),
                IOSurfaceGetBytesPerRow(s), IOSurfaceGetAllocSize(s));
    }
    fprintf(stderr, "\n");
    return s;
}

__attribute__((used)) static struct {
    const void *replacement;
    const void *replacee;
} interposers[] __attribute__((section("__DATA,__interpose"))) = {
    { (const void *)my_IOSurfaceCreate, (const void *)IOSurfaceCreate },
    { (const void *)my_IOSurfaceLookupFromMachPort, (const void *)IOSurfaceLookupFromMachPort },
};
