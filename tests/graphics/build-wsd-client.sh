#!/bin/sh
set -eu
W=$(cd "$(dirname "$0")" && pwd)
mkdir -p "$W/bin"
clang -arch arm64 -std=c11 -O2 -Wall -Wextra -o "$W/bin/wsd_client" "$W/wsd_client.c" -framework CoreFoundation -framework CoreGraphics
codesign -s - -f "$W/bin/wsd_client"
shasum -a 256 "$W/wsd_client.c" "$W/bin/wsd_client"
