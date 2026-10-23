#!/usr/bin/env python3
"""Emit the byte-source Mach-O that lets Lilu patch a dylib with NO file on disk.

2oc established the mechanism. Lilu's user patcher takes patch *addresses*
from the shared cache's .map, but it takes the patch *bytes* it searches for
from a real file:

    loadFilesForPatching():
        auto buf = FileIO::readFileToBuffer(binaryMod[i]->path, fileSize);
        if (buf) { findSectionBounds(...); scan for patch.find; build lookupStorage; }
        // no else, no SYSLOG, returns true regardless

AMDRadeonX6000MTLDriver lives only inside the cache, so that read fails,
lookupStorage stays empty and nothing is ever patched - silently. Boot A.

What Lilu actually needs from the file is one number per patch:

    segOff = vmsection - vmsegment + sectOff          (loadFilesForPatching)
    place  = mod->startTEXT + segOff + slide          (patchSharedCache)

and MachInfo::findSectionBounds validates nothing but bounds - no code
signature, no LINKEDIT, no dyld info. It just reads scmd->vmaddr,
sect->addr, sect->offset, sect->size out of the load commands. So a small
hand-built Mach-O that merely DECLARES the right geometry is a sufficient
byte source. It is not a copy of the driver and does not have to be.

Two traps in Lilu's scanner that shape the layout, both found by reading it:

  * `end = start + size - patch.size` with `while (start < end ...)`, so a
    section whose size EQUALS patch.size never scans. Size must exceed
    sectOff + patch.size.
  * `pageOff = ((vmsection + sectOff) & -PAGE_SIZE) - vmsection` is computed
    in vm_address_t. If sectOff < vmsection % PAGE_SIZE it UNDERFLOWS and
    `lilu_os_memcpy(page->p, sectionptr + pageOff, PAGE_SIZE)` then reads a
    wild kernel address. Keeping vmsection page-aligned makes pageOff
    exactly `sectOff & -PAGE_SIZE`, which is always in range.

Patterns come from the generated header, not from the cache, so the stencil
and the compiled kext can never disagree about what they are looking for.

    python3 tools/gen_stencil.py build/out/rbgpu-mtl.stencil
"""
import re
import struct
import sys

HDR = "src/plugin/rbgpu_mtlcave.h"
PAGE = 0x1000
SECT_SIZE = 0x2000          # > any sectOff + patch.size, and >= one page
FILL = 0xDB                 # cannot begin either find pattern


def header_bits():
    src = open(HDR).read()

    def const(name):
        m = re.search(rf"#define\s+{name}\s+0x([0-9a-fA-F]+)ULL", src)
        if not m:
            raise SystemExit(f"{HDR}: {name} not found")
        return int(m.group(1), 16)

    def array(name):
        m = re.search(rf"static const uint8_t {name}\[\d+\] = \{{(.*?)\}};", src, re.S)
        if not m:
            raise SystemExit(f"{HDR}: {name} not found")
        return bytes(int(x, 16) for x in re.findall(r"0x([0-9a-fA-F]{2})", m.group(1)))

    return const, array


const, array = header_bits()
TEXT_VM = const("RBGPU_MTL_TEXT_VM")
FN_VM = const("RBGPU_MTL_FN_VM")
CAVE_VM = const("RBGPU_MTL_CAVE_VM")
cave_find = array("kRbMtlCaveFind")
fn_find = array("kRbMtlFnFind")

# The cave patch does not start AT the cave: it starts ANCHOR bytes earlier,
# because the cave sits in an all-zero run and needs preceding context to be
# unique in __const. The anchor cannot be recovered by counting trailing
# zeros - the context itself ends in zeros - so take the reserved size from
# the header's own statement of it and assert the shape that implies.
m = re.search(r"in __const, (\d+) bytes used of (\d+)", open(HDR).read())
if not m:
    raise SystemExit(f"{HDR}: cannot find the reserved-bytes comment")
reserved = int(m.group(2))
anchor = len(cave_find) - reserved
if anchor <= 0:
    raise SystemExit(f"reserved {reserved} >= pattern {len(cave_find)}")
if set(cave_find[anchor:]) != {0}:
    raise SystemExit(f"the last {reserved} bytes of kRbMtlCaveFind are not all zero, "
                     "so the anchor is not where the header says")
cave_site = CAVE_VM - anchor

sites = [
    # name        segment   section     target VM   find bytes
    ("__text",   b"__TEXT", b"__text",  FN_VM,     fn_find),
    ("__const",  b"__TEXT", b"__const", cave_site, cave_find),
]

