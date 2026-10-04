#!/usr/bin/env python3
"""Small reader for Mach-O files produced by kc_extract.py: map virtual
addresses to file bytes, decode kernel-collection chained pointers, read C
strings, look up symbols.

    from machoread import Macho
    m = Macho("build/re/AMDRadeonX6000HWServices")
    m.ptr(0xc2e9730)            # decoded pointer stored at that address
    m.cstr(m.ptr(0xc2e9730))    # the C string it points to
    m.sym("__ZN...")            # address of a symbol

Pointers inside a kernel collection are stored as chained fixups
(DYLD_CHAINED_PTR_X86_64_KERNEL_CACHE): the low 30 bits are the target's
offset from the collection's base, which for the pageable (System) collection
is 0, so the target virtual address; the high bits hold the chain's "next"
field, so any value above 32 bits is a fixup. ptr() decodes that; raw64()
returns the stored value.
"""
import struct
import subprocess


class Macho:
    def __init__(self, path):
        self.path = path
        self.data = open(path, "rb").read()
        self.segs = []
        ncmds = struct.unpack_from("<I", self.data, 16)[0]
        off = 32
        for _ in range(ncmds):
            cmd, size = struct.unpack_from("<II", self.data, off)
            if cmd == 0x19:
                name = self.data[off + 8:off + 24].split(b"\0", 1)[0].decode()
                vmaddr, vmsize, fileoff, filesize = struct.unpack_from("<QQQQ", self.data, off + 24)
                self.segs.append((name, vmaddr, vmsize, fileoff, filesize))
            off += size
        self._syms = None

    def off(self, va):
        for name, vmaddr, vmsize, fileoff, filesize in self.segs:
            if vmaddr <= va < vmaddr + filesize and name != "__LINKEDIT":
                return fileoff + (va - vmaddr)
        return None

    def raw64(self, va):
        o = self.off(va)
        return None if o is None else struct.unpack_from("<Q", self.data, o)[0]

    def u32(self, va):
        o = self.off(va)
        return None if o is None else struct.unpack_from("<I", self.data, o)[0]

    def ptr(self, va):
        v = self.raw64(va)
        if v is None:
            return None
        if v >> 32:                  # chained fixup (target:30 cacheLevel:2
            return v & 0x3FFFFFFF    # diversity:16 addrDiv:1 key:2 next:12 isAuth:1)
        return v

    def cstr(self, va, limit=256):
        o = self.off(va) if va else None
        if o is None:
            return None
        end = self.data.find(b"\0", o, o + limit)
        return self.data[o:end].decode("latin-1")

    def syms(self):
        if self._syms is None:
            out = subprocess.run(["nm", "-n", self.path], capture_output=True, text=True).stdout
            self._syms = []
            for line in out.splitlines():
                parts = line.split()
                if len(parts) == 3:
                    self._syms.append((int(parts[0], 16), parts[1], parts[2]))
        return self._syms

    def sym(self, name):
        for a, t, n in self.syms():
            if n == name:
                return a
        return None

    def name_at(self, va):
        best = None
        for a, t, n in self.syms():
            if a <= va:
                best = (a, n)
            else:
                break
        return None if best is None else (best[1], va - best[0])


def find_ptrs(m, targets):
    """Every 8-byte-aligned slot in the data segments whose decoded pointer is
    in `targets`: {target: [slot addresses]}."""
    targets = set(targets)
    hits = {t: [] for t in targets}
    for name, vmaddr, vmsize, fileoff, filesize in m.segs:
        if not name.startswith("__DATA"):
            continue
        for i in range(0, filesize - 7, 8):
            v = struct.unpack_from("<Q", m.data, fileoff + i)[0]
            t = (v & 0x3FFFFFFF) if v >> 32 else v
            if t in targets:
                hits[t].append(vmaddr + i)
    return hits
