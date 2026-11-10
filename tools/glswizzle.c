// glswizzle.c (3gl) - the GL driver's own swizzle chooser, logged.
//
// WHY THIS EXISTS. Every swizzle instrument and every fix in this tree acts on
// `amdMtl_HWL_DetermineSwizzleMode` inside AMDRadeonX6000MTLDriver, which
// lives in the dyld shared cache. 3gl measured the user's broken application
// and it is not a Metal client at all: JavaFX 21 renders through Prism's
// `es2` pipeline - legacy OpenGL 2.1 - and `vmmap` shows it drawing through
//
//     /System/Library/Extensions/AMDRadeonX6000GLDriver.bundle/.../AMDRadeonX6000GLDriver
//
// loaded from disk at its own address, OUTSIDE the shared cache. The cave
// does not reach it, and that bundle carries its own copy of AddrLib and its
// own chooser:
//
//     0x000168b4 (__TEXT,__text)  _glrAMD_GFX10_DetermineSwizzleMode
//
// Nothing in this project has ever read it. Three independent OpenGL clients
// are wrong on this machine (JavaFX es2, JavaFX sw - whose glass layer still
// presents through GL - and Steam's embedded CEF) while every Metal client is
// right, so this function is the candidate. This tool reads it.
//
// RULE 16: THE DEFAULT MODE CHANGES NOTHING. It installs a trampoline and
// logs what the driver already decided. Nothing here overrides a mode yet -
// an override needs the argument meanings established first, and they are not.
//
//   RBGPU_GLSWIZZLE=log   (default) observe only
//   RBGPU_GLSWIZZLE=off   load, find, verify, and patch NOTHING (the control)
//   RBGPU_GLSWIZZLE=cave  place the GENERATED cave and detour from
//                         src/common/rb_glcave.h - the exact bytes the kext
//                         will write, at the same two places, in this
//                         process. No trampoline and no C hook: this is the
//                         kext's own mechanism, proven in userspace before it
//                         is given to the kernel (3mip-b's `newcave`).
//   RBGPU_GLSWIZZLE=nox   map every _X mode to its non-X equivalent, the same
//                         28-byte table the Metal cave uses. NOT the cave's
//                         full policy: nothing is forced LINEAR here, because
//                         `usage` has not been located in this function's
//                         arguments and 2nq's lesson is that a render target
//                         made linear returns a nil pipeline state.
//   RBGPU_GLSWIZZLE_DUMP=N  hexdump N distinct callers' argument blocks
//
// build: make glswizzle
// use:   DYLD_INSERT_LIBRARIES=build/out/glswizzle.dylib RBGPU_GLSWIZZLE=log <app>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <pthread.h>
#include <sys/mman.h>
#include <mach/mach.h>
#include <dlfcn.h>
#include <mach-o/dyld.h>
#include <mach-o/loader.h>
#include <unistd.h>
#include "../src/common/rb_glcave.h"

// The chooser's prologue, read off the on-disk bundle at 0x168b4:
//   push rbp / mov rbp,rsp / push r14 / push rbx / sub rsp,0x70   (11 bytes)
//   vxorps xmm0,xmm0,xmm0                                         (4 bytes)
// Fifteen bytes, a whole number of instructions, none of them a branch and
// none rip-relative - rule 2, and enough room for the 13-byte absolute jump.
static const unsigned char kFind[32] = {
    0x55, 0x48, 0x89, 0xe5, 0x41, 0x56, 0x53, 0x48,
    0x83, 0xec, 0x70, 0xc5, 0xf8, 0x57, 0xc0, 0xc5,
    0xfc, 0x11, 0x45, 0xd0, 0xc5, 0xfc, 0x11, 0x45,
    0xbc, 0xc5, 0xf8, 0x57, 0xc0, 0xc5, 0xf8, 0x11,
};
#define PROLOGUE_COPY 15

// The arity is NOT established. The body reads rcx, r8, r9 and the first
// stack argument, so the call takes at least seven. Passing more arguments
// than a callee reads is harmless under SysV (the caller owns the frame), so
// the trampoline is declared with the widest shape that is certainly safe and
// the extra names say plainly that they are unidentified.
typedef unsigned int (*glswz_fn)(uint64_t a1, uint64_t a2, uint64_t a3,
                                 const void *desc, const void *r8,
                                 uint64_t r9, uint64_t s1, uint64_t s2,
                                 uint64_t s3);

