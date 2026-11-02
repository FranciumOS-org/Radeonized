#!/bin/sh
# Install the framebuffer kext into the auxiliary collection. ONE command,
# every check inside it. Only a person runs this (see docs/RULES.md).
#
#   sudo tools/fbinstall.sh
#
# WHY THIS EXISTS. Every install before 2zt was a hand-typed sequence of six
# commands, and the two expensive failure modes of this machine both live in
# the gaps between them:
#
#   * `kmutil create` WITHOUT --allow-missing-kdk prints "Missing Developer
#     Kit", writes NOTHING and exits 0. The exit code cannot be read as the
#     verdict (rule 26); the output file has to be inspected.
#
#   * The UUID that matters is the one INSIDE THE COLLECTION, not the one in
#     /Library/Extensions. 2xx cost nine boots, a WindowServer crash loop and
#     a nine-stage boot-arg bisect because the plugin was reinstalled every
#     time and the framebuffer never was - every "cleared" result in that
#     bisect was taken against a two-milestone-old binary (rule 15).
#
# So this builds to a SCRATCH path first (no sudo needed, costs nothing),
# refuses unless the freshly built UUID is actually in it, backs the live
# collection up, writes it, and then verifies the live collection carries that
# same UUID. Any check failing stops before the next boot's evidence is
# poisoned.
set -eu

[ "$(id -u)" = 0 ] || { echo "run with sudo: sudo tools/fbinstall.sh" >&2; exit 1; }

root="$(cd "$(dirname "$0")/.." && pwd)"
src="$root/build/out/RBAMDFramebuffer.kext"
bin="$src/Contents/MacOS/RBAMDFramebuffer"
boot=/System/Library/KernelCollections/BootKernelExtensions.kc
sys=/System/Library/KernelCollections/SystemKernelExtensions.kc
live=/Library/KernelCollections/AuxiliaryKernelExtensions.kc
scratch=/private/var/tmp/aux-fbinstall.kc

[ -d "$src" ] || { echo "no kext at $src - run: make fbkext" >&2; exit 1; }
[ -f "$boot" ] && [ -f "$sys" ] || { echo "missing $boot or $sys" >&2; exit 1; }

# The UUID of the thing we are about to install, read from the binary itself
# rather than from anything the build printed.
uuid="$(dwarfdump --uuid "$bin" | awk '{print $2}')"
[ -n "$uuid" ] || { echo "could not read a UUID out of $bin" >&2; exit 1; }
echo "installing RBAMDFramebuffer $uuid"

echo "  staging into /Library/Extensions"
rm -rf /Library/Extensions/RBAMDFramebuffer.kext
cp -R "$src" /Library/Extensions/
chown -R root:wheel /Library/Extensions/RBAMDFramebuffer.kext

echo "  dry run -> $scratch"
rm -f "$scratch"
kmutil create -n aux -a x86_64 --allow-missing-kdk \
    -B "$boot" -S "$sys" -A "$scratch" -r /Library/Extensions >/dev/null
[ -s "$scratch" ] || {
    echo "DRY RUN WROTE NOTHING. kmutil create exits 0 when it refuses, so" >&2
    echo "this is the real failure and the live collection is untouched." >&2
    exit 1
}
kmutil inspect -B "$scratch" --show-kext-uuids 2>/dev/null \
    | grep -qi "$uuid" || {
    echo "THE DRY-RUN COLLECTION DOES NOT CARRY $uuid." >&2
    echo "Whatever it does carry is not this build. Live one untouched." >&2
    kmutil inspect -B "$scratch" --show-kext-uuids 2>/dev/null \
        | grep -i rembrandt >&2 || echo "  (no rembrandt kext in it at all)" >&2
    exit 1
}
echo "  dry run carries $uuid"

echo "  backing up $live"
cp "$live" "$live.bak"

echo "  writing $live"
kmutil create -n aux -a x86_64 --allow-missing-kdk \
    -B "$boot" -S "$sys" -A "$live" -r /Library/Extensions >/dev/null

# The only check that counts: the UUID inside the collection the next boot
# loads. Not /Library/Extensions, not the dry run.
if kmutil inspect -B "$live" --show-kext-uuids 2>/dev/null | grep -qi "$uuid"
then
    echo
    echo "INSTALLED. $live carries $uuid."
    echo "Reboot, then read it back with:"
    echo "  kextstat | grep -i rembrandt"
    rm -f "$scratch"
    exit 0
fi

echo "THE LIVE COLLECTION DOES NOT CARRY $uuid - restoring the backup." >&2
cp "$live.bak" "$live"
exit 1
