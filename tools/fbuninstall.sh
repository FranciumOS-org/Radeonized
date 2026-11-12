#!/bin/sh
# Takes RBAMDFramebuffer.kext out of /Library/Extensions and rebuilds the
# auxiliary kext collection without it. The mirror of tools/fbinstall.sh, with
# the same two traps in mind:
#
#   * `kmutil create` exits 0 when it refuses to write anything, so an empty
#     output file is the real failure (fbinstall.sh's comment, paid for once).
#   * The only check that counts is what is INSIDE the collection the next boot
#     loads - not /Library/Extensions, not the dry run. Here the check is
#     ABSENCE, and absence is exactly the reading a silent failure also
#     produces, so the dry run must be shown to be a real collection (it still
#     carries other kexts) before its missing bundle means anything.
#
# NEVER `kmutil unload` this kext - it panics (2sq). The running instance stays
# loaded until the next boot; this only changes what that boot loads.
#
#   sudo tools/fbuninstall.sh
set -eu

[ "$(id -u)" = 0 ] || { echo "run with sudo: sudo tools/fbuninstall.sh" >&2; exit 1; }

root="$(cd "$(dirname "$0")/.." && pwd)"
boot=/System/Library/KernelCollections/BootKernelExtensions.kc
sys=/System/Library/KernelCollections/SystemKernelExtensions.kc
live=/Library/KernelCollections/AuxiliaryKernelExtensions.kc
scratch=/private/var/tmp/aux-fbuninstall.kc
installed=/Library/Extensions/RBAMDFramebuffer.kext
stamp="$(date +%Y%m%d-%H%M%S)"
keep="$root/logs/fb-backups/$stamp"

[ -f "$boot" ] && [ -f "$sys" ] || { echo "missing $boot or $sys" >&2; exit 1; }

if [ ! -d "$installed" ]; then
    echo "$installed is not there already"
else
    uuid="$(dwarfdump --uuid "$installed/Contents/MacOS/RBAMDFramebuffer" | awk '{print $2}')"
    echo "removing RBAMDFramebuffer ${uuid:-<no uuid>}"
    mkdir -p "$keep"
    cp -R "$installed" "$keep/"
    echo "  kept a copy at $keep/RBAMDFramebuffer.kext"
    rm -rf "$installed"
fi

echo "  dry run -> $scratch"
rm -f "$scratch"
kmutil create -n aux -a x86_64 --allow-missing-kdk \
    -B "$boot" -S "$sys" -A "$scratch" -r /Library/Extensions >/dev/null
[ -s "$scratch" ] || {
    echo "DRY RUN WROTE NOTHING. kmutil create exits 0 when it refuses, so" >&2
    echo "this is the real failure and the live collection is untouched." >&2
    exit 1
}
# Absence only means something if this is a real collection. Show it carries
# other kexts, then show ours is not among them.
others="$(kmutil inspect -B "$scratch" --show-kext-uuids 2>/dev/null | grep -c . || true)"
echo "  dry run carries $others kext(s)"
[ "$others" -gt 0 ] || { echo "the dry run carries NOTHING - refusing to install it" >&2; exit 1; }
kmutil inspect -B "$scratch" --show-kext-uuids 2>/dev/null | grep -i rembrandt && {
    echo "THE DRY RUN STILL CARRIES A REMBRANDT KEXT. Live one untouched." >&2
    exit 1
}
echo "  dry run carries no rembrandt kext"

echo "  backing up $live -> $live.$stamp"
cp "$live" "$live.$stamp"

echo "  writing $live"
kmutil create -n aux -a x86_64 --allow-missing-kdk \
    -B "$boot" -S "$sys" -A "$live" -r /Library/Extensions >/dev/null

if kmutil inspect -B "$live" --show-kext-uuids 2>/dev/null | grep -i rembrandt; then
    echo "THE LIVE COLLECTION STILL CARRIES IT - restoring the backup." >&2
    cp "$live.$stamp" "$live"
    exit 1
fi
if [ ! -s "$live" ]; then
    echo "THE LIVE COLLECTION IS EMPTY - restoring the backup." >&2
    cp "$live.$stamp" "$live"
    exit 1
fi

echo
echo "REMOVED. $live carries $(kmutil inspect -B "$live" --show-kext-uuids 2>/dev/null | grep -c .) kext(s) and no rembrandt one."
echo "The loaded instance stays until you reboot (never kmutil unload it - 2sq)."
echo "Put it back with:  sudo tools/fbinstall.sh   (or let the installer app do it)"
rm -f "$scratch"