enum { G_LOG, G_OFF, G_NOX, G_CAVE };

// The cave's table, unchanged (2nr). Z modes are not touched.
static unsigned int nox(unsigned int m)
{
    switch (m) {
    case 21: return 5;   // 4KB_S_X  -> 4KB_S
    case 22: return 6;   // 4KB_D_X  -> 4KB_D
    case 23: return 6;   // 4KB_R_X  -> 4KB_D
    case 25: return 9;   // 64KB_S_X -> 64KB_S
    case 26: return 10;  // 64KB_D_X -> 64KB_D
    case 27: return 10;  // 64KB_R_X -> 64KB_D
    default: return m;
    }
}
static int gMode = G_LOG;
static glswz_fn gTramp;
static int gDone;
static int gDumpBudget = 8;

struct key { uint64_t a, b, c; unsigned int mode; };
#define NKEYS 192
static struct key gSeen[NKEYS];
static int gNSeen;
static pthread_mutex_t gLock = PTHREAD_MUTEX_INITIALIZER;

static int fresh_key(uint64_t a, uint64_t b, uint64_t c, unsigned int mode)
{
    int fresh = 1;
    pthread_mutex_lock(&gLock);
    for (int i = 0; i < gNSeen; i++) {
        if (gSeen[i].a == a && gSeen[i].b == b && gSeen[i].c == c &&
            gSeen[i].mode == mode) { fresh = 0; break; }
    }
    if (fresh && gNSeen < NKEYS) {
        gSeen[gNSeen].a = a; gSeen[gNSeen].b = b;
        gSeen[gNSeen].c = c; gSeen[gNSeen].mode = mode;
        gNSeen++;
    }
    pthread_mutex_unlock(&gLock);
    return fresh;
}

static unsigned int hook(uint64_t a1, uint64_t a2, uint64_t a3,
                         const void *desc, const void *r8, uint64_t r9,
                         uint64_t s1, uint64_t s2, uint64_t s3)
{
    unsigned int orig = gTramp ? gTramp(a1, a2, a3, desc, r8, r9, s1, s2, s3) : 0;
    unsigned int mode = (gMode == G_NOX) ? nox(orig) : orig;

    // The four descriptor fields the body itself reads, by offset, named by
    // what the instruction does rather than by a guess at the meaning.
    uint64_t d10 = 0, d13 = 0, d14 = 0, d16 = 0, e18 = 0;
    if (desc) {
        const unsigned char *d = (const unsigned char *)desc;
        d10 = d[0x10]; d13 = d[0x13]; d14 = d[0x14];
        d16 = (uint64_t)d[0x16] | ((uint64_t)d[0x17] << 8);
    }
    if (r8) { e18 = *(const uint64_t *)((const char *)r8 + 0x18); }

    if (fresh_key(d10 | (d13 << 8) | (d14 << 16) | (d16 << 24), e18, r9, mode)) {
        fprintf(stderr, "glswz: desc+10=%llu +13=%llu +14=%llu +16=0x%llx | "
                        "r8+18=0x%llx r9=0x%llx stack1=0x%llx => mode=%u\n",
                (unsigned long long)d10, (unsigned long long)d13,
                (unsigned long long)d14, (unsigned long long)d16,
                (unsigned long long)e18, (unsigned long long)r9,
                (unsigned long long)s1, orig);
        if (mode != orig) {
            fprintf(stderr, "glswz:   -> override %u to %u\n", orig, mode);
        }
        if (gDumpBudget > 0 && desc) {
            gDumpBudget--;
            const uint64_t *q = (const uint64_t *)desc;
            fprintf(stderr, "glswz:   desc");
            for (int i = 0; i < 8; i++)
                fprintf(stderr, " %02x:%016llx", i * 8, (unsigned long long)q[i]);
            fprintf(stderr, "\n");
        }
        fflush(stderr);
    }
    return mode;
}


