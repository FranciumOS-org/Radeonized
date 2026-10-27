#!/bin/sh
# Stage and load the framebuffer kext (RBAMDFramebuffer), in rising order of
# risk. Only a person runs this (see CLAUDE.md). Nothing is installed: the
# staged copy lives under /private/var/tmp and is gone after a reboot.
#
#   sudo tools/fbload.sh               stage 1: load WITHOUT matching
#   sudo tools/fbload.sh beside        stage 2: attach BESIDE IONDRVFramebuffer
#
# Stage 1 (`--load-style start-only`) sends no personality to the catalog, so
# the class registers and NOTHING matches - the display is untouched. It answers
# one question: does a kext that links IOGraphicsFamily out of
# SystemKernelExtensions.kc actually load on this machine? 2sl measured that the
# symbols RESOLVE; this measures that the load is ACCEPTED, which is a different
# claim (kmutil libraries does not check authorization).
#
# Stage 2 rewrites the staged IOMatchCategory to a private one, so the service
# attaches ALONGSIDE the boot framebuffer instead of displacing it. The desktop
# keeps IONDRVFramebuffer; what this buys is three readings that cannot be had
# any other way: whether IOFramebuffer::start() completes on a non-Apple
# subclass here, whether the service appears in the registry, and - via
# `make useiop` - whether the class name really earns CoreDisplay's AMD vendor
# bit (0x04). Note that the gate will still read SHUT in stage 2 and that is
# EXPECTED, not a failure: `present` is the OR over all framebuffers, so with
# .Display_boot still there the mask is 0x05 and (0x1e & 0x05) != 0x05. Stage 2
# proves the bit, not the verdict.
#
# Neither stage is the real test. Displacing IONDRVFramebuffer needs the kext in
# the auxiliary collection at boot, which is an install and therefore the user's
# own step.
#
# Both stages need the one-shot: the kext's probe() refuses unless
# `rbgpu-fb` is set in NVRAM, and consumes it durably before it touches
# anything. So a boot or load that goes wrong costs exactly one power cycle.
set -eu

mode="${1:-start-only}"
case "$mode" in
    start-only|beside) ;;
    *) echo "usage: sudo tools/fbload.sh [beside]" >&2; exit 2 ;;
esac

[ "$(id -u)" = 0 ] || { echo "run with sudo: sudo tools/fbload.sh [beside]" >&2; exit 1; }

root="$(cd "$(dirname "$0")/.." && pwd)"
src="$root/build/out/RBAMDFramebuffer.kext"
stage=/private/var/tmp/RBAMDFramebuffer.stage
[ -d "$src" ] || { echo "no kext at $src; run: make fbkext" >&2; exit 1; }

if kmutil showloaded 2>/dev/null | grep -q com.rembrandtgpu.fb; then
    echo "com.rembrandtgpu.fb is already loaded; unload it first:" >&2
    echo "  sudo kmutil unload -b com.rembrandtgpu.fb" >&2
    exit 1
fi

# Refuse before the load if the one-shot is absent, so a forgotten `nvram`
# does not read as "the kext is broken". Absence is not a result.
if ! nvram rbgpu-fb >/dev/null 2>&1; then
    echo "rbgpu-fb is not set, so probe() would decline and this load would"
    echo "prove nothing. Set it first:"
    echo "  sudo nvram rbgpu-fb=1"
    exit 1
fi

rm -rf "$stage"
mkdir -p "$stage"
cp -R "$src" "$stage/"

if [ "$mode" = beside ]; then
    python3 - "$stage/RBAMDFramebuffer.kext/Contents/Info.plist" <<'EOP'
import plistlib, sys
path = sys.argv[1]
with open(path, 'rb') as f:
    d = plistlib.load(f)
p = d['IOKitPersonalities']['RBAMDFramebuffer']
was = p['IOMatchCategory']
p['IOMatchCategory'] = 'RBAMDFramebufferBeside'
with open(path, 'wb') as f:
    plistlib.dump(d, f)
print(f"staged IOMatchCategory {was} -> RBAMDFramebufferBeside "
      "(attaches beside the boot framebuffer, does not displace it)")
EOP
fi

chown -R root:wheel "$stage"
chmod -R go-w "$stage"

echo
echo "--- loaded UUID must match build/out ---"
dwarfdump --uuid "$stage/RBAMDFramebuffer.kext/Contents/MacOS/RBAMDFramebuffer"

echo
if [ "$mode" = start-only ]; then
    echo "--- stage 1: loading WITHOUT sending the personality ---"
    kmutil load --load-style start-only -p "$stage/RBAMDFramebuffer.kext"
else
    echo "--- stage 2: loading AND matching (beside the boot framebuffer) ---"
    kmutil load -p "$stage/RBAMDFramebuffer.kext"
fi

echo
echo "--- loaded? ---"
kmutil showloaded --no-kernel-components 2>/dev/null | grep -i 'rembrandtgpu.fb' || \
    echo "com.rembrandtgpu.fb does NOT appear in showloaded"

echo
echo "--- the kext's own log lines (the only account of what probe() decided) ---"
/usr/bin/log show --last 2m --predicate 'eventMessage CONTAINS "RBAMDFramebuffer"' \
    --style compact 2>/dev/null | tail -20 || true

echo
echo "--- IOFramebuffer services now in the registry ---"
ioreg -c IOFramebuffer -k IOClass 2>/dev/null | grep -E '^\+-o' || true

if [ "$mode" = beside ]; then
    echo
    echo "--- CoreDisplay's vendor mask, as the tool reproduces it ---"
    echo "(0x04 beside the 0x01 is the result worth having; the VERDICT staying"
    echo " 0 is expected while .Display_boot is still present)"
    (cd "$root" && make useiop >/dev/null 2>&1 && ./build/out/useiop) || \
        echo "make useiop failed; run it by hand"
fi

echo
echo "To unload: sudo kmutil unload -b com.rembrandtgpu.fb"
