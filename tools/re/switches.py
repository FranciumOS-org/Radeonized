#!/usr/bin/env python3
"""Decode the switch statements of functions in an extracted kext: for each
`cmpl $N, %reg; ja default; ... leaq T(%rip), %r; movslq (%r,%idx,4)` jump
table, map every case value to the code it runs, named by the first call or
tail jump there (or "inline" when the case does its own work), and which of
the in/out pointer arguments (%rdx / %rcx at entry) the case tests.

    python3 tools/re/switches.py build/re/AMDRadeonX6800HWLibs build/re/hwlibs.dis _smu_11_0_7_dpm_control [...]

The .dis file is `objdump -d --no-show-raw-insn` output of the same binary.
"""
import re
import struct
import sys
import os

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from machoread import Macho  # noqa: E402

INS = re.compile(r'^\s*([0-9a-f]+):\s+(.*)$')
FUNC = re.compile(r'^[0-9a-f]+ <(_[^>]+)>:$')


def load_dis(path):
    funcs, order, cur = {}, [], None
    for line in open(path):
        line = line.rstrip('\n')
        m = FUNC.match(line)
        if m:
            cur = m.group(1)
            funcs[cur] = []
            order.append(cur)
            continue
        m = INS.match(line)
        if m and cur:
            funcs[cur].append((int(m.group(1), 16), m.group(2).strip()))
    return funcs


def index_by_addr(funcs):
    at = {}
    for name, ins in funcs.items():
        for i, (a, t) in enumerate(ins):
            at[a] = (name, i)
    return at


def case_summary(funcs, at, addr, limit=14):
    """What the code at addr does: the first named call/jump, and the pointer
    args it checks."""
    if addr not in at:
        return "?", ""
    name, i = at[addr]
    ins = funcs[name]
    uses = set()
    for a, t in ins[i:i + limit]:
        if re.search(r'test[ql]\s+%rdx, %rdx', t):
            uses.add('in')
        if re.search(r'test[ql]\s+%rcx, %rcx', t):
            uses.add('out')
        m = re.search(r'(?:callq|jmp|jmpq)\s+0x[0-9a-f]+ <(_[A-Za-z0-9_]+)>', t)
        if m and not m.group(1).startswith(name):
            return m.group(1), ','.join(sorted(uses))
        if t.startswith('retq'):
            return "inline", ','.join(sorted(uses))
    return "inline", ','.join(sorted(uses))


def decode(m, funcs, at, fname):
    ins = funcs.get(fname)
    if ins is None:
        return [f"{fname}: not found"]
    out = []
    bound = None
    for k, (a, t) in enumerate(ins):
        mb = re.match(r'cmp[lq]\s+\$0x([0-9a-f]+), %[er]', t)
        if mb:
            bound = int(mb.group(1), 16)
        mt = re.match(r'leaq\s+-?0x[0-9a-f]+\(%rip\), %r\w+\s+## 0x([0-9a-f]+)', t)
        if mt and k + 1 < len(ins) and 'movslq' in ins[k + 1][1] and bound is not None:
            table = int(mt.group(1), 16)
            out.append(f"{fname}: cases 0..{bound:#x}, table {table:#x}")
            for c in range(bound + 1):
                rel = struct.unpack_from('<i', m.data, m.off(table + 4 * c))[0]
                tgt = (table + rel) & 0xFFFFFFFFFFFFFFFF
                what, uses = case_summary(funcs, at, tgt)
                out.append(f"  {c:#04x} -> {tgt:#x} {what}" + (f" [{uses}]" if uses else ""))
            bound = None
    if not out:
        out.append(f"{fname}: no jump table")
    return out


def main():
    m = Macho(sys.argv[1])
    funcs = load_dis(sys.argv[2])
    at = index_by_addr(funcs)
    for f in sys.argv[3:]:
        print('\n'.join(decode(m, funcs, at, f)))
    return 0


if __name__ == '__main__':
    sys.exit(main())
