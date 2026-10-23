#!/bin/sh
# dscdis.sh <va> <len>  -- disassemble cache-resident code at its real address.
# dsc.py's `macho` wrapper exists because nothing here reads raw binary:
# llvm-objdump has no -b binary, there is no llvm-mc and no capstone, and
# otool cannot open a cache-resident path (2ps).
set -e
C=/System/Cryptexes/OS/System/Library/dyld/dyld_shared_cache_x86_64h
D=$(dirname "$0")/dsc.py
T=$(mktemp -t dscdis).o
trap 'rm -f "$T"' EXIT
/usr/bin/python3 "$D" macho "$C" "$1" "$2" > "$T"
/usr/bin/objdump -d --no-show-raw-insn --print-imm-hex "$T" | sed -n '8,100000p'
