#!/usr/bin/env python3
"""Derive the OpenGL swizzle cave and write src/common/rb_glcave.h (3gl-d).

WHY A SECOND CAVE EXISTS. Everything this project patches for the swizzle
defect lives in AMDRadeonX6000MTLDriver, inside the dyld shared cache. 3gl
measured a broken application and it is not a Metal client: OpenGL draws
through AMDRadeonX6000GLDriver, a bundle loaded from
/System/Library/Extensions at a per-process address OUTSIDE the cache, with
its own copy of AddrLib and its own chooser, glrAMD_GFX10_DetermineSwizzleMode.
3gl-c measured the fix in that app's own process with its control in the same
frame: remapping the `_X` modes with the cave's 28-byte table makes it render
correctly. This emits the bytes that do the same thing from the kext.

WHAT IS DIFFERENT FROM tools/gen_cavepatch.py, and it is only two things:

  1. The policy is `nox` ALONE. No LINEAR term, no mip term, no MSAA term.
     The Metal cave can tell a render target from a sampled texture because
     2nq located `usage` in the descriptor; nothing locates it in THIS
     function's arguments, so the kext cannot make that distinction and must
     not pretend to. 2nq is also why it matters: a render target forced linear
     returns a nil pipeline state and the client dereferences it.
  2. Every address here is IMAGE-RELATIVE and every jump is rel32, so the
     emitted bytes are position independent. The Metal rows can be unslid
     cache addresses because the cache has one slide per boot; this image is
     loaded by dyld wherever it likes, per process.

The row says where to LOOK. What authorises the write is the byte compare at
that place, exactly as in rbgpu_mtlcave.h: rbgpu_mtlk_place() reads first and
writes only on an exact match of `find` (or finds `repl` already there), so a
row that is wrong for this build writes nothing anywhere.

  python3 tools/gen_glcave.py        # or: make glcave
"""
import struct
import sys

GL = ("/System/Library/Extensions/AMDRadeonX6000GLDriver.bundle/"
      "Contents/MacOS/AMDRadeonX6000GLDriver")
OUT = "src/common/rb_glcave.h"

# The chooser's prologue, 32 bytes for uniqueness. Only the first 15 are
# copied: push rbp / mov rbp,rsp / push r14 / push rbx / sub rsp,0x70 /
# vxorps xmm0,xmm0,xmm0 - a whole number of instructions, no branch in them
# and nothing rip-relative (rule 2).
FIND = bytes([
    0x55, 0x48, 0x89, 0xe5, 0x41, 0x56, 0x53, 0x48,
    0x83, 0xec, 0x70, 0xc5, 0xf8, 0x57, 0xc0, 0xc5,
    0xfc, 0x11, 0x45, 0xd0, 0xc5, 0xfc, 0x11, 0x45,
    0xbc, 0xc5, 0xf8, 0x57, 0xc0, 0xc5, 0xf8, 0x11,
])
PROLOGUE_COPY = 15

# The cave's table, identical to the Metal cave's (2nr). Z modes untouched.
NOX = {21: 5, 22: 6, 23: 6, 25: 9, 26: 10, 27: 10}
TABLE = bytes([NOX.get(i, i) for i in range(28)])


def sections(data):
    """(name, segname, fileoff, size) for every section, from the headers."""
    ncmds, = struct.unpack_from("<I", data, 16)
    off = 32
    out = []
    for _ in range(ncmds):
        cmd, cmdsize = struct.unpack_from("<II", data, off)
        if cmd == 0x19:  # LC_SEGMENT_64
            segname = data[off + 8:off + 24].rstrip(b"\0").decode()
            nsects, = struct.unpack_from("<I", data, off + 64)
            s = off + 72
            for _ in range(nsects):
                sect = data[s:s + 16].rstrip(b"\0").decode()
                addr, size = struct.unpack_from("<QQ", data, s + 32)
                foff, = struct.unpack_from("<I", data, s + 48)
                out.append((sect, segname, foff, size, addr))
                s += 80
        off += cmdsize
    return out


def find_fn(data, text_off, text_size):
    hits = [o for o in range(text_off, text_off + text_size - len(FIND))
            if data[o:o + len(FIND)] == FIND]
    if len(hits) != 1:
        sys.exit("REFUSING: %d occurrences of the chooser prologue, "
                 "expected exactly 1" % len(hits))
    return hits[0]


