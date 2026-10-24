#!/usr/bin/env python3
"""rbpslot.py <va> <len> <disp>  -- every access to one rbp-relative stack slot.

fldstore.py CANNOT find these: rbp-relative addressing is modrm rm=101, and
that scanner skips rm==5 along with the SIB and rip-relative forms.  That gap
is why 2q2 reported "nothing in UpdateDisplays writes the Compositor* slot" -
a sound negative about a slot that was not the one the call site reads.

Separates loads from stores, which is the whole point: a slot with loads and
no stores is read from somewhere this scan does not cover (an argument, a
memcpy, an SSE spill), not a slot nothing writes.
"""
import struct, sys
sys.path.insert(0, "tools/re")
from dsc import Cache

REGS = ["rax","rcx","rdx","rbx","rsp","rbp","rsi","rdi",
        "r8","r9","r10","r11","r12","r13","r14","r15"]
# opcode -> (text, is_store, extra immediate bytes)
OPS = {0x89: ("mov  SLOT <- %s", True,  0),
       0x8b: ("mov  %s <- SLOT", False, 0),
       0xc7: ("mov  SLOT <- imm32", True, 4),
       0x8d: ("lea  %s = &SLOT", False, 0)}

def scan(blob, va, disp_want):
    out = []
    for i in range(len(blob) - 10):
        j, rex = i, 0
        if blob[j] in (0x48, 0x49, 0x4c, 0x4d):
            rex = blob[j]; j += 1
        op = blob[j]
        if op not in OPS:
            continue
        modrm = blob[j+1]
        if (modrm >> 6) != 2 or (modrm & 7) != 5:   # mod=10, rm=101 -> disp32(%rbp)
            continue
        disp = struct.unpack_from("<i", blob, j+2)[0]
        if disp != disp_want:
            continue
        text, store, imm = OPS[op]
        reg = ((modrm >> 3) & 7) | (0x08 if rex & 0x04 else 0)
        ilen = (j + 6 + imm) - i
        out.append((va + i, text % REGS[reg] if "%s" in text else text,
                    store, blob[i:i+ilen].hex()))
    return out

if __name__ == "__main__":
    C = "/System/Cryptexes/OS/System/Library/dyld/dyld_shared_cache_x86_64h"
    va, ln, disp = (int(x, 0) for x in sys.argv[1:4])
    c = Cache(C)
    hits = scan(c.read(va, ln), va, disp)
    st = sum(1 for h in hits if h[2])
    print("%d access(es) to %+d(%%rbp): %d store(s), %d load(s)"
          % (len(hits), disp, st, len(hits) - st))
    for a, t, store, hx in hits:
        print("  0x%x  %-22s %-6s %s" % (a, t, "STORE" if store else "load", hx))
