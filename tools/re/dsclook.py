#!/usr/bin/env python3
"""dsclook.py <image> <va>...  -- which function each unslid address is in,
against a cached dscinv.sh inventory (no atos round trip)."""
import bisect, sys
img = sys.argv[1]
rows = []
for ln in open("build/re/%s-funcs.txt" % img):
    a, n = ln.split(None, 1)
    rows.append((int(a, 16), n.rstrip()))
keys = [r[0] for r in rows]
for s in sys.argv[2:]:
    a = int(s, 0)
    i = bisect.bisect_right(keys, a) - 1
    print("0x%x -> %s + %d" % (a, rows[i][1], a - rows[i][0]) if i >= 0
          else "0x%x -> ?" % a)