# vmsegment is 0, so segOff == vmsection + sectOff. Split each target's
# segment-relative offset into a page-aligned vmsection and an in-page
# sectOff, which keeps pageOff non-negative (see the docstring).
plan = []
for name, seg, sect, target, find in sites:
    segoff = target - TEXT_VM
    if segoff < 0:
        raise SystemExit(f"{name}: target 0x{target:x} is below __TEXT")
    vmsection = segoff & ~(PAGE - 1)
    sectoff = segoff & (PAGE - 1)
    assert vmsection + sectoff == segoff
    assert SECT_SIZE > sectoff + len(find), f"{name}: section too small"
    assert (sectoff & ~(PAGE - 1)) + PAGE <= SECT_SIZE, f"{name}: page copy out of range"
    plan.append((name, seg, sect, vmsection, sectoff, find, segoff))

# ---- assemble the Mach-O -------------------------------------------------
MH_MAGIC_64, CPU_TYPE_X86_64, MH_DYLIB = 0xFEEDFACF, 0x01000007, 6
LC_SEGMENT_64 = 0x19
HDR_SIZE, SEG_SIZE, SECT_HDR = 32, 72, 80

nsects = len(plan)
cmdsize = SEG_SIZE + SECT_HDR * nsects
data_off = PAGE                              # first section's file offset

offsets = [data_off + i * SECT_SIZE for i in range(nsects)]
filesize = offsets[-1] + SECT_SIZE

out = bytearray(filesize)
out[0:HDR_SIZE] = struct.pack("<IiiIIIII", MH_MAGIC_64, CPU_TYPE_X86_64, 3,
                              MH_DYLIB, 1, cmdsize, 0, 0)
# vmsize must cover the declared section addresses, or otherwise sane tools
# reject the file even though findSectionBounds would not care.
vmsize = max(vmsection + SECT_SIZE for _, _, _, vmsection, _, _, _ in plan)
vmsize = (vmsize + PAGE - 1) & ~(PAGE - 1)

out[HDR_SIZE:HDR_SIZE + SEG_SIZE] = struct.pack(
    "<II16sQQQQiiII", LC_SEGMENT_64, cmdsize, b"__TEXT",
    0,                                       # vmaddr  -> vmsegment == 0
    vmsize, 0, filesize,
    5, 5,                                    # maxprot/initprot r-x
    nsects, 0)

p = HDR_SIZE + SEG_SIZE
for (name, seg, sect, vmsection, sectoff, find, segoff), off in zip(plan, offsets):
    out[p:p + SECT_HDR] = struct.pack("<16s16sQQIIIIIIII", sect, seg,
                                      vmsection, SECT_SIZE, off,
                                      0, 0, 0, 0, 0, 0, 0)
    p += SECT_HDR
    for i in range(off, off + SECT_SIZE):
        out[i] = FILL
    out[off + sectoff:off + sectoff + len(find)] = find

# ---- verify the emitted file the way Lilu will read it ------------------
for (name, seg, sect, vmsection, sectoff, find, segoff), off in zip(plan, offsets):
    body = bytes(out[off:off + SECT_SIZE])
    hits = body.count(find)
    if hits != 1:
        raise SystemExit(f"{name}: find pattern occurs {hits} times, need exactly 1")
    if body.index(find) != sectoff:
        raise SystemExit(f"{name}: pattern at {body.index(find):#x}, expected {sectoff:#x}")

# ---- the end-to-end check: does startTEXT + segOff really hold these bytes
# in the LIVE cache? This is the one thing that validates the whole address
# chain (vmsection + sectOff -> segOff -> place) short of a boot. The only
# runtime unknowns left are `slide` and startTEXT, which comes from the .map.
try:
    sys.path.insert(0, "tools/re")
    from dyldcache import Cache
    cache = Cache()
except Exception as e:                                        # noqa: BLE001
    print(f"WARNING: cannot read the shared cache ({e}); geometry UNVERIFIED",
          file=sys.stderr)
else:
    bad = []
    for name, _, _, _, _, find, segoff in plan:
        place = TEXT_VM + segoff
        if cache.read(place, len(find)) != find:
            bad.append(f"{name} at 0x{place:x}")
    if bad:
        raise SystemExit("the live cache does not hold the find bytes at "
                         + ", ".join(bad) + " - the addresses have moved, "
                         "regenerate src/plugin/rbgpu_mtlcave.h")

dest = sys.argv[1] if len(sys.argv) > 1 else "build/out/rbgpu-mtl.stencil"
open(dest, "wb").write(bytes(out))

print(f"wrote {dest}  ({filesize} bytes)")
print(f"  __TEXT segment vmaddr 0  (so segOff == vmsection + sectOff)")
for (name, seg, sect, vmsection, sectoff, find, segoff), off in zip(plan, offsets):
    place = TEXT_VM + segoff
    print(f"  {name:8s} vmsection 0x{vmsection:06x} sectOff 0x{sectoff:03x} "
          f"-> segOff 0x{segoff:06x}  place 0x{place:x}  ({len(find)} bytes)")
print(f"  startTEXT 0x{TEXT_VM:x} must come from the .map at runtime, not from here")
