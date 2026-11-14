#!/bin/sh
# 3rgr-b: capture a STALLED boot over SSH, in one command.
#
# The failure leaves no log store and no NVRAM note, but the kernel stays
# alive and Wi-Fi stays up. This is the only instrument that reads it while
# it is happening. Run it over SSH DURING the stall:
#
#   sh tools/stallcapture.sh            # everything that needs no password
#   sudo sh tools/stallcapture.sh       # adds the spindump, which is the point
#
# Writes one file and prints its path. Nothing here changes the machine.
set -u
out="${TMPDIR:-/tmp}/rbgpu-stall-$(date +%Y%m%d-%H%M%S).txt"
: > "$out"
say() { printf '\n===== %s =====\n' "$1" >> "$out"; }
run() { say "$1"; shift; "$@" >> "$out" 2>&1 || echo "  (command failed: $*)" >> "$out"; }

say "WHEN"
{
  date
  echo "boottime: $(sysctl -n kern.boottime)"
  echo "uptime:   $(uptime)"
} >> "$out"

# --- did our halves come up at all? Rule 14: absence must be stated. ---
say "OUR KEXTS"
kextstat 2>/dev/null | grep -iE "rembrandt|lilu|nullmoth" >> "$out" || echo "  NONE LOADED" >> "$out"

# 3rgr-g: the four questions a stalled boot has to answer, at the TOP.
# The full dumps below run to ~3900 lines and the numbers that decide where a
# stall got to were buried in them. Read over SSH on a machine that is stalled
# but alive, which 3rgr-b established is the actual failure mode.
say "VERDICT (read this first)"
{
    printf '  %-26s %s\n' "boot" "$(nvram rbgpu-boot 2>/dev/null | awk '{print $2}')"
    printf '  %-26s %s\n' "safe latch" "$(nvram rbgpu-safe 2>/dev/null | awk '{print $2}')"
    if ioreg -rl -c RembrandtGPUPeer 2>/dev/null | grep -q RBGPUDiag; then
        ioreg -rl -c RembrandtGPUPeer 2>/dev/null | tr ',' '\n' \
          | grep -E '"(armed|arm_why|hwlibs_patched|hwlibs_gate_ms|hwlibs_gate_ok|hwlibs_gate_alloc|hwlibs_gate_kick|hwlibs_gate_kick_armed|hwlibs_gate_marks|hwlibs_gate_mark_why|sentinel_cleared|sentinel_cleared_by|sentinel_attach_sched)"' \
          | sed 's/^ */  /'
    else
        # Rule 14: the peer being absent is a reading, not a blank.
        echo "  PEER ABSENT FROM THE REGISTRY - it never published, so every"
        echo "  gate number below is unavailable rather than zero."
    fi
    ioreg -rl -c RBAMDFramebuffer 2>/dev/null | tr ',' '\n' \
      | grep -E '"(RBFBArmWhy|RBFBModeCount|RBFBModeRefuse|RBFBModeArmed|RBFBModeCurMhz|RBFBCrumbBoot)"' \
      | sed 's/^ */  /' \
      || echo "  RBAMDFramebuffer ABSENT FROM THE REGISTRY"
    echo "  -- how to read it --"
    echo "  hwlibs_gate_ok 1 + ms set   the gate closed; a stall after this is"
    echo "                              NOT the gate (3rgr-c's question)"
    echo "  hwlibs_gate_alloc 1, no ok  the gate armed and never closed"
    echo "  hwlibs_gate_mark_why 1      -rbgpunogates is default-on, so the"
    echo "                              rbgpu-gate trail is OFF - expected"
    echo "  arm_why 6                   the sentinel latch; the PREVIOUS boot"
    echo "                              did not survive, this one came up stock"
} >> "$out"
echo >> "$out"

say "PEER DIAG (RembrandtGPUPeer)"
if ioreg -rl -c RembrandtGPUPeer 2>/dev/null | grep -q RBGPUDiag; then
    ioreg -rl -c RembrandtGPUPeer 2>/dev/null | tr ',' '\n' >> "$out"
else
    echo "  NO RembrandtGPUPeer IN THE REGISTRY" >> "$out"
fi

say "FRAMEBUFFER (RBAMDFramebuffer)"
ioreg -rl -c RBAMDFramebuffer 2>/dev/null | tr ',' '\n' >> "$out" \
    || echo "  NOT PRESENT" >> "$out"

say "APPLE'S ACCELERATOR"
for c in AMDRadeonX6000_AMDNavi21GraphicsAccelerator AMDRadeonX6000_AMDRadeonHWServicesNavi IOFramebuffer IODisplayWrangler; do
    n=$(ioreg -rc "$c" 2>/dev/null | grep -c "+-o" || true)
    echo "  $c: $n instance(s)" >> "$out"
done

# --- who is burning the machine? This is what the stall is FOR. ---
run "TOP (2 samples, by CPU)" top -l 2 -s 1 -o cpu -n 20 -stats pid,command,cpu,th,state,time
run "PROCESSES BY CPU"        ps -Ao pid,ppid,pcpu,state,wchan,comm
run "KERNEL TASK / VM"        vm_stat
run "LOAD + THERMAL"          sysctl -n vm.loadavg machdep.xcpm.cpu_thermal_level hw.cpufrequency 2>/dev/null
run "IS LOGD ALIVE"           pgrep -l logd
run "DISK BUSY"               iostat -c 2
run "NVRAM (ours)"            sh -c 'nvram -p | grep -i rbgpu'
run "BOOT-ARGS"               nvram boot-args

# --- the one that names the hog, if a password is available ---
if [ "$(id -u)" = "0" ]; then
    say "SPINDUMP (10 s) - the whole machine, every thread, with stacks"
    spindump -reveal -timeline 10 1 -file "$out.spindump" >/dev/null 2>&1 \
        && echo "  written to $out.spindump" >> "$out" \
        || echo "  spindump FAILED" >> "$out"
    say "POWERMETRICS (3 samples)"
    powermetrics -n 3 -i 1000 --samplers cpu_power,interrupts 2>/dev/null | head -80 >> "$out"
    say "DMESG TAIL"
    dmesg 2>/dev/null | tail -60 >> "$out"
else
    say "NOT ROOT"
    echo "  Re-run with sudo for the spindump, which is the reading that names" >> "$out"
    echo "  what is holding the machine off its schedule." >> "$out"
fi

echo "$out"
[ -f "$out.spindump" ] && echo "$out.spindump"
