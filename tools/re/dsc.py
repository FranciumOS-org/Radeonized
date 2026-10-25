#!/usr/bin/env python3
"""Minimal dyld shared cache reader: VA <-> file offset, string search, and
RIP-relative LEA cross-references.

The cache cannot be extracted as a standalone Mach-O with the tools on this
machine (no `otool`, and `nm`/`objdump` refuse the cache-resident path), but
everything needed for static analysis is reachable directly: the header's
mappings give VA <-> file offset, `dyld_info -segments` gives an image's
unslid segment bases, and a LEA scan finds the code that references a string.

    python3 tools/re/dsc.py maps  <cache>
    python3 tools/re/dsc.py xref  <cache> <text_va> <text_len> <target_va>
    python3 tools/re/dsc.py dump  <cache> <va> <len> > out.bin
"""
import struct
import sys


class Cache:
    """The whole cache, ALL of its files.

    2r2: this used to be `open(path)` on the single path given, enumerating
    only that file's mappings - and the cache on this machine is **nine**
    files, each subcache carrying its own executable mapping at a distinct VA
    base (main 0x7ff800000000+0x25198000, .01 0x7ff900000000, .02
    0x7ffa00000000, .03 0x7ffb00000000, ...). The VA space is global across
    them, so code in .01 can reference a string stored in the main file. Every
    `xref`/`callxref` negative this project produced was therefore only one
    file wide, and that is how the referencer of "Unable to get AGDC
    information" stayed invisible through four searches whose controls all
    passed.

    Subcaches are found by globbing siblings rather than by parsing the
    header's subCacheArray, because the glob is checkable by eye and the
    header layout has moved between OS versions. `.symbols` is skipped: it
    carries no mappings of interest and its VAs overlap nothing.
    """

    def __init__(self, path):
        import glob as _glob
        import os as _os

        base = path
        for suffix in (".01", ".02", ".03", ".04", ".05", ".06", ".07", ".08"):
            if base.endswith(suffix):
                base = base[: -len(suffix)]
                break
        paths = [base] + sorted(
            q for q in _glob.glob(base + ".*")
            if _os.path.basename(q).split(".")[-1].isdigit()
        )

        self.files = []        # open file objects, main first
        self.regions = []      # (addr, size, fileoff, initp, fileobj, name)
        self.maps = []         # (addr, size, fileoff, initp) - legacy shape
        for q in paths:
            try:
                f = open(q, "rb")
            except OSError:
                continue
            h = f.read(0x200)
            if h[:16].rstrip(b"\x00") == b"" or len(h) < 0x200:
                f.close()
                continue
            mo, mc = struct.unpack_from("<II", h, 16)
            if mc == 0 or mc > 64:
                f.close()
                continue
            f.seek(mo)
            got = []
            for _ in range(mc):
                rec = f.read(32)
                if len(rec) < 32:
                    break
                addr, size, fo, maxp, initp = struct.unpack("<QQQII", rec)
                got.append((addr, size, fo, initp))
            if not got:
                f.close()
                continue
            self.files.append(f)
            name = _os.path.basename(q)
            for addr, size, fo, initp in got:
                self.regions.append((addr, size, fo, initp, f, name))
                self.maps.append((addr, size, fo, initp))
        if not self.regions:
            raise ValueError("no mappings found for %s" % path)
        self.f = self.files[0]   # kept: some callers still touch it

    def _loc(self, va):
        """(fileobj, offset) for a VA, searched across every file."""
        for addr, size, fo, _, f, _n in self.regions:
            if addr <= va < addr + size:
                return f, fo + (va - addr)
        return None, None

    def va2off(self, va):
        """Offset WITHIN THAT VA's OWN FILE. Ambiguous across files on its
        own, so `read` uses `_loc` instead; kept for callers that pair it
        with `file_of`."""
        _f, o = self._loc(va)
        return o

    def file_of(self, va):
        for addr, size, _fo, _ip, _f, name in self.regions:
            if addr <= va < addr + size:
                return name
        return None

    def off2va(self, off, name=None):
        """File offsets are per-file, so this needs the file to be meaningful
        once there is more than one. `name` defaults to the main cache."""
        want = name or self.regions[0][5]
        for addr, size, fo, _ip, _f, n in self.regions:
            if n == want and fo <= off < fo + size:
                return addr + (off - fo)
        return None

    def read(self, va, n):
        f, o = self._loc(va)
        if o is None:
            raise ValueError("va 0x%x is not mapped" % va)
        f.seek(o)
        return f.read(n)

    def exec_ranges(self):
        """Every executable mapping, across all files: (va, size, name).
        This is what a cache-wide code scan has to iterate."""
        return [(a, s, n) for a, s, _fo, ip, _f, n in self.regions if ip & 4]


