#!/bin/sh
# Relaunch a GUI app with amdMtl_HWL_DetermineSwizzleMode forced to return
# ADDR_SW_LINEAR in that process only (2no).
#
# The Lilu user patch can never do this: AMDRadeonX6000MTLDriver lives in the
# dyld shared cache and has no file on disk, so a path-based BinaryModInfo
# patches pages that are never mapped and reports success anyway (2nn).
# tools/mtllinear.c edits the in-memory image instead and works every time.
#
# Per process, never a launchd environment (rule 21).
#
#   tools/mtllinear-app.sh Claude
#   tools/mtllinear-app.sh Safari
#
# An Electron app holds a single-instance lock: a second copy started while the
# first is still shutting down forwards its arguments to the first and exits
# immediately - which looks exactly like a crash, and is what happened on the
# first attempt at Claude (2026-10-21, no crash report, clean exit). So quit the
# app and WAIT for every one of its processes to go before starting it again.
#
# The app must be exec'd directly: `open -a` goes through launchd, which does not
# pass DYLD_INSERT_LIBRARIES on.
#
# Env: RBGPU_MTLLINEAR=0 disables the patch while still injecting, as a control.
set -e
root=$(cd "$(dirname "$0")/.." && pwd)
dylib="$root/build/out/mtllinear.dylib"
[ -f "$dylib" ] || { echo "build it first:  make mtllinear" >&2; exit 1; }
[ $# -ge 1 ] || { echo "usage: tools/mtllinear-app.sh <AppName>   (e.g. Claude)" >&2; exit 2; }

name=$1; shift
app="/Applications/$name.app"
[ -d "$app" ] || app="$HOME/Applications/$name.app"
[ -d "$app" ] || { echo "no such app: $name" >&2; exit 1; }
exe="$app/Contents/MacOS/$(/usr/libexec/PlistBuddy -c 'Print :CFBundleExecutable' "$app/Contents/Info.plist")"
[ -x "$exe" ] || { echo "not executable: $exe" >&2; exit 1; }

log="$root/logs/mtllinear-$(echo "$name" | tr 'A-Z ' 'a-z-').log"
mkdir -p "$root/logs"

# Everything under the bundle, so helpers count too.
running() { pgrep -f "^$app/" 2>/dev/null; }

if running >/dev/null 2>&1; then
    echo "quitting $name ..."
    osascript -e "quit app \"$name\"" 2>/dev/null || true
    i=0
    while running >/dev/null 2>&1; do
        i=$((i + 1))
        if [ $i -gt 40 ]; then          # 20 s of grace, then insist
            echo "still running after 20s, sending TERM"
            pids=$(running || true)
            [ -n "$pids" ] && kill $pids 2>/dev/null || true
            sleep 2
            pids=$(running || true)
            [ -n "$pids" ] && kill -9 $pids 2>/dev/null || true
            break
        fi
        sleep 0.5
    done
fi
sleep 1                                  # let the single-instance lock clear
if running >/dev/null 2>&1; then
    echo "ERROR: $name is still running; not starting a second copy" >&2
    exit 1
fi

echo "starting $name under mtllinear"
echo "  log: $log"
: > "$log"
DYLD_INSERT_LIBRARIES="$dylib" \
nohup "$exe" "$@" >> "$log" 2>&1 &
disown 2>/dev/null || true
sleep 3
if running >/dev/null 2>&1; then
    # Rule 20: a launcher reporting success is not evidence the patch applied.
    # The dylib re-reads the bytes after writing them and says so; print that
    # verdict here rather than leaving it to be assumed.
    if grep -q "mtllinear: PATCHED" "$log"; then
        echo "up, and the patch APPLIED:"
        grep "mtllinear:" "$log" | head -3 | sed 's/^/  /'
    else
        echo "up, but the patch did NOT apply - the dylib said:"
        grep "mtllinear:" "$log" | head -5 | sed 's/^/  /'
        echo "  (no mtllinear line at all means the dylib never loaded -"
        echo "   a hardened-runtime platform binary such as Safari strips"
        echo "   DYLD_INSERT_LIBRARIES. Anything measured now is void.)"
    fi
else
    echo "it exited again - the log's last lines:" >&2
    tail -15 "$log" >&2
    exit 1
fi
