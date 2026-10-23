#!/usr/bin/env python3
"""Generic RIP-relative xref: every instruction in [va,va+len) with a
mod=00/rm=101 modrm whose disp32 target is `target`.  Handles REX and
no-REX forms of the common opcodes; prints the instruction bytes so the
caller can tell a read from a write."""
import struct, sys
sys.path.insert(0, "tools/re")
from dsc import Cache

OPS = {0x8b:"mov r<-m", 0x89:"mov m<-r", 0x8d:"lea", 0x63:"movslq",
       0x03:"add", 0x2b:"sub", 0x39:"cmp m,r", 0x3b:"cmp r,m",
       0x85:"test", 0x01:"add m,r", 0x29:"sub m,r", 0x31:"xor m,r",
       0xc7:"mov m,imm", 0x83:"grp1 m,imm8", 0x81:"grp1 m,imm32",
       0xff:"grp5 m", 0x8a:"mov r8<-m", 0x88:"mov m<-r8", 0xc6:"mov m8,imm",
       0x0f:"0f-two-byte"}

def scan(blob, va, target):
    hits = []
    n = len(blob)
    for i in range(n - 8):
        b0 = blob[i]
        j = i
        if 0x40 <= b0 <= 0x4f:
            j = i + 1
            if j + 6 >= n: continue
        op = blob[j]
        if op not in OPS: continue
        if op == 0x0f:
            j += 1
            if j + 6 >= n: continue
        modrm = blob[j+1]
        if (modrm & 0xc7) != 0x05: continue
        disp = struct.unpack_from("<i", blob, j+2)[0]
        ilen = (j + 6) - i
        # immediates extend the instruction, which shifts the RIP base
        if op == 0xc7: ilen += 4
        elif op == 0x81: ilen += 4
        elif op in (0x83, 0xc6): ilen += 1
        tgt = va + i + ilen + disp
        if tgt == target:
            hits.append((va + i, blob[i:i+ilen].hex(), OPS[op]))
    return hits

if __name__ == "__main__":
    C = "/System/Cryptexes/OS/System/Library/dyld/dyld_shared_cache_x86_64h"
    va, ln, tgt = (int(x,0) for x in sys.argv[1:4])
    c = Cache(C)
    blob = c.read(va, ln)
    hits = scan(blob, va, tgt)
    print("%d rip-relative site(s) targeting 0x%x" % (len(hits), tgt))
    for s, hx, kind in hits:
        print("  0x%x  %-22s %s" % (s, hx, kind))
