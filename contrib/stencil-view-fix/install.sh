#!/bin/bash
# sudo bash install.sh            install (or update) the shim; applies to apps launched afterwards
# sudo bash install.sh mode X     copy | noopt | both | off (reopen the app)
# sudo bash install.sh remove     restore the original impostor.dylib
# bash install.sh log             what the shim logged in the last 15 minutes
set -euo pipefail
export PATH=/usr/bin:/bin:/usr/sbin:/sbin
B=/Library/GPUBundles/AppleIntelTGLGraphicsMTLDriver.bundle; M=$B/Contents/MacOS; HERE=$(cd "$(dirname "$0")" && pwd)
C="/Library/Application Support/StencilViewFix/mode"
[ "${1:-}" = log ] && { log show --last 15m --style compact --predicate 'subsystem == "org.wex.stencilviewfix"' | tail -40; exit 0; }
[ "$(id -u)" = 0 ] || { echo "run with sudo"; exit 1; }
[ -f "$M/AppleIntelTGLGraphicsMTLDriver" ] || { echo "WhateverXe Graphics Metal driver not found in $B"; exit 1; }
case "${1:-}" in
  mode) case "${2:-}" in copy|noopt|both|off) ;; *) echo "mode: copy | noopt | both | off"; exit 1;; esac
        mkdir -p "$(dirname "$C")"; echo "$2" > "$C"; echo "mode $2 (reopen the app)"; exit 0;;
  remove) [ -f "$M/impostor-real.dylib" ] || { echo "not installed"; exit 1; }
        mv -f "$M/impostor-real.dylib" "$M/impostor.dylib"; codesign -f -s - "$B" >/dev/null 2>&1 || true
        echo "removed"; exit 0;;
esac
[ -f "$HERE/impostor.dylib" ] || { echo "build first: bash build.sh"; exit 1; }
[ -f "$M/impostor-real.dylib" ] || mv "$M/impostor.dylib" "$M/impostor-real.dylib"
cp "$HERE/impostor.dylib" "$M/impostor.dylib" && chmod 755 "$M/impostor.dylib"
codesign -f -s - "$B" >/dev/null 2>&1 || echo "warning: codesign of the bundle failed"
mkdir -p "$(dirname "$C")"; [ -f "$C" ] || echo copy > "$C"
echo "installed (mode $(cat "$C")); relaunch the app"
