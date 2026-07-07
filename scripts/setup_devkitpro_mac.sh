#!/usr/bin/env bash
set -euo pipefail

echo "WarpPoint dev environment setup helper (macOS)"
echo
echo "This script automates installing devkitPro pacman and the Switch toolchain prerequisites."
echo "It requires sudo for system installer steps."

if ! command -v xcode-select >/dev/null 2>&1; then
  echo "Installing Xcode Command Line Tools..."
  xcode-select --install || true
else
  echo "Xcode Command Line Tools already installed."
fi

TMPDIR="$(mktemp -d)"
PKG="$TMPDIR/devkitpro-pacman-installer.pkg"
echo "Downloading devkitPro pacman installer..."
curl -sL https://github.com/devkitPro/pacman/releases/latest/download/devkitpro-pacman-installer.pkg -o "$PKG"

echo "Running installer (requires sudo)..."
sudo installer -pkg "$PKG" -target /

echo "Installing Switch dev group (dkp-pacman)..."
sudo dkp-pacman --noconfirm -S switch-dev || {
  echo "dkp-pacman failed. Try running: sudo dkp-pacman -S switch-dev"
  exit 1
}

echo "Set the following in your shell rc (~/.zshrc or ~/.bash_profile):"
echo "export DEVKITPRO=/opt/devkitpro"
echo "export PATH=\"\$DEVKITPRO/tools/bin:\$PATH\""
echo
echo "After editing your rc file, run: source ~/.zshrc"
echo "You can verify installation with: dkp-pacman -Ql switch-dev"
echo
echo "Note: The installer may require you to approve in System Preferences for unsigned packages."
echo "For full instructions visit: https://devkitpro.org/wiki/Getting_Started"

echo "Cleanup..."
rm -rf "$TMPDIR"

echo "Done."

