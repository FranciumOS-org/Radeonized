// mtlswizzle.c (2nq) - a *trampoline* hook on amdMtl_HWL_DetermineSwizzleMode,
// where tools/mtllinear.c only had a constant-return stub.
//
// Why this exists. 2np fixed Claude's text by forcing the chooser to return
// ADDR_SW_LINEAR in the app process. Applying the same stub to IntelliJ IDEA
// crashed it in 3.6 s:
//
//     SIGSEGV (si_addr 0x68) in libawt_lwawt.dylib
//     -[EncoderManager getEncoder:renderOptions:]+0x1b2
//     <- MTLRenderer_FillSpans <- sun.java2d.metal.MTLRenderQueue.flushBuffer
//
// The two apps differ in what they draw INTO. Electron/Skia renders to
// IOSurface-backed textures, which take NewTextureWithDescriptor2 and never
// reach this chooser (2nl), and uses a plain MTLTexture only for its glyph
// atlas - so LINEAR fixed the atlas and touched no render target. JetBrains
// Runtime renders every Swing surface into a plain MTLTexture, so the blanket
// stub made each one linear, newRenderPipelineState returned nil and JBR
// dereferenced it. A linear surface is not a legal render target.
//
// So the fix has to be selective, and a stub that cannot call the original
// cannot be selective. This installs a detour instead:
//
//   * copy the first 30 bytes of the prologue (a whole number of
//     instructions - the 32-byte kFind pattern of 2lm cuts the `movq %rdx,
//     %r14` in half, and a trampoline must not) into an RX page, followed by
//     an absolute jump back to original+30;
//   * overwrite original+0 with `movabs %r11, hook; jmp *%r11` (13 bytes,
//     absolute because the shared cache and this dylib are not within rel32);
//   * the hook has the function's real signature, so calling the trampoline
//     is an ordinary C call. SysV puts the 3 stack arguments back at exactly
//     the offsets the original prologue reads them from.
//
// Rule 16: the instrument is a suspect, so the default mode CHANGES NOTHING
// and only logs what the driver already decided. Rule 14: every path says
// something.
//
//   RBGPU_SWIZZLE=log       (default) observe only - log args and the mode
//   RBGPU_SWIZZLE=linear    force ADDR_SW_LINEAR always (= 2np's stub)
//   RBGPU_SWIZZLE=auto      LINEAR for non-render-targets, render targets
//                           left exactly as the driver chose
//   RBGPU_SWIZZLE=fix       LINEAR for non-render-targets, and render targets
//                           mapped from their _X mode to the non-X equivalent
//   RBGPU_SWIZZLE=nox       every _X mode mapped to its non-X equivalent
//   RBGPU_SWIZZLE=fixmip    as `fix`, except a non-render-target WITH A MIP
//                           CHAIN is not made linear: it takes the render
//                           target rule (_X to non-X) instead. The dry run
//                           for 311 - `fix` leaves levels of a mipmapped
//                           sample-only texture reading back as nothing.
//   RBGPU_SWIZZLE=off       inject but do not patch at all (control)
//
//   RBGPU_SWIZZLE_NONRT=n   explicit mode for non-render-targets (overrides)
//   RBGPU_SWIZZLE_RT=n      explicit mode for render targets (overrides)
//   RBGPU_USAGE_OFF=0x68    byte offset of `usage` (measured, see below)
//   RBGPU_USAGE_RT=4        bit mask meaning "render target"
//   RBGPU_SWIZZLE_DUMP=N    hexdump the descriptor for the first N distinct calls
//   RBGPU_GBADDRCONFIG=0x.. feed AddrLib this GB_ADDR_CONFIG instead of the
//                           kernel's. This is the dry run for a kext-side fix:
//                           the value reaches AddrLib by exactly the path the
//                           kext controls, so a value that works here would
//                           work in every process without any injection.
//
// The descriptor layout, from the disassembly and then CONFIRMED by
// tools/usageprobe.m creating one texture per usage value (2nq):
//   +0x00 textureType   +0x08 pixelFormat  +0x10 width   +0x18 height
//   +0x20 depth         +0x28 mipLevels    +0x30 samples +0x38 arrayLength
//   +0x68 usage as REQUESTED   <- 5/5 cases tracked the value set, and the
//                                function itself tests `0x68(%r14) & 6`
//   +0xb0 usage as RESOLVED    <- identical to +0x68 whenever usage is set
//                                explicitly, which is why usageprobe alone
//                                could not tell them apart. IntelliJ did:
//                                JBR leaves usage unset, so +0x68 reads 0
//                                while +0xb0 reads 0x17 (Metal expands an
//                                unspecified usage to all of them). Keying on
//                                +0x68 therefore calls a render target "not a
//                                render target" - it did not crash, but only
//                                by luck. The hook uses the OR of the two.
//
// Why non-X is a candidate for the RIGHT answer rather than a correction.
// The modes the driver picks here are 4KB_D_X, 64KB_R_X, 64KB_Z_X - the `_X`
// suffix means the swizzle applies a pipe-bank XOR derived from the chip's
// pipe/bank configuration. This chip reads GB_ADDR_CONFIG 0x242 (4 pipes, 4
// packers, 1 SE) where Apple's driver is built for Navi 21, and 2na measured
// the corruption as 8x8 micro tiles INTACT with only their placement wrong -
// which is what a pipe-bank XOR computed from the wrong pipe count looks
// like. A non-X mode applies no pipe-bank XOR at all, so it is tiled,
// config-independent, and still a legal render target. Z modes are left
// alone: depth needs _X for HTILE and is neither displayed nor CPU-read.
//
// build: make mtlswizzle
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>
#include <dlfcn.h>
#include <mach/mach.h>
#include <mach-o/dyld.h>
#include <mach-o/loader.h>
#include <sys/mman.h>
#include <unistd.h>
#include <pthread.h>

