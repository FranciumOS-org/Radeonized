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

# 2er: ioreg can block forever, so never run it unguarded.
#
# Every wedged armed boot has stopped this script at the same line - the
# `ioreg -c IOPCIDevice -r -l` walk - and the two 2026-10-13 runs prove it:
# ioreg.txt is written, pci.txt is created and stays 0 bytes, igpu.txt is
# never created at all. That walk recurses IGPU@0's whole subtree and
# serialises every property, so it is the first command that touches the
# branch holding the accelerator still inside start(). The earlier
# `-c RembrandtGPU` lookup finishes because our plugin's own IOService lives
# under IOResources, nowhere near it.
#
# A blocked ioreg is in an uninterruptible kernel wait: Ctrl-C does nothing
# and it cannot be killed, which is also why `sudo reboot` could not finish.
# So this does not try to kill it - it starts the walk in the background,
# waits a bounded time for a completion stamp, and moves on without it. The
# orphan clears on the next boot.
rb_ioreg() {   # rb_ioreg <seconds> <outfile> <ioreg args...>
    _secs="$1"; _dest="$2"; shift 2
    rm -f "$_dest.done"
    ( ioreg "$@" > "$_dest" 2>/dev/null; : > "$_dest.done" ) &
    _i=0
    while [ ! -f "$_dest.done" ] && [ "$_i" -lt "$_secs" ]; do
        sleep 1; _i=$((_i + 1))
    done
    if [ -f "$_dest.done" ]; then rm -f "$_dest.done"; return 0; fi
    echo "  *** ioreg $* did not finish in ${_secs}s."
    echo "      The registry is held by a wedged driver thread, so it cannot be"
    echo "      interrupted or killed. Skipping it; read 'nvram rbgpu-gate'."
    return 1
}

args="$(nvram boot-args 2>/dev/null | cut -f2-)"
printf '%s\n' "$args" > "$out/boot-args.txt"
sysctl -n kern.bootsessionuuid > "$out/boot-session-uuid.txt" 2>/dev/null
sysctl kern.boottime > "$out/boot-time.txt" 2>/dev/null
echo "boot-args: $args"
case " $args " in *" -rbgpuoff "*) off=1; echo "  -rbgpuoff is set: the plugin should report itself disabled" ;; *) off=0 ;; esac

echo
echo "--- nvram breadcrumbs (the only record a wedged boot leaves)"
for v in rbgpu-boot rbgpu-prog rbgpu-sdma rbgpu-1b rbgpu-prog2 rbgpu-gate rbgpu-ea rbgpu-ctxw rbgpu-gvmw rbgpu-vmid rbgpu-ptw rbgpu-ptfix rbgpu-aper rbgpu-pdc rbgpu-walk rbgpu-rst rbgpu-me rbgpu-rd2 rbgpu-cpfw rbgpu-pwr rbgpu-rlcx rbgpu-wtest rbgpu-bl rbgpu-ume rbgpu-ald rbgpu-rstop rbgpu-mehold rbgpu-hyp rbgpu-blbuf rbgpu-grbm rbgpu-mew rbgpu-mew2 rbgpu-cgcg rbgpu-ib rbgpu-cph rbgpu-pde rbgpu-pdw rbgpu-pdv rbgpu-adj rbgpu-disp rbgpu-addr rbgpu-surf rbgpu-res rbgpu-si rbgpu-hwi rbgpu-mode rbgpu-ring rbgpu-vm rbgpu-fault rbgpu-pm4 rbgpu-fence rbgpu-diag rbgpu-diag-prev rbgpu-arm rbgpu-iop rbgpu-mcreg rbgpu-agdc; do
    val="$(nvram "$v" 2>/dev/null | cut -f2-)"
    echo "  $v = ${val:-<unset>}"
done > "$out/nvram.txt" 2>&1
cat "$out/nvram.txt"
# 2rh: FLUSH NOW. The three hangs of 2026-10-26 were ended by a hard reset and
# the capture directory itself never reached the disk - no logs/<ts>-plugin at
# all, so the breadcrumbs this script exists to save were lost with the boot.
# These are the only record a wedged boot leaves (rule 10), so they are synced
# before anything slower, or riskier, runs.
sync; sync
echo "  breadcrumbs synced to disk"

# Save the full timing verdict before log collection. The NVRAM summary is
# only 512 bytes and boot 164 lost these keys to truncation. A depth-one walk
# reads our peer's properties without traversing the accelerator's children.
rb_ioreg 10 "$out/peer.plist" -w0 -p IOService -c RembrandtGPUPeer -r -d1 -l -a
sync

echo
echo "--- loaded kexts"
kmutil showloaded --list-only 2>/dev/null | grep -E 'as.vit9696.Lilu|com.rembrandtgpu' | tee "$out/kexts.txt"
grep -q com.rembrandtgpu.plugin "$out/kexts.txt" || echo "  com.rembrandtgpu.plugin is not loaded"
# Is the loaded plugin the one in build/out? (kmutil prints the binary's UUID)
built="$(dwarfdump --uuid "$root/build/out/RembrandtGPU.kext/Contents/MacOS/RembrandtGPU" 2>/dev/null | awk '{print $2}')"
if [ -n "$built" ] && grep -q com.rembrandtgpu.plugin "$out/kexts.txt"; then
    grep com.rembrandtgpu.plugin "$out/kexts.txt" | grep -q "$built" \
        && echo "  loaded plugin = build/out (UUID $built)" \
        || echo "  NOTE: loaded plugin is NOT the current build/out (UUID $built): reinstall with sudo tools/efi.sh install"
fi

