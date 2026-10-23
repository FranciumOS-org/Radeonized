#!/usr/bin/env python3
"""Disassemble one symbol of an extracted kext by name substring.

    python3 tools/re/dissym.py build/re/<kext> <name-substring> [lines]

otool is the only working disassembler here (no llvm-objdump, and
`objdump -d` on a kc_extract output prints nothing). Symbol names are taken
from nm rather than hand-written, because hand-written Itanium length
prefixes have been wrong three times in this project (rule 5).
"""
import subprocess, sys

f, name = sys.argv[1], sys.argv[2]
n = int(sys.argv[3]) if len(sys.argv) > 3 else 30
syms = [p[2] for ln in subprocess.run(['nm', f], capture_output=True, text=True)
        .stdout.split('\n') if len(p := ln.split()) == 3 and name in p[2]]
if not syms:
    sys.exit(f"no symbol matching {name!r} in {f}")
out = subprocess.run(['otool', '-tV', '-arch', 'x86_64', f],
                     capture_output=True, text=True).stdout.split('\n')
for sym in syms:
    print(f"--- {sym}")
    for i, ln in enumerate(out):
        if ln.startswith(sym + ':'):
            for l in out[i + 1:i + 1 + n]:
                if l.endswith(':') and not l.startswith('0'):
                    break
                print("   ", l.replace('\t', '  ')[:98])
            break
    else:
        print("    (no text found)")
