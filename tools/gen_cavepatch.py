#!/usr/bin/env python3
"""Emit the Lilu BinaryModPatch arrays that deliver the swizzle fix to EVERY
process, by patching the dyld shared cache's pages in the kernel (2nz/2oa).

Why this works where tools/cachepatch*.{py,sh} could not. 2ny established that
the cache is inside a grafted, Apple-signed cryptex and cannot be edited on
disk. Lilu does not edit it on disk. For an image in the shared cache it does
NOT use the cs_validate_page page rewrite either - that is the on-disk path,
and 2oc corrects this docstring's original claim. It uses patchSharedCache,
which runs per process at exec and writes straight into the live image:

    place = mod->startTEXT + ref->segOffs[k] + slide      (startTEXT from the .map)
    orgVmMapReadUser(...);  if bytes == patch.find
    vmProtect(RWX); orgVmMapWriteUser(..., patch.replace, place, patch.size)

The file is never touched, so the seal is irrelevant, and the patch lands in
every process - including the Apple platform binaries that strip
DYLD_INSERT_LIBRARIES.

2op: the Lilu route this was first written for never ran at all (rule 22), and
the kext places these bytes itself with _vm_map_write_user from known unslid
addresses (2og). It needs no byte source on disk, so the 2oc/2od stencil and
tools/gen_stencil.py are gone. What this generator emits is unchanged and
still exactly what the kext compares and writes.

Two constraints shape what is emitted:

  * Lilu patches by FIND/REPLACE inside one section, not by address, so each
    patch needs a byte pattern that is unique within that section.
  * The cave cannot go in __text: the shared cache builder packs it, and this
    driver's __text contains no run of 32 identical bytes anywhere. __const is
    inside the r-x __TEXT segment, so code placed there executes, it has 27
    runs of >= 200 zero bytes, and Lilu can target it as SegmentTextConst.

2xz: this no longer prints one header to stdout. It DERIVES every address
from the running machine's cache and writes src/plugin/mtlcave/darwin<N>.h,
one row per OS, beside the rows already there. See the long comment below.

  python3 tools/gen_cavepatch.py        # or: make mtlcave
"""
import importlib.util
import sys

spec = importlib.util.spec_from_file_location("cp", "tools/cachepatch.py")
cp = importlib.util.module_from_spec(spec)
spec.loader.exec_module(cp)

sys.path.insert(0, "tools/re")
from dyldcache import Cache                                   # noqa: E402

# 2xz: DERIVED, NOT HARDCODED, AND WRITTEN TO A PER-OS ROW.
#
# Until 2xz this file carried one OS's addresses as module constants and
# printed one header that REPLACED the previous OS's. 2xt is what that costs:
# macOS 26 moved the driver's __TEXT by 0x84E1000, every constant had to be
# measured again by hand, and the row that boot 231 ran survived only in git
# history. So now nothing here is hardcoded - every address is read off the
# cache of the machine this runs on - and the output is
# src/plugin/mtlcave/darwin<N>.h, a row BESIDE the others rather than instead
# of them.
#
# That is also what lets a user on Ventura or Sonoma have the userspace half
# of the swizzle fix at all: run this once on their own machine, add the
# include and the RB_OSD_MTLCAVE bit, rebuild. No disassembly and no boot.
#
# The function is found three ways and all three must agree, which is the
# triangulation 2xt did by hand:
#
#   1. LC_SYMTAB has an entry named amdMtl_HWL_DetermineSwizzleMode.
#   2. The 30 bytes at that address are cachepatch.py's known PROLOGUE.
#   3. Those 30 bytes occur exactly ONCE in the whole of __TEXT, which is what
#      makes the pattern safe to use as the detour's anchor.
#
# Rule 26 applies to step 3 in both directions and each gets its own message:
# a count of 1 is the result, 0 means we read the wrong image or the prologue
# changed, and more than 1 means the pattern is not an anchor at all.
import os
import platform
import re

from dyldcache import Image, symtab                             # noqa: E402

DRIVER  = "AMDRadeonX6000MTLDriver"
FN_NAME = "amdMtl_HWL_DetermineSwizzleMode"
ANCHOR  = 48                      # bytes of context before the cave
ZEROS   = 176                     # bytes of the zero run the cave replaces
OUT_DIR = "src/plugin/mtlcave"

