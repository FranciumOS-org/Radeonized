#!/bin/sh
# dscinv.sh <SkyLight|CoreDisplay|CoreGraphics>  -- the image's complete
# function inventory, "<unslid-hex> <name>" per line, cached in build/re/.
#
# This is the single most useful instrument in the cache work so far (2py):
# `atos` over a 0x40 grid of the whole __TEXT names every function in seconds,
# and a NAME found the accelerator writer after two byte-level scans had
# missed it. Build it FIRST, before any byte scan.
#
# CAVEAT (2pz): the grid can only start a function where a grid point lands
# inside it, so a function shorter than 0x40 bytes that no grid point hits is
# absent - CDDisplay::accelerator() is missing from the SkyLight inventory for
# exactly that reason. An absent name is not an absent function.
set -e
cd "$(dirname "$0")/../.."
case "$1" in
  SkyLight)     BASE=0x7FF807B42000; LEN=0x4CE000 ;;
  CoreDisplay)  BASE=0x7FF80404C000; LEN=0x12A800 ;;
  CoreGraphics) BASE=0x7FF807D00000; LEN=0x400000 ;;   # approximate
  *) echo "usage: $0 <SkyLight|CoreDisplay|CoreGraphics>" >&2; exit 2 ;;
esac
mkdir -p build/re
OUT="build/re/$1-funcs.txt"
G=$(mktemp -t dscinv); N=$(mktemp -t dscinv)
trap 'rm -f "$G" "$N"' EXIT
/usr/bin/python3 -c "
b=$BASE; n=$LEN
print('\n'.join('%x'%(b+i) for i in range(0,n,0x40)))" > "$G"
"$(dirname "$0")/dscsym_many.sh" "$G" > "$N"
/usr/bin/python3 - "$G" "$N" > "$OUT" <<'PY'
import re, sys
addrs = [int(l, 16) for l in open(sys.argv[1])]
names = [l.rstrip("\n") for l in open(sys.argv[2])]
best = {}
for a, n in zip(addrs, names):
    m = re.search(r" \+ (\d+)$", n)
    if not m:
        continue
    b, nm = a - int(m.group(1)), n[:m.start()]
    if nm not in best or b < best[nm]:
        best[nm] = b
for nm, b in sorted(best.items(), key=lambda kv: kv[1]):
    print("%x %s" % (b, nm))
PY
printf '%s: %s functions\n' "$OUT" "$(wc -l < "$OUT" | tr -d ' ')"
