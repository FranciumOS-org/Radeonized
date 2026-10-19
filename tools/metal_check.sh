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
# 2hi: run it under sudo. Root is what produces the kernel stacks, and the
# kernel stacks are what located the fEnabled gate (2hf) and the GFX-ring
# stamp (2hh). Without root this script still runs and says so in the file,
# but it cannot see the frontier.
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

# 2hi: anything that walks the IO registry or asks about displays can itself
# block on a wedged accelerator - and a capture that stalls before the Metal
# probe tells us nothing (absence is not a result). Run such a step in the
# background with a bounded wait, and if it does not finish, SAY SO in its
# own file rather than leaving a truncated one that reads like an answer.
#   bounded <seconds> <outfile> <command...>
bounded() {
  _secs=$1; _file=$2; shift 2
  "$@" > "$_file" 2>&1 &
  _pid=$!
  _w=0
  while [ "$_w" -lt "$_secs" ]; do
    kill -0 "$_pid" 2>/dev/null || break
    sleep 1
    _w=$((_w + 1))
  done
  if kill -0 "$_pid" 2>/dev/null; then
    kill -9 "$_pid" 2>/dev/null
    echo "" >> "$_file"
    echo "*** $* DID NOT FINISH in ${_secs}s - killed." >> "$_file"
    echo "*** Anything above this line is partial. This is itself a result:" >> "$_file"
    echo "*** the command blocked, which points at the wedged stack." >> "$_file"
    sync
    return 1
  fi
  wait "$_pid" 2>/dev/null
  sync
  return 0
}

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
  # 2hi: the live boot-args, not config.plist's. This is what says whether
  # wdt=-1 and -rbgpuforceenable actually took effect this boot; a capture
  # that does not record it cannot be read later.
  say "live boot-args"
  sysctl -n kern.bootargs 2>/dev/null
  # 2hi: every note channel, in the armed boot, so this capture does not
  # depend on plugin_check.sh having been run separately (2gq).
  say "nvram notes"
  for v in rbgpu-arm rbgpu-boot rbgpu-diag rbgpu-diag-prev rbgpu-ea \
           rbgpu-ctxw rbgpu-gvmw rbgpu-vmid rbgpu-ptw rbgpu-ptfix rbgpu-aper rbgpu-pdc rbgpu-walk rbgpu-rst rbgpu-me rbgpu-rd2 rbgpu-cpfw rbgpu-pwr rbgpu-rlcx rbgpu-wtest rbgpu-bl rbgpu-ume rbgpu-ald rbgpu-rstop rbgpu-ring rbgpu-vm rbgpu-fault rbgpu-pm4 rbgpu-fence rbgpu-prog rbgpu-prog2 rbgpu-gate rbgpu-1b rbgpu-sdma; do
    printf '%s = ' "$v"
    nvram "$v" 2>/dev/null | sed 's/.*	//' || echo "(unset)"
  done
} > "$out/state.txt" 2>&1
sync

say "state saved"

# 2hi: with -rbgpuforceenable the gate is open, WindowServer composites and
# then hangs on a surface sync whose stamp never retires (2hh). THAT hang is
# the frontier, so capture it whether or not the Metal probe hangs too - and
# capture it before the probe, which may add load of its own.
if [ "$(id -u)" = "0" ]; then
  if spindump WindowServer 3 -file "$out/ws-spindump.txt" >/dev/null 2>&1; then
    say "WindowServer spindump saved"
  else
    echo "spindump WindowServer failed - not running, or already killed" \
      > "$out/ws-spindump.txt"
    say "WindowServer spindump FAILED (recorded)"
  fi
else
  echo "needs root for kernel stacks; rerun with sudo" > "$out/ws-spindump.txt"
  say "WindowServer spindump skipped (not root) - recorded"
fi
sync

if bounded 30 "$out/displays.txt" system_profiler SPDisplaysDataType; then
  say "displays saved"
else
  say "displays BLOCKED (recorded)"
fi

if bounded 30 "$out/ioreg.txt" ioreg -l -w0; then
  say "ioreg saved"
else
  say "ioreg BLOCKED (recorded)"
fi

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
