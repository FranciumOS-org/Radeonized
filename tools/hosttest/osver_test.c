/* The OS support matrix (2xz). Like the default-on table, the matrix IS data,
 * so this checks the data and the one decision function that reads it.
 *
 * What these assertions are for: a row that claims an artifact it has not
 * measured is indistinguishable, from inside the kext, from a row that has -
 * and the consequence is a measured constant written on an OS it was never
 * measured on, which is rule 17. So the claims are pinned here, with the
 * evidence named, and a change to any of them has to come through this file. */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>

#include "rb_osver.h"
/* 2xz: the cave rows are checked AGAINST the matrix here, because they are
 * the one artifact whose claim bit and whose data live in two different
 * files. A row with no bit, or a bit with no row, is drift that nothing else
 * in the build would notice. */
#include "rbgpu_mtlcave.h"
#include "rb_glcave.h"

#define CHECK(cond, what) do { \
    printf((cond) ? "ok   %s\n" : "FAIL %s\n", (what)); \
    failures += !(cond); \
} while (0)

int osver_tests(void);

#if defined(__x86_64__)

/* 3gl-d: THE OPENGL CAVE, RUN THE SAME WAY.
 *
 * It is a second cave in a second image - AMDRadeonX6000GLDriver, which an
 * OpenGL client draws through and which is NOT in the dyld shared cache - and
 * its policy is `nox` alone, with no LINEAR term, because nothing has located
 * `usage` among that chooser's arguments (3gl-c). So the model here is the
 * table and nothing else, and the test executes the emitted bytes rather than
 * describing them.
 *
 * The chooser takes at least seven arguments, four of which the body reads,
 * so the cave copies three stack arguments before calling the trampoline.
 * This type passes nine, which is what makes those copies legal to perform.
 */
typedef unsigned (*rb_glcave_fn)(uint64_t, uint64_t, uint64_t, const void *,
                                 const void *, uint64_t, uint64_t, uint64_t,
                                 uint64_t);

/* The `call TRAMP` in gen_glcave.py's build_cave(): push rbp(1) mov(3)
 * push rbx(1) sub(4) and three load/store pairs (4+4, 4+5, 4+5) = 35. */
#define RB_GLCAVE_CALL_AT 35u

static uint8_t *glcave_map(uint32_t *tramp)
{
    const uint8_t *code = rb_glcave_cave_repl + RB_GLCAVE_ANCHOR;
    uint32_t used = RB_GLCAVE_CAVE_LEN - RB_GLCAVE_ANCHOR;
    int32_t rel;
    uint8_t *m;

    if (used < RB_GLCAVE_CALL_AT + 5u || code[RB_GLCAVE_CALL_AT] != 0xE8) {
        return 0;
    }
    memcpy(&rel, code + RB_GLCAVE_CALL_AT + 1, 4);
    *tramp = (uint32_t)((int32_t)(RB_GLCAVE_CALL_AT + 5u) + rel);
    if (*tramp + 6u > used) {
        return 0;
    }
    m = mmap(0, 4096, PROT_READ | PROT_WRITE | PROT_EXEC,
             MAP_PRIVATE | MAP_ANON, -1, 0);
    if (m == MAP_FAILED) {
        return 0;
    }
    memcpy(m, code, used);
    return m;
}

static unsigned glcave_ask(uint8_t *m, uint32_t tramp, unsigned orig)
{
    m[tramp] = 0xB8;                         /* mov eax, orig */
    memcpy(m + tramp + 1, &orig, 4);
    m[tramp + 5] = 0xC3;                     /* ret */
    return ((rb_glcave_fn)(void *)m)(0, 0, 0, 0, 0, 0, 0, 0, 0);
}

/* The table written out, from the journal's statement of it and not from the
 * emitted bytes. Z modes and every non-_X mode are left alone. */
static unsigned glcave_model(unsigned orig)
{
    switch (orig) {
    case 21: return 5;
    case 22: return 6;
    case 23: return 6;
    case 25: return 9;
    case 26: return 10;
    case 27: return 10;
    default: return orig;
    }
}
#endif

