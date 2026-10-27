#!/usr/bin/env bash
# Baut mkvst0_4 (VST3, Audio Unit und Standalone-App) auf dem Mac.
# Aufruf im Projektordner:  ./build.sh
set -e
cd "$(dirname "$0")"

command -v cmake >/dev/null || { echo "CMake fehlt. Installieren mit: brew install cmake"; exit 1; }
command -v git   >/dev/null || { echo "Git fehlt. Installieren mit: xcode-select --install"; exit 1; }

if [ ! -d JUCE ]; then
  echo "Lade JUCE herunter ..."
  git clone --depth 1 https://github.com/juce-framework/JUCE.git
fi

# Mit vollem Xcode den Xcode-Generator nehmen, sonst den Standard (nur Kommandozeilen-Werkzeuge)
GEN=""
if xcodebuild -version >/dev/null 2>&1; then GEN="-G Xcode"; fi

# Universal-Build (Apple Silicon + Intel), damit die App auch auf anderen Macs läuft
cmake -B build $GEN -DCMAKE_OSX_ARCHITECTURES="arm64;x86_64" -DCMAKE_OSX_DEPLOYMENT_TARGET=11.0
cmake --build build --config Release

echo
echo "Fertig. Installiert nach:"
echo "  ~/Library/Audio/Plug-Ins/VST3/mkvst0_4.vst3"
echo "  ~/Library/Audio/Plug-Ins/Components/mkvst0_4.component"
echo "Standalone-App:"
echo "  $(pwd)/build/Mkvst04_artefacts/Release/Standalone/mkvst0_4.app"
