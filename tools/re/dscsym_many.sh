#!/bin/sh
# dscsym_many.sh <file-of-unslid-hex-addrs>  -- symbolize them all.
# Slide measured fresh every run (2ps). Addresses are fed to atos in chunks
# because a whole-image grid (78 720 addresses for SkyLight) overflows argv.
set -e
SL_UNSLID=$((0x7FF807B42000))
/usr/bin/python3 -c '
import ctypes, time
for p in ("/System/Library/PrivateFrameworks/SkyLight.framework/SkyLight",
          "/System/Library/Frameworks/CoreDisplay.framework/CoreDisplay",
          "/System/Library/Frameworks/CoreGraphics.framework/CoreGraphics",
          # 2r5: IOPresentment and IOKit, so the same grid names the frameworks
          # the display chain actually ends in. dlopen failures are printed and
          # skipped rather than fatal - a missing image must not silently
          # rename every address (rule 26 on the symbolizer itself).
          "/System/Library/PrivateFrameworks/IOPresentment.framework/IOPresentment",
          "/System/Library/Frameworks/IOKit.framework/IOKit"):
    try: ctypes.CDLL(p)
    except Exception as e: print("dlopen FAILED", p, e)
time.sleep(1800)' &
PID=$!
trap 'kill $PID 2>/dev/null' EXIT
sleep 2
BASE=$(vmmap "$PID" 2>/dev/null | awk '/__TEXT/ && /SkyLight/ {split($2,a,"-"); print a[1]; exit}')
[ -n "$BASE" ] || { echo "no SkyLight __TEXT in vmmap" >&2; exit 1; }
SLIDE=$(( $((0x$BASE)) - SL_UNSLID ))
printf 'slide 0x%x\n' "$SLIDE" >&2
/usr/bin/python3 -c "
import sys
s=int(sys.argv[1])
print('\n'.join(hex(int(l,16)+s) for l in open(sys.argv[2])))" "$SLIDE" "$1" \
  | xargs -n 2000 atos -p "$PID"
