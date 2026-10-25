// 2r9: observe IOAccelResourceCreate in ONE scratch process. Never launchd.
// Signature measured at unslid 0x7ff80d33ce77: (device, args, size) -> handle;
// it forwards args/size unchanged to IOConnectCallMethod selector 0.
// All reads are bounded by size. No argument, return, or driver byte is changed.
// Build: make resourcewatch
// Run: DYLD_INSERT_LIBRARIES="$PWD/build/out/resourcewatch.dylib" build/out/texturepath
#include <inttypes.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>

extern void *IOAccelResourceCreate(void *, const void *, size_t);
static atomic_uint calls, failures;

static void *watch_create(void *device, const void *args, size_t size) {
    unsigned seq = atomic_fetch_add(&calls, 1) + 1;
    unsigned char b[0x108] = {0};
    mach_vm_size_t got = 0;
    size_t want = size < sizeof b ? size : sizeof b;
    kern_return_t kr = mach_vm_read_overwrite(mach_task_self(),
        (mach_vm_address_t)(uintptr_t)args, want,
        (mach_vm_address_t)(uintptr_t)b, &got);
    void *result = IOAccelResourceCreate(device, args, size);
    if (!result) atomic_fetch_add(&failures, 1);
    fprintf(stderr, "resourcewatch: #%u size=0x%zx result=%s read=%d/%llu",
        seq, size, result ? "ok" : "NULL", kr, (unsigned long long)got);
    if (kr == KERN_SUCCESS && got >= 0x28) {
        uint32_t type, flags; uint64_t length;
        memcpy(&type, b, sizeof type);
        memcpy(&length, b + 0x10, sizeof length);
        memcpy(&flags, b + 0x24, sizeof flags);
        fprintf(stderr, " type=0x%x length=%" PRIu64 " flags24=0x%x",
            type, length, flags);
    }
    if (kr == KERN_SUCCESS && got >= sizeof b) {
        uint32_t vendor_flags;
        memcpy(&vendor_flags, b + 0x104, sizeof vendor_flags);
        fprintf(stderr, " format68=0x%x vendor104=0x%x", b[0x68], vendor_flags);
    }
    fputc('\n', stderr);
    return result;
}

__attribute__((used)) static const struct {
    const void *replacement, *replacee;
} interpose __attribute__((section("__DATA,__interpose"))) = {
    (const void *)watch_create, (const void *)IOAccelResourceCreate
};

__attribute__((constructor)) static void start(void) {
    fprintf(stderr, "resourcewatch: loaded; observe-only IOAccelResourceCreate\n");
}
__attribute__((destructor)) static void finish(void) {
    unsigned n = atomic_load(&calls);
    fprintf(stderr, "resourcewatch: calls=%u failures=%u %s\n", n,
        atomic_load(&failures), n ? "interposition reached" : "INCONCLUSIVE: no intercepted calls");
}
