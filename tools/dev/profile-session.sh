#!/bin/bash
# records where the cpu goes while a windows game runs in obelisk's linux engine, so a change can be
# judged on numbers instead of impressions. nothing in here touches the engine: it watches from the
# outside (top, engine-top, the game's log) and only writes the env file the runner reads at start.
#
# usage: tools/dev/profile-session.sh ID [--store Steam|Epic|GOG] [--profile] [--guest NAME=VALUE]...
#                                        [--host NAME=VALUE]... [--sample SECONDS] [--out DIR]
#   ID                 the game as its log names it: steam app id, epic app name, gog product id
#                      (the log is ~/Library/Logs/Obelisk/<store>/wine-<ID>.log)
#   --profile          put FEXDARWIN_PROFILE=1 into the runtime's engine/fexdarwin.env for this session:
#                      every 10 s each guest process logs its cpu per thread, syscalls per thread and the
#                      costliest syscalls into the game's log (docs/V1.md, "Debugging"). costs two clock
#                      reads per syscall. the file is removed when this script ends and the script refuses
#                      to run if one exists already.
#   --guest NAME=VALUE a guest (wine side) variable for this session, through the same env file
#   --host NAME=VALUE  a runner variable, e.g. FEXDARWIN_HITCH_LOG=1. both imply the env file like --profile
#   --sample SECONDS   also take a 5 s `sample` of the runner and the render server at that time. off by
#                      default: sample(1) suspends every thread about 1000 times a second and halves the
#                      frame rate while it runs, so it skews the rest of the session
#   --out DIR          where to write (default ./profiles/<date-time>)
#
# start it before launching the game, play, quit. it waits for the runner (fexdarwin-run), then until it
# exits (or ctrl-c), and records:
#   top.raw / cpu.csv   top every 2 s for the runner (game + wine + neptune's guest driver, one process),
#                       the render server (x86_64 under rosetta), wsd, WindowServer, Obelisk, obelisk-input
#   engine-top.txt      cpu of every thread of wsd, the render server and the runner, plus gpu use, every
#                       5 s (tools/dev/engine-top.c, built on first use, no thread suspended)
#   vsz.txt             host virtual size of the runner every 10 s: a leaked per-thread reservation shows
#                       up here long before anything fails
#   thermal.txt         macOS thermal pressure changes (the cpu and gpu clocks follow it), no sudo needed
#   dumps/              with the env file: on a stall wsd logs "no frame shown", and the script then asks
#                       the runner for a thread dump of every guest process (FEXDARWIN_DUMP_DIR) and
#                       samples wsd and the render server. `touch dumps/dump-request` does it by hand.
#   wine-<ID>.log       a copy of the game's log, and analysis.txt from tools/dev/analyze-log.py
# the window and mouse lines the analysis counts need wine's obelisk channel: put
# `err+all,fixme-all,trace+obelisk` in a file named `winedebug` at the root of the runtime (see obelisk's
# CONTRIBUTING.md). without it only the wsd, runner and fexdarwin lines are there.
set -uo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
LIBRARY="${OBELISK_LIBRARY_DIRECTORY:-$HOME/Library/Application Support/com.zagrios.obelisk}"
ID=; STORE=Steam; SAMPLE_AT=0; PROFILE=; ENVLINES=; OUT=
while [[ $# -gt 0 ]]; do
    case "$1" in
        --store) STORE="${2:?}"; shift ;;
        --profile) PROFILE=1; ENVLINES+=$'FEXDARWIN_PROFILE=1\n' ;;
        --guest) PROFILE=1; ENVLINES+="guest ${2:?}"$'\n'; shift ;;
        --host) PROFILE=1; ENVLINES+="${2:?}"$'\n'; shift ;;
        --sample) SAMPLE_AT="${2:-120}"; shift ;;
        --out) OUT="${2:?}"; shift ;;
        -*) echo "unknown option $1"; exit 2 ;;
        *) ID="$1" ;;
    esac
    shift
