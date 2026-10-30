#!/usr/bin/env python3
"""Derive and verify the "XHCI interrupter 0" patch sites in AppleUSBXHCI.

    python3 tools/re/xhci_intr0_derive.py [kext-or-collection]   # default: this
                                                                 # Mac's boot KC

Why these patches exist (measured 2026-10-08, re-derived 2026-10-30): the five
xHCI controllers on this machine expose ONE MSI vector, so only interrupter 0
can raise an interrupt. AppleUSBXHCI targets interrupters 1 (bulk/control) and
2 (isochronous) for transfer events, and those events are then only noticed by
a ~2 s watchdog. Forcing every Transfer TRB's Interrupter Target field to 0
puts the completions back on the one vector that works.

A Transfer TRB is 16 bytes; dword 2 (offset +0x8) holds
TRB-Transfer-Length[16:0] | TD-Size[21:17] | Interrupter-Target[31:22]. So an
interrupter of 1 appears as the immediate 0x00400000 and 2 as 0x00800000 in
whatever feeds that store, and an index computed at run time appears as a
shift left by 22 (0x16). This script finds every such site inside the request
preparation functions, prints a patch table, and then checks it:

  * every interrupter immediate in the kext is either patched or explained
  * each Find is unique in the kext's __text (OpenCore applies Count 1)
  * each Find really sits at the derived address
  * Find and Replace are the same length, and Replace zeroes only the
    interrupter field

Read-only. Nothing here writes to the EFI or to the running kernel.
"""
import re
import subprocess
import sys
import os

HERE = os.path.dirname(os.path.abspath(__file__))
KC = "/System/Library/KernelCollections/BootKernelExtensions.kc"
BUNDLE = "com.apple.driver.usb.AppleUSBXHCI"

# The functions that build Transfer TRBs. Anything outside these is not a
# transfer ring and must not be patched (AppleUSBXHCIPort::interruptOccurred
# ORs 0x400000/0x800000 into a port state word - a different field entirely).
FUNCS = (
    "__ZN19AppleUSBXHCIRequest7prepareEP19AppleUSBHostRequest",
    "__ZN19AppleUSBXHCIRequest5addTDEP14AppleUSBXHCITDjjj",
    "__ZN30AppleUSBXHCIIsochronousRequest7prepareEP19AppleUSBHostRequest",
    "__ZN25AppleUSBXHCISparseRequest7prepareEP19AppleUSBHostRequest",
)

# Instruction forms that put a non-zero interrupter into the status dword.
# Each gives the byte offset of the 4-byte immediate inside the instruction,
# or None for the shift form, which is replaced wholesale.
IMM = re.compile(r"\$0x(400000|800000),")
SHL = re.compile(r"shll?\s+\$0x16,")


def run(*argv):
    r = subprocess.run(argv, capture_output=True, text=True)
    if r.returncode:
        sys.exit(f"{argv[0]} failed: {r.stderr.strip()[:400]}")
    return r.stdout


def extract(path):
    """Return a standalone Mach-O for AppleUSBXHCI, extracting if needed."""
    if b"\xcf\xfa\xed\xfe" != open(path, "rb").read(4):
        sys.exit(f"{path}: not a Mach-O")
    head = open(path, "rb").read(0x20)
    if head[12:16] == b"\x0c\x00\x00\x00":          # MH_FILESET
        out = os.path.join(os.path.dirname(HERE), "..", "build", "re", "AppleUSBXHCI")
        out = os.path.normpath(out)
        run(sys.executable, os.path.join(HERE, "kc_extract.py"), path, BUNDLE, out)
        return out
    return path


def main(argv):
    path = extract(argv[1] if len(argv) > 1 else KC)
    data = open(path, "rb").read()
    dis = run("objdump", "-d", path).splitlines()

    # address -> (bytes, text) and the enclosing symbol of every instruction
    insns, sym, order = {}, {}, []
    cur = "?"
    for line in dis:
        m = re.match(r"^([0-9a-f]{8,16}) <(.+)>:$", line)
        if m:
            cur = m.group(2)
            continue
        m = re.match(r"^([0-9a-f]{8,16}): ((?:[0-9a-f]{2} )+)\s*(.*)$", line)
        if not m:
            continue
        a = int(m.group(1), 16)
        insns[a] = (bytes.fromhex(m.group(2).replace(" ", "")), m.group(3).strip())
        sym[a] = cur
        order.append(a)

    hits, foreign = [], []
    for a in order:
        raw, text = insns[a]
        if not (IMM.search(text) or SHL.search(text)):
            continue
        (hits if sym[a] in FUNCS else foreign).append((a, sym[a], raw, text))

    print(f"{path}  ({len(insns)} instructions)")
    print(f"\n{len(hits)} interrupter site(s) inside the transfer-ring builders:")
    bad = 0
    table = []
    for a, s, raw, text in hits:
        nxt = insns[min((x for x in insns if x > a), default=a)]
        find = raw + nxt[0]
        if SHL.search(text):
            # shll $0x16,%r12d  ->  xorl %r12d,%r12d ; nop   (same length)
            assert raw == b"\x41\xc1\xe4\x16", raw.hex()
            repl = b"\x45\x31\xe4\x90" + nxt[0]
        else:
            k = raw.rfind(b"\x00\x00\x40\x00")
            if k < 0:
                k = raw.rfind(b"\x00\x00\x80\x00")
            assert k >= 0, raw.hex()
            repl = raw[:k] + b"\x00\x00\x00\x00" + raw[k + 4:] + nxt[0]
        n = data.count(find)
        off = a - min(order)
        ok = n == 1 and len(find) == len(repl)
        bad += not ok
        print(f"  {'ok  ' if ok else 'FAIL'} {hex(a)}  {s.split('E')[0][:34]:34s} "
              f"count={n}  {text}")
        print(f"         find {find.hex()}\n         repl {repl.hex()}")
        table.append((hex(a), s, text, find.hex(), repl.hex()))

    print(f"\n{len(foreign)} site(s) OUTSIDE those functions - not patched, "
          f"and each must be a different field:")
    for a, s, raw, text in foreign:
        print(f"  {hex(a)}  {s[:60]:60s} {text}")

    print("\nchecks")
    print(f"  {'ok  ' if not bad else 'FAIL'} every Find unique and same length as Replace")
    missing = [f for f in FUNCS if f not in set(s for _, s, _, _ in hits)]
    print(f"  {'ok  ' if not missing else 'WARN'} all four builders contributed a site"
          + (f" (missing: {missing})" if missing else ""))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
