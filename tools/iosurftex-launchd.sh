#!/bin/sh
# Put the IOSurface interposer into the GUI launch environment, so apps started
# the NORMAL way (Dock, Spotlight, Finder) load it.
#
#   tools/iosurftex-launchd.sh on     then launch the app from the Dock
#   tools/iosurftex-launchd.sh off    then quit and relaunch it to go back
#   tools/iosurftex-launchd.sh status
#
# Why this rather than exec'ing the binary: `open -a` and the Dock go through
# launchd, which does not pass a DYLD_INSERT_LIBRARIES set in a shell on to the
# app. Exec'ing Claude's binary directly made it exit cleanly after one second,
# twice, with the dylib loaded and installed and no crash report - so direct
# exec is itself suspect, and this route avoids it.
#
# Scope and reversal: this affects GUI processes started AFTER `on`, in this
# login session only. Already-running processes are untouched, nothing is
# written to disk, and it does not survive a reboot. `off` removes it; apps
# already running keep it until they are quit.
set -e
root=$(cd "$(dirname "$0")/.." && pwd)
dylib="$root/build/out/iosurftex.dylib"
log="$root/logs/iosurftex-launchd.log"

case "${1:-}" in
on)
    [ -f "$dylib" ] || { echo "build it first:  make iosurftex" >&2; exit 1; }
    mkdir -p "$root/logs"
    launchctl setenv DYLD_INSERT_LIBRARIES "$dylib"
    launchctl setenv RBGPU_IOSURFTEX_LOG "${RBGPU_IOSURFTEX_LOG:-2}"
    launchctl setenv RBGPU_IOSURFTEX_LOGFILE "$log"
    [ "${RBGPU_IOSURFTEX_ANYUSAGE:-0}" = 0 ] || launchctl setenv RBGPU_IOSURFTEX_ANYUSAGE 1
    echo "on. Now QUIT the app and launch it again from the Dock."
    echo "  log: $log"
    echo "  turn it off with: tools/iosurftex-launchd.sh off"
    ;;
off)
    launchctl unsetenv DYLD_INSERT_LIBRARIES
    launchctl unsetenv RBGPU_IOSURFTEX_LOG
    launchctl unsetenv RBGPU_IOSURFTEX_LOGFILE
    launchctl unsetenv RBGPU_IOSURFTEX_ANYUSAGE
    echo "off. Apps already running keep it until you quit them."
    ;;
status)
    v=$(launchctl getenv DYLD_INSERT_LIBRARIES || true)
    [ -n "$v" ] && echo "DYLD_INSERT_LIBRARIES=$v" || echo "not set"
    f=$(launchctl getenv RBGPU_IOSURFTEX_LOGFILE || true)
    [ -n "$f" ] && echo "logfile=$f" || true
    ;;
*)
    sed -n '2,12p' "$0" | sed 's/^# \{0,1\}//' >&2; exit 2 ;;
esac