OS_NAMES = {22: "Ventura", 23: "Sonoma", 24: "Sequoia", 25: "Tahoe"}


def darwin_major():
    """The Darwin major of the RUNNING kernel - the key the kext looks a row
    up by (version_major, read the same way in both kexts). Taken from the
    machine and never from an argument: a row named for an OS it was not
    derived on is exactly the substituted value rule 17 is about."""
    m = re.match(r"(\d+)\.", platform.release())
    assert m, "cannot read a Darwin major out of %r" % platform.release()
    return int(m.group(1))


def arr(name, b):
    out = ["static const uint8_t %s[%d] = {" % (name, len(b))]
    for i in range(0, len(b), 12):
        out.append("    " + " ".join("0x%02X," % x for x in b[i:i + 12]))
    out.append("};")
    return "\n".join(out)


c = Cache()

# --- the image, its __TEXT and its __const ------------------------------
# Cache.find returns (loadAddress, textSize, path) and exits if nothing
# matches, so a bad DRIVER name can never reach Image() as a tuple.
img_addr, _img_textsize, img_path = c.find(DRIVER)
im = Image(c, img_addr)

TEXT_VM = TEXT_LEN = None
for _name, _va, _vs, _fo, _fs, _sects in im.segs:
    if _name == "__TEXT":
        TEXT_VM, TEXT_LEN = _va, _vs
assert TEXT_VM, "the image has no __TEXT segment"

# __const is inside the r-x __TEXT segment, so code placed there EXECUTES.
# The cave cannot go in __text: the cache builder packs it, and this driver's
# __text has no run of 32 identical bytes anywhere (2oa).
sec = im.section("__TEXT", "__const")
assert sec, "the image has no __TEXT,__const section to put a cave in"
CONST_VM, CONST_LEN = sec

# --- the function, three independent ways -------------------------------
# The symbol is C++-mangled with Apple's leading underscore:
# `__Z31amdMtl_HWL_DetermineSwizzleModeP18AMDCommonHwInfoRec...`. Rule 5 says a
# hand-written mangled name with the wrong length prefix silently matches
# nothing, so the prefix is COMPUTED from the name rather than typed: 31 is
# len("amdMtl_HWL_DetermineSwizzleMode"). The parameter list is deliberately
# not part of the match - a signature change should still find the function and
# then fail on the prologue check, which says something useful, rather than
# fail here, which would say the symbol is gone.
MANGLED = "_Z%d%s" % (len(FN_NAME), FN_NAME)
syms = sorted({a for a, n, _t in symtab(c, im)
               if n.startswith("_" + MANGLED) or n.startswith(MANGLED)
               or n == FN_NAME})
assert syms, ("LC_SYMTAB has no symbol starting %s and none named %s; the "
              "driver was rebuilt with a different name and the cave has to "
              "be re-derived by hand" % (MANGLED, FN_NAME))
assert len(syms) == 1, ("%s appears at %d addresses (%s) - it is not a unique "
                        "symbol here" % (MANGLED, len(syms),
                                         ", ".join("%#x" % a for a in syms)))
FN_VM = syms[0]

fnfind = c.read(FN_VM, len(cp.PROLOGUE))
assert fnfind == cp.PROLOGUE, (
    "%s is at %#x but its first %d bytes are not the prologue this tool knows "
    "how to detour - the function was rebuilt, re-derive by hand"
    % (FN_NAME, FN_VM, len(cp.PROLOGUE)))

text = c.read(TEXT_VM, TEXT_LEN)
hits = text.count(fnfind)
assert hits != 0, "the prologue is not in __TEXT at all - wrong image read"
assert hits == 1, ("the prologue occurs %d times in __TEXT, so it is not an "
                   "anchor and the detour cannot be placed by pattern" % hits)

# --- the cave: a zero run in __const with a UNIQUE anchor ---------------
const = c.read(CONST_VM, CONST_LEN)


def pick_cave():
    """The first 16-aligned offset whose next ZEROS bytes are all zero AND
    whose ANCHOR bytes of preceding context make the whole pattern unique in
    __const. Both halves matter: the zero run is what the cave may overwrite,
    and the anchor is the only thing that tells one run of zeroes from the 27
    others this section has."""
    for off in range(ANCHOR, CONST_LEN - ZEROS, 16):
        if any(const[off:off + ZEROS]):
            continue
        pat = const[off - ANCHOR:off + ZEROS]
        if const.count(pat) == 1:
            return CONST_VM + off, pat
    return None, None


