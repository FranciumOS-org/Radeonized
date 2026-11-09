#!/bin/sh
# dscinv.sh <SkyLight|CoreDisplay|CoreGraphics|IOPresentment|IOKit|MTLDriver|/abs/path>  -- the image's complete
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
# 2xz: the base and the length come from this machine's cache, not from a
# table. The table they came from held macOS 15 addresses - SkyLight
# 0x7FF807B42000 against 0x7FF8091F1000 on macOS 26 - so the grid was laid
# over whatever lives at the old address and every name in the inventory would
# have belonged to another library. One of them was even marked
# "# approximate", which is the honest version of the same problem. dscbase.sh
# reads the load commands, so the answer cannot go stale.
# 311: any image dscbase.sh can place, by name or by absolute path - the list
# of five was why the Metal plugin could not be inventoried. MTLDriver (and
# any path under /System/Library/Extensions) is mapped into a process only
# once a Metal device exists, so the symbolizer is told to create one.
case "$1" in
  SkyLight|CoreDisplay|CoreGraphics|IOPresentment|IOKit) ;;
  MTLDriver) DSC_METAL=1; export DSC_METAL ;;
  /System/Library/Extensions/*) DSC_METAL=1; export DSC_METAL ;;
  /*) ;;
  *) echo "usage: $0 <SkyLight|CoreDisplay|CoreGraphics|IOPresentment|IOKit|MTLDriver|/abs/path>" >&2; exit 2 ;;
esac
TAG=$(basename "$1")
read -r B L _P <<EOF
$("$(dirname "$0")/dscbase.sh" "$1")
EOF
[ -n "$B" ] && [ -n "$L" ] || { echo "dscbase.sh could not place $1 in this cache" >&2; exit 1; }
BASE=0x$B; LEN=0x$L
printf 'inventory of %s: unslid __TEXT %s, %s bytes\n' "$1" "$BASE" "$LEN" >&2
mkdir -p build/re
OUT="build/re/$TAG-funcs.txt"
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
# 311: the 0x40-grid caveat, made loud instead of left in a comment. Grid
# points atos could not name (no " + N" offset) are reported: those, plus any
# function shorter than 0x40 that no point lands in, are ABSENT from the
# inventory - an absent name is not an absent function.
TOT=$(wc -l < "$G" | tr -d ' ')
UNR=$(grep -vc ' + [0-9][0-9]*$' "$N" || true)
printf '%s grid points, %s unresolved; functions under 0x40 bytes may be absent\n' "$TOT" "$UNR" >&2
