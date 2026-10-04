#!/bin/sh
# Stage the built probe kext as root:wheel, load it, and print its report.
#
#   sudo tools/load.sh
#
# Only a person runs this (see CLAUDE.md). Nothing is installed: the staged
# copy lives under /private/var/tmp and the kext is gone after a reboot.
set -eu

[ "$(id -u)" = 0 ] || { echo "run with sudo: sudo tools/load.sh" >&2; exit 1; }

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