def xref(c, text_va, text_len, target):
    """Every `lea <reg>, [rip+disp32]` in [text_va, text_va+text_len) whose
    target is `target`.

    Uses bytes.find per 3-byte opcode prefix rather than a per-byte Python
    loop, so the whole 622 MB r-x mapping can be scanned in seconds. The
    prefixes are REX.W (48 for regs 0-7, 4C for 8-15) + 8D + modrm with
    mod=00 and rm=101, i.e. RIP-relative with a disp32.

    NOTE a bare `8D` with no REX also exists but writes a 32-bit register,
    which is never used to form a 64-bit string pointer, so it is excluded
    deliberately rather than overlooked.
    """
    blob = c.read(text_va, text_len)
    out = []
    for rex in (0x48, 0x4C):
        for reg in range(8):
            pre = bytes((rex, 0x8D, 0x05 | (reg << 3)))
            i = blob.find(pre)
            while i != -1:
                if i + 7 <= len(blob):
                    disp = struct.unpack_from("<i", blob, i + 3)[0]
                    site = text_va + i
                    if site + 7 + disp == target:
                        out.append((site, reg | (8 if rex == 0x4C else 0)))
                i = blob.find(pre, i + 1)
    out.sort()
    return out


REGS = ["rax","rcx","rdx","rbx","rsp","rbp","rsi","rdi",
        "r8","r9","r10","r11","r12","r13","r14","r15"]


def slide_regions(path):
    """The cache's mappings that carry slide info, with their v2 parameters.

    Data pointers in those mappings are NOT stored as plain addresses: a
    64-bit slot holds the target in the low bits and a chain delta in
    `delta_mask`, so a raw byte search for a pointer finds nothing there.
    `__TEXT` (map0) and the map at 0x7ff880000000 have no slide info, and
    their contents ARE literal - which is why a raw search turns up symbol
    tables and misses vtables entirely.

    Only version 2 is handled; anything else is reported rather than guessed
    at, because decoding the wrong format silently yields plausible garbage.
    """
    f = open(path, "rb")
    h = f.read(0x400)
    mwso, mwsc = struct.unpack_from("<II", h, 0x138)
    out = []
    for i in range(mwsc):
        f.seek(mwso + i * 56)
        d = f.read(56)
        addr, size, fo, sio, sis, _flags = struct.unpack_from("<QQQQQQ", d, 0)
        if not sio:
            continue
        f.seek(sio)
        ver = struct.unpack("<I", f.read(4))[0]
        if ver != 2:
            out.append((addr, size, fo, None, None, ver))
            continue
        f.seek(sio)
        si = f.read(40)
        dmask, vadd = struct.unpack_from("<QQ", si, 24)
        out.append((addr, size, fo, dmask, vadd, ver))
    return out


def ptr_decode(value, dmask, vadd):
    """A v2 slot's target. Zero means "no pointer here", not address vadd."""
    if value == 0:
        return 0
    return (value & ~dmask) + vadd


def dataxref(path, target):
    """Every slide-info slot whose DECODED value is `target`."""
    import array
    c = Cache(path)
    f = open(path, "rb")
    hits = []
    for addr, size, fo, dmask, vadd, ver in slide_regions(path):
        if dmask is None:
            print("  (skipping va 0x%x: slide info version %s, not 2)" % (addr, ver))
            continue
        low = target - vadd
        keep = ~dmask & 0xFFFFFFFFFFFFFFFF
        if low < 0 or low & dmask:
            continue
        f.seek(fo)
        done = 0
        while done < size:
            n = min(1 << 24, size - done)
            n -= n % 8
            if n <= 0:
                break
            a = array.array("Q")
            a.frombytes(f.read(n))
            for k, v in enumerate(a):
                if v and (v & keep) == low:
                    hits.append(addr + done + k * 8)
            done += n
    return hits


def callxref(c, text_va, text_len, target):
    """Direct `call rel32` (E8) and `jmp rel32` (E9) sites whose target is
    `target`. Both are 5 bytes, so the destination is site + 5 + disp32."""
    blob = c.read(text_va, text_len)
    out = []
    for op, name in ((0xE8, "call"), (0xE9, "jmp")):
        i = blob.find(bytes((op,)))
        while i != -1:
            if i + 5 <= len(blob):
                disp = struct.unpack_from("<i", blob, i + 1)[0]
                site = text_va + i
                if site + 5 + disp == target:
                    out.append((site, name))
            i = blob.find(bytes((op,)), i + 1)
    out.sort()
    return out


