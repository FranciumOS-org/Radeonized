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
echo "--- verdict"
if grep -q "loaded, doing nothing (milestone 0)" "$out/log.txt"; then
    [ $off = 1 ] && echo "WRONG: plugin ran although -rbgpuoff is set" \
                 || echo "OK: plugin loaded and ran (milestone 0)"
elif grep -q "should not continue 5" "$out/log.txt"; then
    [ $off = 1 ] && echo "OK: -rbgpuoff works, the plugin stopped itself" \
                 || echo "WRONG: plugin says disabled but -rbgpuoff is not set"
elif grep -q "should not continue 4" "$out/log.txt"; then
    echo "Lilu refused it: macOS version outside the plugin's range (IncompatibleOS)"
elif grep -q "should not continue" "$out/log.txt"; then
    echo "Lilu refused it: $(grep -o 'should not continue [0-9]*' "$out/log.txt" | head -1) (see kern_api.hpp Error)"
elif grep -q "failed to call parent" "$out/log.txt"; then
    echo "the plugin could not reach Lilu (is it after Lilu in config.plist?)"
else
    echo "no RembrandtGPU messages found."
    [ "$(id -u)" = 0 ] || echo "try again with sudo (searches the kernel buffer too), then run: sudo tools/efi.sh status"
fi

[ -n "${SUDO_USER:-}" ] && chown -R "$SUDO_USER" "$out"
echo
echo "saved to ${out#$root/}"
