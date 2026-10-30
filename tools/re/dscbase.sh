#!/bin/sh
# dscbase.sh <LibName>  --  "<unslid-__TEXT-hex> <segsize-hex> <path>" for a
# library in THIS machine's dyld shared cache.
#
# 2xz: WHY THIS EXISTS. dscsym.sh and dscinv.sh each carried a table of unslid
# __TEXT bases written down on macOS 15. On macOS 26 SkyLight's base is
# 0x7FF8091F1000, not the 0x7FF807B42000 in that table - so dscsym.sh was
# computing its slide against the wrong library base and `atos` was answering
# for whatever happened to live at base + wrong_slide. That is rule 26's exact
# trap, and rule 17's: a written-down address used on a machine it was not
# measured on is a substituted value, and the tool reports confidently either
# way. CLAUDE.md has said "every Apple address in this repo is a macOS 15
# address; re-derive before any static read" for several milestones, and these
# two tools were the ones that could not be re-derived by reading - they had
# to be edited.
#
# dyld_info reads the cache's own load commands, so the answer is this
# machine's by construction and no table can go stale. The seg-size comes back
# in KB or MB from dyld_info, which is why it is re-derived in bytes from the
# section list rather than parsed out of that column.
set -e

case "$1" in
  SkyLight)      P=/System/Library/PrivateFrameworks/SkyLight.framework/SkyLight ;;
  CoreDisplay)   P=/System/Library/Frameworks/CoreDisplay.framework/CoreDisplay ;;
  CoreGraphics)  P=/System/Library/Frameworks/CoreGraphics.framework/CoreGraphics ;;
  IOPresentment) P=/System/Library/PrivateFrameworks/IOPresentment.framework/IOPresentment ;;
  IOKit)         P=/System/Library/Frameworks/IOKit.framework/IOKit ;;
  /*)            P="$1" ;;
  *) echo "usage: $0 <SkyLight|CoreDisplay|CoreGraphics|IOPresentment|IOKit|/abs/path>" >&2
     exit 2 ;;
esac

# Two numbers out of one dyld_info run: the __TEXT line's unslid address, and
# the segment's size in bytes. dyld_info prints the size as "5491KB", so the
# bytes come from the "seg-size" column parsed with its unit rather than
# assumed - a KB read as bytes would make the inventory grid 1024x too small
# and the absence of every name would look like an absent library.
env -u DYLD_INSERT_LIBRARIES dyld_info -segments "$P" 2>/dev/null |
/usr/bin/python3 -c '
import re, sys

base = size = None
for line in sys.stdin:
    f = line.split()
    # "0x7FF8091F1000    __TEXT                            5491KB     r.x/r.x"
    if len(f) >= 3 and f[0].startswith("0x") and f[1] == "__TEXT":
        base = int(f[0], 16)
        for tok in f[2:]:
            m = re.fullmatch(r"(\d+)(KB|MB|B)", tok)
            if m:
                n = int(m.group(1))
                size = n * {"B": 1, "KB": 1024, "MB": 1024 * 1024}[m.group(2)]
                break
        break
if base is None:
    sys.exit("no __TEXT segment line in dyld_info output - wrong path, or "
             "the library is not in this cache")
if size is None:
    sys.exit("found __TEXT at %#x but could not read its size; dyld_info "
             "changed its output format" % base)
print("%x %x" % (base, size))
' | { read -r B S; printf '%s %s %s\n' "$B" "$S" "$P"; }
