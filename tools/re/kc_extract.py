#!/usr/bin/env python3
"""Extract one kext from a kernel collection (MH_FILESET) as a standalone
Mach-O, so otool / llvm-objdump / nm work on it.

    python3 tools/re/kc_extract.py <collection.kc> <bundle-id> <out>
    python3 tools/re/kc_extract.py <collection.kc> --list

Each fileset entry is a Mach-O header inside the collection whose segment and
section offsets are absolute offsets into the collection, and whose symbol and
string tables point into the collection's shared __LINKEDIT. This rebuilds a
self-contained file: segments copied back to back (page aligned) with every
offset rewritten, and a fresh __LINKEDIT holding the entry's symbols, the
string table and its function starts. Other linkedit data (fixups, indirect
symbols, split info) is dropped: the result is for reading, not for loading.

Read-only on the collection; no dependencies beyond the standard library.
"""
import struct
import sys

LC_SEGMENT_64 = 0x19
LC_SYMTAB = 0x2
LC_DYSYMTAB = 0xB
LC_FILESET_ENTRY = 0x80000035
LINKEDIT_DATA_CMDS = {0x1D, 0x1E, 0x26, 0x29, 0x2B, 0x2E, 0x31, 0x80000033, 0x80000034}
LC_FUNCTION_STARTS = 0x26
PAGE = 0x1000


def entries(kc):
    magic, _, _, _, ncmds, _, _, _ = struct.unpack_from("<IiiIIIII", kc, 0)
    assert magic == 0xFEEDFACF, "not a 64-bit Mach-O"
    off = 32
    for _ in range(ncmds):
        cmd, size = struct.unpack_from("<II", kc, off)
        if cmd == LC_FILESET_ENTRY:
            vmaddr, fileoff, name_off = struct.unpack_from("<QQI", kc, off + 8)
            name = kc[off + name_off:off + size].split(b"\0", 1)[0].decode()
            yield name, vmaddr, fileoff
        off += size


def align(x, a=PAGE):
    return (x + a - 1) & ~(a - 1)


def extract(kc, fileoff):
    magic, cputype, cpusub, ftype, ncmds, sizeofcmds, flags, _ = struct.unpack_from("<IiiIIIII", kc, fileoff)
    assert magic == 0xFEEDFACF
    hdr_end = 32 + sizeofcmds
    cmds = bytearray(kc[fileoff + 32:fileoff + hdr_end])

    # Pass 1: collect what needs copying.
    segs, symtab, fstarts = [], None, None
    off = 0
    for _ in range(ncmds):
        cmd, size = struct.unpack_from("<II", cmds, off)
        if cmd == LC_SEGMENT_64:
            name = cmds[off + 8:off + 24].split(b"\0", 1)[0].decode()
            segs.append((off, name))
        elif cmd == LC_SYMTAB:
            symtab = off
        elif cmd == LC_FUNCTION_STARTS:
            fstarts = off
        off += size

    out = bytearray(align(hdr_end))
    linkedit_off = None
    for off, name in segs:
        vmaddr, vmsize, sfo, sfs = struct.unpack_from("<QQQQ", cmds, off + 24)
        nsects = struct.unpack_from("<I", cmds, off + 64)[0]
        if name == "__LINKEDIT":
            linkedit_off = off
            continue
        if name == "__TEXT":
            # __TEXT starts with the Mach-O header itself: keep it at 0.
            new = 0
            data = kc[sfo:sfo + sfs]
            out[0:len(data)] = data if len(data) <= len(out) else data[:len(out)]
            if len(data) > len(out):
                out += data[len(out):]
            out += bytes(align(len(out)) - len(out))
        else:
            new = len(out)
            out += kc[sfo:sfo + sfs]
            out += bytes(align(len(out)) - len(out))
        struct.pack_into("<Q", cmds, off + 40, new)
        for i in range(nsects):
            s = off + 72 + i * 80
            soff = struct.unpack_from("<I", cmds, s + 48)[0]
            if soff:
                struct.pack_into("<I", cmds, s + 48, new + (soff - sfo))
            struct.pack_into("<II", cmds, s + 56, 0, 0)          # reloff, nreloc

    # Fresh __LINKEDIT: symbols, strings, function starts.
    le_start = len(out)
    if symtab is not None:
        symoff, nsyms, stroff, strsize = struct.unpack_from("<IIII", cmds, symtab + 8)
        new_sym = len(out)
        out += kc[symoff:symoff + nsyms * 16]
        new_str = len(out)
        out += kc[stroff:stroff + strsize]
        struct.pack_into("<IIII", cmds, symtab + 8, new_sym, nsyms, new_str, strsize)
    if fstarts is not None:
        dataoff, datasize = struct.unpack_from("<II", cmds, fstarts + 8)
        new_fs = len(out)
        out += kc[dataoff:dataoff + datasize]
        struct.pack_into("<II", cmds, fstarts + 8, new_fs, datasize)
    out += bytes(align(len(out), 8) - len(out))

    # Drop every other linkedit reference.
    off = 0
    for _ in range(ncmds):
        cmd, size = struct.unpack_from("<II", cmds, off)
        if cmd == LC_DYSYMTAB:
            # keep the local/external/undefined symbol index ranges (into the
            # copied symtab), zero the tables we did not copy
            vals = list(struct.unpack_from("<18I", cmds, off + 8))
            for i in range(6, 18):
                vals[i] = 0
            struct.pack_into("<18I", cmds, off + 8, *vals)
        elif cmd in LINKEDIT_DATA_CMDS and off != fstarts:
            struct.pack_into("<II", cmds, off + 8, 0, 0)
        off += size
    if linkedit_off is not None:
        struct.pack_into("<QQ", cmds, linkedit_off + 40, le_start, len(out) - le_start)

    struct.pack_into("<IiiIIIII", out, 0, magic, cputype, cpusub, ftype, ncmds, sizeofcmds, flags, 0)
    out[32:hdr_end] = cmds
    return bytes(out)


def main():
    kc = open(sys.argv[1], "rb").read()
    if sys.argv[2] == "--list":
        for name, vmaddr, fileoff in entries(kc):
            print(f"{vmaddr:#014x} {fileoff:#012x} {name}")
        return 0
    for name, vmaddr, fileoff in entries(kc):
        if name == sys.argv[2]:
            data = extract(kc, fileoff)
            with open(sys.argv[3], "wb") as f:
                f.write(data)
            print(f"{name}: {len(data)} bytes -> {sys.argv[3]}")
            return 0
    print(f"{sys.argv[2]} not in {sys.argv[1]}", file=sys.stderr)
    return 1


if __name__ == "__main__":
    sys.exit(main())
