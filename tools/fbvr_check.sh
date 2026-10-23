#!/bin/sh
# 2pq: did enlarging the accelerator's flip window change anything?
#
#   sudo tools/fbvr_check.sh      # sudo only for `sample WindowServer`
#
# Read-only. Saves everything to logs/<time>-fbvr/. Run this in the armed
# boot, over SSH, BEFORE rebooting (rule 10): live NVRAM does not survive a
# power cycle, the files on disk do.
set -u

root="$(cd "$(dirname "$0")/.." && pwd)"
out="$root/logs/$(date +%Y%m%d-%H%M%S)-fbvr"
mkdir -p "$out"

# Same bounded walk as plugin_check.sh: a wedged driver thread puts ioreg in
# an uninterruptible kernel wait, so it is never run unguarded (2er).
rb_ioreg() {   # rb_ioreg <seconds> <outfile> <ioreg args...>
    _secs="$1"; _dest="$2"; shift 2
    rm -f "$_dest.done"
    ( ioreg "$@" > "$_dest" 2>/dev/null; : > "$_dest.done" ) &
    _i=0
    while [ ! -f "$_dest.done" ] && [ "$_i" -lt "$_secs" ]; do
        sleep 1; _i=$((_i + 1))
    done
    if [ -f "$_dest.done" ]; then rm -f "$_dest.done"; return 0; fi
    echo "  *** ioreg $* did not finish in ${_secs}s - registry held by a"
    echo "      wedged driver thread. The orphan clears on the next boot."
    return 1
}

echo "=== 0. rule 8: is this boot's capture this boot's? ============="
nvram rbgpu-boot 2>/dev/null | tee "$out/boot.txt" \
    || echo "rbgpu-boot absent - the boot was NEVER ARMED and nothing below is this boot's"
echo "   If this has not increased since the last capture, STOP: every"
echo "   reading below is stale (rule 8)."
echo

echo "=== 1. boot-args actually in force ============================="
nvram boot-args 2>/dev/null | tr ' ' '\n' | grep -E 'rbgpu' | tee "$out/bootargs.txt"
echo

echo "=== 2. the 2pq keys ============================================"
# fbvr_orig_len is the GATE: if it is not 8294400 the machine was not in the
# state 2pp measured, and nothing else here means anything.
nvram -p 2>/dev/null | grep -E 'rbgpu-diag|rbgpu-arm' > "$out/nvram-diag.txt"
tr ' ' '\n' < "$out/nvram-diag.txt" | grep -E '^fbvr_' | tee "$out/fbvr.txt"
[ -s "$out/fbvr.txt" ] || echo "   NO fbvr_* keys - the route never armed, or the diag is stale"
echo
echo "   how to read it:"
echo "     fbvr_orig_len  8294400 = the state 2pp measured; anything else voids the rest"
echo "     fbvr_verify    1 = the six call-site bytes matched; 0 = nothing was enlarged"
echo "     fbvr_hit       >=1 = find_vram_descriptor asked us; 0 = it cached before we armed"
echo "     fbvr_big       >=1 = the enlarged range went out"
echo "     fbvr_fail      1 no orig, 2 no base, 3 base unaligned, 4 already big, 5 withRange refused"
echo

echo "=== 3. CONTROL: IOFBMemorySize must still be 8294400 ==========="
# Only find_vram_descriptor's one call site is enlarged. IOGraphics uses a
# different call site for this property, so an unchanged 8294400 proves the
# change was surgical rather than blanket.
rb_ioreg 20 "$out/fb.txt" -w0 -c IONDRVFramebuffer -r -l
grep -E 'IOFBMemorySize|IOAccelIndex|IOAccelRevision|IOAccelTypes|IOFramebufferOpenGLIndex' \
    "$out/fb.txt" 2>/dev/null | tee "$out/fb-keys.txt"
echo

echo "=== 4. did flipping start? ====================================="
rb_ioreg 30 "$out/accel.txt" -w0 -c IOAccelerator -r -l
# -c matches subclasses, so AMDRadeonX6000_AMDNavi21GraphicsAccelerator should
# be in there. If it is not, fall back to the full tree rather than reporting
# an empty result as a zero - absence is not a result (rule 14).
if ! grep -q 'GraphicsAccelerator' "$out/accel.txt" 2>/dev/null; then
    echo "   -c IOAccelerator found no accelerator; falling back to the full tree"
    rb_ioreg 40 "$out/accel.txt" -w0 -l
fi
for k in bufferFlipCount bufferSwapCount; do
    printf '%s = ' "$k"
    grep -o "\"$k\"=[0-9]*" "$out/accel.txt" 2>/dev/null | head -2 | tr '\n' ' '
    echo
done
grep -o '"IOAccelDisplayPipeCapabilities" = {[^}]*}' "$out/accel.txt" 2>/dev/null | head -1
grep -c 'IOAccelDisplayPipeUserClient2' "$out/accel.txt" 2>/dev/null \
    | sed 's/^/IOAccelDisplayPipeUserClient2 nodes: /'
echo "   2pf's control: both counts were 0 with the pipe bound and a client open."
echo

echo "=== 5. did the flip probe fire? (-rbgpuflipprobe, 2pe) ========="
tr ' ' '\n' < "$out/nvram-diag.txt" | grep -E '^flip|^fliprq' | tee "$out/flip.txt"
[ -s "$out/flip.txt" ] || echo "   no flip keys - probe not armed, or it never fired (2pf: never, once)"
echo

echo "=== 6. is software compositing still 48% of WindowServer? ======"
if [ "$(id -u)" = "0" ]; then
    ws=$(ps -axo pid=,command= | awk '/WindowServer/ && !/awk/ {print $1; exit}')
    if [ -n "${ws:-}" ]; then
        sample "$ws" 3 -file "$out/ws-sample.txt" >/dev/null 2>&1
        grep -cE 'CompositeLayersToDestination' "$out/ws-sample.txt" 2>/dev/null \
            | sed 's/^/CompositorSW::CompositeLayersToDestination frames: /'
        grep -E 'CompositorSW|CompositorMetal' "$out/ws-sample.txt" 2>/dev/null \
            | head -5
        echo "   (full sample in $out/ws-sample.txt - 2pd measured 48% of the main thread)"
    else
        echo "   WindowServer not found by ps"
    fi
else
    echo "   skipped - needs sudo. Re-run as: sudo tools/fbvr_check.sh"
fi
echo

echo "=== 7. did the vblank regress? (2oz/2ov) ======================="
if [ -x "$root/build/out/dispclk" ]; then
    "$root/build/out/dispclk" 2>&1 | tee "$out/dispclk.txt" | head -12
    echo "   nominal ~129 Hz = 2oz/2ov still good; 30.00 = the pacing regressed"
else
    echo "   build/out/dispclk missing - run: make tools"
fi
echo

echo "saved to $out"
