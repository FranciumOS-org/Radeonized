#!/usr/bin/env python3
"""Name the virtual method at a vtable byte offset, using a class vtable in
the boot kernel collection (kernel / boot kexts).

    python3 tools/re/vtslots.py __ZTV9IOService 0x6a8 0x128 0x2b8

Offsets are as they appear in `callq *OFF(%reg)`: from the vtable's address
point, which is the vtable symbol + 16.
"""
import struct
import sys
import os

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import kc_stubs as K  # noqa: E402


def main():
    boot = open(K.BOOT, "rb").read()
    syms = K.entry_symbols(boot)
    by_name = {v: k for k, v in syms.items()}
    segs = []
    _, entries, _ = K.segments(boot)
    ncmds = struct.unpack_from("<I", boot, 16)[0]
    off = 32
    for _ in range(ncmds):
        cmd, size = struct.unpack_from("<II", boot, off)
        if cmd == 0x19:
            segs.append(struct.unpack_from("<QQQQ", boot, off + 24))
        off += size
    base = 0xffffff8000100000

    def read(va):
        for vm, vs, fo, fs in segs:
            if vm <= va < vm + fs:
                return struct.unpack_from("<Q", boot, fo + va - vm)[0]
        return None

    vt = by_name[sys.argv[1]]
    for o in sys.argv[2:]:
        v = read(vt + 16 + int(o, 16))
        t = (base + (v & 0x3FFFFFFF)) if v and v >> 32 != 0xffffff80 else v
        print(f"{o:>6}  {syms.get(t, hex(t) if t else None)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
