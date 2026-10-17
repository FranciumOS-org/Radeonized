#!/bin/sh
# 2hc: capture the Metal state of an ARMED boot, writing every result to
# disk as it is produced.
#
# Armed boots have no display (2hb) and have been ending in a power cycle,
# which takes RBGPULog and anything left on a terminal with it. So nothing
# here is meant to be read on screen: each step lands in
# logs/<timestamp>-metal/ and is flushed before the next one starts.
#
#   tools/metal_check.sh              enumerate only - dispatches nothing
#   tools/metal_check.sh --dispatch   also run and VERIFY a compute kernel
#
# --dispatch is the one that can hang the GPU. It is separate for that
# reason: run the plain form first, and only add --dispatch once its output
# is safely on disk.
set -u
cd "$(dirname "$0")/.." || exit 1
out="logs/$(date +%Y%m%d-%H%M%S)-metal"
mkdir -p "$out" || exit 1
echo "saving to $out"

say() { echo "=== $1 ==="; }

{
  say "armed?"
  # The 2gw rule: a capture is the armed boot's only if rbgpu-diag reads
  # armed=1 LIVE. Recorded here so the file can never be misread later.
  nvram rbgpu-diag 2>/dev/null | sed 's/.*	//' | cut -c1-120
  nvram rbgpu-boot 2>/dev/null | sed 's/.*	//'
  say "loaded"
  kmutil showloaded 2>/dev/null | grep -i -E 'rembrandt|lilu'
  say "accelerator in registry"
  ioreg -c AMDRadeonX6000_AMDNavi21GraphicsAccelerator -d1 2>/dev/null | head -60
} > "$out/state.txt" 2>&1
sync

say "state saved"

system_profiler SPDisplaysDataType > "$out/displays.txt" 2>&1
sync
say "displays saved"

ioreg -l -w0 > "$out/ioreg.txt" 2>&1
sync
say "ioreg saved"

if [ ! -x build/out/metal_probe ]; then
  echo "build/out/metal_probe missing - build it with:" > "$out/metal.txt"
  echo "  clang -fobjc-arc -O2 -framework Foundation -framework Metal \\" >> "$out/metal.txt"
  echo "        -o build/out/metal_probe tools/metal_probe.m" >> "$out/metal.txt"
else
  # metal_probe fflushes before each step that can hang, so a wedge still
  # leaves everything up to that point in the file (macOS has no stdbuf).
  ./build/out/metal_probe "$@" > "$out/metal.txt" 2>&1
  echo "metal_probe exit=$?" >> "$out/metal.txt"
fi
sync
say "metal saved"

echo
echo "--- $out/metal.txt ---"
cat "$out/metal.txt"
echo
echo "saved to $out"
