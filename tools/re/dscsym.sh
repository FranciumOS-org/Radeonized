#!/bin/sh
# dscsym.sh <unslid-va> [more...]  -- symbolize dyld-shared-cache addresses.
#
# The cache slide is per-boot and system-wide, so it MUST be measured every
# run (2ps: reusing an old slide makes atos answer `libBLAS` for everything).
# A scratch process dlopens the frameworks, vmmap gives the live __TEXT base
# of a library whose UNSLID base is known, and the difference is the slide.
#
# 2xz: the reference library's UNSLID base is now derived by dscbase.sh from
# this machine's own cache, not written down here. It was written down, as
# SkyLight 0x7FF807B42000 - a macOS 15 address - and on macOS 26 SkyLight is
# at 0x7FF8091F1000. So this tool was subtracting the wrong base and handing
# `atos` an address 0x16AF000 away from the one asked about, confidently. Rule
# 26's trap and rule 17's: the slide was measured every run exactly as the
# comment above says, and the thing it was measured AGAINST was stale.
set -e
SL_UNSLID=$((0x$("$(dirname "$0")/dscbase.sh" SkyLight | cut -d" " -f1)))

/usr/bin/python3 -c '
import ctypes, time
for p in ("/System/Library/PrivateFrameworks/SkyLight.framework/SkyLight",
          "/System/Library/Frameworks/CoreDisplay.framework/CoreDisplay",
          "/System/Library/Frameworks/CoreGraphics.framework/CoreGraphics",
          "/System/Library/PrivateFrameworks/IOPresentment.framework/IOPresentment",
          "/System/Library/Frameworks/IOKit.framework/IOKit"):
    try: ctypes.CDLL(p)
    except Exception as e: print("dlopen FAILED", p, e)
time.sleep(600)' &
PID=$!
trap 'kill $PID 2>/dev/null' EXIT
sleep 2

BASE=$(vmmap "$PID" 2>/dev/null | awk '/__TEXT/ && /SkyLight/ {split($2,a,"-"); print a[1]; exit}')
[ -n "$BASE" ] || { echo "could not find SkyLight __TEXT in vmmap $PID" >&2; exit 1; }
BASE=$((0x$BASE))
SLIDE=$((BASE - SL_UNSLID))
printf 'slide 0x%x (SkyLight __TEXT live 0x%x)\n' "$SLIDE" "$BASE"

for VA in "$@"; do
    SV=$(printf '0x%x' $(( $VA + SLIDE )))
    printf '  %s -> %s\n' "$VA" "$(atos -p "$PID" "$SV" 2>/dev/null)"
done
