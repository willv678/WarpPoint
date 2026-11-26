#!/bin/bash
# Build a WarpPoint release into build-release/dist/:
#   WarpPoint-<version>.dmg          the Mac app (drag to Applications)
#   WarpPoint-Switch-<version>.zip   unzip onto the SD card root:
#       atmosphere/contents/4200000000005750/   background module (starts at boot)
#       switch/.overlays/warppoint.ovl          Tesla overlay
#       switch/warppoint.nro                    the app (optional, instead of the module)
#   SHA256SUMS.txt
set -euo pipefail
ROOT="$(cd "$(dirname "$0")" && pwd)"
VERSION=$(/usr/libexec/PlistBuddy -c "Print CFBundleShortVersionString" "$ROOT/app/mac/Support/Info.plist")
OUT="$ROOT/build-release"

for part in agent sysmodule overlay; do
  (cd "$ROOT/switch/$part" && make -j8 >/dev/null)
done
"$ROOT/app/mac/build.sh"  # after the Switch parts: the app carries them (SwitchPackage)
[ -f "$OUT/WarpPoint.app/Contents/Resources/SwitchPackage/package.json" ] || { echo "no SwitchPackage in the app"; exit 1; }

DIST="$OUT/dist"
rm -rf "$DIST" "$OUT/dmg" "$OUT/sd"
mkdir -p "$DIST" "$OUT/dmg"

# Mac
cp -R "$OUT/WarpPoint.app" "$OUT/dmg/"
ln -s /Applications "$OUT/dmg/Applications"
hdiutil create -volname "WarpPoint $VERSION" -srcfolder "$OUT/dmg" -ov -format UDZO \
  "$DIST/WarpPoint-$VERSION.dmg" >/dev/null

# Switch: the SD card layout
MODULE="$OUT/sd/atmosphere/contents/4200000000005750"
mkdir -p "$MODULE/flags" "$OUT/sd/switch/.overlays"
cp "$ROOT/switch/sysmodule/warppoint-sys.nsp" "$MODULE/exefs.nsp"
cp "$ROOT/switch/sysmodule/toolbox.json" "$MODULE/toolbox.json"
touch "$MODULE/flags/boot2.flag"
cp "$ROOT/switch/overlay/warppoint.ovl" "$OUT/sd/switch/.overlays/warppoint.ovl"
cp "$ROOT/switch/agent/warppoint.nro" "$OUT/sd/switch/warppoint.nro"
(cd "$OUT/sd" && COPYFILE_DISABLE=1 zip -qr -X "$DIST/WarpPoint-Switch-$VERSION.zip" atmosphere switch)

(cd "$DIST" && shasum -a 256 * > SHA256SUMS.txt)
ls -la "$DIST"
unzip -l "$DIST/WarpPoint-Switch-$VERSION.zip"
