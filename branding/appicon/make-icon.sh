#!/bin/bash
# Build AppIcon.icns from branding/appicon/main.swift. Output: build-release/AppIcon.icns
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
OUT="$ROOT/build-release"
mkdir -p "$OUT"
swiftc -O "$ROOT/branding/appicon/main.swift" -o "$OUT/make-appicon"
rm -rf "$OUT/AppIcon.iconset"
"$OUT/make-appicon" "$OUT/AppIcon.iconset"
iconutil -c icns "$OUT/AppIcon.iconset" -o "$OUT/AppIcon.icns"
echo "$OUT/AppIcon.icns"
