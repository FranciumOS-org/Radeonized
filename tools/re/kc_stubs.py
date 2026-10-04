#!/usr/bin/env python3
"""Resolve the System kernel collection's branch stubs to symbol names, and
annotate llvm-objdump output with them.

    python3 tools/re/kc_stubs.py map  build/re/stubs.json
    python3 tools/re/kc_stubs.py annotate build/re/stubs.json < x.dis > x.ann.dis

Kexts in the pageable (System) collection call the kernel and boot kexts
through __BRANCH_STUBS (6-byte `jmp *disp32(%rip)`) and __BRANCH_GOTS, whose
slots are chained pointers: target = low 30 bits, cacheLevel = bits 30-31
(0 = boot collection, 1 = this collection). A boot-collection target is an
offset from a base that is not the collection's __TEXT (on macOS 15.8.1 it
is 0xffffff8000100000, 1 MB below it). The base is chosen as the 1 MB-aligned
candidate that resolves the most stubs, and the count is printed. Symbols come from every
fileset entry's LC_SYMTAB in each collection.
"""
import json
import re
import struct
import sys

SYS = "/System/Library/KernelCollections/SystemKernelExtensions.kc"
BOOT = "/System/Library/KernelCollections/BootKernelExtensions.kc"


def segments(kc, base_off=0):
    ncmds = struct.unpack_from("<I", kc, base_off + 16)[0]
    off, segs, entries, symtab = base_off + 32, {}, [], None
    for _ in range(ncmds):
        cmd, size = struct.unpack_from("<II", kc, off)
        if cmd == 0x19:
            name = kc[off + 8:off + 24].split(b"\0", 1)[0].decode()
            segs.setdefault(name, struct.unpack_from("<QQQQ", kc, off + 24))
        elif cmd == 0x80000035:
            vm, fo, no = struct.unpack_from("<QQI", kc, off + 8)
            entries.append((kc[off + no:off + size].split(b"\0", 1)[0].decode(), vm, fo))
        elif cmd == 0x2:
            symtab = struct.unpack_from("<IIII", kc, off + 8)
        off += size
    return segs, entries, symtab


def entry_symbols(kc):
    """address -> name over every fileset entry (defined symbols only)."""
    _, entries, _ = segments(kc)
    out = {}
    for name, vm, fo in entries:
        _, _, st = segments(kc, fo)
        if not st:
            continue
        symoff, nsyms, stroff, _ = st
        for i in range(nsyms):
            strx, ntype, sect, desc, value = struct.unpack_from("<IBBhQ", kc, symoff + 16 * i)
            if ntype & 0xE == 0xE and value:          # N_SECT
                end = kc.find(b"\0", stroff + strx)
                out.setdefault(value, kc[stroff + strx:end].decode("latin-1"))
    return out


def build_map():
    sysk = open(SYS, "rb").read()
    boot = open(BOOT, "rb").read()
    ssegs, _, _ = segments(sysk)
    bsegs, _, _ = segments(boot)
    boot_syms = entry_symbols(boot)
    sys_syms = entry_symbols(sysk)
    vm, size, fo, _ = ssegs["__BRANCH_STUBS"]
    gvm, gsize, gfo, _ = ssegs["__BRANCH_GOTS"]
    slots = []
    for s in range(vm, vm + size, 6):
        b = sysk[fo + (s - vm):fo + (s - vm) + 6]
        if b[:2] != b"\xff\x25":
            continue
        got = s + 6 + struct.unpack_from("<i", b, 2)[0]
        if not (gvm <= got < gvm + gsize):
            continue
        v = struct.unpack_from("<Q", sysk, gfo + (got - gvm))[0]
        slots.append((s, v & 0x3FFFFFFF, (v >> 30) & 3))
    text = bsegs["__TEXT"][0]
    candidates = [text - k * 0x100000 for k in range(0, 4)]
    boot_base = max(candidates, key=lambda b: sum(1 for _, t, lv in slots if lv == 0 and b + t in boot_syms))
    result = {}
    for s, target, level in slots:
        name = boot_syms.get(boot_base + target) if level == 0 else sys_syms.get(target)
        if name:
            result[hex(s)] = name
    print(f"boot base {boot_base:#x}; {len(result)} of {len(slots)} stubs resolved", file=sys.stderr)
    return result


def annotate(mapping):
    pat = re.compile(r"<_[A-Za-z0-9_]+\+(0x[0-9a-f]+)>")
    for line in sys.stdin:
        m = re.search(r"(?:callq|jmp)\s+(0x[0-9a-f]+)\s+<[^>]*>", line)
        if m and m.group(1) in mapping:
            line = pat.sub(f"<{mapping[m.group(1)]}>", line, count=1)
        sys.stdout.write(line)


def main():
    if sys.argv[1] == "map":
        mp = build_map()
        json.dump(mp, open(sys.argv[2], "w"), indent=0)
        print(f"{len(mp)} stubs resolved -> {sys.argv[2]}")
    else:
        annotate(json.load(open(sys.argv[2])))
    return 0


if __name__ == "__main__":
    sys.exit(main())
