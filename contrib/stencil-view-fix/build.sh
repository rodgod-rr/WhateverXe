#!/bin/bash
# Build on macOS (Command Line Tools). Links against the installed impostor.dylib of WhateverXe Graphics 0.4.0,
# re-exported as @loader_path/impostor-real.dylib. Output: ./impostor.dylib
set -euo pipefail
cd "$(dirname "$0")"
M=/Library/GPUBundles/AppleIntelTGLGraphicsMTLDriver.bundle/Contents/MacOS
REAL=$M/impostor-real.dylib; [ -f "$REAL" ] || REAL=$M/impostor.dylib
cp "$REAL" ./impostor-real.dylib
install_name_tool -id @loader_path/impostor-real.dylib impostor-real.dylib 2>/dev/null
clang++ -std=c++17 -fobjc-arc -O2 -Wall -arch x86_64 -dynamiclib -mmacosx-version-min=13.0 \
  -install_name @rpath/impostor.dylib -framework Foundation -framework Metal \
  -Wl,-reexport_library,impostor-real.dylib stencilfix.mm -o impostor.dylib
rm -f impostor-real.dylib
codesign -f -s - impostor.dylib
echo "built ./impostor.dylib"