// 32 bytes for UNIQUENESS (2lm measured 14 bytes = 2099 hits, 32 bytes = 1),
// but only PROLOGUE_COPY of them are a whole number of instructions.
static const unsigned char kFind[32] = {
    0x55, 0x48, 0x89, 0xE5, 0x41, 0x57, 0x41, 0x56,
    0x41, 0x55, 0x41, 0x54, 0x53, 0x48, 0x81, 0xEC,
    0x88, 0x00, 0x00, 0x00, 0x45, 0x89, 0xCD, 0x44,
    0x89, 0xC3, 0x48, 0x89, 0x4D, 0xD0, 0x49, 0x89,
};
// push rbp / mov rbp,rsp / push r15,r14,r13,r12,rbx / sub rsp,0x88 /
// mov r13d,r9d / mov ebx,r8d / mov [rbp-0x30],rcx   == exactly 30 bytes.
#define PROLOGUE_COPY 30

// amdMtl_HWL_InitAddrLibHwRegFields in full - 22 bytes, self-contained:
//     push rbp; mov rbp,rsp
//     movl $0xd, 0x4(%rdi)          ADDR_CREATE_INPUT.chipFamily = GFX10
//     movl 0xa4(%rsi),%eax
//     movl %eax, 0x30(%rdi)         regValue.gbAddrConfig
//     pop rbp; ret
// This is the ONLY place AddrLib is told anything about the hardware, and the
// value comes from AMDCommonHwInfoRec+0xa4, which the kext fills via
// AMDRadeonX6000_AMDAccelDevice::getHardwareInfo. Replacing it with a
// constant here is a faithful dry run of changing it kernel-side: whatever
// value works in this process would work in EVERY process if the kext
// reported it, platform binaries included - which is the only route to a fix
// that needs no injection at all (2nt).
static const unsigned char kAddrFind[22] = {
    0x55, 0x48, 0x89, 0xE5,
    0xC7, 0x47, 0x04, 0x0D, 0x00, 0x00, 0x00,
    0x8B, 0x86, 0xA4, 0x00, 0x00, 0x00,
    0x89, 0x47, 0x30,
    0x5D, 0xC3,
};

