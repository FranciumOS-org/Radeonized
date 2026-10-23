#!/bin/sh
# SUPERSEDED (2on, 2026-10-22). DO NOT USE TO FIX APPS.
#
# The kext now fixes EVERY process on the system by itself: boot with
# -rbgpumtlkfix and rbgpumtlkscope=* (see docs/RULES.md "Boot-args"). That reaches
# what this script never could - Apple platform binaries like Safari, which
# strip DYLD_INSERT_LIBRARIES, and sandboxed App Store apps, which cannot load
# an inserted dylib from a home directory at all (rule 21). It also survives
# app updates, because no app bundle is touched.
#
# All 11 wrappers were removed on 2026-10-22. A leftover wrapper is harmless
# if one reappears: mtlswizzle refuses unless it finds exactly one match of a
# 32-byte prologue pattern, and the kernel detour leaves zero (2om).
#
# Kept only for `verify`, which reads a live process with vmmap, and as the
# reference for how per-process injection worked.
#
# Build a Dock-able wrapper .app that starts a real app with the swizzle fix
# applied (2nr). The fix is a per-process patch of a dyld-shared-cache driver,
# so it cannot live anywhere but in the process - rule 21 forbids the only
# system-wide form (a launchd DYLD_INSERT_LIBRARIES environment), which once
# killed every newly launched app on this machine and needed a reboot.
#
# A wrapper bundle is the persistent form that keeps the blast radius at one
# app: ~/Applications/<Name> (GPU).app execs the real binary with the dylib
# inserted. Dock the wrapper and the app comes up fixed every time.
#
#   tools/rbgpu-app.sh install "IntelliJ IDEA"
#   tools/rbgpu-app.sh install the assistant
#   tools/rbgpu-app.sh install-all          every injectable app in /Applications
#   tools/rbgpu-app.sh verify               which RUNNING apps are really patched
#   tools/rbgpu-app.sh relaunch <AppName>   quit and restart it patched, detached
#   tools/rbgpu-app.sh list
#   tools/rbgpu-app.sh remove "IntelliJ IDEA"
#   tools/rbgpu-app.sh remove-all
#
# Env at install time is baked into the wrapper:
#   RBGPU_SWIZZLE=fix   (default) LINEAR for non-render-targets, and render
#                       targets kept TILED with the pipe-bank XOR removed
#   RBGPU_SWIZZLE=linear   the blanket 2np behaviour; fixes Electron, crashes
#                          JetBrains - kept only as a fallback
#
# What this CANNOT do, and why there is no global form (2ns):
#
#  * Apple platform binaries (Safari, Finder, Mail, Terminal, WindowServer)
#    strip DYLD_INSERT_LIBRARIES whatever amfi says. Nothing here reaches them.
#  * Editing the real app's Info.plist to carry LSEnvironment does work in
#    principle but invalidates the bundle signature, and LaunchServices then
#    refuses the app outright with error -54 - measured on IntelliJ, restored
#    from logs/plist-backups/. It would also be undone by every app update,
#    where a wrapper survives one.
#  * A launchd environment is the only true system-wide form and rule 21
#    forbids it: it is not per app, dyld KILLS any process that cannot load
#    the inserted dylib, and the one attempt took the machine down.
#
# A wrapper per app is therefore the persistent form. Install once, then keep
# only the wrapper in the Dock - it carries the real app's icon.
set -e
root=$(cd "$(dirname "$0")/.." && pwd)
dylib="$root/build/out/mtlswizzle.dylib"
policy="${RBGPU_SWIZZLE:-fix}"
dest="$HOME/Applications"

# pgrep is NOT usable here: BSD pgrep never matches the invoking process or
# its ancestors, so a script run from inside the assistant Code cannot see the the assistant
# app at all. That is what made 2no's first attempt at the assistant "exit cleanly
# with no crash report" - running() said no, the quit-and-wait was skipped,
# and the second copy forwarded into the single-instance lock and died. ps has
# no such exclusion.
procs_under() {
    ps -axo pid=,command= | awk -v pre="$1/" '
        { pid = $1; sub(/^[ \t]*[0-9]+[ \t]+/, ""); if (index($0, pre) == 1) print pid }'
}

real_app() {
    for p in "/Applications/$1.app" "$HOME/Applications/$1.app"; do
        [ -d "$p" ] && { echo "$p"; return 0; }
    done
    return 1
}

