#!/usr/bin/env python3
"""vcall.py <va> <len> <slot>  -- every `call *slot(%reg)` in a code range.

A virtual call is invisible to callxref (no direct branch) and to ripxref
(the displacement is off a register, not RIP), which is why 2pz found ZERO
callers of CDDisplay::compositor() and read it as a limit of the instrument.
It is: this is the missing instrument.  Encodings covered:

  ff /2 mod=10        call *disp32(%reg)      reg = rax..rdi (no rsp/rbp SIB)
  41 ff /2 mod=10     call *disp32(%r8..%r15)
  ff /2 mod=01        call *disp8(%reg)       for small slots
  ff /3 mod=10        lcall - excluded, /2 only

A hit is a call through SOME object's vtable at that offset; whether that
object is the class you care about needs the surrounding code.  Report, do
not conclude.
"""
import struct, sys
sys.path.insert(0, "tools/re")
from dsc import Cache

REGS = ["rax","rcx","rdx","rbx","rsp","rbp","rsi","rdi",
        "r8","r9","r10","r11","r12","r13","r14","r15"]

def scan(blob, va, slot):
    out = []
    n = len(blob)
    for i in range(n - 7):
        j = i
        rex = 0
        if blob[j] in (0x41, 0x49, 0x4d):      # REX.B for r8..r15
            rex = blob[j]; j += 1
        if blob[j] != 0xff:
            continue
        modrm = blob[j+1]
        if (modrm >> 3) & 7 != 2:              # /2 = near call
            continue
        mod, rm = modrm >> 6, modrm & 7
        if rm in (4, 5):                       # SIB or rip-relative
            continue
        if mod == 2:
            disp = struct.unpack_from("<i", blob, j+2)[0]; ilen = j + 6 - i
        elif mod == 1:
            disp = struct.unpack_from("<b", blob, j+2)[0]; ilen = j + 3 - i
        else:
            continue
        if disp != slot:
            continue
        base = rm | (1 << 3) if rex else rm
        out.append((va + i, REGS[base], blob[i:i+ilen].hex()))
    return out

if __name__ == "__main__":
    C = "/System/Cryptexes/OS/System/Library/dyld/dyld_shared_cache_x86_64h"
    va, ln, slot = (int(x, 0) for x in sys.argv[1:4])
    c = Cache(C)
    hits = scan(c.read(va, ln), va, slot)
    print("%d call *0x%x(%%reg) site(s)" % (len(hits), slot))
    for a, r, hx in hits:
        print("  0x%x  call *0x%x(%%%s)   %s" % (a, slot, r, hx))