// ---------------------------------------------------------------------------
// The kext's own mechanism, in userspace. Same order as rbgpu_mtlk_place():
// read first, write only on an exact match of `find` (or find `repl` already
// there), and the CAVE is verified present before the detour that jumps into
// it is written. A row that is wrong for this build writes nothing.
static int place_one(unsigned char *at, const uint8_t *find,
                     const uint8_t *repl, unsigned int len, const char *what)
{
    if (!memcmp(at, repl, len)) {
        fprintf(stderr, "glswz: %s already placed at %p\n", what, (void *)at);
        return 1;
    }
    if (memcmp(at, find, len)) {
        fprintf(stderr, "glswz: %s at %p is NEITHER the original nor ours - "
                        "writing nothing (the row is for another build)\n",
                what, (void *)at);
        return 0;
    }
    vm_address_t pg = (vm_address_t)at & ~(vm_address_t)(getpagesize() - 1);
    vm_size_t span = ((vm_address_t)at + len) - pg;
    kern_return_t kr = vm_protect(mach_task_self(), pg, span, FALSE,
                                  VM_PROT_READ | VM_PROT_WRITE | VM_PROT_COPY);
    if (kr != KERN_SUCCESS) {
        fprintf(stderr, "glswz: %s vm_protect(rw) failed with %d - nothing "
                        "written\n", what, kr);
        return 0;
    }
    memcpy(at, repl, len);
    vm_protect(mach_task_self(), pg, span, FALSE,
               VM_PROT_READ | VM_PROT_EXECUTE);
    int ok = memcmp(at, repl, len) == 0;
    fprintf(stderr, "glswz: %s written at %p, verify %s\n", what, (void *)at,
            ok ? "ok" : "FAILED");
    return ok;
}

static void place_cave(unsigned char *base)
{
    unsigned char *cave = base + RB_GLCAVE_CAVE_OFF - RB_GLCAVE_ANCHOR;
    unsigned char *fn   = base + RB_GLCAVE_FN_OFF;
    fprintf(stderr, "glswz: cave row - fn_off 0x%x cave_off 0x%x anchor %u "
                    "cave_len %u fn_len %u\n",
            RB_GLCAVE_FN_OFF, RB_GLCAVE_CAVE_OFF, RB_GLCAVE_ANCHOR,
            RB_GLCAVE_CAVE_LEN, RB_GLCAVE_FN_LEN);
    if (!place_one(cave, rb_glcave_cave_find, rb_glcave_cave_repl,
                   RB_GLCAVE_CAVE_LEN, "CAVE")) {
        fprintf(stderr, "glswz: the cave is not in place, so the DETOUR is "
                        "not written - it would jump into padding\n");
        return;
    }
    place_one(fn, rb_glcave_fn_find, rb_glcave_fn_repl,
              RB_GLCAVE_FN_LEN, "DETOUR");
    fflush(stderr);
}