echo
echo "--- display startup and GPU progress this boot"
# 2rc: a frozen Apple logo can leave SSH and the entire registry responsive.
# Keep the display's own errors with the same capture, before reboot loses
# the live stack. Filtering only for RembrandtGPU missed this in boot 156.
/usr/bin/log show --last boot --style compact --predicate \
    '(process == "WindowServer" AND (subsystem == "com.apple.CoreDisplay" OR subsystem == "com.apple.SkyLight" OR senderImagePath CONTAINS "IOPresentment")) OR subsystem == "com.apple.GPUWrangler" OR eventMessage CONTAINS "checkGPUProgress"' \
    > "$out/display-log.txt" 2> "$out/display-log-errors.txt"
echo "  $(wc -l < "$out/display-log.txt" | tr -d ' ') display log lines saved"
# 2rh: THE SAMPLE IS NOW OPT-IN, AND IT IS THE REASON THIS SCRIPT KILLED THE
# MACHINE TWICE. `sample` suspends the target's threads and walks their stacks.
# A WindowServer wedged inside a kernel call - which is the exact state you run
# this script in - cannot be suspended, so `sample` blocks in the kernel and
# takes the box with it. On 2026-10-26 that happened on two of three attempts,
# each time destroying the capture it was part of. 2rc added this step; rule 16
# said the instrument is a suspect, and here it was the culprit.
#
# A shell timeout does NOT make it safe: killing the wrapper cannot unblock a
# task_suspend already stuck in the kernel, exactly as rb_ioreg says of a
# blocked ioreg. So the default is OFF. The guard below is a second line only,
# and it reports honestly that it cannot recover the thread.
if [ -z "${RBGPU_WS_SAMPLE:-}" ]; then
    {
      echo "WindowServer sample SKIPPED (default since 2rh)."
      echo "It hung this machine twice on 2026-10-26 by suspending a wedged"
      echo "WindowServer, losing the capture both times. Run it only when the"
      echo "desktop is RESPONSIVE: RBGPU_WS_SAMPLE=1 sudo tools/plugin_check.sh"
    } > "$out/ws-sample-status.txt"
    echo "  WindowServer sample skipped by default (RBGPU_WS_SAMPLE=1 to force)"
elif [ "$(id -u)" != 0 ]; then
    echo "  WindowServer sample skipped: requires root" > "$out/ws-sample-status.txt"
else
    ws="$(ps -axo pid=,comm= | awk '/WindowServer$/{print $1; exit}')"
    if [ -z "$ws" ]; then
        echo "  WindowServer not running" > "$out/ws-sample-status.txt"
    else
        rm -f "$out/ws-sample.done"
        ( /usr/bin/sample "$ws" 3 -file "$out/ws-sample.txt" \
              > "$out/ws-sample-status.txt" 2>&1; : > "$out/ws-sample.done" ) &
        _i=0
        while [ ! -f "$out/ws-sample.done" ] && [ "$_i" -lt 15 ]; do
            sleep 1; _i=$((_i + 1))
        done
        if [ -f "$out/ws-sample.done" ]; then
            rm -f "$out/ws-sample.done"
            echo "  WindowServer sample: $out/ws-sample.txt"
        else
            echo "  *** sample did not finish in 15s - WindowServer is wedged in"
            echo "      the kernel and CANNOT be suspended, interrupted or killed."
            echo "      The machine may now hang. Everything above is already on"
            echo "      disk and synced. Do not run this again with the display"
            echo "      frozen; the breadcrumbs are the evidence, not the sample."
        fi
    fi
fi

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
rb_ioreg 20 "$out/ioreg-raw.txt" -w0 -p IOService -c RembrandtGPU -r -l
grep -E '\+-o RembrandtGPU|CFBundleIdentifier"' "$out/ioreg-raw.txt" 2>/dev/null \
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
rb_ioreg 30 "$out/pci.txt" -w0 -p IOService -r -c IOPCIDevice -l
# The 680M's IOPCIDevice (IGPU@0) and what is attached to it.
rb_ioreg 30 "$out/igpu.txt" -w0 -p IOService -n IGPU@0 -r -d1 -l
rb_ioreg 30 "$out/igpu-raw.txt" -w0 -p IOService -n IGPU@0 -r -d2
grep -- "+-o" "$out/igpu-raw.txt" 2>/dev/null | sed 's/<class \([^,]*\),.*/(\1)/' | tee "$out/igpu-children.txt"
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
echo
echo "--- plugin diagnostics (RBGPULog / RBGPUDiag on the peer)"
# Already captured before the slower display log. Do not issue a second walk
# if the first blocked: a timeout cannot cancel an in-kernel registry wait.
python3 - "$out/peer.plist" <<'PY'
import plistlib, sys
try:
    tree = plistlib.load(open(sys.argv[1], "rb"))
except Exception:
    print("  peer not found"); sys.exit(0)
node = tree[0] if isinstance(tree, list) and tree else tree
diag, log = node.get("RBGPUDiag"), node.get("RBGPULog")
if diag is None and log is None:
    print("  (the peer publishes no RBGPUDiag / RBGPULog)")
for k, v in sorted((diag or {}).items()):
    # ioreg -a writes 64-bit numbers as signed integers; show them as unsigned.
    print(f"  {k} = {v & 0xffffffffffffffff:#x}" if isinstance(v, int) else f"  {k} = {v}")
for line in (log or []):
    print("  | " + str(line).replace("\n", "\n  |     "))
PY
echo "--- logged pointers inside AMD kexts (tools/re/ptrsym.py)"
python3 "$root/tools/re/ptrsym.py" "$out/peer.plist" 2>&1
ls -t /Library/Logs/DiagnosticReports/*.panic 2>/dev/null | head -1 | sed 's/^/  newest panic report: /'

[ -n "${SUDO_USER:-}" ] && chown -R "$SUDO_USER" "$out"
echo
echo "saved to ${out#$root/}"