typedef unsigned int (*swz_fn)(void *hw, void *addrlib, const void *desc,
                              const void *settings, unsigned int samples,
                              unsigned int frags, unsigned int *out,
                              bool b8, bool b9);

enum { M_LOG, M_LINEAR, M_AUTO, M_FIX, M_NOX, M_OFF, M_FIXMIP };
static int gMode = M_LOG;
static int gNonRT = -1, gRT = -1;      // explicit overrides, -1 = per-mode default
static unsigned long gUsageOff  = 0x68; // usage as requested
static unsigned long gUsageOff2 = 0xb0; // usage as resolved by Metal
static unsigned long gUsageRT = 4;      // MTLTextureUsageRenderTarget
static int gDumpBudget = 8;

static swz_fn gTramp;
static int gDone;
static int gSaidHw;
static long gGbAddrConfig = -1;        // RBGPU_GBADDRCONFIG, -1 = leave alone

// ---- dedup table so a per-draw function does not flood the log ----------
struct key { uint64_t type, fmt, w, h, mip, smp, arr; unsigned int mode; };
#define NKEYS 192
static struct key gSeen[NKEYS];
static int gNSeen;
static pthread_mutex_t gLock = PTHREAD_MUTEX_INITIALIZER;

// _X -> non-X. Z modes (20, 24) deliberately map to themselves.
static unsigned int nox(unsigned int m)
{
    switch (m) {
    case 21: return 5;   // 4KB_S_X  -> 4KB_S
    case 22: return 6;   // 4KB_D_X  -> 4KB_D
    case 23: return 6;   // 4KB_R_X  -> 4KB_D
    case 25: return 9;   // 64KB_S_X -> 64KB_S
    case 26: return 10;  // 64KB_D_X -> 64KB_D
    case 27: return 10;  // 64KB_R_X -> 64KB_D
    default: return m;   // already non-X, or a Z mode we do not touch
    }
}

