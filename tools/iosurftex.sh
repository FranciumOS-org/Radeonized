#!/bin/sh
# Run a program with the IOSurface texture interposer (2mo/2mp).
#
#   tools/iosurftex.sh /Applications/Foo.app            # a .app bundle
#   tools/iosurftex.sh /path/to/binary [args...]        # a plain binary
#
# RBGPU_IOSURFTEX_LOG=1 in the environment logs each decision to stderr.
# This changes nothing outside the process it launches.
set -e
DYLIB="$(cd "$(dirname "$0")/.." && pwd)/build/out/iosurftex.dylib"
[ -f "$DYLIB" ] || { echo "build it first:  make iosurftex" >&2; exit 1; }
[ $# -ge 1 ] || { sed -n '2,9p' "$0" | sed 's/^# \{0,1\}//' >&2; exit 2; }

target=$1; shift
case "$target" in
*.app|*.app/)
    name=$(/usr/libexec/PlistBuddy -c 'Print :CFBundleExecutable' \
           "${target%/}/Contents/Info.plist" 2>/dev/null) \
      || { echo "no CFBundleExecutable in ${target%/}" >&2; exit 1; }
    target="${target%/}/Contents/MacOS/$name" ;;
esac
[ -x "$target" ] || { echo "not executable: $target" >&2; exit 1; }

# Log to a file as well as the terminal: asking for logs/iosurftex-<app>.log
# after running this script used to find an empty file, because only the
# -app.sh and -launchd.sh variants wrote one.
base=$(basename "$target" | tr 'A-Z ' 'a-z-')
log="$(cd "$(dirname "$0")/.." && pwd)/logs/iosurftex-$base.log"
mkdir -p "$(dirname "$log")"
echo "logging to $log" >&2
exec env DYLD_INSERT_LIBRARIES="$DYLIB" \
         RBGPU_IOSURFTEX_LOGFILE="${RBGPU_IOSURFTEX_LOGFILE:-$log}" \
         "$target" "$@"
