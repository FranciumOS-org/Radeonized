/* rbgpu_mtlcave.h - the swizzle-fix patch rows, one per macOS build.
 *
 * NOT generated. The ROWS are (src/plugin/mtlcave/darwin*.h, from
 * tools/gen_cavepatch.py); this file is the table and the lookup.
 *
 * What the patch does, unchanged since 2oa: it delivers the
 * RBGPU_SWIZZLE=fix policy into AMDRadeonX6000MTLDriver inside the dyld
 * shared cache by writing straight into the live image at exec time - every
 * process, no injection, and the cryptex is never written, which 2ny proved
 * it cannot be. The bytes are a code cave placed in the driver's __const
 * (inside the r-x __TEXT segment, so it executes) plus a detour at the
 * function's prologue that jumps into it.
 *
 * WHY THIS IS A TABLE NOW (2xz). Every value in a row is an address inside
 * one specific build of one specific dylib. 2xt is what that costs when it is
 * a single row: macOS 26 moved the driver's __TEXT by 0x84E1000 and rebuilt
 * the image, every constant had to be measured again, and the macOS 15 values
 * were REPLACED - so the tree then supported exactly one OS, and the row that
 * boot 231 ran existed only in git history. Keeping the rows side by side
 * costs 254 bytes each and means an OS update, or a user on a different
 * macOS, does not invalidate what was already measured.
 *
 * WHAT A ROW IS AND IS NOT AUTHORITY FOR. A row says where to LOOK. What
 * authorises the write is the byte-pattern compare at that place, which
 * rbgpu_mtlk_place() does on every single process: read the bytes, and write
 * only if they are exactly `cave_find` (or are already `cave_repl`). So a row
 * that is right for Darwin 25 but wrong for this particular 25.x build does
 * not write anything anywhere - it fails safe and 2r6's verdict says the
 * driver moved, with the unslid addresses and this boot's slide in the log so
 * the next row can be derived without another boot. That property is why
 * keying on the Darwin MAJOR is honest even though the addresses are per
 * BUILD: the key picks a candidate, the bytes decide.
 *
 * An OS with no row gets no patch and says so. That is the Ventura and Sonoma
 * state today, and it is a dim-but-correct desktop rather than a wrong write:
 * without the userspace half the swizzle fix is incomplete (2on-2op say both
 * halves are needed), the kernel linear force still applies, and nothing is
 * corrupted.
 */
#ifndef RBGPU_MTLCAVE_H
#define RBGPU_MTLCAVE_H
#include <stdint.h>

/* The one path, identical in every row ever derived - the driver has not
 * moved in the filesystem, only inside the cache. */
#define RBGPU_MTL_PATH "/System/Library/Extensions/AMDRadeonX6000MTLDriver.bundle/Contents/MacOS/AMDRadeonX6000MTLDriver"

/* The largest `cave_len` any row may carry. The read buffer in
 * rbgpu_mtlk_place() is sized from this rather than from one row's array, so
 * adding a row with a longer pattern is a compile-time failure here instead
 * of a silent truncation there. Asserted against every row by the hosttest. */
#define RBGPU_MTL_MAXLEN 256u

struct rb_mtlcave_row {
    int             darwin;         /* the Darwin major this was derived on */
    const char     *os;             /* its name, for the one log line */
    uint64_t        text_vm;        /* the dylib's __TEXT, as the .map reports */
    uint64_t        fn_vm;          /* amdMtl_HWL_DetermineSwizzleMode */
    uint64_t        cave_vm;        /* the cave, 16-aligned, inside a zero run */
    uint32_t        cave_anchor;    /* bytes of context BEFORE the cave: the
                                     * cave is all zeroes, so the pattern needs
                                     * preceding context to be unique */
    uint32_t        cave_reserved;  /* the zero run the cave replaces */
    uint32_t        cave_used;      /* how much of it the cave code occupies */
    const uint8_t  *cave_find;
    const uint8_t  *cave_repl;
    uint32_t        cave_len;       /* = cave_anchor + cave_reserved */
    const uint8_t  *fn_find;        /* the function's prologue */
    const uint8_t  *fn_repl;        /* ...with a jmp into the cave over it */
    uint32_t        fn_len;
};

#include "mtlcave/darwin24.h"
#include "mtlcave/darwin25.h"

static const struct rb_mtlcave_row rb_mtlcave_rows[] = {
    RB_MTLCAVE_ROW_D24,
    RB_MTLCAVE_ROW_D25,
};

#define RB_MTLCAVE_ROW_COUNT \
    ((int)(sizeof(rb_mtlcave_rows) / sizeof(rb_mtlcave_rows[0])))

/* The row derived on this Darwin major, or 0 if none was. Deliberately an
 * exact match and never a nearest-neighbour fall-back: the whole content of a
 * row is addresses inside one build, so "close" is worth nothing and offering
 * it would turn a clean decline into a pattern miss on every process. */
static inline const struct rb_mtlcave_row *rbgpu_mtlcave_row(int darwin)
{
    int i;

    for (i = 0; i < RB_MTLCAVE_ROW_COUNT; i++) {
        if (rb_mtlcave_rows[i].darwin == darwin) {
            return &rb_mtlcave_rows[i];
        }
    }
    return 0;
}

#endif /* RBGPU_MTLCAVE_H */
