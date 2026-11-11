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
# SwiftPM puts resources in a bundle next to the binary; Bundle.module finds it there.
cp -R "$bin_dir/Radeonize_Radeonize.bundle" "$app/Contents/Resources/"

cat > "$app/Contents/Info.plist" <<'PLIST'
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
  <key>CFBundleName</key><string>Radeonize</string>
  <key>CFBundleDisplayName</key><string>Radeonize</string>
  <key>CFBundleIdentifier</key><string>org.franciumos.radeonize</string>
  <key>CFBundleExecutable</key><string>Radeonize</string>
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