static void patch_image(const struct mach_header *mh, intptr_t slide)
{
    (void)slide;
    if (gDone) { return; }
    Dl_info info;
    if (!dladdr((const void *)mh, &info) || !info.dli_fname) { return; }
    if (!strstr(info.dli_fname, "AMDRadeonX6000GLDriver")) { return; }

    const char *m = getenv("RBGPU_GLSWIZZLE");
    if (m && !strcmp(m, "off")) { gMode = G_OFF; }
    else if (m && !strcmp(m, "nox")) { gMode = G_NOX; }
    else if (m && !strcmp(m, "cave")) { gMode = G_CAVE; }
    if (getenv("RBGPU_GLSWIZZLE_DUMP")) {
        gDumpBudget = atoi(getenv("RBGPU_GLSWIZZLE_DUMP"));
    }

    const struct mach_header_64 *h = (const struct mach_header_64 *)mh;
    const unsigned char *cmd = (const unsigned char *)mh + sizeof(*h);
    unsigned char *base = NULL; unsigned long size = 0;
    for (uint32_t i = 0; i < h->ncmds; i++) {
        const struct load_command *lc = (const struct load_command *)cmd;
        if (lc->cmd == LC_SEGMENT_64) {
            const struct segment_command_64 *s =
                (const struct segment_command_64 *)cmd;
            if (!strcmp(s->segname, "__TEXT")) {
                base = (unsigned char *)(s->vmaddr + slide);
                size = (unsigned long)s->vmsize;
            }
        }
        cmd += lc->cmdsize;
    }
    if (!base || size < sizeof kFind) {
        fprintf(stderr, "glswz: GL driver found but no usable __TEXT - "
                        "nothing patched\n");
        gDone = 1; return;
    }

    // Rule 22's companion, and rule 26: a search is an instrument. Exactly one
    // occurrence or nothing is touched, and the count is printed either way.
    unsigned char *fn = NULL; int hits = 0;
    for (unsigned long o = 0; o + sizeof kFind <= size; o++) {
        if (!memcmp(base + o, kFind, sizeof kFind)) { hits++; fn = base + o; }
    }
    if (hits != 1) {
        fprintf(stderr, "glswz: REFUSING - %d occurrences of the chooser's "
                        "prologue, expected exactly 1. Nothing patched.\n", hits);
        gDone = 1; return;
    }
    fprintf(stderr, "glswz: GL driver at %p, chooser at %p (file offset "
                    "0x%lx), mode=%s\n", (void *)base, (void *)fn,
            (unsigned long)(fn - base),
            gMode == G_OFF ? "off" : gMode == G_NOX ? "nox"
                                   : gMode == G_CAVE ? "cave" : "log");
    if (gMode == G_OFF) {
        fprintf(stderr, "glswz: mode=off - the chooser is deliberately NOT "
                        "patched\n");
        gDone = 1; return;
    }

    if (gMode == G_CAVE) {
        place_cave(base);
        gDone = 1; return;
    }

    size_t tsz = (size_t)getpagesize();
    unsigned char *t = mmap(NULL, tsz, PROT_READ | PROT_WRITE,
                            MAP_PRIVATE | MAP_ANON, -1, 0);
    if (t == MAP_FAILED) {
        fprintf(stderr, "glswz: mmap for the trampoline failed - nothing "
                        "patched\n");
        gDone = 1; return;
    }
    memcpy(t, fn, PROLOGUE_COPY);
    uint64_t back = (uint64_t)(fn + PROLOGUE_COPY);
    unsigned char *p = t + PROLOGUE_COPY;
    *p++ = 0x49; *p++ = 0xBB; memcpy(p, &back, 8); p += 8;   // movabs r11, back
    *p++ = 0x41; *p++ = 0xFF; *p++ = 0xE3;                   // jmp *r11
    if (mprotect(t, tsz, PROT_READ | PROT_EXEC) != 0) {
        fprintf(stderr, "glswz: mprotect(rx) on the trampoline failed - "
                        "nothing patched\n");
        gDone = 1; return;
    }
    gTramp = (glswz_fn)t;

    unsigned char det[PROLOGUE_COPY];
    memset(det, 0x90, sizeof det);
    uint64_t target = (uint64_t)&hook;
    det[0] = 0x49; det[1] = 0xBB; memcpy(det + 2, &target, 8);
    det[10] = 0x41; det[11] = 0xFF; det[12] = 0xE3;

    vm_address_t pg = (vm_address_t)fn & ~(vm_address_t)(getpagesize() - 1);
    vm_size_t span = ((vm_address_t)fn + sizeof det) - pg;
    kern_return_t kr = vm_protect(mach_task_self(), pg, span, FALSE,
                                  VM_PROT_READ | VM_PROT_WRITE | VM_PROT_COPY);
    if (kr != KERN_SUCCESS) {
        fprintf(stderr, "glswz: vm_protect(rw) failed with %d - nothing "
                        "patched\n", kr);
        gDone = 1; return;
    }
    memcpy(fn, det, sizeof det);
    kr = vm_protect(mach_task_self(), pg, span, FALSE,
                    VM_PROT_READ | VM_PROT_EXECUTE);
    if (kr != KERN_SUCCESS) {
        fprintf(stderr, "glswz: vm_protect(rx) failed with %d AFTER writing - "
                        "the detour is in place but the page is not "
                        "executable\n", kr);
        gDone = 1; return;
    }
    fprintf(stderr, "glswz: HOOKED glrAMD_GFX10_DetermineSwizzleMode at %p "
                    "(verify %s) - mode=%s%s\n",
            (void *)fn, memcmp(fn, det, sizeof det) == 0 ? "ok" : "FAILED",
            gMode == G_NOX ? "nox" : "log",
            gMode == G_NOX ? " - _X modes ARE being overridden"
                           : " - this mode OVERRIDES NOTHING");
    fflush(stderr);
    gDone = 1;
}

__attribute__((constructor))
static void init(void)
{
    _dyld_register_func_for_add_image(patch_image);
}
