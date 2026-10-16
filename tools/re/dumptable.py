#!/usr/bin/env python3
"""Dump a data symbol of an extracted kext word by word: each 8-byte slot as
hex, decoded as a pointer when it is one (symbol name or C string), and as
version-shaped integers otherwise.

    python3 tools/re/dumptable.py build/re/AMDRadeonX6800HWLibs _vm_ip_version_mapping [max_bytes]
"""
import signal
import struct
import sys
import os

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from machoread import Macho  # noqa: E402


def describe(m, v):
    if not (v >> 32):
        lo, hi = v & 0xFFFFFFFF, v >> 32
        return f"u32 {lo:#x} {hi:#x}" if v else ""
    t = v & 0x3FFFFFFF
    n = m.name_at(t)
    if n and n[1] == 0:
        return n[0]
    s = m.cstr(t, 80)
    if s and s.isprintable() and len(s) >= 3:
        return f'"{s}"'
    return f"-> {t:#x}" + (f" ({n[0]}+{n[1]:#x})" if n else "")


def main():
    m = Macho(sys.argv[1])
    t = m.sym(sys.argv[2])
    if t is None:
        print("no such symbol", file=sys.stderr)
        return 1
    later = [a for a, _, _ in m.syms() if a > t]
    size = (min(later) - t) if later else 512
    if len(sys.argv) > 3:
        size = min(size, int(sys.argv[3], 0))   # 0: accept 0x-prefixed
    o = m.off(t)
    print(f"{sys.argv[2]} at {t:#x}, {size} bytes")
    for i in range(0, size, 8):
        v = struct.unpack_from("<Q", m.data, o + i)[0]
        print(f"  +{i:#05x} {v:#018x}  {describe(m, v)}")
    return 0


if __name__ == "__main__":
    # Piped into head/grep -m: stop quietly when the reader goes away.
    signal.signal(signal.SIGPIPE, signal.SIG_DFL)
    sys.exit(main())
