#!/bin/bash
# Package a release: WarpPoint.app in a .dmg (drag to Applications), plus the Switch
# app. Output: build-release/dist/
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
VERSION=$(/usr/libexec/PlistBuddy -c "Print CFBundleShortVersionString" "$ROOT/app/mac/Support/Info.plist")
"$ROOT/app/mac/build.sh"
(cd "$ROOT/switch/agent" && make -j8 >/dev/null)

DIST="$ROOT/build-release/dist"
STAGE="$ROOT/build-release/dmg"
rm -rf "$DIST" "$STAGE"
mkdir -p "$DIST" "$STAGE"
cp -R "$ROOT/build-release/WarpPoint.app" "$STAGE/"
ln -s /Applications "$STAGE/Applications"
hdiutil create -volname "WarpPoint $VERSION" -srcfolder "$STAGE" -ov -format UDZO \
  "$DIST/WarpPoint-$VERSION.dmg" >/dev/null
cp "$ROOT/switch/agent/warppoint.nro" "$DIST/warppoint.nro"
(cd "$DIST" && shasum -a 256 * > SHA256SUMS.txt)
ls -la "$DIST"
