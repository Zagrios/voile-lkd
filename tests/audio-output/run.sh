#!/bin/sh
# Builds wsd and the witness, then plays silence through wsd, started each time on a private 0700 directory, on three
# output devices: one as slow to start as a Bluetooth headset, one that stops rendering for a while as the Mac changes
# its output device, and one that is ready. WSD selects another wsd than the one built here.
set -eu
W=$(cd "$(dirname "$0")" && pwd)
"$W/../../wsd/build.sh" > /dev/null
mkdir -p "$W/bin"
clang -arch arm64 -std=c11 -O2 -Wall -Wextra -o "$W/bin/wsd_audio_output" "$W/wsd_audio_output.c"
clang -arch arm64 -dynamiclib -O2 -Wall -Wextra -o "$W/bin/slow_device.dylib" "$W/slow_device.c" -framework AudioToolbox
rc=0
play() { # title, start delay in ms, stall in ms
  echo "## $1"
  D=$(mktemp -d "${TMPDIR:-/tmp}/wsd.XXXXXX")
  DYLD_INSERT_LIBRARIES="$W/bin/slow_device.dylib" WSD_TEST_START_DELAY_MS="$2" WSD_TEST_STALL_MS="$3" \
    "${WSD:-$W/../../wsd/bin/wsd}" --dir "$D" --exit-on-disconnect > "$D/wsd.log" 2>&1 &
  SPID=$!
  i=0; while [ ! -S "$D/wsd.sock" ] && [ $i -lt 50 ]; do sleep 0.1; i=$((i+1)); done
  "$W/bin/wsd_audio_output" "$D/wsd.sock" || rc=$?
  kill "$SPID" 2>/dev/null || true
  wait "$SPID" 2>/dev/null || true
  grep -v "audio output stream" "$D/wsd.log" | sed 's/^/wsd log: /'
  rm -rf "$D"
}
play "a device that takes 700 ms to start" 700 0
play "a device that stops rendering for 600 ms" 0 600
play "a device that is ready" 0 0
exit $rc
