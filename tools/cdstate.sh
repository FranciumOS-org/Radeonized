#!/bin/sh
# cdstate.sh (2q1) - read mpPriv->accelerator as WINDOWSERVER sees it.
#
# This is the one number every remaining question about software compositing
# turns on, and a client process CANNOT compute it: SkyLight's client-side
# SLSAcceleratorForDisplayNumber is a different code path and answers 5127
# while WindowServer's CDDisplay::accelerator() answers 0. That mismatch is
# what made 2px wrong.
#
# CoreDisplay registers an os_state handler in CGXMappedDisplayStart,
# UNCONDITIONALLY (verified: the call to os_state_add_handler at
# CGXMappedDisplayStart+3698 has no guard), whose block runs
# DumpStateForSysDiagnose() -> CreateCoreDisplayStateDictWithOptions(0x3e53b)
# -> CreateMPPrivDict, the dict whose CFString keys named `accelerator` in the
# first place (2py). os_state handlers are invoked by sysdiagnose and the
# payload is written to the unified log as a state event, so the ARCHIVE IS
# NOT NEEDED - this script reads the log.
#
#   1. in the boot you care about:
#        sudo sysdiagnose -u -b -n -f ~/rbgpu-sd
#
#      DO NOT add -R -G -D. One run was spent learning why: sysdiagnose invokes
#      os_state handlers as part of collecting the LOG ARCHIVE, and `-R` means
#      "do not collect log archive", so the handler was never called and the
#      27 MB archive contained no CoreDisplay state at all. The flags were an
#      optimisation that removed the one thing being measured.
#
#      `-f` also refuses a directory that does not exist ("Specified invalid
#      base directory"), so this script creates it. NOT /tmp: cleared on
#      reboot, and a run was lost that way too.
#   2. then:                         tools/cdstate.sh
#
# RULE 28: every line is attributed to a boot by timestamp AND pid, because
# `log show --last boot` silently changes meaning across a reboot.
set -u
cd "$(dirname "$0")/.." || exit 1
out="logs/$(date +%Y%m%d-%H%M%S)-cdstate"
mkdir -p "$out" || exit 1
# sysdiagnose -f refuses a directory that does not exist, and /tmp does not
# survive a reboot, so make a durable one up front.
mkdir -p "$HOME/rbgpu-sd" 2>/dev/null

echo "== this boot"
printf 'booted            : %s\n' "$(sysctl -n kern.boottime | sed 's/.*} //')"
printf 'rbgpu-boot        : %s\n' "$(nvram rbgpu-boot 2>/dev/null | awk '{print $2}')"
printf 'IOAccelerator svcs: %s   <- 0 means UNACCELERATED; any 0 below is then meaningless\n' \
    "$(ioreg -rw0 -d1 -c IOAccelerator 2>/dev/null | grep -c '^+-o')"
printf 'WindowServer pid  : %s\n' \
    "$(ps -axo pid=,comm= | awk '/WindowServer$/{print $1; exit}')"

echo
echo "== CoreDisplay state events from WindowServer this boot"
/usr/bin/log show --last boot \
    --predicate 'eventType == "stateEvent" AND process == "WindowServer"' \
    --style compact > "$out/stateevents.txt" 2>/dev/null
n=$(grep -c . "$out/stateevents.txt" 2>/dev/null)
printf 'lines: %s\n' "${n:-0}"

echo
echo "== sysdiagnose log archives found (the dump lands HERE, not in the live log)"
for a in "$HOME"/rbgpu-sd/sysdiagnose_*/system_logs.logarchive; do
    [ -d "$a" ] || continue
    printf -- '--- %s\n' "$a"
    /usr/bin/log show --archive "$a" \
        --predicate 'eventType == "stateEvent" AND process == "WindowServer"' \
        --style compact >> "$out/stateevents.txt" 2>/dev/null
    printf '    cumulative state-event lines: %s\n' \
        "$(grep -c . "$out/stateevents.txt" 2>/dev/null)"
done

echo
echo "== the fields (MPPriv keys, named by Apple in CreateMPPrivDict)"
found=0
for f in "$out/stateevents.txt" /var/tmp/CoreDisplay_State.plist \
         "$HOME"/rbgpu-sd/*/WindowServer*state* ; do
    [ -f "$f" ] || continue
    if grep -qE '"?(accelerator|mpPriv)"?' "$f" 2>/dev/null; then
        echo "--- $f"
        grep -oE '"(accelerator|service|unit|aliasID|fbIndex|deviceFlags|gpuVendor)" *[:=] *[-0-9a-zA-Zx"]*' \
            "$f" | sort | uniq -c | head -40
        found=1
    fi
done
[ -f /var/tmp/CoreDisplay_State.plist ] && \
    cp /var/tmp/CoreDisplay_State.plist "$out/" 2>/dev/null

if [ "$found" -eq 0 ]; then
    cat <<'MSG'
nothing yet - the handler has not been invoked in this boot.

RULE 14: that is not a result. os_state handlers only run when something asks,
and nothing has. Trigger it, in the boot you care about:

    sudo sysdiagnose -u -b -n -f ~/rbgpu-sd

then re-run this script. The directory already exists - this script made it.
DO NOT add -R: it skips the log-archive collection, which is the step that
invokes os_state handlers, so the dump never happens. That cost one run.
MSG
fi
echo
echo "saved: $out"