def find_zero_run(data, off, size, need):
    """Return (cave_off, anchor) for a 16-aligned zero run of at least `need`
    bytes inside __const.

    The cave is placed at the FRONT of the run, not 16 bytes into it, and the
    anchor is the sixteen REAL bytes before the run plus whatever alignment
    padding follows them. The first draft anchored on 16 bytes of the run
    itself, which are zeroes: the pattern then matched 12486 places in
    __const. A pattern made of the thing it is looking for is not a pattern
    (rule 26 - a search is an instrument).
    """
    run = 0
    start = None
    for i in range(off + 16, off + size):
        if data[i] == 0:
            if run == 0:
                start = i
            run += 1
            continue
        if run >= need + 15 and start is not None:
            place = (start + 15) & ~15
            if place + need <= start + run:
                return place, place - (start - 16)
        run = 0
    if run >= need + 15 and start is not None:
        place = (start + 15) & ~15
        if place + need <= start + run:
            return place, place - (start - 16)
    sys.exit("REFUSING: no zero run of %d bytes in __const" % need)


def build_cave(cave_off, fn_off):
    """The wrapper, the table and the trampoline, all rel32.

    Stack discipline, the same as tools/cachepatch.py's and checked the same
    way: at entry rsp = 8 (mod 16); push rbp -> 0; push rbx -> 8;
    sub 0x18 -> 0, which is what the ABI wants at the call. The three stack
    arguments are copied so the copied prologue finds them at 0x10/0x18/0x20
    of ITS rbp - the `call` below pushes a return address, so without this the
    body would read every stack argument eight bytes low. The chooser reads at
    least the first of them.
    """
    w = bytearray()
    w += bytes([0x55])                               # push rbp
    w += bytes([0x48, 0x89, 0xE5])                   # mov rbp, rsp
    w += bytes([0x53])                               # push rbx
    w += bytes([0x48, 0x83, 0xEC, 0x18])             # sub rsp, 0x18
    w += bytes([0x48, 0x8B, 0x45, 0x10])             # mov rax,[rbp+0x10]
    w += bytes([0x48, 0x89, 0x04, 0x24])             # mov [rsp],rax
    w += bytes([0x48, 0x8B, 0x45, 0x18])             # mov rax,[rbp+0x18]
    w += bytes([0x48, 0x89, 0x44, 0x24, 0x08])       # mov [rsp+8],rax
    w += bytes([0x48, 0x8B, 0x45, 0x20])             # mov rax,[rbp+0x20]
    w += bytes([0x48, 0x89, 0x44, 0x24, 0x10])       # mov [rsp+0x10],rax
    call_at = len(w)
    w += bytes([0xE8, 0, 0, 0, 0])                   # call TRAMP
    w += bytes([0x48, 0x83, 0xC4, 0x18])             # add rsp, 0x18
    w += bytes([0x83, 0xF8, 0x1B])                   # cmp eax, 27
    ja_at = len(w)
    w += bytes([0x77, 0])                            # ja OUT
    lea_at = len(w)
    w += bytes([0x48, 0x8D, 0x0D, 0, 0, 0, 0])       # lea rcx,[rip+TABLE]
    w += bytes([0x0F, 0xB6, 0x04, 0x01])             # movzx eax, byte[rcx+rax]
    out = len(w)
    w += bytes([0x5B, 0x5D, 0xC3])                   # pop rbx; pop rbp; ret

    table_at = len(w)
    w += TABLE
    tramp_at = len(w)
    w += FIND[:PROLOGUE_COPY]                        # the copied prologue
    back = fn_off + PROLOGUE_COPY
    jmp_at = len(w)
    w += bytes([0xE9, 0, 0, 0, 0])                   # jmp fn+PROLOGUE_COPY

    # rel32 fixups, all image-relative so the bytes are position independent.
    struct.pack_into("<i", w, call_at + 1, tramp_at - (call_at + 5))
    w[ja_at + 1] = out - (ja_at + 2)
    struct.pack_into("<i", w, lea_at + 3, table_at - (lea_at + 7))
    struct.pack_into("<i", w, jmp_at + 1,
                     back - (cave_off + jmp_at + 5))
    return bytes(w)


