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
  #
  # 2he: the 2hd hang is process-local - SSH and the rest of the machine
  # stay responsive while newCommandQueue never returns. So the blocked
  # thread can be SAMPLED, and its stack names what it is waiting on
  # instead of leaving us to guess. Run the probe in the background, give
  # it a bounded wait, and sample it if it is still alive.
  ./build/out/metal_probe "$@" > "$out/metal.txt" 2>&1 &
  probe=$!
  waited=0
  while [ "$waited" -lt 25 ]; do
    kill -0 "$probe" 2>/dev/null || break
    sleep 1
    waited=$((waited + 1))
  done

  if kill -0 "$probe" 2>/dev/null; then
    echo "metal_probe STILL RUNNING after ${waited}s - sampling it" >> "$out/metal.txt"
    sync
    say "probe is hung - sampling pid $probe"
    # Userspace stacks: shows the frames down to the trap, which names the
    # call the thread is blocked in.
    sample "$probe" 3 -file "$out/sample.txt" >/dev/null 2>&1
    sync
    say "sample saved"
    # Kernel stacks need root; skipped rather than silently producing
    # nothing (absence is not a result).
    if [ "$(id -u)" = "0" ]; then
      spindump "$probe" 3 -file "$out/spindump.txt" >/dev/null 2>&1
      say "spindump saved"
    else
      echo "spindump needs root; rerun with sudo for kernel stacks" > "$out/spindump.txt"
    fi
    sync
    echo "--- hung; left running. kill it with: kill $probe" >> "$out/metal.txt"
  else
    wait "$probe"
    echo "metal_probe exit=$?" >> "$out/metal.txt"
  fi
fi
sync
say "metal saved"

echo
echo "--- $out/metal.txt ---"
cat "$out/metal.txt"
echo
echo "saved to $out"