def setimm(c, text_va, text_len, field, maximm=16):
    """`movl $imm32, field(reg)` for every 64-bit base register, decoding the
    optional REX prefix so r8-r15 are named correctly and not silently
    reported as their low-register twin. Covers mod=01 (disp8) and mod=10
    (disp32); skips rm=100, whose SIB byte shifts the displacement."""
    blob = c.read(text_va, text_len)
    out = []
    for rex in (None, 0x41):                 # no REX, or REX.B for r8-r15
        for rm in range(8):
            if rm == 4:                      # SIB form
                continue
            for mod, dispfmt, dlen in ((0x40, "<b", 1), (0x80, "<i", 4)):
                pre = bytes(([rex] if rex else []) + [0xC7, mod | rm])
                i = blob.find(pre)
                while i != -1:
                    o = i + len(pre)
                    if o + dlen + 4 <= len(blob):
                        disp = struct.unpack_from(dispfmt, blob, o)[0]
                        imm = struct.unpack_from("<I", blob, o + dlen)[0]
                        if disp == field and imm < maximm:
                            out.append((text_va + i, imm,
                                        REGS[rm + (8 if rex else 0)]))
                    i = blob.find(pre, i + 1)
    out.sort()
    return out


def func_start(c, va, back=0x1200):
    """Nearest `push rbp; mov rsp, rbp` (55 48 89 e5) at or before `va`."""
    blob = c.read(va - back, back + 4)
    i = blob.rfind(b"\x55\x48\x89\xe5")
    return None if i == -1 else va - back + i


def macho(blob, vmaddr):
    """Wrap raw bytes in a minimal x86-64 MH_OBJECT with one __TEXT,__text
    section at `vmaddr`, so llvm-objdump disassembles them at the right
    addresses. Needed because this machine has no raw-binary disassembler:
    llvm-objdump has no `-b binary`, there is no llvm-mc and no capstone, and
    `otool` cannot open a cache-resident path at all."""
    hdr = struct.pack("<IiiIIIII", 0xFEEDFACF, 0x01000007, 3, 1, 1, 152, 0, 0)
    dataoff = 32 + 152
    seg = struct.pack("<II16sQQQQiiII", 0x19, 152, b"__TEXT", vmaddr,
                      len(blob), dataoff, len(blob), 7, 7, 1, 0)
    sect = struct.pack("<16s16sQQIIIIIIII", b"__text", b"__TEXT", vmaddr,
                       len(blob), dataoff, 4, 0, 0, 0x80000400, 0, 0, 0)
    return hdr + seg + sect + blob