done
[[ -n "$ID" ]] || { sed -n '2,/^set -uo/p' "$0" | sed '$d' | sed 's/^# \{0,1\}//'; exit 2; }
# the runtime obelisk runs: a development runtime (Voile/current) wins over every release
RUNTIME="$LIBRARY/Voile/current"
[[ -d "$RUNTIME" ]] || RUNTIME="$(/bin/ls -d "$LIBRARY"/Voile/v* 2>/dev/null | sort -V | tail -1)"
[[ -d "$RUNTIME/linux/engine" ]] || { echo "no voile runtime with a linux engine under $LIBRARY/Voile"; exit 1; }
LOG="$HOME/Library/Logs/Obelisk/$STORE/wine-$ID.log"
OUT="${OUT:-$PWD/profiles/$(date +%Y%m%d-%H%M%S)}"
PROCS='fexdarwin-run|virgl_render_server|wsd|WindowServer|Obelisk|obelisk-input'
mkdir -p "$OUT"
echo "profile -> $OUT (runtime $RUNTIME)"
ENVFILE="$RUNTIME/linux/engine/fexdarwin.env"
if [[ -n "$PROFILE" ]]; then
    if pgrep -x fexdarwin-run >/dev/null; then echo "the env file must be in place before the game starts"; exit 1; fi
    if [[ -e "$ENVFILE" ]]; then echo "$ENVFILE exists already; not touching it"; exit 1; fi
    mkdir -p "$OUT/dumps"
    ENVLINES+="FEXDARWIN_DUMP_DIR=$OUT/dumps"$'\n'
    printf '%s' "$ENVLINES" > "$ENVFILE"
    echo "wrote $ENVFILE (removed when this script ends):"; sed 's/^/    /' "$ENVFILE"
fi
remove_env() { [[ -n "$PROFILE" && -f "$ENVFILE" ]] && rm "$ENVFILE" && echo "removed $ENVFILE"; }
trap 'remove_env; exit 1' INT TERM
# engine-top is built next to the profiles, once
ETOP_BIN="$(dirname "$OUT")/engine-top"
if [[ ! -x "$ETOP_BIN" ]]; then
    clang -O2 -o "$ETOP_BIN" "$HERE/engine-top.c" -framework IOKit -framework CoreFoundation \
        || echo "engine-top did not build; going on without the per-thread view"
fi
if ! pgrep -x fexdarwin-run >/dev/null; then
    echo "waiting for the game to start (fexdarwin-run)..."
    while ! pgrep -x fexdarwin-run >/dev/null; do sleep 1; done
fi
START=$(date +%s)
echo "game running since $(date +%H:%M:%S)"
# top in logging mode: a header and the 60 busiest processes every 2 s, filtered to ours at the end
top -l 0 -s 2 -stats pid,cpu,th,command -o cpu -n 60 > "$OUT/top.raw" 2>/dev/null &
TOP=$!
# ps vsz counts PROT_NONE reservations too, which is what a leak of per-thread host mappings looks like
( while sleep 10; do ps -axo pid=,vsz=,rss=,comm= | awk -v t="$(date +%H:%M:%S)" '/fexdarwin-run/{print t, $1, $2, $3}'; done ) > "$OUT/vsz.txt" 2>/dev/null &
VSZ=$!
ETOP=
if [[ -x "$ETOP_BIN" ]]; then
    "$ETOP_BIN" 5 > "$OUT/engine-top.txt" 2>&1 &
    ETOP=$!
