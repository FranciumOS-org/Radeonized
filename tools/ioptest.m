// ioptest (2qm): can an IOPresentment display pipe actually be created for
// OUR framebuffer and OUR accelerator?  Non-root, no boot, no kext.
//
// 2ql closed `Display::GetNextSurface`: its pool path (the one that makes a
// real IOSurface) is gated on `UseIOPresentment()`, which is false here only
// because CoreDisplay classifies GPUs by the IOFramebuffer's CLASS NAME and
// `IONDRVFramebuffer` matches none of Intel/AMD/NVDA/AppleParavirt, yielding
// the "unknown" bit 0x1 that the supported mask can never contain.
//
// That makes the gate look like the whole problem - but only if the thing it
// gates would work.  So: what does IOPresentment actually need?  Its own
// image (/System/Library/PrivateFrameworks/IOPresentment.framework, present
// on this machine) names `IOFramebuffer`, `IOAccelIndex`,
// `IOAccelDisplayPipeTriage`, `IOAccelDisplayPipeTransactionWait`,
// `DisplayPipeSupported` and `ioPresentmentFindAGDCMatch` - no
// IOMobileFramebuffer, nothing AMD-specific in the kernel.  And 2pp measured
// every one of those live on `.Display_boot`.
//
// Signature read from the disassembly, NOT from the name (this has bitten
// four times): `IOPresentment::IOPresentmentCreateForRegistryService(unsigned
// int, unsigned int)` at image+0x400C takes TWO io_service_t, refuses when
// they are equal (`cmpl %esi,%edi; jne`, else it logs and returns 0), converts
// each to a registry ID, and forwards to CreateForRegistryID(uint64, uint64).
// So the pair is (framebuffer, accelerator).
//
// RULE 14 - the branches, pre-stated:
//   ref != NULL  The pipe is creatable for this framebuffer by an ORDINARY
//                process.  The IOPresentment path is viable and CoreDisplay's
//                class-name test is the only thing refusing it.
//   ref == NULL  INCONCLUSIVE, not a refutation: WindowServer has entitlements
//                this process does not, and 2pp measured it already holding
//                two IOAccelDisplayPipeUserClient2 instances.  The framework's
//                own os_log says which failure it was - "Unable to connect to
//                IOPresentment client" (entitlement) vs "Unable to create
//                IOAccelDisplayPipe" (capability).  Read it with:
//                  log show --last 2m --predicate 'process == "ioptest"' --info
//
// MEASURED on the session's boot, unprivileged: the framework ABORTS rather
// than returning NULL, after logging
//   Error: ioPresentmentFindAGDCMatch() failed - retCode: 0xe00002e2
//   Error: Unable to acquire map
//   GPUWranglerOpenWithTimeout: failed to connect; status=0xe00002e2
// and 0xe00002e2 is kIOReturnNotPermitted - an ENTITLEMENT, not a capability
// answer.  So the call is made in a forked child, and the parent interprets
// the exit status: a crash is reported as INCONCLUSIVE with the reason, never
// as a refutation.
#import <Foundation/Foundation.h>
#import <IOKit/IOKitLib.h>
#include <dlfcn.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/wait.h>

