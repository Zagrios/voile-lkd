#!/bin/sh
# Builds wsd and the witness, starts wsd on a private 0700 directory, records from the default microphone, stops wsd.
# The first run may ask for microphone access for the app that runs this script.
set -eu
W=$(cd "$(dirname "$0")" && pwd)
"$W/../../wsd/build.sh" > /dev/null
mkdir -p "$W/bin"
clang -arch arm64 -std=c11 -O2 -Wall -Wextra -o "$W/bin/wsd_audio_input" "$W/wsd_audio_input.c"
D=$(mktemp -d "${TMPDIR:-/tmp}/wsd.XXXXXX")
"$W/../../wsd/bin/wsd" --dir "$D" --exit-on-disconnect > "$D/wsd.log" 2>&1 &
SPID=$!
i=0; while [ ! -S "$D/wsd.sock" ] && [ $i -lt 50 ]; do sleep 0.1; i=$((i+1)); done
rc=0
"$W/bin/wsd_audio_input" "$D/wsd.sock" || rc=$?
kill "$SPID" 2>/dev/null || true
wait "$SPID" 2>/dev/null || true
echo "## wsd log"; cat "$D/wsd.log"
rm -rf "$D"
exit $rc