fi
# the render server that draws: the busy one, not the idle 4 MB helper
render_pid() { ps -axo pid=,rss=,comm= | awk '/virgl_render_server/{if($2>m){m=$2;p=$1}} END{print p}'; }
cleanup() {
    kill "$TOP" "$VSZ" ${ETOP:+"$ETOP"} 2>/dev/null
    wait "$TOP" "$VSZ" ${ETOP:+"$ETOP"} 2>/dev/null
    # host vsz per runner pid, first and last sample (KiB -> GiB)
    awk '{if(!($2 in f))f[$2]=$3; l[$2]=$3; n[$2]++} END{for(p in f) if(n[p]>5) printf "host vsz pid %s: %.1f GiB -> %.1f GiB over %d samples\n", p, f[p]/1048576, l[p]/1048576, n[p]}' "$OUT/vsz.txt" | tee "$OUT/vsz-summary.txt"
    remove_env
    python3 -I - "$OUT/top.raw" "$PROCS" > "$OUT/cpu.csv" <<'PY'
import re, sys
raw, procs = sys.argv[1], re.compile(sys.argv[2])
t = -1
print("sample,pid,cpu,threads,command")
for line in open(raw, errors='replace'):
    if line.startswith('Processes:'):
        t += 1; continue
    p = line.split(None, 3)
    if len(p) == 4 and p[0].isdigit() and procs.search(p[3]):
        print(f"{t},{p[0]},{p[1]},{p[2]},{p[3].strip()}")
PY
    python3 -I - "$OUT/cpu.csv" <<'PY'
import csv, statistics, sys
from collections import defaultdict
by = defaultdict(list); th = defaultdict(list)
for r in csv.DictReader(open(sys.argv[1])):
    try: by[r['command']].append(float(r['cpu'])); th[r['command']].append(int(r['threads'].split('/')[0]))
    except ValueError: pass
print("cpu per process (% of one core; 100 = one core busy), over the session:")
for name, v in sorted(by.items(), key=lambda kv: -statistics.fmean(kv[1])):
    print(f"  {name:22s} mean {statistics.fmean(v):6.1f}  median {statistics.median(v):6.1f}  max {max(v):6.1f}  "
          f"threads {min(th[name])}-{max(th[name])}  ({len(v)} samples)")
PY
    if [[ -f "$LOG" ]]; then
        cp "$LOG" "$OUT/"
        python3 "$HERE/analyze-log.py" "$OUT/wine-$ID.log" | tee "$OUT/analysis.txt"
    else
        echo "no log at $LOG (wrong --store or id?)"
    fi
    # thermal pressure from the system log: 0 nominal, 1 elevated, 2 heavy
    /usr/bin/log show --start "$(date -r "$START" '+%Y-%m-%d %H:%M:%S')" \
        --predicate 'eventMessage CONTAINS "kThermalPressureContext"' --style compact 2>/dev/null \
        | sed -n 's/^\([0-9-]* [0-9:.]*\).*is now : \([0-9]\)/\1 \2/p' > "$OUT/thermal.txt"
    python3 -I - "$OUT/thermal.txt" "$START" "$(date +%s)" <<'PY' | tee -a "$OUT/analysis.txt"
import sys, datetime
rows = [l.split() for l in open(sys.argv[1]) if l.strip()]
start, end = int(sys.argv[2]), int(sys.argv[3])
lvl, t, at, tl = 0, start, [0, 0, 0], []
for d, tm, v in rows:
    ts = datetime.datetime.strptime(d + " " + tm[:8], "%Y-%m-%d %H:%M:%S").timestamp()
    at[lvl] += max(0, ts - t); tl.append(f"+{int(ts - start)}s:{v}"); lvl, t = int(v), ts
at[lvl] += max(0, end - t)
print(f"thermal pressure: nominal {at[0]:.0f} s, elevated {at[1]:.0f} s, heavy {at[2]:.0f} s; changes: {' '.join(tl) or 'none'} (level at start assumed 0)")
PY
    echo "done: $OUT"
}
trap 'cleanup; exit 0' INT TERM
# stall watch: wsd logs "no frame shown for N s" once per stall while the game holds the pointer. on a
# new one, ask for the guest thread dumps and sample the native side while the freeze lasts; at quit
# time the processes are gone and the samples come back empty, which is fine.
stalls=0
stall_check() {
    local n
    n=$(tail -c 400000 "$LOG" 2>/dev/null | /usr/bin/grep -c 'no frame shown for')
    [[ "$n" -gt "$stalls" ]] || return
    stalls=$n
    echo "stall at +${now}s: runner dumps + host samples -> $OUT"
    [[ -n "$PROFILE" ]] && touch "$OUT/dumps/dump-request"
    pid=$(render_pid); [[ -n "$pid" ]] && sample "$pid" 3 -mayDie -file "$OUT/stall-$stalls.server.sample.txt" >/dev/null 2>&1 &
    pid=$(pgrep -x wsd | head -1); [[ -n "$pid" ]] && sample "$pid" 3 -mayDie -file "$OUT/stall-$stalls.wsd.sample.txt" >/dev/null 2>&1 &
    date "+%H:%M:%S stall $stalls" >> "$OUT/stalls.txt"
}
sampled=; now=0
while pgrep -x fexdarwin-run >/dev/null; do
    sleep 1
    now=$(( $(date +%s) - START ))
    [[ -f "$LOG" ]] && stall_check
    if [[ -z "$sampled" && "$SAMPLE_AT" -gt 0 && "$now" -ge "$SAMPLE_AT" ]]; then
        sampled=1
        echo "5-second samples of the runner and the render server at +${now}s (frame rate drops meanwhile)"
        pid=$(pgrep -x fexdarwin-run | head -1)
        [[ -n "$pid" ]] && sample "$pid" 5 -mayDie -file "$OUT/runner.sample.txt" >/dev/null 2>&1 &
        pid=$(render_pid)
        [[ -n "$pid" ]] && sample "$pid" 5 -mayDie -file "$OUT/server.sample.txt" >/dev/null 2>&1 &
    fi
done
echo "game exited after ${now}s"
cleanup