CAVE_VM, find = pick_cave()
assert CAVE_VM, ("no 16-aligned %d-byte zero run in __const has a unique "
                 "%d-byte anchor; nothing can be placed by pattern here"
                 % (ZEROS, ANCHOR))
assert set(find[ANCHOR:]) == {0}, "the reserved area is not all zero"

# build_cave()'s jump back to FN+30 and detour()'s rel32 both read
# cachepatch.py's OWN module constants, so they are rebased here. The line was
# latent until macOS 26 moved the driver (2xt) and is load-bearing now that
# both values are derived per machine rather than written down.
cp.CAVE_VM = CAVE_VM
cp.FN_VM = FN_VM
cave = cp.build_cave()
assert len(cave) <= ZEROS, "cave is %d bytes, only %d reserved" % (len(cave), ZEROS)

repl = find[:ANCHOR] + cave + bytes(ZEROS - len(cave))
fnrepl = cp.detour() + fnfind[5:]

D = darwin_major()
OS = OS_NAMES.get(D, "darwin%d" % D)
TAG = "d%d" % D
path = os.path.join(OUT_DIR, "darwin%d.h" % D)

HDR = """/* GENERATED by tools/gen_cavepatch.py - do not edit by hand.
 *
 * The AMDRadeonX6000MTLDriver swizzle-fix row for Darwin {D} ({OS}), derived
 * on {PLAT} from that machine's own shared cache.
 *
 *   function  {FN:#x}  {FNNAME}
 *   cave      {CV:#x}  in __const, {USED} bytes used of {ZEROS} reserved
 *   __TEXT    {TX:#x} (the LC_SEGMENT_64 vmaddr)
 *
 * The function was confirmed three ways and all three agreed: LC_SYMTAB names
 * it at this address, the {PLEN} bytes there are the known prologue, and those
 * bytes occur exactly once in __TEXT. The cave anchor was likewise checked
 * unique in __const - a run of zeroes is identifiable only by its preceding
 * context, and this section has 27 other runs of them.
 */
#ifndef RBGPU_MTLCAVE_D{D}_H
#define RBGPU_MTLCAVE_D{D}_H
#include <stdint.h>

{A1}

{A2}

{A3}

{A4}

#define RB_MTLCAVE_ROW_D{D} {{                   \\
    /* darwin        */ {D},                  \\
    /* os            */ "{OS}",               \\
    /* text_vm       */ {TX:#x}ULL,           \\
    /* fn_vm         */ {FN:#x}ULL,           \\
    /* cave_vm       */ {CV:#x}ULL,           \\
    /* cave_anchor   */ {ANCHOR}u,            \\
    /* cave_reserved */ {ZEROS}u,             \\
    /* cave_used     */ {USED}u,              \\
    /* cave_find     */ kRbMtlCaveFind_{TAG}, \\
    /* cave_repl     */ kRbMtlCaveRepl_{TAG}, \\
    /* cave_len      */ {CLEN}u,              \\
    /* fn_find       */ kRbMtlFnFind_{TAG},   \\
    /* fn_repl       */ kRbMtlFnRepl_{TAG},   \\
    /* fn_len        */ {PLEN}u,              \\
}}

#endif
"""

body = HDR.format(
    D=D, OS=OS, PLAT=platform.platform(), FNNAME=FN_NAME,
    FN=FN_VM, CV=CAVE_VM, TX=TEXT_VM, USED=len(cave), ZEROS=ZEROS,
    ANCHOR=ANCHOR, CLEN=len(find), PLEN=len(fnfind), TAG=TAG,
    A1=arr("kRbMtlCaveFind_" + TAG, find),
    A2=arr("kRbMtlCaveRepl_" + TAG, repl),
    A3=arr("kRbMtlFnFind_" + TAG, fnfind),
    A4=arr("kRbMtlFnRepl_" + TAG, fnrepl))

os.makedirs(OUT_DIR, exist_ok=True)
existed = os.path.exists(path)
with open(path, "w") as fh:
    fh.write(body)

