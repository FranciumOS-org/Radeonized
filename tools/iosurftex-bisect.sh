#!/bin/sh
# Why does Claude exit when iosurftex is inserted? Bisect it.
#
#   tools/iosurftex-bisect.sh [AppName]        default: Claude
#
# The control is already known: exec'ing the binary with nothing inserted works
# (2026-10-21). So the dylib is implicated, and the question is which part. Each
# variant launches the app, waits, and records whether it stayed up:
#
#   A  nothing inserted          - the control, repeated here for the record
#   B  noop-insert.dylib         - does the app refuse ANY insertion?
#   C  iosurftex, RBGPU_IOSURFTEX=0  - loaded and interposing, never installs
#   D  iosurftex, INERT=1        - same, by the explicit knob
#   E  iosurftex, full           - the real thing
#
# B failing means insertion itself is refused and iosurftex is not at fault.
# C or D failing means merely loading it is fatal. Only E failing means the
# swizzle is. Per process only - nothing is set in any launchd environment
# (see rule 21 and 2mq).
#
# The app is quit between variants and relaunched normally at the end.
set -e
root=$(cd "$(dirname "$0")/.." && pwd)
name=${1:-Claude}
app="/Applications/$name.app"
[ -d "$app" ] || app="$HOME/Applications/$name.app"
[ -d "$app" ] || { echo "no such app: $name" >&2; exit 1; }
exe="$app/Contents/MacOS/$(/usr/libexec/PlistBuddy -c 'Print :CFBundleExecutable' "$app/Contents/Info.plist")"
mkdir -p "$root/logs"

for f in "$root/build/out/iosurftex.dylib" "$root/build/out/noop-insert.dylib"; do
    [ -f "$f" ] || { echo "missing $f - run: make iosurftex noop" >&2; exit 1; }
done

running() { pgrep -f "^$app/" 2>/dev/null; }

quit_and_wait() {
    running >/dev/null 2>&1 || return 0
    osascript -e "quit app \"$name\"" 2>/dev/null || true
    i=0
    while running >/dev/null 2>&1; do
        i=$((i+1))
        if [ $i -gt 30 ]; then
            pids=$(running || true); [ -n "$pids" ] && kill $pids 2>/dev/null || true
            sleep 2
            pids=$(running || true); [ -n "$pids" ] && kill -9 $pids 2>/dev/null || true
            break
        fi
        sleep 0.5
    done
    sleep 1
}

results=""
run_variant() {                       # label, log-suffix, env assignments...
    label=$1; suffix=$2; shift 2
    log="$root/logs/bisect-$suffix.log"
    : > "$log"
    printf '%-44s ' "$label"
    quit_and_wait
    env "$@" nohup "$exe" >> "$log" 2>&1 &
    sleep 10
    if running >/dev/null 2>&1; then
        echo "STAYED UP"
        results="$results
  $label  STAYED UP"
    else
        echo "EXITED   (log: logs/bisect-$suffix.log)"
        results="$results
  $label  EXITED"
    fi
    quit_and_wait
}

echo "bisecting $name - each variant gets 10 s; the app will open and close repeatedly"
echo ""
run_variant "A  nothing inserted (control)" a \
    RBGPU_UNUSED=1
run_variant "B  noop-insert.dylib" b \
    DYLD_INSERT_LIBRARIES="$root/build/out/noop-insert.dylib" \
    RBGPU_NOOP_LOGFILE="$root/logs/bisect-b.log"
run_variant "C  iosurftex, RBGPU_IOSURFTEX=0" c \
    DYLD_INSERT_LIBRARIES="$root/build/out/iosurftex.dylib" \
    RBGPU_IOSURFTEX=0 RBGPU_IOSURFTEX_LOG=1 \
    RBGPU_IOSURFTEX_LOGFILE="$root/logs/bisect-c.log"
run_variant "D  iosurftex, INERT=1" d \
    DYLD_INSERT_LIBRARIES="$root/build/out/iosurftex.dylib" \
    RBGPU_IOSURFTEX_INERT=1 RBGPU_IOSURFTEX_LOG=1 \
    RBGPU_IOSURFTEX_LOGFILE="$root/logs/bisect-d.log"
run_variant "E  iosurftex, full" e \
    DYLD_INSERT_LIBRARIES="$root/build/out/iosurftex.dylib" \
    RBGPU_IOSURFTEX_LOG=2 \
    RBGPU_IOSURFTEX_LOGFILE="$root/logs/bisect-e.log"

echo ""
echo "summary:$results"
echo ""
echo "relaunching $name normally"
open -a "$name" || true
