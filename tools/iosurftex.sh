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

exec env DYLD_INSERT_LIBRARIES="$DYLIB" "$target" "$@"