/* 3mip: THE CAVE IS CODE, SO RUN IT.
 *
 * Until 3mip nothing here executed a cave. Every check above is about where
 * the bytes go and none is about what they decide, so a policy that emptied
 * every mipmapped ShaderRead-only texture on the machine - Spotify's album
 * covers - passed this file for a year. The policy is 96 bytes of x86 with
 * one external reference (the trampoline into the driver), so it can be run
 * right here: map the row's cave executable, overwrite its trampoline with
 * `mov eax, <mode>; ret` standing in for the driver's own choice, and call it
 * with a descriptor the way the driver would. What comes back is what a
 * process on that OS would be told.
 *
 * The descriptor offsets are 2nq's, measured: mipLevels +0x28, sample count
 * +0x30 (312), usage as requested +0x68, usage as resolved +0xb0. */
#if defined(__x86_64__)
typedef unsigned (*rb_cave_fn)(void *, void *, const void *, const void *,
                               unsigned, unsigned, unsigned *, bool, bool);

#define RB_CAVE_CALL_AT 38u     /* the `call TRAMP` in build_cave()'s wrapper */

static uint8_t *cave_map(const struct rb_mtlcave_row *r, uint32_t *tramp)
{
    const uint8_t *code = r->cave_repl + r->cave_anchor;
    int32_t rel;
    uint8_t *m;

    if (r->cave_used < RB_CAVE_CALL_AT + 5u || code[RB_CAVE_CALL_AT] != 0xE8) {
        return 0;
    }
    memcpy(&rel, code + RB_CAVE_CALL_AT + 1, 4);
    *tramp = (uint32_t)((int32_t)(RB_CAVE_CALL_AT + 5u) + rel);
    if (*tramp + 6u > r->cave_used) {
        return 0;
    }
    m = mmap(0, 4096, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (m == MAP_FAILED) {
        return 0;
    }
    memcpy(m, code, r->cave_used);
    return m;
}

static unsigned cave_ask(uint8_t *m, uint32_t tramp, unsigned orig,
                         uint64_t use68, uint64_t useb0, uint64_t mips,
                         uint64_t samples)
{
    uint64_t desc[32];
    unsigned out = 0;

    memset(desc, 0, sizeof(desc));
    desc[0x28 / 8] = mips;
    desc[0x30 / 8] = samples;
    desc[0x68 / 8] = use68;
    desc[0xb0 / 8] = useb0;
    m[tramp] = 0xB8;                         /* mov eax, orig */
    memcpy(m + tramp + 1, &orig, 4);
    m[tramp + 5] = 0xC3;                     /* ret */
    return ((rb_cave_fn)(void *)m)(0, 0, desc, 0, 1, 0, &out, false, false);
}

/* The policy in C, written from the journal's statement of it and not from
 * the bytes: LINEAR for a flat non-render-target; a multisampled surface that
 * is not made linear keeps the driver's mode untouched (312); everything else
 * keeps the driver's mode with the _X family mapped to its non-X equivalent. */
static unsigned cave_model(unsigned orig, uint64_t usage, uint64_t mips,
                           uint64_t samples)
{
    static const unsigned char nox[28] = {
        0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19,
        20, 5, 6, 6, 24, 9, 10, 10,
    };

    if (!(usage & 4) && mips <= 1) {
        return 0;
    }
    if (samples >= 2) {
        return orig;
    }
    return orig < 28 ? nox[orig] : orig;
}
#endif


int osver_tests(void)
{
    int failures = 0;
    int i, j;

    /* --- the range ---------------------------------------------------- */

    CHECK(RB_OS_MIN_DARWIN == RB_OS_VENTURA,
          "the floor is Ventura - the user's ask, and the oldest IOGraphics shape in scope");
    CHECK(!rb_os_in_range(RB_OS_VENTURA - 1),
          "Monterey and earlier are out of range");
    CHECK(rb_os_in_range(RB_OS_VENTURA) && rb_os_in_range(RB_OS_SONOMA) &&
          rb_os_in_range(RB_OS_SEQUOIA) && rb_os_in_range(RB_OS_TAHOE),
          "Ventura through Tahoe are all in range");
    /* The ceiling is on the CLAIM, not on loading: an OS newer than this tree
     * knows must still load, because a boot that refuses teaches nothing. */
    CHECK(rb_os_in_range(RB_OS_MAX_DARWIN + 1) &&
          rb_os_row_for(RB_OS_MAX_DARWIN + 1) == 0 &&
          rb_os_derived_mask(RB_OS_MAX_DARWIN + 1) == 0u,
          "a Darwin newer than this tree loads, has no row, and claims nothing");
    CHECK(rb_os_derived_mask(RB_OS_VENTURA - 1) == 0u,
          "an out-of-range OS claims nothing either");

    /* --- the rows are well formed ------------------------------------- */

    for (i = 0; i < RB_OS_ROW_COUNT; i++) {
        CHECK(rb_os_rows[i].name != 0 && rb_os_rows[i].name[0] != '\0',
              "every row has a name to print (rule 14: silence is not a result)");
        CHECK(rb_os_rows[i].note != 0 && rb_os_rows[i].note[0] != '\0',
              "every row says what its claim rests on");
        CHECK((rb_os_rows[i].derived & ~(unsigned)RB_OSD_ALL) == 0u,
              "no row claims an artifact bit this tree does not define");
        CHECK(rb_os_in_range(rb_os_rows[i].darwin),
              "no row is below the declared floor");
        for (j = i + 1; j < RB_OS_ROW_COUNT; j++) {
            CHECK(rb_os_rows[i].darwin != rb_os_rows[j].darwin,
                  "no two rows share a Darwin major (the lookup takes the first)");
        }
        CHECK(rb_os_row_for(rb_os_rows[i].darwin) == &rb_os_rows[i],
              "the lookup finds each row at its own key");
    }
    CHECK(RB_OS_ROW_COUNT == 4,
          "four rows: Ventura, Sonoma, Sequoia, Tahoe");

    /* --- the claims themselves --------------------------------------- */

    /* The two that have never been booted claim nothing. This is the
     * assertion that keeps the honest answer honest: adding a bit here
     * without a boot behind it has to break a named test. */
    CHECK(rb_os_derived_mask(RB_OS_VENTURA) == 0u,
          "Ventura claims nothing - no boot of this project has happened there");
    CHECK(rb_os_derived_mask(RB_OS_SONOMA) == 0u,
          "Sonoma claims nothing - no boot of this project has happened there");
    CHECK(!rb_os_derived(RB_OS_VENTURA, RB_OSD_KEXTOFF) &&
          !rb_os_derived(RB_OS_VENTURA, RB_OSD_MTLCAVE) &&
          !rb_os_derived(RB_OS_SONOMA, RB_OSD_KEXTOFF) &&
          !rb_os_derived(RB_OS_SONOMA, RB_OSD_MTLCAVE),
          "so neither answers yes to any artifact");

    /* Sequoia: where everything was derived, and the cave row that boot 231
     * ran. No USB table - 2xv was derived on Tahoe. */
    CHECK(rb_os_derived(RB_OS_SEQUOIA, RB_OSD_KEXTOFF),
          "Sequoia has the object offsets - they were all derived there");
    CHECK(rb_os_derived(RB_OS_SEQUOIA, RB_OSD_MTLCAVE),
          "Sequoia has a cave row - the one boot 231 ran");
    CHECK(!rb_os_derived(RB_OS_SEQUOIA, RB_OSD_XHCI),
          "Sequoia has no USB interrupter table - 2xv was derived on Tahoe");

    /* Tahoe: the current machine. Everything. */
    CHECK(rb_os_derived(RB_OS_TAHOE, RB_OSD_ALL),
          "Tahoe has all three - it is the machine boot 247 ran on");

    /* --- the all-or-nothing rule ------------------------------------- */

    /* A caller that needs two measured things and gets one is in the same
     * position as one that got neither, so the answer is AND and not OR. */
    CHECK(!rb_os_derived(RB_OS_SEQUOIA, RB_OSD_KEXTOFF | RB_OSD_XHCI),
          "a mask naming one present and one absent artifact answers no");
    CHECK(rb_os_derived(RB_OS_SEQUOIA, RB_OSD_KEXTOFF | RB_OSD_MTLCAVE),
          "...and answers yes only when every named artifact is present");
    CHECK(rb_os_derived(RB_OS_VENTURA, 0u),
          "an empty mask is vacuously satisfied - a caller that needs nothing measured proceeds");

    /* --- names, which are the log line ------------------------------- */

    CHECK(strcmp(rb_os_name(RB_OS_TAHOE), "Tahoe") == 0 &&
          strcmp(rb_os_name(RB_OS_VENTURA), "Ventura") == 0,
          "a known OS prints its name");
    CHECK(rb_os_name(RB_OS_MAX_DARWIN + 1) != 0 &&
          strcmp(rb_os_name(RB_OS_MAX_DARWIN + 1), "newer-than-this-tree") == 0,
          "a newer OS prints that it is newer, not nothing");
    CHECK(strcmp(rb_os_name(RB_OS_VENTURA - 1), "out-of-range") == 0,
          "an older OS prints that it is out of range");

    /* --- the cave rows against the matrix ---------------------------- */

    /* The invariant: a row exists for a Darwin major IF AND ONLY IF that OS's
     * matrix entry claims RB_OSD_MTLCAVE. Either half alone is a lie - a row
     * without the bit is data nothing will reach, and a bit without a row
     * makes `rb_os_derived` promise a patch that cannot be placed. */
    for (i = 0; i < RB_OS_ROW_COUNT; i++) {
        const int d = rb_os_rows[i].darwin;
        const int claims = rb_os_derived(d, RB_OSD_MTLCAVE);
        const int has = rbgpu_mtlcave_row(d) != 0;

        CHECK(claims == has,
              "each OS claims RB_OSD_MTLCAVE exactly when a cave row exists for it");
    }
    CHECK(rbgpu_mtlcave_row(RB_OS_VENTURA) == 0 &&
          rbgpu_mtlcave_row(RB_OS_SONOMA) == 0,
          "Ventura and Sonoma have no cave row - stated twice, in the matrix and in the table");
    CHECK(rbgpu_mtlcave_row(RB_OS_SEQUOIA) != 0 &&
          rbgpu_mtlcave_row(RB_OS_TAHOE) != 0,
          "Sequoia's row survived 2xt instead of being replaced, and Tahoe has its own");
    CHECK(rbgpu_mtlcave_row(RB_OS_MAX_DARWIN + 1) == 0,
          "the lookup is exact, never nearest-neighbour - a wrong build's addresses are worth nothing");

    for (i = 0; i < RB_MTLCAVE_ROW_COUNT; i++) {
        const struct rb_mtlcave_row *r = &rb_mtlcave_rows[i];

        /* The read buffer in rbgpu_mtlk_place() is sized from MAXLEN, not from
         * a row's own array, so a longer pattern must fail HERE and not
         * silently truncate a compare there. */
        CHECK(r->cave_len <= RBGPU_MTL_MAXLEN && r->fn_len <= RBGPU_MTL_MAXLEN,
              "every pattern fits the fixed read buffer (RBGPU_MTL_MAXLEN)");
        CHECK(r->cave_len == r->cave_anchor + r->cave_reserved,
              "cave_len is exactly the anchor plus the reserved run it covers");
        CHECK(r->cave_used > 0 && r->cave_used <= r->cave_reserved,
              "the cave code fits the zero run it replaces");
        /* The find pattern's reserved tail is the all-zero run the cave goes
         * into. If it is not zero, the row was derived against something that
         * was not a zero run and the write would clobber real data. */
        {
            int nonzero = 0;
            for (j = (int)r->cave_anchor; j < (int)r->cave_len; j++) {
                nonzero += r->cave_find[j] != 0;
            }
            CHECK(nonzero == 0,
                  "the reserved area of every find pattern is all zero - it is a zero run, not live data");
        }
        /* The anchor is what makes an all-zero cave identifiable at all, so a
         * replacement that changed it would move the match. */
        CHECK(memcmp(r->cave_find, r->cave_repl, r->cave_anchor) == 0,
              "the replacement leaves the anchor bytes alone - it is context, not payload");
        CHECK(memcmp(r->cave_find, r->cave_repl, r->cave_len) != 0,
              "...and does change something past it");
        /* find and repl must differ, or a verify after the write cannot tell
         * a landed write from an untouched page (2lq in byte form). */
        CHECK(memcmp(r->fn_find, r->fn_repl, r->fn_len) != 0,
              "the detour's replacement differs from its find - otherwise the verify proves nothing");
        /* The detour is a jmp, so the first byte changes and the rest of the
         * prologue is left as context. */
        CHECK(r->fn_repl[0] == 0xE9,
              "the detour starts with a rel32 jmp");
        /* 312: a multisampled render target must keep Apple's own swizzle
         * mode. The guard is `cmp qword [rbx+0x30], 2 ; jae OUT`. Without it
         * the remap turns 64KB_R_X into 64KB_D, AddrLib answers
         * ADDR_INVALIDPARAMS and every MSAA colour texture is nil.
         * 3mip: this was Tahoe-only, because 312 regenerated only the row of
         * the machine it ran on. gen_cavepatch.py now re-emits every other
         * row's policy bytes from that row's own recorded addresses, so the
         * guard is required of every row. */
        {
            static const unsigned char guard[] = { 0x48, 0x83, 0x7B, 0x30, 0x02, 0x73 };
            int found = 0;
            for (j = 0; j + (int)sizeof(guard) <= (int)r->cave_len; j++) {
                found += memcmp(r->cave_repl + j, guard, sizeof(guard)) == 0;
            }
            CHECK(found == 1,
                  "every cave skips the remap for sampleCount >= 2 - MSAA colour keeps its _X mode (312)");
        }
        CHECK(r->cave_vm > r->text_vm && r->fn_vm > r->text_vm,
              "both addresses are inside the image, above its __TEXT base");
        CHECK(r->cave_vm >= r->cave_anchor,
              "the anchor does not underflow the cave address");
        CHECK(r->os != 0 && r->os[0] != '\0' && rb_os_row_for(r->darwin) != 0,
              "every cave row names an OS the matrix knows");
        for (j = i + 1; j < RB_MTLCAVE_ROW_COUNT; j++) {
            CHECK(rb_mtlcave_rows[i].darwin != rb_mtlcave_rows[j].darwin,
                  "no two cave rows share a Darwin major");
            /* 2xt's whole lesson: the rows must be DIFFERENT data. Two rows
             * with the same addresses would mean one was copied rather than
             * derived. */
            CHECK(rb_mtlcave_rows[i].fn_vm != rb_mtlcave_rows[j].fn_vm &&
                  rb_mtlcave_rows[i].cave_vm != rb_mtlcave_rows[j].cave_vm,
                  "no two cave rows share addresses - a copied row is not a derived one");
        }
    }

#if defined(__x86_64__)
    /* --- 3mip: what each row's cave DECIDES ---------------------------- */
    for (i = 0; i < RB_MTLCAVE_ROW_COUNT; i++) {
        const struct rb_mtlcave_row *r = &rb_mtlcave_rows[i];
        uint32_t tramp = 0;
        uint8_t *m = cave_map(r, &tramp);
        static const uint64_t usages[] = { 0x1, 0x4, 0x5, 0x7, 0x17 };
        static const uint64_t mipsv[] = { 0, 1, 2, 7, 9 };
        static const uint64_t sampv[] = { 0, 1, 2, 4, 8 };
        unsigned o, u, v, k;
        int wrong = 0;

        CHECK(m != 0, "the cave maps and its call into the trampoline is where build_cave() puts it");
        if (!m) {
            continue;
        }
        /* The defect, as measured: Spotify's planes are R8, usage 0x1, mip 7
         * to 9, and the driver picks 22, 27 or 2 for them. */
        CHECK(cave_ask(m, tramp, 22, 0x1, 0x1, 9, 1) == 6,
              "3mip: ShaderRead-only, 9 mips, 4KB_D_X -> 4KB_D, NOT linear (the 300x300 cover plane)");
        CHECK(cave_ask(m, tramp, 27, 0x1, 0x1, 8, 1) == 10,
              "3mip: ShaderRead-only, 8 mips, 64KB_R_X -> 64KB_D, NOT linear");
        CHECK(cave_ask(m, tramp, 2, 0x1, 0x1, 7, 1) == 2,
              "3mip: ShaderRead-only, 7 mips, a non-X mode is left exactly as the driver chose");
        /* The boundary, both sides. One mip level is a flat texture. */
        CHECK(cave_ask(m, tramp, 22, 0x1, 0x1, 2, 1) == 6,
              "3mip: two mip levels is a chain - not linear");
        CHECK(cave_ask(m, tramp, 22, 0x1, 0x1, 1, 1) == 0,
              "a flat ShaderRead-only texture is still LINEAR (2np's fix for the glyph atlas stands)");
        CHECK(cave_ask(m, tramp, 27, 0x1, 0x1, 0, 1) == 0,
              "mipLevels 0 is flat too - LINEAR");
        /* Render targets: unchanged by 3mip, flat or not. */
        CHECK(cave_ask(m, tramp, 27, 0x5, 0x5, 1, 1) == 10 &&
              cave_ask(m, tramp, 22, 0x5, 0x5, 9, 1) == 6,
              "a render target is mapped off the _X family, with or without mips");
        CHECK(cave_ask(m, tramp, 27, 0x0, 0x17, 1, 1) == 10,
              "usage unset as requested but resolved to a render target (JBR) is a render target");
        CHECK(cave_ask(m, tramp, 24, 0x4, 0x4, 1, 1) == 24 &&
              cave_ask(m, tramp, 20, 0x4, 0x4, 1, 1) == 20,
              "Z modes are left alone - depth needs them");
        CHECK(cave_ask(m, tramp, 31, 0x4, 0x4, 1, 1) == 31 &&
              cave_ask(m, tramp, 28, 0x1, 0x1, 9, 1) == 28,
              "a mode past the table is returned as it came, never indexed");
        /* 312, now RUN rather than pattern-matched: a multisampled render
         * target keeps the driver's own mode, both sides of the boundary. */
        CHECK(cave_ask(m, tramp, 27, 0x5, 0x5, 1, 4) == 27 &&
              cave_ask(m, tramp, 27, 0x7, 0x7, 1, 8) == 27,
              "312: a multisampled render target keeps 64KB_R_X - the remap is what made MSAA colour nil");
        CHECK(cave_ask(m, tramp, 27, 0x5, 0x5, 1, 2) == 27 &&
              cave_ask(m, tramp, 27, 0x5, 0x5, 1, 1) == 10,
              "312: two samples is multisampled and one is not");
        /* And everything, against the policy written down in C. */
        for (o = 0; o < 40; o++) {
            for (u = 0; u < sizeof(usages) / sizeof(usages[0]); u++) {
                for (v = 0; v < sizeof(mipsv) / sizeof(mipsv[0]); v++) {
                    for (k = 0; k < sizeof(sampv) / sizeof(sampv[0]); k++) {
                        wrong += cave_ask(m, tramp, o, usages[u], usages[u], mipsv[v], sampv[k]) !=
                                 cave_model(o, usages[u], mipsv[v], sampv[k]);
                    }
                }
            }
        }
        CHECK(wrong == 0,
              "the cave agrees with the stated policy for every mode x usage x mip count x sample count (5000 cases)");
        munmap(m, 4096);

        /* One policy, every OS. The cave is position-dependent only in the
         * trampoline's last rel32, so everything before it must be identical
         * across rows - otherwise an OS is still running an older policy. */
        CHECK(r->cave_used == rb_mtlcave_rows[0].cave_used &&
              memcmp(r->cave_repl + r->cave_anchor,
                     rb_mtlcave_rows[0].cave_repl + rb_mtlcave_rows[0].cave_anchor,
                     r->cave_used - 4u) == 0,
              "every row carries the same cave outside the one address-dependent displacement");
    }
#endif

#if defined(__x86_64__)
    /* --- 3gl-d: the OpenGL cave ------------------------------------- */
    CHECK(RB_GLCAVE_CAVE_LEN > RB_GLCAVE_ANCHOR,
          "the GL cave's length covers more than its anchor");
    CHECK(RB_GLCAVE_CAVE_LEN <= RBGPU_MTL_MAXLEN &&
          RB_GLCAVE_FN_LEN <= RBGPU_MTL_MAXLEN,
          "both GL patterns fit rbgpu_mtlk_place()'s buffer, which is what the kext reuses");
    {
        uint32_t i, zero = 1, anchored = 1;
        for (i = RB_GLCAVE_ANCHOR; i < RB_GLCAVE_CAVE_LEN; i++) {
            if (rb_glcave_cave_find[i] != 0) { zero = 0; }
        }
        for (i = 0; i < RB_GLCAVE_ANCHOR; i++) {
            if (rb_glcave_cave_find[i] != rb_glcave_cave_repl[i]) { anchored = 0; }
        }
        CHECK(zero, "the GL cave goes into a run of zeroes - nothing is overwritten");
        CHECK(anchored,
              "find and repl share the anchor, so the bytes before the cave are not touched");
        /* An anchor made of the zeroes it is looking for matched 12486 places
         * in __const when this was first written. A pattern made of the thing
         * it searches for is not a pattern (rule 26). */
        zero = 1;
        for (i = 0; i < RB_GLCAVE_ANCHOR; i++) {
            if (rb_glcave_cave_find[i] != 0) { zero = 0; }
        }
        CHECK(!zero, "the anchor carries REAL context bytes, not more of the zero run");
    }
    CHECK(rb_glcave_fn_find[0] == 0x55 && rb_glcave_fn_find[1] == 0x48 &&
          rb_glcave_fn_find[2] == 0x89 && rb_glcave_fn_find[3] == 0xE5,
          "the GL detour replaces the chooser's own prologue");
    {
        int32_t rel = 0;
        uint32_t i, nops = 1;
        memcpy(&rel, rb_glcave_fn_repl + 1, 4);
        CHECK(rb_glcave_fn_repl[0] == 0xE9,
              "the GL detour is a rel32 jump, so the bytes are position independent");
        CHECK((uint32_t)((int64_t)RB_GLCAVE_FN_OFF + 5 + rel) == RB_GLCAVE_CAVE_OFF,
              "and it lands exactly on the cave");
        for (i = 5; i < RB_GLCAVE_FN_LEN; i++) {
            if (rb_glcave_fn_repl[i] != 0x90) { nops = 0; }
        }
        CHECK(nops,
              "the rest of the replaced prologue is nops, so no half instruction is left");
    }
    {
        uint32_t tramp = 0;
        uint8_t *m = glcave_map(&tramp);
        CHECK(m != 0, "the GL cave maps executable and its call reaches its own trampoline");
        if (m) {
            unsigned orig, wrong = 0;
            CHECK(glcave_ask(m, tramp, 27) == 10,
                  "64KB_R_X (27) becomes 64KB_D (10) - the mode every caller in the user's app got");
            CHECK(glcave_ask(m, tramp, 22) == 6,
                  "4KB_D_X (22) becomes 4KB_D (6) - the other mode measured in that app");
            CHECK(glcave_ask(m, tramp, 24) == 24,
                  "a Z mode (24) is left alone");
            CHECK(glcave_ask(m, tramp, 0) == 0,
                  "LINEAR stays LINEAR - this cave never forces it and never undoes it");
            CHECK(glcave_ask(m, tramp, 40) == 40,
                  "a mode past the table is returned untouched rather than read out of it");
            for (orig = 0; orig < 64; orig++) {
                if (glcave_ask(m, tramp, orig) != glcave_model(orig)) { wrong++; }
            }
            CHECK(wrong == 0,
                  "the GL cave agrees with the stated nox table for all 64 modes");
            munmap(m, 4096);
        }
    }
#endif

    return failures;
}