static unsigned int hook(void *hw, void *al, const void *desc, const void *st,
                         unsigned int samples, unsigned int frags,
                         unsigned int *out, bool b8, bool b9)
{
    unsigned int orig = gTramp(hw, al, desc, st, samples, frags, out, b8, b9);
    const uint64_t *d = (const uint64_t *)desc;
    struct key k = { d[0], d[1], d[2], d[3], d[5], d[6], d[7], orig };

    pthread_mutex_lock(&gLock);
    int fresh = 1;
    for (int i = 0; i < gNSeen; i++)
        if (memcmp(&gSeen[i], &k, sizeof k) == 0) { fresh = 0; break; }
    if (fresh && gNSeen < NKEYS) gSeen[gNSeen++] = k;
    int dump = fresh && gDumpBudget > 0 ? gDumpBudget-- : 0;
    pthread_mutex_unlock(&gLock);

    if (fresh && !gSaidHw) {
        // amdMtl_HWL_InitAddrLibHwRegFields is four instructions:
        //     movl $0xd, 0x4(%rdi)        chipFamily = GFX10
        //     movl 0xa4(%rsi), %eax
        //     movl %eax, 0x30(%rdi)       regValue.gbAddrConfig
        // so AddrLib's ENTIRE hardware configuration is AMDCommonHwInfoRec+0xa4,
        // and that record is filled by gfx10_HwInfoInitData from the kernel's
        // _sAMD_GET_HW_INFO_VALUES. This chip's GB_ADDR_CONFIG reads 0x242
        // (4 pipes, 4 packers, 1 SE). If the two disagree, every `_X` swizzle
        // in every process is computed from the wrong pipe config, and the
        // place to fix it is the kext - once, for everyone.
        gSaidHw = 1;
        unsigned int cfg = *(const unsigned int *)((const char *)hw + 0xa4);
        fprintf(stderr, "swz: AddrLib gbAddrConfig (hwInfo+0xa4) = 0x%x"
                        "  pipes=%u pkrs=%u se=%u interleave=%u\n",
                cfg, 1u << (cfg & 7), 1u << ((cfg >> 8) & 7),
                1u << ((cfg >> 19) & 3), 256u << ((cfg >> 3) & 7));
        fprintf(stderr, "swz:   hardware GB_ADDR_CONFIG is 0x242 -> pipes=4 "
                        "pkrs=4 se=1 interleave=256  (%s)\n",
                cfg == 0x242 ? "AGREES - the _X theory is wrong"
                             : "DISAGREES - this is the global defect");
    }
    if (fresh) {
        // 311: usage on the line itself. It was only in the hexdump, which the
        // budget stops after eight descriptors, so "is this one a render
        // target" could not be read for anything an app made after start-up.
        fprintf(stderr, "swz: type=%llu fmt=%-3llu %llux%llu depth=%llu mip=%llu "
                        "smp=%llu arr=%llu use=0x%llx | samples=%u frags=%u b8=%d b9=%d "
                        "=> mode=%u out=%u\n",
                d[0], d[1], d[2], d[3], d[4], d[5], d[6], d[7],
                *(const uint64_t *)((const char *)desc + gUsageOff) |
                *(const uint64_t *)((const char *)desc + gUsageOff2),
                samples, frags, (int)b8, (int)b9, orig, out ? *out : 0u);
        if (dump) {
            fprintf(stderr, "swz:   desc");
            for (int i = 0; i < 26; i++)
                fprintf(stderr, " %02x:%016llx", i * 8, d[i]);
            fprintf(stderr, "\n");
        }
        fflush(stderr);
    }

    if (gMode == M_LOG || gMode == M_OFF) return orig;
    if (gMode == M_LINEAR) return 0;

    uint64_t usage = *(const uint64_t *)((const char *)desc + gUsageOff) |
                     *(const uint64_t *)((const char *)desc + gUsageOff2);
    int isRT = (usage & gUsageRT) != 0;
    unsigned int want = orig;
    if (gMode == M_NOX) want = nox(orig);
    else if (isRT) want = (gMode == M_FIX || gMode == M_FIXMIP) ? nox(orig) : orig;
    else if (gMode == M_FIXMIP && d[5] > 1) want = nox(orig);  // mipmapped
    else want = 0;                                     // non-render-target
    if (isRT) { if (gRT >= 0) want = (unsigned int)gRT; }
    else      { if (gNonRT >= 0) want = (unsigned int)gNonRT; }

    if (fresh && want != orig)
        fprintf(stderr, "swz:   -> override %u to %u (usage=0x%llx, %s)\n",
                orig, want, usage, isRT ? "render target" : "not a render target");
    return want;
}

