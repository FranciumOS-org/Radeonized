#!/bin/sh
# Builds Radeonize.app from the SwiftPM package. Ad-hoc signed; no notarization
# (the driver targets Intel Macs, and this app is for personal use).
#
#   ./scripts/make-app.sh          -> build/Radeonize.app
set -eu

root="$(cd "$(dirname "$0")/.." && pwd)"
cd "$root"

swift build -c release
bin_dir="$(swift build -c release --show-bin-path)"
app="$root/build/Radeonize.app"

rm -rf "$app"
mkdir -p "$app/Contents/MacOS" "$app/Contents/Resources"
cp "$bin_dir/Radeonize" "$app/Contents/MacOS/Radeonize"
# ISSUE #6: ship the helper BOTH ways.
#
# The flat copy is the one the app actually looks for first. It is a plain file
# in Contents/Resources, so it is sealed by the signature like any other
# resource and there is no nested bundle to load, seal separately, or lose on
# a quarantined download - which is the class of failure that crashed v0.1.0
# in `Bundle.module` on a reporter's Sequoia machine, before the app's own
# "helper is missing" path could ever run.
cp "$root/Sources/Radeonize/Resources/radeonize-helper.sh" \
   "$app/Contents/Resources/radeonize-helper.sh"
# The SwiftPM bundle stays as the fallback, for `swift run` and for anything
# that still expects the old layout.
cp -R "$bin_dir/Radeonize_Radeonize.bundle" "$app/Contents/Resources/"
cp "$root/Resources/AppIcon.icns" "$app/Contents/Resources/AppIcon.icns"

# ISSUE #6, the packaging half: REFUSE TO SHIP AN APP WITH NO HELPER.
# v0.1.0 shipped, ran, and died in `Bundle.module` on a machine where the
# script could not be found. The app can now report that instead of crashing,
# but a build that cannot find its own helper is a packaging failure and
# belongs here, loudly, not in a user's crash report. Checks the flat copy the
# app looks for FIRST, and that it is non-empty and a shell script.
helper="$app/Contents/Resources/radeonize-helper.sh"
if [ ! -s "$helper" ]; then
    echo "make-app.sh: REFUSING - $helper is missing or empty." >&2
    echo "  The app would build, launch, and then fail at the first" >&2
    echo "  privileged step. Check Package.swift's resources and the" >&2
    echo "  copy above." >&2
    exit 1
fi
if ! head -n 1 "$helper" | grep -q '^#!'; then
    echo "make-app.sh: REFUSING - $helper has no #! line, so it is not the" >&2
    echo "  helper script." >&2
    exit 1
fi

cat > "$app/Contents/Info.plist" <<'PLIST'
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
  <key>CFBundleName</key><string>Radeonize</string>
  <key>CFBundleDisplayName</key><string>Radeonize</string>
  <key>CFBundleIdentifier</key><string>org.franciumos.radeonize</string>
  <key>CFBundleExecutable</key><string>Radeonize</string>
  <key>CFBundleIconFile</key><string>AppIcon</string>
  <key>CFBundleShortVersionString</key><string>0.1.0</string>
  <key>CFBundleVersion</key><string>1</string>
  <key>CFBundlePackageType</key><string>APPL</string>
  <key>LSMinimumSystemVersion</key><string>13.0</string>
  <key>LSApplicationCategoryType</key><string>public.app-category.utilities</string>
  <key>NSHighResolutionCapable</key><true/>
</dict></plist>
PLIST

codesign --force --sign - "$app"
echo "built $app"