static io_service_t find_by_class(const char *cls, char *namebuf, size_t n)
{
    io_iterator_t it = IO_OBJECT_NULL;
    if (IOServiceGetMatchingServices(kIOMainPortDefault,
            IOServiceMatching(cls), &it) != KERN_SUCCESS)
        return IO_OBJECT_NULL;
    io_service_t s = IOIteratorNext(it);
    if (s && namebuf) {
        io_name_t nm = {0};
        IORegistryEntryGetName(s, nm);
        snprintf(namebuf, n, "%s", nm);
    }
    IOObjectRelease(it);
    return s;
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    printf("ioptest (2qm) - is an IOPresentment display pipe creatable here?\n\n");

    char fbname[128] = "?", acname[128] = "?";
    io_service_t fb = find_by_class("IOFramebuffer", fbname, sizeof fbname);
    io_service_t ac = find_by_class("IOAccelerator", acname, sizeof acname);

    printf("1. The two services\n");
    if (!fb) { printf("   no IOFramebuffer - INCONCLUSIVE\n"); return 3; }
    if (!ac) { printf("   no IOAccelerator - this boot is not accelerated; INCONCLUSIVE\n"); return 3; }
    io_name_t fbc = {0}, acc = {0};
    IOObjectGetClass(fb, fbc); IOObjectGetClass(ac, acc);
    uint64_t fbid = 0, acid = 0;
    IORegistryEntryGetRegistryEntryID(fb, &fbid);
    IORegistryEntryGetRegistryEntryID(ac, &acid);
    printf("   framebuffer  %-40s name=%-16s id=0x%llx (port 0x%x)\n", fbc, fbname, fbid, fb);
    printf("   accelerator  %-40s name=%-16s id=0x%llx (port 0x%x)\n", acc, acname, acid, ac);
    if (fb == ac) { printf("   same port - the function refuses that; INCONCLUSIVE\n"); return 3; }

    printf("\n2. Preconditions the framework's own strings name\n");
    const char *keys[] = {"IOAccelIndex", "IOAccelRevision", "IOAccelDisplayPipeCapabilities", NULL};
    for (int i = 0; keys[i]; i++) {
        CFStringRef k = CFStringCreateWithCString(NULL, keys[i], kCFStringEncodingUTF8);
        CFTypeRef v = IORegistryEntrySearchCFProperty(fb, kIOServicePlane, k, NULL,
                kIORegistryIterateRecursively);
        const char *where = "fb";
        if (!v) {
            v = IORegistryEntrySearchCFProperty(ac, kIOServicePlane, k, NULL,
                    kIORegistryIterateRecursively);
            where = "accel";
        }
        CFRelease(k);
        printf("   %-32s [%-5s] %s\n", keys[i], v ? where : "-",
               v ? [[(__bridge id)v description] UTF8String] : "ABSENT");
        if (v) CFRelease(v);
    }

    printf("\n3. IOPresentmentCreateForRegistryService(framebuffer, accelerator)\n");
    void *h = dlopen("/System/Library/PrivateFrameworks/IOPresentment.framework/IOPresentment", RTLD_LAZY);
    if (!h) { printf("   dlopen failed: %s - INCONCLUSIVE\n", dlerror()); return 3; }
    void *(*create)(unsigned int, unsigned int) =
        (void *(*)(unsigned int, unsigned int))dlsym(h, "IOPresentmentCreateForRegistryService");
    CFDictionaryRef (*caps)(void *) = (CFDictionaryRef (*)(void *))dlsym(h, "IOPresentmentCopyCapabilities");
    if (!create) { printf("   symbol absent - INCONCLUSIVE\n"); return 3; }

    // The framework aborts on failure, so make the call where a crash cannot
    // take the verdict with it.
    fflush(stdout);
    pid_t kid = fork();
    if (kid == 0) {
        void *r = create(fb, ac);
        _exit(r ? 0 : 1);
    }
    int st = 0;
    waitpid(kid, &st, 0);
    void *ref = NULL;
    if (WIFSIGNALED(st)) {
        printf("   the call CRASHED (signal %d) inside IOPresentment\n", WTERMSIG(st));
    } else if (WIFEXITED(st) && WEXITSTATUS(st) == 0) {
        printf("   the call SUCCEEDED in the child; repeating it here\n");
        ref = create(fb, ac);
        printf("   -> %p\n", ref);
    } else {
        printf("   the call returned NULL\n");
    }
    if (!ref) {
        printf("\n4. Verdict: INCONCLUSIVE (pre-stated branch)\n");
        printf("   A NULL here does not refute the path: WindowServer holds two\n");
        printf("   IOAccelDisplayPipeUserClient2 instances (2pp) with entitlements this\n");
        printf("   process lacks.  Which failure it was is in this process's own log:\n");
        printf("     log show --last 2m --predicate 'process == \"ioptest\"' --info\n");
        printf("   \"Unable to connect to IOPresentment client\" = entitlement,\n");
        printf("   GPUWranglerOpenWithTimeout status 0xe00002e2 = kIOReturnNotPermitted,\n");
        printf("     also an entitlement - that is what an unprivileged run shows,\n");
        printf("     and it says NOTHING about whether the pipe could be created.\n");
        printf("   \"Unable to create IOAccelDisplayPipe\"      = capability.\n");
        return 3;
    }

    printf("\n4. Capabilities of the pipe that was just created\n");
    if (caps) {
        CFDictionaryRef d = caps(ref);
        if (d) { printf("   %s\n", [[(__bridge id)d description] UTF8String]); CFRelease(d); }
        else printf("   IOPresentmentCopyCapabilities returned NULL\n");
    } else printf("   IOPresentmentCopyCapabilities absent\n");

    printf("\n5. Verdict: the IOPresentment display pipe IS creatable for this\n");
    printf("   framebuffer, from an ordinary process.  CoreDisplay's class-name\n");
    printf("   vendor test is then the only thing refusing the path that puts a\n");
    printf("   real IOSurface under the desktop.\n");
    return 0;
}
