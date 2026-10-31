#!/bin/sh
# wsd --render-percent: one wsd run per case, with and without the option, each checked by wsd_client --render-check
# (HELLO display and scale, a window and SET_FRAME round trip, the window's size in points as macOS lists it) and by
# the "render percent: requested ..., applied ..." line of wsd's log. Opens a short-lived window per case on the main
# display; --fullscreen cases also cover the screen for a moment.
# Usage: run-wsd-render-percent.sh   (after wsd/build.sh and build-wsd-client.sh)
set -u
W=$(cd "$(dirname "$0")" && pwd)
CLIENT="$W/bin/wsd_client"
TMP="${TMPDIR:-/tmp}"
pass=0
fail=0

# run_case <wsd arguments, may be empty> <what wsd's log must say it was asked for> <percent wsd must apply> [client arguments]
run_case() {
  args=$1; requested=$2; applied=$3; shift 3
  out=$(WSD_ARGS="$args" "$W/run-wsd-client.sh" "$CLIENT" --render-check "$applied" "$@" 2>&1)
  rc=$?
  if [ $rc -eq 0 ] && printf '%s\n' "$out" | grep -qF "wsd: render percent: requested $requested, applied $applied;"; then
    pass=$((pass + 1)); echo "PASS  wsd [$args] -> $applied"
    printf '%s\n' "$out" | grep -E "^(render check|hello|window shown|set frame|window on screen|fullscreen|note|wsd: render percent)" |
      sed 's/^/      /'
  else
    fail=$((fail + 1)); echo "FAIL  wsd [$args] -> $applied, requested $requested (client rc=$rc)"
    printf '%s\n' "$out" | sed 's/^/      /'
  fi
}

# run_at <percent> [client arguments]: --render-percent N, which wsd takes as N
run_at() {
  n=$1; shift
  run_case "--render-percent $n" "$n" "$n" "$@"
}

run_case ""                         none 100 --fullscreen
run_at 100
# the slider's stops, and values between them, which no multiple of 25 may stand for (73, 99) or a window of whole
# points may turn into a wrong size (99: the window comes back 2 px over)
run_at 99
run_at 75
run_at 73
run_at 50 --fullscreen
run_at 37
run_at 25
# 1 percent: the 640x480 guard holds the desktop up (on a 1512x982 point screen the scale stays at 0.489)
run_at 1 --fullscreen
# out of range: clamped
run_case "--render-percent 0"          0   1
run_case "--render-percent 500"      500 100
# not made of digits only, or no value (a following option is not the value): ignored. The log says what wsd took
# for the value, which tells a following option left alone from one swallowed as the value.
run_case "--render-percent abc"      abc 100
run_case "--render-percent 50x"      50x 100
run_case "--render-percent -50"      -50 100
run_case "--render-percent"          "(no value)" 100
run_case "--render-percent --exit-on-disconnect" "(no value)" 100

# no wsd of these runs may be left behind (their private directories are $TMP/wsd.XXXXXX)
left=$(pgrep -fl "bin/wsd --dir $TMP/wsd\\." || true)
if [ -n "$left" ]; then
  fail=$((fail + 1)); echo "FAIL  wsd left running:"; echo "$left"
fi
echo "render percent: $pass passed, $fail failed"
[ $fail -eq 0 ]