def main():
    cmd, path = sys.argv[1], sys.argv[2]
    c = Cache(path)
    if cmd == "maps":
        for addr, size, fo, initp, _f, name in c.regions:
            print("va 0x%012x size 0x%09x fileoff 0x%09x prot %d  %s"
                  % (addr, size, fo, initp, name))
        print("# %d file(s), %d mapping(s), %d executable"
              % (len(c.files), len(c.regions), len(c.exec_ranges())),
              file=sys.stderr)
    elif cmd == "ripall":
        # 2r4: EVERY RIP-relative encoding, not just `lea`, across every
        # executable mapping of every cache file.
        #
        # `xref`/`xrefall` match only `lea <reg>,[rip+disp32]`, so their "0
        # hits" means "nothing LEAs it" - which is exactly how the referencer
        # of the AGDC error string survived four searches in 2r2/2r3.
        # `ripxref.py` has carried the full opcode table since 2pz but scans
        # with a per-byte Python loop, which cannot be pointed at 1.4 GB.
        #
        # This is the same table driven by ONE `re.finditer` pass per mapping:
        # the regex matches an optional REX, an opcode from the table, and a
        # modrm with mod=00 / rm=101 (any reg field), all at C speed. The
        # per-match work is then just the length fix-up and one comparison.
        import re as _re
        tgt = int(sys.argv[3], 0)
        ops1 = [0x8b, 0x89, 0x8d, 0x63, 0x03, 0x2b, 0x39, 0x3b, 0x85, 0x01,
                0x29, 0x31, 0xc7, 0x83, 0x81, 0xff, 0x8a, 0x88, 0xc6, 0x80,
                0x38, 0x3a, 0x84, 0x86, 0xf6, 0xf7, 0xfe, 0x00, 0x08, 0x20,
                0x30]
        names = {0x8b: "mov r<-m", 0x89: "mov m<-r", 0x8d: "lea",
                 0x63: "movslq", 0xc7: "mov m,imm", 0x83: "grp1 m,imm8",
                 0x81: "grp1 m,imm32", 0xff: "grp5 m", 0x80: "grp1 m8,imm8",
                 0x8a: "mov r8<-m", 0x88: "mov m<-r8", 0xc6: "mov m8,imm"}
        opcls = b"[" + bytes(ops1) + b"]"
        modrm = b"[\x05\x0d\x15\x1d\x25\x2d\x35\x3d]"
        pat = _re.compile(b"[\x40-\x4f]?" + opcls + modrm, _re.DOTALL)
        extra = {0xc7: 4, 0x81: 4, 0x83: 1, 0xc6: 1, 0x80: 1}
        total = 0
        for va, size, name in c.exec_ranges():
            blob = c.read(va, size)
            for m in pat.finditer(blob):
                i = m.start()
                j = i + 1 if 0x40 <= blob[i] <= 0x4f else i
                op = blob[j]
                if j + 6 > len(blob):
                    continue
                disp = struct.unpack_from("<i", blob, j + 2)[0]
                ilen = (j + 6) - i + extra.get(op, 0)
                if va + i + ilen + disp == tgt:
                    total += 1
                    print("  0x%x   %-14s %s   %s"
                          % (va + i, names.get(op, "op %#x" % op),
                             blob[i:i + ilen].hex(), name))
        print("%d RIP-relative site(s) targeting 0x%x across %d mapping(s)"
              % (total, tgt, len(c.exec_ranges())))
    elif cmd == "xrefall":
        # 2r3: every executable mapping in EVERY cache file. This is the
        # command to use for "does anything reference X"; the single-range
        # `xref` below answers only "does anything in THIS range".
        tgt = int(sys.argv[3], 0)
        regs = ["rax","rcx","rdx","rbx","rsp","rbp","rsi","rdi",
                "r8","r9","r10","r11","r12","r13","r14","r15"]
        total = 0
        for va, size, name in c.exec_ranges():
            hits = xref(c, va, size, tgt)
            total += len(hits)
            for site, reg in hits:
                print("  0x%x   lea %s, [rip+...]   %s"
                      % (site, regs[reg], name))
        print("%d LEA site(s) targeting 0x%x across %d executable mapping(s)"
              % (total, tgt, len(c.exec_ranges())))
    elif cmd == "xref":
        tva, tlen, tgt = (int(x, 0) for x in sys.argv[3:6])
        hits = xref(c, tva, tlen, tgt)
        regs = ["rax","rcx","rdx","rbx","rsp","rbp","rsi","rdi",
                "r8","r9","r10","r11","r12","r13","r14","r15"]
        print("%d LEA site(s) targeting 0x%x:" % (len(hits), tgt))
        for site, reg in hits:
            print("  0x%x   lea %s, [rip+...]" % (site, regs[reg]))
    elif cmd == "dump":
        va, n = int(sys.argv[3], 0), int(sys.argv[4], 0)
        sys.stdout.buffer.write(c.read(va, n))
    elif cmd == "macho":
        va, n = int(sys.argv[3], 0), int(sys.argv[4], 0)
        sys.stdout.buffer.write(macho(c.read(va, n), va))
    elif cmd == "callxref":
        tva, tlen, tgt = (int(x, 0) for x in sys.argv[3:6])
        hits = callxref(c, tva, tlen, tgt)
        print("%d direct branch(es) to 0x%x:" % (len(hits), tgt))
        for site, kind in hits:
            print("  0x%x   %s" % (site, kind))
    elif cmd == "setimm":
        tva, tlen, field = (int(x, 0) for x in sys.argv[3:6])
        for site, imm, reg in setimm(c, tva, tlen, field):
            print("  0x%x   movl $%d, 0x%x(%%%s)" % (site, imm, field, reg))
    elif cmd == "slide":
        for addr, size, fo, dmask, vadd, ver in slide_regions(path):
            print("va 0x%012x size 0x%09x v%s delta_mask 0x%016x value_add 0x%x"
                  % (addr, size, ver, dmask or 0, vadd or 0))
    elif cmd == "dataxref":
        tgt = int(sys.argv[3], 0)
        hits = dataxref(path, tgt)
        print("%d slide-info slot(s) decoding to 0x%x:" % (len(hits), tgt))
        for v in hits:
            print("  0x%x" % v)
    elif cmd == "rdptr":
        va = int(sys.argv[3], 0)
        for addr, size, fo, dmask, vadd, ver in slide_regions(path):
            if dmask is not None and addr <= va < addr + size:
                raw = struct.unpack("<Q", c.read(va, 8))[0]
                print("raw 0x%016x -> 0x%x" % (raw, ptr_decode(raw, dmask, vadd)))
                return 0
        raw = struct.unpack("<Q", c.read(va, 8))[0]
        print("raw 0x%016x (no slide info for this mapping: literal)" % raw)
    elif cmd == "func":
        va = int(sys.argv[3], 0)
        f = func_start(c, va)
        print("0x%x" % f if f else "no prologue found")
    else:
        print(__doc__)
        return 2
    return 0


if __name__ == "__main__":
    sys.exit(main())
