#!/bin/sh
# Start wsd on a private 0700 directory, run a client against it, stop wsd. All under timeouts.
# Usage: run-wsd-client.sh <client-binary> [client args...]
# The socket path replaces an argument spelled @SOCK@; without one it is passed as the first argument.
# WSD_ARGS, if set, holds more arguments for wsd (split on spaces), e.g. WSD_ARGS="--render-percent 50".
set -u
W=$(cd "$(dirname "$0")" && pwd)
EXP=$(cd "$W/../.." && pwd)
TO="$EXP/fexdarwin/scripts/run-with-timeout.sh"
D=$(mktemp -d "${TMPDIR:-/tmp}/wsd.XXXXXX")
echo "dir=$D mode=$(stat -f %Lp "$D") socket_len=$(printf %s "$D/wsd.sock" | wc -c | tr -d ' ')"
"$TO" 90 "$EXP/wsd/bin/wsd" --dir "$D" --exit-on-disconnect ${WSD_ARGS:-} > "$D/wsd.log" 2>&1 &
SPID=$!
i=0; while [ ! -S "$D/wsd.sock" ] && [ $i -lt 50 ]; do sleep 0.1; i=$((i+1)); done
echo "socket mode=$(stat -f %Lp "$D/wsd.sock")"
CLIENT="$1"; shift
case " $* " in
  *" @SOCK@ "*) set -- $(printf '%s\n' "$@" | sed "s#^@SOCK@\$#$D/wsd.sock#") ;;
  *) set -- "$D/wsd.sock" "$@" ;;
esac
"$TO" 80 "$CLIENT" "$@"
RC=$?
i=0; while kill -0 "$SPID" 2>/dev/null && [ $i -lt 30 ]; do sleep 0.1; i=$((i+1)); done
kill "$SPID" 2>/dev/null; wait "$SPID" 2>/dev/null
echo "## wsd log"; cat "$D/wsd.log"
rm -rf "$D"
echo "client_rc=$RC"
exit $RC
