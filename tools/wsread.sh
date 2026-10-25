#!/bin/bash
# wsread.sh (2qg) - read the compositor decision's four live values out of
# WindowServer with a one-shot lldb batch, then detach.
#
# WHY THIS AND NOT SOMETHING SAFER: `sample` gives stacks, not values, and
# 2qd/2qf exhausted what stacks can say.  DTrace's pid provider is unavailable -
# csr-active-config is 0xA43 and CSR_ALLOW_UNRESTRICTED_DTRACE (0x20) is clear
# (2qe).  A client process cannot walk the device list at all: the server hook
# at 0x7ff843bb2280 is NULL outside WindowServer and calling through it
# segfaults (2qc).
#
# THE RISK, PLAINLY: lldb STOPS WindowServer while attached, so the desktop
# freezes for the duration.  This script therefore (a) evaluates no expression
# and calls no function in the target - reads only, because calling into a
# stopped WindowServer can deadlock on a lock its own stopped thread holds;
# (b) runs lldb in --batch with an explicit detach; and (c) arms a watchdog
# that SIGKILLs lldb if it outlives the timeout, since the kernel resumes a
# task whose debugger dies.  Expect a freeze of a few seconds.
#
# If it does wedge: the watchdog fires on its own.  Over SSH you can also
# `sudo pkill -9 lldb`.
set -u
cd "$(dirname "$0")/.."
TIMEOUT=${RBGPU_WSREAD_TIMEOUT:-25}

if [ "$(id -u)" -ne 0 ]; then
    echo "NOT ROOT. Re-run as: sudo tools/wsread.sh"
    exit 3
fi
if ! command -v lldb >/dev/null 2>&1; then
    echo "lldb not installed (CommandLineTools provides it: xcode-select -p)."
    exit 3
fi
ws=$(pgrep -x WindowServer)
if [ -z "${ws:-}" ]; then
    echo "no WindowServer process."
    exit 3
fi
out="logs/$(date +%Y%m%d-%H%M%S)-wsread"
mkdir -p "$out"

echo "WindowServer pid : $ws"
echo "timeout          : ${TIMEOUT}s (watchdog will SIGKILL lldb past this)"
echo "the desktop will freeze briefly while lldb is attached."
echo

lldb -p "$ws" --batch \
     -o "script exec(open('tools/wsread.py').read())" \
     -o "detach" \
     > "$out/wsread.txt" 2>&1 &
lldbpid=$!

( sleep "$TIMEOUT"
  if kill -0 "$lldbpid" 2>/dev/null; then
      echo "WATCHDOG: lldb outlived ${TIMEOUT}s - killing it so WindowServer resumes." \
          >> "$out/wsread.txt"
      kill -9 "$lldbpid" 2>/dev/null
  fi ) &
wd=$!

wait "$lldbpid" 2>/dev/null
rc=$?
kill "$wd" 2>/dev/null

cat "$out/wsread.txt"
echo
echo "saved: $out/wsread.txt   (lldb rc=$rc)"
