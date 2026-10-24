#!/usr/bin/env python3
"""Every store whose displacement lands in a given byte range of a struct.

2px searched only for `movl %reg, 0x5c(%reg)` and found ten unrelated sites.
That form is not the only way to write a 4-byte field: a 64-bit store to
+0x58 covers 0x5c too, and an SSE store to +0x50 covers 0x50..0x5f.  This
scans for all of them, with the width, so the caller can see which bytes
each one actually touches.
"""
import struct, sys
sys.path.insert(0, "tools/re")
from dsc import Cache

# opcode -> (mnemonic, default operand width in bytes, extra immediate bytes)
STORES = {
    0x88: ("mov m8,r8",  1, 0),
    0x89: ("mov m,r",    4, 0),   # 8 with REX.W, 2 with 0x66
    0xc6: ("mov m8,imm", 1, 1),
    0xc7: ("mov m,imm",  4, 4),   # imm is still 4 bytes with REX.W
    # 2q3: read-modify-write forms. A BIT FLAG is never set with a `mov`, so a
    # scanner that knows only the mov family cannot find the writer of a flags
    # word - which is exactly what `isAcceleratorBacked` (bit 22 of +0x64) is.
    0x80: ("grp1 m8,imm8", 1, 1),  # or/and/xor m8, imm8
    0x81: ("grp1 m,imm32", 4, 4),  # or/and/xor m, imm32
    0x83: ("grp1 m,imm8",  4, 1),  # or/and/xor m, sign-extended imm8
}
# DELIBERATELY ABSENT: the single-byte reg-to-mem forms (0x01 add, 0x09 or,
# 0x21 and, 0x31 xor, 0xff inc/dec). They are real instructions, but as bare
# byte values they match constantly inside __cstring and the GCC exception
# tables, and adding them took one scan from a handful of hits to 909, nearly
# all of them data. If you need them, restrict the scan to the code range -
# CoreDisplay's __text ends around 0x7ff804141000, before the stubs and
# strings - rather than passing the whole __TEXT segment.
# 0f ba /5 = bts, /6 = btr, /7 = btc - the other way a single bit is written.
TWOBYTE = {0xba: ("bt-group m,imm8", 4, 1), 0xab: ("bts m,r", 4, 0),
           0xb3: ("btr m,r", 4, 0), 0xbb: ("btc m,r", 4, 0)}
SSE = {  # (prefix, 0f, op) -> (mnemonic, width)
    (0x00, 0x11): ("movups m,x", 16),
    (0x66, 0x11): ("movupd m,x", 16),
    (0x00, 0x29): ("movaps m,x", 16),
    (0x66, 0x29): ("movapd m,x", 16),
    (0x66, 0xd6): ("movq m,x",    8),
    (0xf3, 0x7f): ("movdqu m,x", 16),
    (0x66, 0x7f): ("movdqa m,x", 16),
    (0x00, 0x7e): ("movntq m,x",  8),
}
REGS = ["rax","rcx","rdx","rbx","rsp","rbp","rsi","rdi",
        "r8","r9","r10","r11","r12","r13","r14","r15"]

def scan(blob, va, lo, hi):
    out, n = [], len(blob)
    for i in range(n - 12):
        j, rex, p66, pf3 = i, 0, False, False
        while j < i + 4:
            b = blob[j]
            if b == 0x66: p66 = True; j += 1
            elif b == 0xf3: pf3 = True; j += 1
            elif 0x40 <= b <= 0x4f: rex = b; j += 1
            else: break
        op = blob[j]
        mn = width = imm = None
        if op == 0x0f:
            key = (0x66 if p66 else 0xf3 if pf3 else 0x00, blob[j+1])
            if key in SSE:
                mn, width = SSE[key]; imm = 0; j += 1
            elif blob[j+1] in TWOBYTE:
                mn, width, imm = TWOBYTE[blob[j+1]]
                if rex & 0x08: width = 8
                j += 1
        elif op in STORES:
            mn, width, imm = STORES[op]
            if rex & 0x08: width = 8 if width == 4 else width
            elif p66 and width == 4: width = 2
        if mn is None: continue
        modrm = blob[j+1]
        mod, rm = modrm >> 6, modrm & 7
        if mod == 0 or rm == 4 or rm == 5: continue   # no disp / SIB / rip-rel
        if mod == 1: disp, dl = struct.unpack_from("<b", blob, j+2)[0], 1
        elif mod == 2: disp, dl = struct.unpack_from("<i", blob, j+2)[0], 4
        else: continue
        if not (disp + width > lo and disp < hi): continue
        ilen = (j + 2 + dl + imm) - i
        base = rm | ((rex & 1) << 3)
        out.append((va + i, mn, disp, width, REGS[base], blob[i:i+ilen].hex()))
    return out

if __name__ == "__main__":
    C = "/System/Cryptexes/OS/System/Library/dyld/dyld_shared_cache_x86_64h"
    va, ln, lo, hi = (int(x, 0) for x in sys.argv[1:5])
    c = Cache(C)
    for s, mn, d, w, reg, hx in scan(c.read(va, ln), va, lo, hi):
        print("0x%x  %-12s 0x%02x(%%%s) w=%-2d  %s" % (s, mn, d, reg, w, hx))
