#!/bin/bash
# Build WarpPoint.app: the C++ engine (CMake, Release) and the Swift menu bar app around
# it. Output: build-release/WarpPoint.app
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
BUILD="$ROOT/build-release"
MAC="$ROOT/app/mac"

cmake -S "$ROOT" -B "$BUILD" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_OSX_DEPLOYMENT_TARGET=14.0 >/dev/null
cmake --build "$BUILD" --target warp_host_core warp

APP="$BUILD/WarpPoint.app"
rm -rf "$APP"
mkdir -p "$APP/Contents/MacOS" "$APP/Contents/Resources"
cp "$MAC/Support/Info.plist" "$APP/Contents/Info.plist"
"$ROOT/branding/appicon/make-icon.sh" >/dev/null
cp "$BUILD/AppIcon.icns" "$APP/Contents/Resources/AppIcon.icns"

swiftc -O -swift-version 5 -parse-as-library -target arm64-apple-macos14.0 \
  -import-objc-header "$MAC/Support/bridging.h" -I "$ROOT/host/capi" \
  "$MAC"/Sources/*.swift \
  -L "$BUILD/host" -L "$BUILD/common" -L "$BUILD/net" \
  -lwarp_host_core -lwarp_common -lwarp_monocypher -lwarp_net -lcurl -lc++ -framework SystemConfiguration \
  -o "$APP/Contents/MacOS/WarpPoint"

# The matching Switch files, so the app can update the Switch from the overlay
# (host/switch_package.hpp). Skipped when the Switch parts haven't been built.
SW="$ROOT/switch"
if [ -f "$SW/sysmodule/warppoint-sys.nsp" ] && [ -f "$SW/overlay/warppoint.ovl" ] && [ -f "$SW/agent/warppoint.nro" ]; then
  PKG="$APP/Contents/Resources/SwitchPackage"
  mkdir -p "$PKG"
  cp "$SW/sysmodule/warppoint-sys.nsp" "$PKG/exefs.nsp"
  cp "$SW/overlay/warppoint.ovl" "$PKG/warppoint.ovl"
  cp "$SW/agent/warppoint.nro" "$PKG/warppoint.nro"
  SWV=$(sed -n 's/.*kVersion = "\([0-9.]*\)".*/\1/p' "$SW/sysmodule/source/main.cpp" | head -1)
  sha() { shasum -a 256 "$PKG/$1" | cut -d' ' -f1; }
  printf '{"version": "%s", "files": {"exefs.nsp": "%s", "warppoint.ovl": "%s", "warppoint.nro": "%s"}}\n' \
    "$SWV" "$(sha exefs.nsp)" "$(sha warppoint.ovl)" "$(sha warppoint.nro)" > "$PKG/package.json"
fi

# Ad-hoc signature: enough to run on this Mac. Real signing comes with packaging.
codesign --force --sign - "$APP" >/dev/null
echo "Built $APP"
