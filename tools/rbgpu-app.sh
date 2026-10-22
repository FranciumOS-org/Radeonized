#!/bin/sh
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
#   tools/rbgpu-app.sh list
#   tools/rbgpu-app.sh remove "IntelliJ IDEA"
#
# Env at install time is baked into the wrapper:
#   RBGPU_SWIZZLE=fix   (default) LINEAR for non-render-targets, and render
#                       targets kept TILED with the pipe-bank XOR removed
#   RBGPU_SWIZZLE=linear   the blanket 2np behaviour; fixes Electron, crashes
#                          JetBrains - kept only as a fallback
#
# What this CANNOT do: Apple platform binaries (Safari, Finder, Mail,
# WindowServer) strip DYLD_INSERT_LIBRARIES whatever amfi says, so they need
# the kernel-side route instead. See docs/HANDOFF.md.
set -e
root=$(cd "$(dirname "$0")/.." && pwd)
dylib="$root/build/out/mtlswizzle.dylib"
policy="${RBGPU_SWIZZLE:-fix}"
dest="$HOME/Applications"

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
if pgrep -f "^\$APP/" >/dev/null 2>&1; then
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
