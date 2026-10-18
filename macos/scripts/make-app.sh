#!/bin/bash
# Builds dist/EntropyBackup.app (universal: Apple silicon and Intel), ad-hoc signed, and a zip of it.
set -euo pipefail
cd "$(dirname "$0")/.."
ARCHS=(--arch arm64 --arch x86_64)
swift build -c release "${ARCHS[@]}"
BIN="$(swift build -c release "${ARCHS[@]}" --show-bin-path)/EntropyBackup"

APP=dist/EntropyBackup.app
rm -rf dist
mkdir -p "$APP/Contents/MacOS" "$APP/Contents/Resources"
cp "$BIN" "$APP/Contents/MacOS/EntropyBackup"
cp Resources/Info.plist "$APP/Contents/Info.plist"
if swift scripts/make-icon.swift dist/AppIcon.iconset && iconutil -c icns dist/AppIcon.iconset -o "$APP/Contents/Resources/AppIcon.icns"; then
    rm -rf dist/AppIcon.iconset
else
    echo "warning: could not render the app icon; continuing without it"
fi
codesign --force --sign - "$APP"
ditto -c -k --keepParent "$APP" dist/EntropyBackup-macOS.zip
echo "Built $APP and dist/EntropyBackup-macOS.zip"
