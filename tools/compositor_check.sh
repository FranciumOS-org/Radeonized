#!/bin/sh
# compositor_check.sh (2pz) - WHY is the desktop compositing in software?
#
#   sudo tools/compositor_check.sh      # sudo only for `sample WindowServer`
#
# 2px/2py mapped the whole decision and 2pz measured that the test it named
# does NOT fire: our display's accelerator id is 5127, not 0, and
# SLSDisplayCopyCurrentMetalDevice hands back our 680M. So the gate is
# somewhere else and the live stack is the only thing that can say where.
#
# RULE 14: every section says something, including "not present".
# RULE 26: the preconditions are printed FIRST, because every number below is
# meaningless on a boot with no accelerator (2py measured exactly that).
set -u
cd "$(dirname "$0")/.." || exit 1
out="logs/$(date +%Y%m%d-%H%M%S)-compositor"
mkdir -p "$out" || exit 1
echo "writing to $out"

say() { printf '\n== %s\n' "$1"; }

{
say "preconditions (rule 26)"
# -r prints each match's whole SUBTREE, so grep -c '+-o' counts children as
# well and read 69 where the real answer was 1. -d1 limits the depth.
printf 'IOAccelerator services : %s\n' \
    "$(ioreg -rw0 -d1 -c IOAccelerator 2>/dev/null | grep -c '^+-o')"
# The class name differs between boots, so match the line and print it rather
# than grepping for a state that read 0 while the accelerator was up.
printf 'AMD HWLibs node        : %s\n' \
    "$(ioreg -w0 2>/dev/null | grep -oE 'AMDRadeonX6[0-9]*_AMDRadeonHWLibsX6[0-9]*.*(registered|!registered), *(matched|!matched)' | head -1)"
printf 'plugin loaded          : %s\n' \
    "$(kmutil showloaded --list-only 2>/dev/null | grep -c com.rembrandtgpu.plugin)"
printf 'rbgpu-boot             : %s\n' \
    "$(nvram rbgpu-boot 2>/dev/null | awk '{print $2}')"

say "the accelerator id (2py oracle)"
if [ -x build/out/accelport ]; then build/out/accelport; else
    echo "build/out/accelport missing - run: make accelport"; fi

say "what SkyLight says about THIS display"
if [ -x build/out/slquery ]; then build/out/slquery; else
    echo "build/out/slquery missing - run: make slquery"; fi

say "CoreDisplay state dict (2q1) - the accelerator field from WindowServer"
# CreateCoreDisplayStateDictWithOptions embeds CreateMPPrivDict, so this plist
# carries mpPriv->accelerator as WINDOWSERVER sees it - the one number a client
# process cannot compute. The handler is read-only: it reads state, logs
# "DataDump - DumpState" and writes this path.
# It is gated on the `com.apple.CoreDisplay` preference `enableDataDump`, which
# does not exist by default, so absence here is the gate and not a finding.
rm -f /var/tmp/CoreDisplay_State.plist
notifyutil -p com.apple.CoreDisplay.DumpState >/dev/null 2>&1
sleep 2
if [ -f /var/tmp/CoreDisplay_State.plist ]; then
    cp /var/tmp/CoreDisplay_State.plist "$out/CoreDisplay_State.plist"
    echo "got it; accelerator / service / unit / fbIndex per MPPriv:"
    plutil -p /var/tmp/CoreDisplay_State.plist 2>/dev/null \
      | grep -E '"(accelerator|service|unit|aliasID|fbIndex|deviceFlags)"' | head -40
else
    echo "no /var/tmp/CoreDisplay_State.plist - dump not enabled (expected by"
    echo "default). Enabling it is a persistent preference change + a"
    echo "WindowServer restart; sudo sysdiagnose needs no config change."
fi

say "the frame clock"
if [ -x build/out/dispclk ]; then build/out/dispclk 2>&1 | tail -6; else
    echo "build/out/dispclk missing - run: make dispclk"; fi

say "flip counters (0/0 = no page flip is happening; 2pf/2pr)"
ioreg -l -w0 2>/dev/null \
  | grep -oE '(bufferFlipCount|bufferSwapCount)"=[0-9]*' | sort -u \
  || echo "(no flip counters present)"
printf 'IOAccelDisplayPipeUserClient2 nodes: %s\n' \
    "$(ioreg -rw0 -c IOAccelDisplayPipeUserClient2 2>/dev/null | grep -c '+-o')"

say "the CoreDisplay/SkyLight log for this boot"
# NOTE: `log` is a zsh BUILTIN that shadows /usr/bin/log and answers
# "too many arguments", which looks exactly like an empty log (rule 26).
# Always the absolute path.
wslog=$(/usr/bin/log show --last boot --predicate \
    'process == "WindowServer" AND (subsystem == "com.apple.CoreDisplay" OR subsystem == "com.apple.SkyLight")' \
    --style compact 2>/dev/null)
printf '%s\n' "$wslog" > "$out/ws-log.txt"
printf 'lines: %s\n' "$(printf '%s\n' "$wslog" | wc -l)"
echo '--- "Creating FB N of M": GPU::GPU logs its own framebuffer count ---'
printf '%s\n' "$wslog" | grep "Creating FB" | sed 's/^.*\] //' | sort -u \
    || echo "(none - the GPU got no framebuffers)"
echo '--- distinct message shapes ---'
printf '%s\n' "$wslog" | sed 's/^.*\] //' | sed 's/[0-9]\{2,\}/N/g' \
    | sort | uniq -c | sort -rn | head -12

say "WindowServer CPU burn - software compositing pegs a core"
ws=$(ps -axo pid=,comm= | awk '/WindowServer$/{print $1; exit}')
if [ -z "$ws" ]; then
    echo "WindowServer not found"
else
    printf 'pid %s  elapsed/cpu/%%cpu: %s\n' "$ws" "$(ps -o etime=,time=,%cpu= -p "$ws")"
    a=$(ps -o time= -p "$ws"); /usr/bin/python3 -c 'import time;time.sleep(4)'
    b=$(ps -o time= -p "$ws")
    printf 'cpu time %s -> %s over 4 s of wall clock\n' "$a" "$b"

    say "the live stack - which compositor, and who called it"
    if [ "$(id -u)" -ne 0 ]; then
        echo "NOT ROOT: skipped. Re-run as: sudo tools/compositor_check.sh"
    else
        sample "$ws" 5 -file "$out/ws-sample.txt" >/dev/null 2>&1
        n=$(grep -cE 'CompositeLayersToDestination' "$out/ws-sample.txt" 2>/dev/null)
        printf 'CompositeLayersToDestination frames : %s\n' "${n:-0}"
        printf 'CompositorSW  mentions              : %s\n' \
            "$(grep -c CompositorSW "$out/ws-sample.txt" 2>/dev/null)"
        printf 'CompositorMetal mentions            : %s\n' \
            "$(grep -c CompositorMetal "$out/ws-sample.txt" 2>/dev/null)"
        # The compositor each CALLER gets is the whole point: Metal and SW
        # can both be live in one process, and that discriminates the gate.
        echo "--- who got which compositor ---"
        grep -B1 -E "Compositor(SW|Metal)::CompositeLayersToDestination" \
            "$out/ws-sample.txt" | sed 's/^ *[+!:|-]*  *//' \
            | grep -vE '^--$' | cut -c1-120 | head -30
        echo "(full sample in $out/ws-sample.txt)"
    fi
fi
} 2>&1 | tee "$out/compositor.txt"
echo
echo "saved: $out/compositor.txt"
