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
#include "../src/plugin/rbgpu_mtlcave.h"

#define ANCHOR 48       // bytes of context before the cave in the find pattern

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

        const uint8_t *cave = (const uint8_t *)(RBGPU_MTL_CAVE_VM + slide);
        const uint8_t *fn   = (const uint8_t *)(RBGPU_MTL_FN_VM + slide);
        printf("\ncave at %p, function at %p\n", (const void *)cave, (const void *)fn);
        show("CAVE (stage A)", cave, kRbMtlCaveRepl + ANCHOR, RBGPU_MTL_CAVE_USED);
        show("DETOUR (stage B)", fn, kRbMtlFnRepl, 5);

        int caveOk = memcmp(cave, kRbMtlCaveRepl + ANCHOR, RBGPU_MTL_CAVE_USED) == 0;
        int detOk  = memcmp(fn, kRbMtlFnRepl, 5) == 0;
        printf("\nVERDICT: ");
        if (caveOk && detOk) printf("cave AND detour present - the fix is live for every process.\n");
        else if (caveOk)     printf("CAVE PRESENT - Lilu's user patcher WORKS on this macOS. "
                                    "Stage B (-rbgpumtlfix) is now worth arming.\n");
        else if (detOk)      printf("detour present but cave is NOT - do not reboot into this, "
                                    "the detour would jump into unpatched padding.\n");
        else                 printf("NEITHER - Lilu's user patcher did not reach the shared "
                                    "cache. The kext saying ARMED means nothing (rule 20).\n");
        return caveOk ? 0 : 1;
    }
}
