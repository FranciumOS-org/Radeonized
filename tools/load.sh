#!/bin/sh
# Stage the built probe kext as root:wheel, load it, and print its report.
#
#   sudo tools/load.sh          probe level from Info.plist (1: safe blocks)
#   sudo tools/load.sh 2        + the display engine (OTG/HUBP/DMCUB)
#   sudo tools/load.sh 3        + the graphics core
#
# 2oq: the level is an argument rather than a new default, so the committed
# default stays 1 and a level is a thing someone chose at load time. It is
# applied to the STAGED copy only; src/kext/Info.plist is never touched.
# Level 2 is what reads OTG0's timing and measures the refresh rate.
#
# Only a person runs this (see CLAUDE.md). Nothing is installed: the staged
# copy lives under /private/var/tmp and the kext is gone after a reboot.
set -eu

# Validate the level BEFORE the root check, so a typo does not cost a
# password prompt first.
level="${1:-}"
case "$level" in
    "") ;;
    1|2|3) ;;
    *) echo "probe level must be 1, 2 or 3 (got '$level')" >&2; exit 2 ;;
esac

[ "$(id -u)" = 0 ] || { echo "run with sudo: sudo tools/load.sh [1|2|3]" >&2; exit 1; }

root="$(cd "$(dirname "$0")/.." && pwd)"
src="$root/build/out/RembrandtProbe.kext"
stage=/private/var/tmp/RembrandtProbe.stage
[ -d "$src" ] || { echo "no kext at $src; run: make kext" >&2; exit 1; }

if kmutil showloaded 2>/dev/null | grep -q com.rembrandtgpu.probe; then
    echo "com.rembrandtgpu.probe is already loaded; run sudo tools/unload.sh first" >&2
    exit 1
fi

rm -rf "$stage"
mkdir -p "$stage"
cp -R "$src" "$stage/"
if [ -n "$level" ]; then
    python3 - "$stage/RembrandtProbe.kext/Contents/Info.plist" "$level" <<'EOP'
import plistlib, sys
path, level = sys.argv[1], int(sys.argv[2])
with open(path, 'rb') as f:
    d = plistlib.load(f)
pers = d.get('IOKitPersonalities') or {}
if not pers:
    sys.exit("no IOKitPersonalities in the staged Info.plist")
for name, p in pers.items():
    was = p.get('RBProbeLevel')
    p['RBProbeLevel'] = level
    print(f"staged {name}: RBProbeLevel {was} -> {level}")
with open(path, 'wb') as f:
    plistlib.dump(d, f)
EOP
fi

chown -R root:wheel "$stage"
chmod -R go-w "$stage"

# Get everything on disk first, so a panic costs nothing but the reboot.
sync

echo "loading $stage/RembrandtProbe.kext"
kmutil load -p "$stage/RembrandtProbe.kext"

# start() runs the whole probe synchronously; matching takes a moment.
sleep 3
echo "--- kernel message buffer (RembrandtProbe)"
/sbin/dmesg | grep 'RembrandtProbe' | tail -80

# The report runs as the invoking user so its files are theirs.
if [ -n "${SUDO_USER:-}" ]; then
    sudo -u "$SUDO_USER" python3 "$root/tools/report.py"
else
    python3 "$root/tools/report.py"
fi
