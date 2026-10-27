#!/usr/bin/env bash
# Packt VST3 und Standalone-App fuer Linux: .deb (Debian/Ubuntu/Mint) und .tar.gz (alle anderen).
# Aufruf:  installer/linux/make_deb.sh <Artefakt-Ordner> <Version> <Ausgabe-Ordner>
set -euo pipefail

ART="$1"; VERSION="$2"; OUT="$3"
NAME=mkvst0_4
PKG=mkvst
mkdir -p "$OUT"
OUT="$(cd "$OUT" && pwd)"

# --- .deb ---
ROOT="$(mktemp -d)/${PKG}_${VERSION}_amd64"
mkdir -p "$ROOT/DEBIAN" "$ROOT/usr/lib/vst3" "$ROOT/usr/bin" "$ROOT/usr/share/applications"
cp -R "$ART/VST3/$NAME.vst3" "$ROOT/usr/lib/vst3/"
install -m 755 "$ART/Standalone/$NAME" "$ROOT/usr/bin/$NAME"
cat > "$ROOT/usr/share/applications/$NAME.desktop" <<DESK
[Desktop Entry]
Type=Application
Name=$NAME
Comment=Synthesizer
Exec=$NAME
Terminal=false
Categories=AudioVideo;Audio;Music;
DESK
cat > "$ROOT/DEBIAN/control" <<CTRL
Package: $PKG
Version: $VERSION
Architecture: amd64
Maintainer: Sarin <noreply@example.com>
Section: sound
Priority: optional
Depends: libasound2 | libasound2t64, libfreetype6, libfontconfig1, libx11-6, libxext6, libxrandr2, libxinerama1, libxcursor1
Description: $NAME Synthesizer (VST3 und Standalone)
CTRL
dpkg-deb --build --root-owner-group "$ROOT" "$OUT/$NAME-$VERSION-Linux-amd64.deb"

# --- .tar.gz mit Installationsskript (ins Benutzerverzeichnis, ohne root) ---
TAR="$(mktemp -d)/$NAME-$VERSION-Linux"
mkdir -p "$TAR"
cp -R "$ART/VST3/$NAME.vst3" "$TAR/"
install -m 755 "$ART/Standalone/$NAME" "$TAR/$NAME"
cat > "$TAR/install.sh" <<'INST'
#!/usr/bin/env bash
# Installiert mkvst0_4 fuer den aktuellen Benutzer: VST3 nach ~/.vst3, App nach ~/.local/bin
set -e
cd "$(dirname "$0")"
mkdir -p ~/.vst3 ~/.local/bin
rm -rf ~/.vst3/mkvst0_4.vst3
cp -R mkvst0_4.vst3 ~/.vst3/
install -m 755 mkvst0_4 ~/.local/bin/mkvst0_4
echo "Installiert: ~/.vst3/mkvst0_4.vst3 und ~/.local/bin/mkvst0_4"
INST
chmod +x "$TAR/install.sh"
tar -C "$(dirname "$TAR")" -czf "$OUT/$NAME-$VERSION-Linux-x86_64.tar.gz" "$(basename "$TAR")"

echo "Fertig: $OUT/$NAME-$VERSION-Linux-amd64.deb, $OUT/$NAME-$VERSION-Linux-x86_64.tar.gz"
