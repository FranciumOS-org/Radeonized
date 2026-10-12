#!/usr/bin/env python3
"""Name the code and data addresses in a boot's RBGPULog: every pointer that
falls inside an AMD kext the plugin saw load ("HWLibs loaded at A (size S)",
"X6000 loaded at ...", "HWSvc loaded at ...") is mapped back to the extracted
kext in build/re/ and printed as symbol+offset, with its file address.

    python3 tools/re/ptrsym.py logs/<dir>/peer.plist

The kexts are loaded contiguously from __TEXT, so file address =
__TEXT.vmaddr + (pointer - load address). Pointers outside every loaded kext
(heap, stack, MMIO) are skipped.
"""
import os
import plistlib
import re
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from machoread import Macho  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
KEXTS = {
    "HWLibs": "AMDRadeonX6800HWLibs",
    "X6000": "AMDRadeonX6000",
    "HWSvc": "AMDRadeonX6000HWServices",
}
LOADED = re.compile(r"\b(HWLibs|X6000|HWSvc) loaded at (0x[0-9a-fA-F]+) \(size (0x[0-9a-fA-F]+)\)")
POINTER = re.compile(r"0x[0-9a-fA-F]{12,16}\b")


KC = "/System/Library/KernelCollections/SystemKernelExtensions.kc"


def extracted(kext):
    """build/re/<kext>, extracted again if `make clean` removed it."""
    path = os.path.join(ROOT, "build", "re", kext)
    if not os.path.exists(path):
        os.makedirs(os.path.dirname(path), exist_ok=True)
        subprocess.run([sys.executable, os.path.join(ROOT, "tools", "re", "kc_extract.py"),
                        KC, "com.apple.kext." + kext, path],
                       check=True, stdout=subprocess.DEVNULL)
    return path


def log_lines(path):
    try:
        tree = plistlib.load(open(path, "rb"))
    except (OSError, plistlib.InvalidFileException):
        return []  # no peer this boot (-rbgpuoff, or the plugin did not run)
    node = tree[0] if isinstance(tree, list) and tree else tree
    return [str(x) for x in (node.get("RBGPULog") or [])]


def main():
    lines = log_lines(sys.argv[1])
    ranges = []
    for line in lines:
        m = LOADED.search(line)
        if m:
            macho = Macho(extracted(KEXTS[m.group(1)]))
            text = next(s for s in macho.segs if s[0] == "__TEXT")
            ranges.append((int(m.group(2), 16), int(m.group(3), 16), KEXTS[m.group(1)], macho, text[1]))
    if not ranges:
        print("  (no 'loaded at' lines in the log)")
        return 0
    seen = {load for load, *_ in ranges}
    named = 0
    for line in lines:
        for tok in POINTER.findall(line):
            p = int(tok, 16)
            if p in seen:
                continue
            for load, size, kext, macho, base in ranges:
                if load <= p < load + size:
                    seen.add(p)
                    named += 1
                    va = base + (p - load)
                    n = macho.name_at(va)
                    name = f"{n[0]}+{n[1]:#x}" if n and n[1] else (n[0] if n else "?")
                    print(f"  {tok} = {kext} {va:#x} {name}")
    if not named:
        print("  (no logged pointer falls inside a loaded AMD kext)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
