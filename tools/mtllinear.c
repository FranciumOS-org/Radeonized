// mtllinear.c (2nc) — force amdMtl_HWL_DetermineSwizzleMode to return LINEAR,
// in ONE process, with no kext and no reboot.
//
// Why this exists. 2nb widened the Lilu user patch (-rbgpumtllinear) to cover
// screenprobe and boot 117 carried it: the boot-arg was present, the kext UUID
// matched the build exactly, and rbgpu-arm had been consumed. The corruption
// was unchanged - but the process's own memory said why:
//
//     mtl-patch: original prologue present 1x, LINEAR stub present 0x
//     mtl-patch: -> NOT PATCHED
//
// Lilu's user patcher never reached it, so that boot tested nothing. Rule 20
// in its plainest form: the hook reporting ARMED is not evidence the patch
// applied, and here the plugin log had already rotated past its own line while
// the diag string is truncated when written to NVRAM, so neither could answer.
//
// This does the same edit from userspace instead of spending reboots finding
// out why Lilu did not. It is injected per process with DYLD_INSERT_LIBRARIES,
// which rule 21 requires: the blast radius is one process started on purpose,
// never a launchd environment.
//
// The patch is byte-for-byte the one in src/plugin/kern_start.cpp (2lm): the
// 32-byte prologue of amdMtl_HWL_DetermineSwizzleMode, verified unique against
// the driver's __text, replaced by a stub that writes 0 through the uint& out
// parameter and returns 0 = ADDR_SW_LINEAR.
//
// What it decides. 2na measured the corruption's address map exactly: the
// 8x8 micro tiles come out INTACT and only their placement is wrong. If
// forcing LINEAR here makes screenprobe's CPU window stop scrambling, the
// cause IS the client surface layout chosen in userspace and the kernel-side
// -rbgpulinear simply never reached these surfaces. If it still scrambles,
// layout chosen here is exonerated.
//
// build: make mtllinear
// use:   DYLD_INSERT_LIBRARIES=build/out/mtllinear.dylib build/out/screenprobe ...
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dlfcn.h>
#include <mach/mach.h>
#include <mach-o/dyld.h>
#include <mach-o/loader.h>
#include <unistd.h>

static const unsigned char kFind[32] = {
    0x55, 0x48, 0x89, 0xE5, 0x41, 0x57, 0x41, 0x56,
    0x41, 0x55, 0x41, 0x54, 0x53, 0x48, 0x81, 0xEC,
    0x88, 0x00, 0x00, 0x00, 0x45, 0x89, 0xCD, 0x44,
    0x89, 0xC3, 0x48, 0x89, 0x4D, 0xD0, 0x49, 0x89,
};
static const unsigned char kRepl[32] = {
    0x48, 0x8B, 0x44, 0x24, 0x08,               // mov rax, [rsp+8]
    0xC7, 0x00, 0x00, 0x00, 0x00, 0x00,         // mov dword [rax], 0
    0x31, 0xC0,                                 // xor eax, eax   (LINEAR)
    0xC3,                                       // ret
    0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90,
    0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90,
    0x90, 0x90,
};

static int gDone = 0;

static void patch_image(const struct mach_header *mh, intptr_t slide)
{
    if (gDone) return;
    if (getenv("RBGPU_MTLLINEAR") && getenv("RBGPU_MTLLINEAR")[0] == '0') return;

    Dl_info info;
    if (!dladdr((const void *)mh, &info) || !info.dli_fname) return;
    if (!strstr(info.dli_fname, "AMDRadeonX6000MTLDriver")) return;

    // locate __TEXT
    const struct mach_header_64 *h = (const struct mach_header_64 *)mh;
    const unsigned char *cmd = (const unsigned char *)mh + sizeof(*h);
    unsigned char *base = NULL;
    unsigned long size = 0;
    for (uint32_t i = 0; i < h->ncmds; i++) {
        const struct load_command *lc = (const struct load_command *)cmd;
        if (lc->cmd == LC_SEGMENT_64) {
            const struct segment_command_64 *seg = (const struct segment_command_64 *)cmd;
            if (strcmp(seg->segname, "__TEXT") == 0) {
                base = (unsigned char *)(seg->vmaddr + slide);
                size = (unsigned long)seg->vmsize;
            }
        }
        cmd += lc->cmdsize;
    }
    if (!base || size < sizeof(kFind)) {
        fprintf(stderr, "mtllinear: found the driver but no usable __TEXT\n");
        return;
    }

    // Rule 22: the find pattern was measured unique against this driver's
    // __text. Count every occurrence and refuse on anything but exactly one,
    // rather than patching the first hit and hoping.
    unsigned char *hit = NULL;
    int hits = 0;
    for (unsigned long o = 0; o + sizeof(kFind) <= size; o++) {
        if (memcmp(base + o, kFind, sizeof(kFind)) == 0) { hits++; hit = base + o; }
    }
    if (hits != 1) {
        fprintf(stderr, "mtllinear: REFUSING - found %d occurrences of the "
                        "prologue, expected exactly 1. Nothing patched.\n", hits);
        gDone = 1;
        return;
    }

    vm_address_t page = (vm_address_t)hit & ~(vm_address_t)(getpagesize() - 1);
    vm_size_t span = ((vm_address_t)hit + sizeof(kRepl)) - page;
    kern_return_t kr = vm_protect(mach_task_self(), page, span, FALSE,
                                  VM_PROT_READ | VM_PROT_WRITE | VM_PROT_COPY);
    if (kr != KERN_SUCCESS) {
        fprintf(stderr, "mtllinear: vm_protect(rw) failed with %d - nothing patched\n", kr);
        gDone = 1;
        return;
    }
    memcpy(hit, kRepl, sizeof(kRepl));
    kr = vm_protect(mach_task_self(), page, span, FALSE, VM_PROT_READ | VM_PROT_EXECUTE);
    if (kr != KERN_SUCCESS) {
        fprintf(stderr, "mtllinear: vm_protect(rx) failed with %d AFTER writing - "
                        "the bytes are in place but the page is not executable\n", kr);
        gDone = 1;
        return;
    }

    // Rule 14: say something on every path, and say the thing that makes the
    // reading meaningful - that the bytes really are there now.
    int ok = memcmp(hit, kRepl, sizeof(kRepl)) == 0;
    fprintf(stderr, "mtllinear: PATCHED DetermineSwizzleMode at %p (verify %s) - "
                    "it now returns ADDR_SW_LINEAR in this process only\n",
            (void *)hit, ok ? "ok" : "FAILED");
    gDone = 1;
}

__attribute__((constructor))
static void mtllinear_init(void)
{
    _dyld_register_func_for_add_image(patch_image);
}
