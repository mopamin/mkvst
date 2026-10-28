#!/usr/bin/env bash
# Packt VST3, Audio Unit und Standalone-App in einen macOS-Installer (.pkg).
# Aufruf:  installer/macos/make_pkg.sh <Artefakt-Ordner> <Version> <Ausgabe-Ordner>
# Beispiel: installer/macos/make_pkg.sh build/Mkvst04_artefacts/Release 0.5.0 dist
set -euo pipefail

ART="$1"; VERSION="$2"; OUT="$3"
NAME=mkvst0_4
ID=com.sarin.mkvst04

ROOT="$(mktemp -d)/root"
mkdir -p "$ROOT/Library/Audio/Plug-Ins/VST3" "$ROOT/Library/Audio/Plug-Ins/Components" "$ROOT/Applications" "$OUT"
cp -R "$ART/VST3/$NAME.vst3"      "$ROOT/Library/Audio/Plug-Ins/VST3/"
cp -R "$ART/AU/$NAME.component"   "$ROOT/Library/Audio/Plug-Ins/Components/"
cp -R "$ART/Standalone/$NAME.app" "$ROOT/Applications/"

# Ohne Signatur-Zertifikat: wenigstens ad-hoc signieren (Pflicht auf Apple Silicon)
for b in "$ROOT"/Library/Audio/Plug-Ins/VST3/*.vst3 "$ROOT"/Library/Audio/Plug-Ins/Components/*.component "$ROOT"/Applications/*.app; do
  codesign --force --deep --sign - "$b"
done

# Bundles nicht "umziehen" lassen, sonst landet ein Update ggf. bei einer alten Kopie statt im Plug-In-Ordner
PLIST="$(dirname "$ROOT")/components.plist"
pkgbuild --analyze --root "$ROOT" "$PLIST"
i=0
while /usr/libexec/PlistBuddy -c "Print :$i" "$PLIST" >/dev/null 2>&1; do
  /usr/libexec/PlistBuddy -c "Delete :$i:BundleIsRelocatable" "$PLIST" 2>/dev/null || true
  /usr/libexec/PlistBuddy -c "Add :$i:BundleIsRelocatable bool false" "$PLIST"
  i=$((i + 1))
done

pkgbuild --root "$ROOT" --component-plist "$PLIST" --identifier "$ID" --version "$VERSION" \
  --install-location / "$OUT/$NAME-$VERSION-macOS.pkg"

echo "Fertig: $OUT/$NAME-$VERSION-macOS.pkg"
