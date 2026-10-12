#!/bin/sh
# Dump this boot's RembrandtGPU diagnostics using nothing but ioreg and sh.
#
#   tools/dump_peer.sh              # writes logs/<time>-dump/
#   tools/dump_peer.sh /some/dir    # or to a directory you name
#
# Why this exists: plugin_check.sh needs python3, `log show` and dwarfdump,
# none of which are reliable in single-user mode (boot with Cmd+S, or -s).
# When the accelerator starts but the desktop never appears, single-user is
# the way to get the log off the machine, because RBGPULog and RBGPUDiag live
# in the IORegistry of the running boot and are gone after a reboot.
#
# Read-only with respect to the system. Parse the result later, on a boot that
# reaches a desktop:  python3 tools/re/ptrsym.py <dir>/peer.plist
set -u

root="$(cd "$(dirname "$0")/.." && pwd)"
out="${1:-$root/logs/$(date +%Y%m%d-%H%M%S)-dump}"

if ! mkdir -p "$out" 2>/dev/null; then
    echo "cannot write to $out" >&2
    echo "in single-user mode, run 'mount -uw /' first" >&2
    exit 1
fi

echo "writing to $out"

# The one that matters: the peer's properties, RBGPULog and RBGPUDiag included.
ioreg -w0 -p IOService -c RembrandtGPUPeer -r -l -a > "$out/peer.plist" 2>/dev/null
if [ -s "$out/peer.plist" ]; then
    echo "  peer.plist        $(wc -c < "$out/peer.plist" | tr -d ' ') bytes"
else
    echo "  peer.plist        EMPTY - no peer this boot (-rbgpuoff, or the plugin did not run)"
fi

# Context, all cheap and all ioreg.
ioreg -w0 -p IOService -n IGPU@0 -r -d2 -l -a > "$out/igpu.plist" 2>/dev/null
ioreg -w0 -p IOService -n IGPU@0 -r -d2 2>/dev/null \
    | grep -- "+-o" | sed 's/<class \([^,]*\),.*/(\1)/' > "$out/igpu-children.txt"
ioreg -w0 -p IOService -c IOAccelerator -r -l > "$out/accel.txt" 2>/dev/null
ioreg -w0 -p IOService -c IOFramebuffer -r -l > "$out/framebuffer.txt" 2>/dev/null

nvram boot-args 2>/dev/null | cut -f2- > "$out/boot-args.txt"

echo
echo "--- boot-args"
cat "$out/boot-args.txt" 2>/dev/null

echo
echo "--- what is attached to IGPU@0"
cat "$out/igpu-children.txt" 2>/dev/null

# The plain-text tail of the log, so a console with no scrollback still shows
# the end of the run. The full ring is in peer.plist.
echo
echo "--- last RBGPULog lines (full ring is in peer.plist)"
sed -n 's/^[[:space:]]*<string>\(\[[0-9]*\].*\)<\/string>$/\1/p' "$out/peer.plist" 2>/dev/null | tail -25

echo
echo "done: $out"
echo "parse it later with: python3 tools/re/ptrsym.py $out/peer.plist"
