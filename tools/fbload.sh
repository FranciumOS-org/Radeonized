#!/bin/sh
# Stage and load the framebuffer kext (RBAMDFramebuffer), in rising order of
# risk. Only a person runs this (see docs/RULES.md). Nothing is installed: the
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
# bit (0x04).
#
# The kext knows which configuration it is in - it reads its own
# IOMatchCategory - and in the beside one it reports isConsoleDevice() FALSE and
# getConnectionCount() ZERO. Both answers are the truthful ones there, and both
# matter: claiming the console beside the real framebuffer invites IOFramebuffer
# to hand it the kernel console, and claiming a connection conjures a phantom
# 1920x1080 display for WindowServer. So stage 2 exercises start() without
# offering to drive anything. Reboot to remove it; unloading panics (2sq).
#
# Note that the gate will still read SHUT in stage 2 and that is EXPECTED, not a
# failure: `present` is the OR over all framebuffers, so with .Display_boot
# still there the mask is 0x05 and (0x1e & 0x05) != 0x05. Stage 2 proves the
# bit, not the verdict.
#
# Neither stage is the real test. Displacing IONDRVFramebuffer needs the kext in
# the auxiliary collection at boot, which is an install and therefore the user's
# own step.
#
# Stage 2 and stage 3 need the one-shot: the kext's probe() refuses unless
# -rbgpuownfb AND `rbgpu-fb` are present. The NVRAM removal is not proven
# durable (2sq/2sr); remove the boot-arg from the config between experiments.
# Stage 1 does NOT need it, and does not consume it, because no personality is
# sent and probe() is therefore never called.
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
    echo "com.rembrandtgpu.fb is already loaded. Reboot before another load." >&2
    echo "Do NOT use kmutil unload: unloading this framebuffer panics (2sq)." >&2
    exit 1
fi

# Stage 1 never calls probe(): --load-style start-only sends no personality, so
# no instance is ever created and the one-shot is neither read nor consumed.
# Requiring it there would be theatre. Stage 2 does match, so it does need it -
# and refusing BEFORE the load keeps a forgotten `nvram` from reading as "the
# kext is broken", because absence is not a result.
# A WHOLE-TOKEN test. 2ss: `grep -q -- '-rbgpufb'` reported the arg present
# because -rbgpufbvbl, -rbgpufbvram and -rbgpufbaccel all contain it, and that
# was a SAFETY check reading as armed. The kernel is stricter than the grep was
# (PE_parse_boot_argn requires token length == strlen(name)), so the kext was
# never actually at risk - our tooling was.
if [ "$mode" = beside ] && ! nvram boot-args 2>/dev/null | tr ' \t' '\n\n' | grep -qx -- '-rbgpuownfb'; then
    echo "-rbgpuownfb is not in boot-args, so probe() will decline (2sr: the"
    echo "boot-arg is the durable disarm, not the NVRAM key). Add it with:"
    echo "  sudo tools/efi.sh bootarg add -rbgpuownfb"
    exit 1
fi
if [ "$mode" = beside ] && ! nvram rbgpu-fb >/dev/null 2>&1; then
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
echo "--- the kext's own log lines ---"
# BEGINSWITH, not CONTAINS: CONTAINS "RBAMDFramebuffer" also matches every
# kernelmanagerd line carrying the bundle PATH, which buries our output in
# noise that looks like output. 15m rather than --last boot because the log
# archive's boot bookkeeping is not trustworthy here - `--last boot` returned
# TWO `Darwin Kernel Version` banners in one run, and `--start/--end` printed
# "Wall Clock adjustment detected". Rule 28.
/usr/bin/log show --last 15m --info --debug \
    --predicate 'eventMessage BEGINSWITH "RBAMDFramebuffer:"' \
    --style compact 2>/dev/null | grep 'RBAMDFramebuffer:' || \
    echo "(none captured - NOT a result: this log pipeline has already been seen
 to drop them while the kext was demonstrably loaded and matched. The registry
 dump below and the one-shot state are the evidence; read those instead.)"

echo
echo "--- IOFramebuffer services now in the registry ---"
# `ioreg -c X -k Y` does NOT filter the way it reads: the stage-1 run printed
# only `+-o Root` and said nothing about framebuffers. Rule 26 - the search was
# the instrument. `-rl -c` roots the tree at the class and prints properties.
ioreg -rl -c IOFramebuffer 2>/dev/null | grep -E '^\+-o|"IOClass"' || \
    echo "no IOFramebuffer service found - which would itself be a result"


if [ "$mode" = beside ]; then
    echo
    echo "--- CoreDisplay's vendor mask, as the tool reproduces it ---"
    echo "(0x04 beside the 0x01 is the result worth having; the VERDICT staying"
    echo " 0 is expected while .Display_boot is still present)"
    # useiop EXITS 1 when a framebuffer classifies as unknown vendor - that is
    # its verdict, not a failure, and chaining it with && reported "failed"
    # under output that had printed perfectly. Build and run as separate steps.
    (cd "$root" && make useiop >/dev/null 2>&1) || echo "make useiop: build failed"
    [ -x "$root/build/out/useiop" ] && "$root/build/out/useiop"
    echo "(useiop exit $? - 1 means 'an unknown-vendor framebuffer is present',"
    echo " which in THIS configuration is the expected answer, not a failure)"
fi

echo
echo "TO REMOVE THIS KEXT: REBOOT. Do NOT run kmutil unload."
echo
echo "  2sq: \`sudo kmutil unload -b com.rembrandtgpu.fb\` PANICKED the machine."
echo "  OSKext::removeKext -> IOCatalogue::terminateDriversForModule ->"
echo "  IOFramebuffer::terminate -> actionStop -> IOFramebuffer::stop+0x48,"
echo "  page fault, CR2 0x120. Apple's stop() does"
echo "    rax = this->0x1c8 ; rax = *rax ; r12 = rax->0x120"
echo "  and *(this->0x1c8) is NULL on a framebuffer that never completed"
echo "  configuration - which a beside instance never does, by design."
echo "  A reboot tears it down without running that path."
