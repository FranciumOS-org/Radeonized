#!/usr/bin/env python3
"""Find the functions that reference a C string (RIP-relative lea) in an
extracted kext, using its llvm-objdump output.

    python3 tools/re/strxref.py build/re/<kext> <disassembly.dis> "<string>" [...]
"""
import re
import sys
import os

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from machoread import Macho  # noqa: E402


def main():
    m = Macho(sys.argv[1])
    dis = open(sys.argv[2]).read().splitlines()
    for s in sys.argv[3:]:
        needle = s.encode() + b"\0"
        addrs = []
        start = 0
        while True:
            i = m.data.find(needle, start)
            if i < 0:
                break
            if i == 0 or m.data[i - 1] == 0:
                for name, vm, vs, fo, fs in m.segs:
                    if fo <= i < fo + fs and name != "__LINKEDIT":
                        addrs.append(vm + i - fo)
            start = i + 1
        print(f'"{s}" at {[hex(a) for a in addrs]}')
        func = None
        for line in dis:
            mm = re.match(r"^[0-9a-f]+ <(.*)>:$", line)
            if mm:
                func = mm.group(1)
                continue
            for a in addrs:
                if f"## {a:#x}" in line:
                    print(f"   {line.split(':')[0].strip()}  in {func}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
