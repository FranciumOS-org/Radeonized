#!/bin/sh
# After a reboot: did the RembrandtGPU plugin load, and what did it say?
#
#   tools/plugin_check.sh          # no sudo needed
#   sudo tools/plugin_check.sh     # also searches the kernel message buffer
#
# Read-only. Saves what it found to logs/<time>-plugin/.
set -u

root="$(cd "$(dirname "$0")/.." && pwd)"
out="$root/logs/$(date +%Y%m%d-%H%M%S)-plugin"
mkdir -p "$out"

args="$(nvram boot-args 2>/dev/null | cut -f2-)"
echo "boot-args: $args"
case " $args " in *" -rbgpuoff "*) off=1; echo "  -rbgpuoff is set: the plugin should report itself disabled" ;; *) off=0 ;; esac

echo
echo "--- loaded kexts"
kmutil showloaded --list-only 2>/dev/null | grep -E 'as.vit9696.Lilu|com.rembrandtgpu' | tee "$out/kexts.txt"
grep -q com.rembrandtgpu.plugin "$out/kexts.txt" || echo "  com.rembrandtgpu.plugin is not loaded"

echo
echo "--- plugin messages this boot"
/usr/bin/log show --last boot --style compact --predicate 'eventMessage CONTAINS "RembrandtGPU"' 2>/dev/null \
    | grep RembrandtGPU | grep -v "log run noninteractively" > "$out/log.txt"
if [ "$(id -u)" = 0 ]; then
    /sbin/dmesg | grep RembrandtGPU >> "$out/log.txt"
fi
cat "$out/log.txt"

echo
echo "--- registry"
# Lilu's plugin_start.cpp: the RembrandtGPU IOService only survives start()
# when Lilu accepted the plugin and pluginStart() ran (startSuccess). A loaded
# kext without the instance stopped itself (-rbgpuoff, or Lilu refused it).
ioreg -w0 -p IOService -c RembrandtGPU -r -l | grep -E '\+-o RembrandtGPU|CFBundleIdentifier"' \
    | tee "$out/ioreg.txt"
loaded=0; grep -q com.rembrandtgpu.plugin "$out/kexts.txt" && loaded=1
running=0; grep -q '+-o RembrandtGPU' "$out/ioreg.txt" && running=1

echo
echo "--- verdict"
if [ $running = 1 ]; then
    [ $off = 1 ] && echo "WRONG: plugin ran although -rbgpuoff is set" \
                 || echo "OK: plugin loaded and ran (milestone 0)"
elif [ $loaded = 1 ]; then
    if [ $off = 1 ]; then
        echo "OK: -rbgpuoff works, the plugin stopped itself"
    else
        echo "plugin loaded but did not run: Lilu refused it."
        grep -o 'should not continue [0-9]*\|failed to call parent [0-9]*' "$out/log.txt" | head -1
        echo "(Lilu errors: 4 = macOS version out of range, 5 = disabled, 6 = too late)"
    fi
else
    echo "plugin not loaded: OpenCore did not inject it. Run: sudo tools/efi.sh status"
fi
echo "(early-boot plugin log lines are usually not kept; the registry is the check)"

echo
echo "--- milestone 1: peer, GPU properties, HWServices"
ioreg -w0 -p IOService -r -c IOPCIDevice -l 2>/dev/null > "$out/pci.txt"
# The 680M's IOPCIDevice (IGPU@0) and what is attached to it.
ioreg -w0 -p IOService -n IGPU@0 -r -d1 -l 2>/dev/null > "$out/igpu.txt"
ioreg -w0 -p IOService -n IGPU@0 -r -d2 2>/dev/null | grep -- "+-o" | sed 's/<class \([^,]*\),.*/(\1)/' | tee "$out/igpu-children.txt"
for k in '"@0,name"' '"RBGPULoad"' '"LoadHWServices"' '"LoadAccelerator"'; do
    v="$(grep -m1 "$k" "$out/igpu.txt" | sed 's/^[ |]*//')"
    echo "  ${v:-$k: not set}"
done
binsz="$(grep -m1 '"ATY,bin_image"' "$out/igpu.txt" | sed 's/.*<\(.*\)>.*/\1/' | tr -d '\n' | wc -c)"
[ "$binsz" -gt 0 ] && echo "  ATY,bin_image: $((binsz / 2)) bytes" || echo "  ATY,bin_image: not set"
peer=0; grep -q "RembrandtGPUPeer" "$out/igpu-children.txt" && peer=1
hws=0; grep -q "HWServices" "$out/igpu-children.txt" && hws=1
echo "  peer attached: $peer   HWServices attached: $hws"
kmutil showloaded --list-only 2>/dev/null | grep -iE "AMDRadeonX6000|AMDRadeonX6800|AMDSupport" | tee "$out/amd-kexts.txt"

/usr/bin/log show --last boot --style compact --predicate \
    'eventMessage CONTAINS "RembrandtGPUPeer" OR eventMessage CONTAINS[c] "AMDRadeon" OR eventMessage CONTAINS "HWLibs" OR eventMessage CONTAINS "HWServices"' \
    2>/dev/null | grep -v -e "log run noninteractively" -e "^Timestamp" > "$out/amd-log.txt"
[ "$(id -u)" = 0 ] && /sbin/dmesg | grep -iE "RembrandtGPUPeer|AMDRadeon|HWLibs|HWServices|firmware device|Unsupported hw" >> "$out/amd-log.txt"
echo "  $(wc -l < "$out/amd-log.txt" | tr -d ' ') log lines from the peer and Apple's AMD kexts (first 40):"
head -40 "$out/amd-log.txt" | sed 's/^/    /'
ls -t /Library/Logs/DiagnosticReports/*.panic 2>/dev/null | head -1 | sed 's/^/  newest panic report: /'

[ -n "${SUDO_USER:-}" ] && chown -R "$SUDO_USER" "$out"
echo
echo "saved to ${out#$root/}"