static void patch_image(const struct mach_header *mh, intptr_t slide)
{
    if (gDone) return;
    Dl_info info;
    if (!dladdr((const void *)mh, &info) || !info.dli_fname) return;
    if (!strstr(info.dli_fname, "AMDRadeonX6000MTLDriver")) return;

    const char *m = getenv("RBGPU_SWIZZLE");
    if (m && !strcmp(m, "linear")) gMode = M_LINEAR;
    else if (m && !strcmp(m, "auto")) gMode = M_AUTO;
    else if (m && !strcmp(m, "fix")) gMode = M_FIX;
    else if (m && !strcmp(m, "nox")) gMode = M_NOX;
    else if (m && !strcmp(m, "fixmip")) gMode = M_FIXMIP;
    else if (m && !strcmp(m, "off")) gMode = M_OFF;
    if (getenv("RBGPU_USAGE_OFF")) gUsageOff = strtoul(getenv("RBGPU_USAGE_OFF"), 0, 0);
    if (getenv("RBGPU_USAGE_OFF2")) gUsageOff2 = strtoul(getenv("RBGPU_USAGE_OFF2"), 0, 0);
    if (getenv("RBGPU_USAGE_RT"))  gUsageRT  = strtoul(getenv("RBGPU_USAGE_RT"), 0, 0);
    if (getenv("RBGPU_SWIZZLE_DUMP")) gDumpBudget = atoi(getenv("RBGPU_SWIZZLE_DUMP"));
    if (getenv("RBGPU_SWIZZLE_NONRT")) gNonRT = atoi(getenv("RBGPU_SWIZZLE_NONRT"));
    if (getenv("RBGPU_SWIZZLE_RT"))    gRT    = atoi(getenv("RBGPU_SWIZZLE_RT"));
    if (getenv("RBGPU_GBADDRCONFIG"))
        gGbAddrConfig = strtol(getenv("RBGPU_GBADDRCONFIG"), 0, 0);
    if (gMode == M_OFF)
        fprintf(stderr, "swz: mode=off - the chooser is deliberately NOT patched\n");

    const struct mach_header_64 *h = (const struct mach_header_64 *)mh;
    const unsigned char *cmd = (const unsigned char *)mh + sizeof(*h);
    unsigned char *base = NULL; unsigned long size = 0;
    for (uint32_t i = 0; i < h->ncmds; i++) {
        const struct load_command *lc = (const struct load_command *)cmd;
        if (lc->cmd == LC_SEGMENT_64) {
            const struct segment_command_64 *s = (const struct segment_command_64 *)cmd;
            if (!strcmp(s->segname, "__TEXT")) {
                base = (unsigned char *)(s->vmaddr + slide); size = s->vmsize;
            }
        }
        cmd += lc->cmdsize;
    }
    if (!base || size < sizeof kFind) {
        fprintf(stderr, "swz: driver found but no usable __TEXT - nothing patched\n");
        gDone = 1; return;
    }

    // Rule 22's companion: refuse on anything but exactly one match.
    unsigned char *fn = NULL; int hits = 0;
    for (unsigned long o = 0; o + sizeof kFind <= size; o++)
        if (!memcmp(base + o, kFind, sizeof kFind)) { hits++; fn = base + o; }
    if (hits != 1) {
        fprintf(stderr, "swz: REFUSING - %d occurrences of the prologue, "
                        "expected exactly 1. Nothing patched.\n", hits);
        gDone = 1; return;
    }

    // ---- the gbAddrConfig override, installed BEFORE anything creates
    // AddrLib. It is independent of the chooser policy, so the two can be
    // tested one at a time (one variable per experiment).
    if (gGbAddrConfig >= 0) {
        unsigned char *af = NULL; int ahits = 0;
        for (unsigned long o = 0; o + sizeof kAddrFind <= size; o++)
            if (!memcmp(base + o, kAddrFind, sizeof kAddrFind)) { ahits++; af = base + o; }
        if (ahits != 1) {
            fprintf(stderr, "swz: REFUSING the gbAddrConfig override - %d "
                            "occurrences of InitAddrLibHwRegFields, expected 1\n", ahits);
        } else {
            unsigned char st[sizeof kAddrFind];
            memset(st, 0x90, sizeof st);
            unsigned int v = (unsigned int)gGbAddrConfig;
            unsigned char *q = st;
            *q++ = 0xC7; *q++ = 0x47; *q++ = 0x04;            // movl $0xd, 0x4(%rdi)
            *q++ = 0x0D; *q++ = 0x00; *q++ = 0x00; *q++ = 0x00;
            *q++ = 0xC7; *q++ = 0x47; *q++ = 0x30;            // movl $v, 0x30(%rdi)
            memcpy(q, &v, 4); q += 4;
            *q++ = 0xC3;                                      // ret
            vm_address_t ap = (vm_address_t)af & ~(vm_address_t)(getpagesize() - 1);
            vm_size_t asp = ((vm_address_t)af + sizeof st) - ap;
            kern_return_t akr = vm_protect(mach_task_self(), ap, asp, FALSE,
                                           VM_PROT_READ | VM_PROT_WRITE | VM_PROT_COPY);
            if (akr != KERN_SUCCESS) {
                fprintf(stderr, "swz: vm_protect(rw) for gbAddrConfig failed with %d - "
                                "AddrLib keeps the kernel's value\n", akr);
            } else {
                memcpy(af, st, sizeof st);
                vm_protect(mach_task_self(), ap, asp, FALSE, VM_PROT_READ | VM_PROT_EXECUTE);
                fprintf(stderr, "swz: gbAddrConfig FORCED to 0x%x at %p (verify %s)"
                                "  pipes=%u pkrs=%u se=%u interleave=%u\n",
                        v, (void *)af,
                        memcmp(af, st, sizeof st) == 0 ? "ok" : "FAILED",
                        1u << (v & 7), 1u << ((v >> 8) & 7),
                        1u << ((v >> 19) & 3), 256u << ((v >> 3) & 7));
            }
        }
    }

    if (gMode == M_OFF) { gDone = 1; return; }

    // ---- trampoline: original prologue + absolute jump past the detour ----
    size_t tsz = (size_t)getpagesize();
    unsigned char *t = mmap(NULL, tsz, PROT_READ | PROT_WRITE,
                            MAP_PRIVATE | MAP_ANON, -1, 0);
    if (t == MAP_FAILED) {
        fprintf(stderr, "swz: mmap for the trampoline failed - nothing patched\n");
        gDone = 1; return;
    }
    memcpy(t, fn, PROLOGUE_COPY);
    uint64_t back = (uint64_t)(fn + PROLOGUE_COPY);
    unsigned char *p = t + PROLOGUE_COPY;
    *p++ = 0x49; *p++ = 0xBB; memcpy(p, &back, 8); p += 8;   // movabs r11, back
    *p++ = 0x41; *p++ = 0xFF; *p++ = 0xE3;                   // jmp *r11
    if (mprotect(t, tsz, PROT_READ | PROT_EXEC) != 0) {
        fprintf(stderr, "swz: mprotect(rx) on the trampoline failed - nothing patched\n");
        gDone = 1; return;
    }
    gTramp = (swz_fn)t;

    // ---- the detour itself ----
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
        fprintf(stderr, "swz: vm_protect(rw) failed with %d - nothing patched\n", kr);
        gDone = 1; return;
    }
    memcpy(fn, det, sizeof det);
    kr = vm_protect(mach_task_self(), pg, span, FALSE, VM_PROT_READ | VM_PROT_EXECUTE);
    if (kr != KERN_SUCCESS) {
        fprintf(stderr, "swz: vm_protect(rx) failed with %d AFTER writing - the "
                        "detour is in place but the page is not executable\n", kr);
        gDone = 1; return;
    }
    int ok = memcmp(fn, det, sizeof det) == 0;
    // One name per M_* value, in enum order. 311 added a mode and not a name, and
    // names[gMode] then read one slot past the table: an empty string on the
    // first run and a SIGSEGV inside fprintf on the next.
    static const char *names[] = { "log", "linear", "auto", "fix", "nox", "off", "fixmip" };
    _Static_assert(sizeof names / sizeof names[0] == M_FIXMIP + 1, "one name per mode");
    fprintf(stderr, "swz: HOOKED DetermineSwizzleMode at %p (verify %s) mode=%s",
            (void *)fn, ok ? "ok" : "FAILED", names[gMode]);
    if (gMode != M_LOG && gMode != M_LINEAR)
        fprintf(stderr, " usage@(0x%lx|0x%lx) & 0x%lx", gUsageOff, gUsageOff2, gUsageRT);
    if (gNonRT >= 0) fprintf(stderr, " nonRT=%d", gNonRT);
    if (gRT >= 0)    fprintf(stderr, " RT=%d", gRT);
    fprintf(stderr, "\n");
    fflush(stderr);
    gDone = 1;
}

__attribute__((constructor))
static void mtlswizzle_init(void) { _dyld_register_func_for_add_image(patch_image); }