def main():
    data = open(GL, "rb").read()
    secs = sections(data)
    text = [s for s in secs if s[0] == "__text" and s[1] == "__TEXT"][0]
    const = [s for s in secs if s[0] == "__const" and s[1] == "__TEXT"][0]

    fn_off = find_fn(data, text[2], text[3])
    # Build once with a placeholder to learn the size, then again at the real
    # place: only the trampoline's jump-back depends on where the cave is.
    size = len(build_cave(0, fn_off))
    cave_off, anchor = find_zero_run(data, const[2], const[3], size)
    cave = build_cave(cave_off, fn_off)
    assert len(cave) == size

    cave_find = data[cave_off - anchor:cave_off + size]
    if cave_find[anchor:] != bytes(size):
        sys.exit("REFUSING: the chosen cave is not all zeroes")
    hits = sum(1 for o in range(const[2], const[2] + const[3] - len(cave_find))
               if data[o:o + len(cave_find)] == cave_find)
    if hits != 1:
        sys.exit("REFUSING: the cave pattern occurs %d times, expected 1" % hits)
    cave_repl = cave_find[:anchor] + cave

    det = bytearray(b"\x90" * PROLOGUE_COPY)
    det[0] = 0xE9
    struct.pack_into("<i", det, 1, cave_off - (fn_off + 5))

    def carr(b):
        return "\n".join("    " + " ".join("0x%02x," % c for c in b[i:i + 12])
                         for i in range(0, len(b), 12))

    h = '''/* rb_glcave.h - GENERATED by tools/gen_glcave.py. Do not edit.
 *
 * The OpenGL swizzle cave: the `nox` policy, placed in
 * AMDRadeonX6000GLDriver's __const and reached by a detour over
 * glrAMD_GFX10_DetermineSwizzleMode's prologue.
 *
 * Every offset is IMAGE-RELATIVE and every jump is rel32, because this image
 * is not in the dyld shared cache - dyld loads it wherever it likes, per
 * process, so the kext adds that process's base and the bytes themselves
 * never change.
 *
 * The policy is `nox` ALONE - no LINEAR term - because nothing has located
 * `usage` among this function's arguments and 2nq measured what a render
 * target forced linear does: a nil pipeline state and a dereference of it.
 */
#ifndef RB_GLCAVE_H
#define RB_GLCAVE_H
#include <stdint.h>

#define RB_GLCAVE_PATH "%s"

/* The Darwin major the row was derived on. The key picks a candidate; the
 * byte compare at the place decides, so a wrong row writes nothing. */
#define RB_GLCAVE_DARWIN   %d
#define RB_GLCAVE_FN_OFF   0x%xu   /* glrAMD_GFX10_DetermineSwizzleMode */
#define RB_GLCAVE_CAVE_OFF 0x%xu   /* inside __const, 16-aligned zero run */
#define RB_GLCAVE_ANCHOR   %du     /* context bytes BEFORE the cave */
#define RB_GLCAVE_CAVE_LEN %du     /* anchor + the cave itself */
#define RB_GLCAVE_FN_LEN   %du     /* the prologue the detour replaces */

static const uint8_t rb_glcave_cave_find[RB_GLCAVE_CAVE_LEN] = {
%s
};
static const uint8_t rb_glcave_cave_repl[RB_GLCAVE_CAVE_LEN] = {
%s
};
static const uint8_t rb_glcave_fn_find[RB_GLCAVE_FN_LEN] = {
%s
};
static const uint8_t rb_glcave_fn_repl[RB_GLCAVE_FN_LEN] = {
%s
};

#endif /* RB_GLCAVE_H */
''' % (GL, 25, fn_off, cave_off, anchor, len(cave_find), PROLOGUE_COPY,
       carr(cave_find), carr(cave_repl), carr(FIND[:PROLOGUE_COPY]),
       carr(bytes(det)))
    open(OUT, "w").write(h)
    print("fn   0x%x  cave 0x%x  cave bytes %d (anchor %d)  detour %d"
          % (fn_off, cave_off, size, anchor, PROLOGUE_COPY))
    print("wrote " + OUT)


main()