case "${1:-}" in
install)
    [ -n "${2:-}" ] || { echo "usage: $0 install <AppName>" >&2; exit 2; }
    [ -f "$dylib" ] || { echo "build it first:  make mtlswizzle" >&2; exit 1; }
    name=$2
    app=$(real_app "$name") || { echo "no such app: $name" >&2; exit 1; }
    case "$app" in "$dest"/*\ \(GPU\).app) echo "that IS a wrapper" >&2; exit 1 ;; esac
    exe="$app/Contents/MacOS/$(/usr/libexec/PlistBuddy -c 'Print :CFBundleExecutable' "$app/Contents/Info.plist")"
    [ -x "$exe" ] || { echo "not executable: $exe" >&2; exit 1; }

    wrap="$dest/$name (GPU).app"
    rm -rf "$wrap"
    mkdir -p "$wrap/Contents/MacOS" "$wrap/Contents/Resources"

    # The real app's icon, so the Dock tile is recognisable.
    icon=$(/usr/libexec/PlistBuddy -c 'Print :CFBundleIconFile' "$app/Contents/Info.plist" 2>/dev/null || true)
    case "$icon" in *.icns) ;; "") ;; *) icon="$icon.icns" ;; esac
    if [ -n "$icon" ] && [ -f "$app/Contents/Resources/$icon" ]; then
        cp "$app/Contents/Resources/$icon" "$wrap/Contents/Resources/icon.icns"
    else
        icon=""
    fi

    cat > "$wrap/Contents/Info.plist" <<PLIST
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
  <key>CFBundleExecutable</key><string>launch</string>
  <key>CFBundleIdentifier</key><string>org.rbgpu.wrap.$(echo "$name" | tr 'A-Z ' 'a-z-')</string>
  <key>CFBundleName</key><string>$name (GPU)</string>
  <key>CFBundlePackageType</key><string>APPL</string>
  <key>CFBundleInfoDictionaryVersion</key><string>6.0</string>
  <key>CFBundleShortVersionString</key><string>1.0</string>
  <key>LSMinimumSystemVersion</key><string>10.15</string>
$( [ -n "$icon" ] && echo '  <key>CFBundleIconFile</key><string>icon.icns</string>' )
</dict></plist>
PLIST

    # `open -a` would go through launchd, which does not pass
    # DYLD_INSERT_LIBRARIES on, so the binary is exec'd directly. An app that
    # is already running holds a single-instance lock and a second copy would
    # just forward and exit, which looks exactly like a crash (2no) - so
    # activate the running one instead and say why.
    cat > "$wrap/Contents/MacOS/launch" <<LAUNCH
#!/bin/sh
APP="$app"
EXE="$exe"
DYLIB="$dylib"
if ps -axo command= | grep -q "^\$APP/"; then
    osascript -e "tell application \"$name\" to activate" >/dev/null 2>&1 || true
    exit 0
fi
[ -f "\$DYLIB" ] || exec "\$EXE" "\$@"
export DYLD_INSERT_LIBRARIES="\$DYLIB"
export RBGPU_SWIZZLE="$policy"
exec "\$EXE" "\$@"
LAUNCH
    chmod +x "$wrap/Contents/MacOS/launch"
    touch "$wrap"
    echo "installed: $wrap"
    echo "  wraps:   $exe"
    echo "  policy:  RBGPU_SWIZZLE=$policy"
    echo "  Quit $name, then open the wrapper (drag it to the Dock to keep it)."
    ;;
install-all)
    # Only apps that can actually take an insert: a Developer ID or ad-hoc
    # signed third-party bundle. Apple platform binaries are skipped by name
    # rather than attempted, because a wrapper for them would silently do
    # nothing and look like a fix.
    [ -f "$dylib" ] || { echo "build it first:  make mtlswizzle" >&2; exit 1; }
    for a in /Applications/*.app; do
        [ -d "$a" ] || continue
        n=$(basename "$a" .app)
        [ -f "$a/Contents/Info.plist" ] || {
            printf 'skipped  %-32s (not a real bundle)\n' "$n"; continue; }
        # An UNSIGNED or ad-hoc app is the easiest thing to inject into, not the
        # hardest - the first version of this skipped them and was wrong. Only
        # two classes are genuinely out of reach: Apple's own platform
        # binaries, which strip DYLD_INSERT_LIBRARIES outright, and App Store
        # apps, which are sandboxed and cannot read a dylib out of a home
        # directory (the failure mode rule 21 was learned from).
        auth=$(codesign -dv --verbose=2 "$a" 2>&1 | grep -m1 '^Authority=' || true)
        case "$auth" in
            *"Software Signing"*)                   skip="Apple platform binary" ;;
            *"Apple Mac OS Application Signing"*)   skip="App Store, sandboxed" ;;
            *)                                      skip="" ;;
        esac
        if [ -n "$skip" ]; then printf 'skipped  %-32s (%s)\n' "$n" "$skip"; continue; fi
        "$0" install "$n" >/dev/null 2>&1 && printf 'wrapped  %s\n' "$n" \
            || printf 'FAILED   %s\n' "$n"
    done
    echo
    echo "Keep only the (GPU) wrappers in your Dock."
    ;;
remove-all)
    n=0
    for w in "$dest"/*\ \(GPU\).app; do
        [ -d "$w" ] || continue
        rm -rf "$w"; n=$((n+1))
    done
    echo "removed $n wrapper(s)"
    ;;
verify)
    # Rule 20: a wrapper existing is not evidence that anything was patched.
    # Ask the running process itself, via vmmap, which is the only thing that
    # revealed the inert Lilu patches of 2nn.
    acc=$(ioreg -rc IOAccelerator -k IOClass 2>/dev/null | grep -c IOAccelerator || true)
    [ "${acc:-0}" -gt 0 ] || echo "WARNING: no IOAccelerator in the registry - this boot looks UNARMED."
    printf '%-26s %-9s %s\n' APP STATE DETAIL
    for w in "$dest"/*\ \(GPU\).app; do
        [ -d "$w" ] || continue
        app=$(sed -n 's/^APP="\(.*\)"$/\1/p' "$w/Contents/MacOS/launch")
        n=$(basename "$w" .app); n=${n% (GPU)}
        pid=$(procs_under "$app/Contents/MacOS" | head -1)
        if [ -z "$pid" ]; then
            printf '%-26s %-9s %s\n' "$n" "-" "not running"
        elif vmmap "$pid" 2>/dev/null | grep -q mtlswizzle.dylib; then
            printf '%-26s %-9s %s\n' "$n" "PATCHED" "pid $pid"
        else
            printf '%-26s %-9s %s\n' "$n" "UNPATCHED" "pid $pid - quit it and open the (GPU) wrapper"
        fi
    done
    ;;
relaunch)
    [ -n "${2:-}" ] || { echo "usage: $0 relaunch <AppName>" >&2; exit 2; }
    # Detached on purpose. Relaunching the app this script is RUNNING INSIDE
    # (the assistant, when the assistant Code drives it) would otherwise kill the script
    # halfway and leave no app at all, so double-fork into a new session first.
    python3 - "$root" "$2" <<'DETACH'
import os, sys, subprocess
root, name = sys.argv[1], sys.argv[2]
if os.fork(): sys.exit(0)
os.setsid()
if os.fork(): sys.exit(0)
env = dict(os.environ)
env.pop("DYLD_INSERT_LIBRARIES", None)
env["RBGPU_INJECT"] = "build/out/mtlswizzle.dylib"
env.setdefault("RBGPU_SWIZZLE", "fix")
log = open(os.path.join(root, "logs", "rbgpu-relaunch.log"), "w")
subprocess.call(["/bin/sh", "tools/mtllinear-app.sh", name],
                cwd=root, env=env, stdout=log, stderr=subprocess.STDOUT)
log.close()
DETACH
    echo "relaunching $2 detached; verdict lands in logs/rbgpu-relaunch.log"
    ;;
list)
    found=0
    for w in "$dest"/*\ \(GPU\).app; do
        [ -d "$w" ] || continue
        found=1
        printf '%s\n' "$w"
        sed -n 's/^export RBGPU_SWIZZLE="\(.*\)"$/    policy: \1/p;s/^EXE="\(.*\)"$/    wraps:  \1/p' \
            "$w/Contents/MacOS/launch" 2>/dev/null
    done
    [ $found -eq 1 ] || echo "no wrappers installed in $dest"
    ;;
remove)
    [ -n "${2:-}" ] || { echo "usage: $0 remove <AppName>" >&2; exit 2; }
    w="$dest/$2 (GPU).app"
    [ -d "$w" ] || { echo "not installed: $w" >&2; exit 1; }
    rm -rf "$w"
    echo "removed: $w"
    ;;
*)
    sed -n '2,30p' "$0" | sed 's/^# \{0,1\}//'
    exit 2
    ;;
esac
