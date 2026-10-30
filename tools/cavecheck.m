// cavecheck.m (2oa) - did Lilu's user patcher actually reach the dyld shared
// cache on this macOS?
//
// This is the whole point of Boot A. The kext says ARMED; that has never been
// evidence (rule 20, and 2nn where three milestones of user patches were
// inert while reporting success). The only thing that settles it is reading
// the bytes out of a process's own mapped copy of the driver, which is what
// this does.
//
// It forces Metal up so AMDRadeonX6000MTLDriver is mapped, finds that image,
// takes the shared-cache slide from dyld, and reads the two places the patch
// would land.
//
// build: make cavecheck
#import <Metal/Metal.h>
#include <mach-o/dyld.h>
#include <string.h>
#include <stdio.h>
#include <sys/sysctl.h>
#include "../src/plugin/rbgpu_mtlcave.h"

// 2xz: the cave is a TABLE now, one row per OS, so this reads the row for the
// kernel it is running on instead of the only row there used to be. Same
// kernel number the kexts key on (version_major); sysctl is how userspace
// reads it. A machine with no row cannot have been patched, which is a result
// and not an error, so it says so and exits clean.
static int darwinMajor(void)
{
    char rel[64];
    size_t n = sizeof(rel);
    int maj = 0;

    if (sysctlbyname("kern.osrelease", rel, &n, NULL, 0) != 0) {
        return 0;
    }
    for (const char *c = rel; *c >= '0' && *c <= '9'; c++) {
        maj = maj * 10 + (*c - '0');
    }
    return maj;
}

static void show(const char *what, const uint8_t *got, const uint8_t *want, size_t n)
{
    int ok = memcmp(got, want, n) == 0;
    printf("  %-18s %s\n", what, ok ? "PRESENT" : "absent");
    printf("    in memory :");
    for (size_t i = 0; i < (n < 16 ? n : 16); i++) printf(" %02x", got[i]);
    printf("%s\n", n > 16 ? " ..." : "");
    printf("    expected  :");
    for (size_t i = 0; i < (n < 16 ? n : 16); i++) printf(" %02x", want[i]);
    printf("%s\n", n > 16 ? " ..." : "");
}

int main(void)
{
    @autoreleasepool {
        id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
        if (!dev) {
            printf("NO METAL DEVICE - this boot is not armed, so nothing here "
                   "means anything. Check: nvram rbgpu-boot\n");
            return 2;
        }
        printf("device: %s\n", [[dev name] UTF8String]);

        const int major = darwinMajor();
        const struct rb_mtlcave_row *row = rbgpu_mtlcave_row(major);
        if (!row) {
            printf("NO CAVE ROW for Darwin %d, so the userspace half of the "
                   "swizzle fix was never placed in this boot and there is "
                   "nothing here to read. That is the expected state on an OS "
                   "nobody has derived a row for - run `make mtlcave` on this "
                   "machine to derive one.\n", major);
            return 0;
        }
        printf("row: Darwin %d (%s)\n", row->darwin, row->os);

        intptr_t slide = 0; int found = 0;
        for (uint32_t i = 0; i < _dyld_image_count(); i++) {
            const char *nm = _dyld_get_image_name(i);
            if (nm && strstr(nm, "AMDRadeonX6000MTLDriver")) {
                slide = _dyld_get_image_vmaddr_slide(i);
                printf("driver: %s\n  slide 0x%lx\n", nm, (unsigned long)slide);
                found = 1;
                break;
            }
        }
        if (!found) { printf("the Metal driver is not mapped in this process\n"); return 2; }

        const uint8_t *cave = (const uint8_t *)(row->cave_vm + slide);
        const uint8_t *fn   = (const uint8_t *)(row->fn_vm + slide);
        printf("\ncave at %p, function at %p\n", (const void *)cave, (const void *)fn);
        show("CAVE (stage A)", cave, row->cave_repl + row->cave_anchor, row->cave_used);
        show("DETOUR (stage B)", fn, row->fn_repl, 5);

        int caveOk = memcmp(cave, row->cave_repl + row->cave_anchor, row->cave_used) == 0;
        int detOk  = memcmp(fn, row->fn_repl, 5) == 0;
        printf("\nVERDICT: ");
        if (caveOk && detOk) printf("cave AND detour present - the fix is live for every process.\n");
        else if (caveOk)     printf("CAVE PRESENT - the KERNEL-side patcher (2og/2oi) placed it. "
                                    "Lilu's own user patcher is disabled on this OS and was never "
                                    "involved (rule 22). Stage B is -rbgpumtlkfix.\n");
        else if (detOk)      printf("detour present but cave is NOT - do not reboot into this, "
                                    "the detour would jump into unpatched padding.\n");
        else                 printf("NEITHER - nothing was placed in THIS process. Read mtlk_* "
                                    "from the IORegistry to see how far it got; the kext saying "
                                    "ARMED means nothing (rule 20).\n");
        return caveOk ? 0 : 1;
    }
}
