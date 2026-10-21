#!/bin/sh
# disassemble a symbol of build/re/AMDRadeonX6000MTLDriver by name (regex)
BIN=build/re/AMDRadeonX6000MTLDriver
SYMS=build/re/mtl-syms.txt
n=${2:-0x200}
a=$(grep -E "$1" "$SYMS" | head -1 | awk '{print $1}')
[ -z "$a" ] && { echo "no symbol matching $1"; exit 1; }
echo "=== $(grep -E "$1" "$SYMS" | head -1 | cut -d' ' -f3-)  @ $a"
objdump -d --start-address=$a --stop-address=$(python3 -c "print(hex($a+$n))") \
        --no-show-raw-insn "$BIN" 2>/dev/null | tail -n +5
