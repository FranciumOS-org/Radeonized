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
    def __init__(self, path):
        self.f = open(path, "rb")
        h = self.f.read(0x200)
        mo, mc = struct.unpack_from("<II", h, 16)
        self.maps = []
        self.f.seek(mo)
        for _ in range(mc):
            addr, size, fo, maxp, initp = struct.unpack("<QQQII", self.f.read(32))
            self.maps.append((addr, size, fo, initp))

    def va2off(self, va):
        for addr, size, fo, _ in self.maps:
            if addr <= va < addr + size:
                return fo + (va - addr)
        return None

    def off2va(self, off):
        for addr, size, fo, _ in self.maps:
            if fo <= off < fo + size:
                return addr + (off - fo)
        return None

    def read(self, va, n):
        o = self.va2off(va)
        if o is None:
            raise ValueError("va 0x%x is not mapped" % va)
        self.f.seek(o)
        return self.f.read(n)


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
        for addr, size, fo, initp in c.maps:
            print("va 0x%012x size 0x%09x fileoff 0x%09x prot %d"
                  % (addr, size, fo, initp))
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
