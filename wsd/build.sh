#!/bin/sh
# Build wsd (native arm64, ad-hoc signed, no entitlements).
set -eu
S=$(cd "$(dirname "$0")" && pwd)
mkdir -p "$S/bin"
clang -arch arm64 -fobjc-arc -O2 -Wall -Wno-deprecated-declarations -o "$S/bin/wsd" "$S/wsd.m" \
  -framework Cocoa -framework Metal -framework QuartzCore -framework Carbon -framework AudioToolbox -framework CoreAudio
codesign -s - -f "$S/bin/wsd"
codesign -d --entitlements - "$S/bin/wsd" 2>&1 | tail -2
shasum -a 256 "$S/wsd.m" "$S/wsd_protocol.h" "$S/bin/wsd"