# 3mip: EVERY ROW CARRIES THE SAME POLICY, BY CONSTRUCTION.
#
# A row is addresses plus the cave's bytes, and the cave's bytes are the
# policy. Until 3mip a change to build_cave() reached only the row of the OS
# this ran on - the others kept whatever policy they were generated with, and
# nothing said so. The cave is position-dependent in exactly one place (the
# trampoline's rel32 back to FN+30), and a row records the two addresses that
# displacement is made of. So every OTHER row's replacement is re-emitted here
# from its OWN recorded fn_vm and cave_vm: nothing about that OS is re-derived
# or assumed, only the policy bytes move. tools/hosttest/osver_test.c asserts
# the rows agree byte for byte outside that one displacement.
def rebase_row(p):
    src = open(p).read()

    def num(field):
        m = re.search(r"/\* %s\s*\*/ (0x[0-9a-fA-F]+|\d+)" % field, src)
        assert m, "%s: no %s in the row" % (p, field)
        return int(m.group(1), 0)

    def array(prefix):
        m = re.search(r"static const uint8_t (%s_d\d+)\[(\d+)\] = \{(.*?)\};"
                      % prefix, src, re.S)
        assert m, "%s: no %s array" % (p, prefix)
        data = bytes(int(x, 16) for x in re.findall(r"0x([0-9A-Fa-f]{2})", m.group(3)))
        assert len(data) == int(m.group(2)), "%s: %s length disagrees" % (p, prefix)
        return m, data

    anchor, reserved, used = num("cave_anchor"), num("cave_reserved"), num("cave_used")
    _fm, rfind = array("kRbMtlCaveFind")
    rm, rrepl = array("kRbMtlCaveRepl")
    assert len(rfind) == anchor + reserved and set(rfind[anchor:]) == {0}, \
        "%s: the find pattern is not anchor + zero run" % p
    cp.CAVE_VM, cp.FN_VM = num("cave_vm"), num("fn_vm")
    rcave = cp.build_cave()
    assert len(rcave) <= reserved, "%s: cave is %d bytes, %d reserved" % (p, len(rcave), reserved)
    new = rfind[:anchor] + rcave + bytes(reserved - len(rcave))
    if new == rrepl:
        return False
    src = src[:rm.start()] + arr(rm.group(1), new) + src[rm.end():]
    src = re.sub(r"(/\* cave_used\s*\*/ )\d+u", r"\g<1>%du" % len(rcave), src)
    src = src.replace("%d bytes used of" % used, "%d bytes used of" % len(rcave))
    with open(p, "w") as fh:
        fh.write(src)
    return True


rebased = []
for _f in sorted(os.listdir(OUT_DIR)):
    _p = os.path.join(OUT_DIR, _f)
    if re.fullmatch(r"darwin\d+\.h", _f) and _p != path:
        rebased.append((_p, rebase_row(_p)))
cp.CAVE_VM, cp.FN_VM = CAVE_VM, FN_VM

e = sys.stderr
print("%s %s" % ("rewrote" if existed else "wrote", path), file=e)
for _p, _changed in rebased:
    print("%s %s (policy bytes only, from its own recorded addresses)"
          % ("rebased" if _changed else "unchanged", _p), file=e)
print("  Darwin %d (%s), %s" % (D, OS, platform.platform()), file=e)
print("  image  %s at %#x" % (img_path, img_addr), file=e)
print("  __TEXT %#x  fn %#x  cave %#x" % (TEXT_VM, FN_VM, CAVE_VM), file=e)
print("  cave %dB of %d reserved; anchor %dB unique in __const; prologue "
      "unique in __TEXT" % (len(cave), ZEROS, ANCHOR), file=e)
if not existed:
    print("""
TWO THINGS ARE STILL BY HAND, and both are deliberate - a row appearing in
the tree must not by itself make the kext claim an OS:

  1. src/plugin/rbgpu_mtlcave.h: add `#include "mtlcave/darwin%d.h"` and
     `RB_MTLCAVE_ROW_D%d,` to rb_mtlcave_rows[].
  2. src/common/rb_osver.h: add RB_OSD_MTLCAVE to the Darwin %d row, and say
     in its note where the row came from.

tools/hosttest/osver_test.c asserts those two agree with each other, so
leaving one out fails the build instead of shipping half a claim.""" % (D, D, D),
          file=e)
