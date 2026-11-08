#!/bin/bash
# Records where the CPU goes while a Windows game runs on Obelisk's Linux engine, so that a change is judged on
# numbers instead of impressions. It watches from outside (top, engine-top, the game's log) and changes nothing in
# the engine, except the env file the runner reads at start when an option below asks for one.
#
# Usage: tools/dev/profile-session.sh ID [--store Steam|Epic|GOG] [--profile] [--guest NAME=VALUE]...
#                                        [--host NAME=VALUE]... [--sample [SECONDS]] [--out DIR]
#   ID                 The game as its log names it: Steam app id, Epic app name, GOG product id (the log is
#                      ~/Library/Logs/Obelisk/<store>/wine-<ID>.log).
#   --profile          Puts FEXDARWIN_PROFILE=1 into the runtime's linux/engine/fexdarwin.env for this session:
#                      every 10 s, each guest process logs its CPU and syscalls per thread and its costliest
#                      syscalls into the game's log (docs/V1.md, "Debugging"). It costs two clock reads per syscall.
#                      The script refuses to run if the file exists already, and removes it when it ends, ctrl-C,
#                      a closed terminal and errors included.
#   --guest NAME=VALUE A guest (Wine side) variable for this session, through the same env file, for instance
#                      WINEDEBUG=+timestamp,err+all,fixme-all,trace+obelisk.
#   --host NAME=VALUE  A runner variable, such as FEXDARWIN_HITCH_LOG=1. Both write the env file like --profile.
#   --sample [SECONDS] Also takes a 5 s `sample` of the busiest runner and of the render server that many seconds
#                      in (120 by default). Off without the option: sample(1) suspends every thread about 1000 times
#                      a second and halves the frame rate while it runs, which skews the rest of the session.
#   --out DIR          Where to write (default build/profiles/<date-time> in this repository, which git ignores).
#
# Start it before launching the game, play, quit. It waits for the game's runner (fexdarwin-run with wsd, so that
# the wineboot run of a game's first launch is not taken for the game), then records until the runners exit (or
# ctrl-C):
#   top.raw, cpu.csv    top every 2 s for the runners (one fexdarwin-run per guest process: the game, wineserver,
#                       Wine's services...), the render servers (arm64 with DXMT, x86_64 under Rosetta with
#                       D3DMetal), wsd, WindowServer, Obelisk and obelisk-input. cpu-summary.txt adds up the
#                       processes of one name and lists the busiest runners on their own.
#   engine-top.txt      CPU of every thread of wsd, the render server and the runners, plus GPU use, every 5 s
#                       (tools/dev/engine-top.c, built into build/ when it changes; no thread is suspended).
#   vsz.txt             Host virtual size of each runner every 10 s: a leak of per-thread reservations shows up
#                       here long before anything fails.
#   thermal.txt         macOS thermal pressure changes (the CPU and GPU clocks follow it), without sudo.
#   dumps/              With the env file: when wsd logs a stall ("no frame shown"), the script asks the runner for
#                       a thread dump of every guest process (FEXDARWIN_DUMP_DIR) and samples wsd and the render
#                       server. `touch dumps/dump-request` does it by hand.
#   wine-<ID>.log       A copy of the game's log, and analysis.txt from tools/dev/analyze-log.py.
# The window and mouse lines the analysis counts need Wine's obelisk channel and its timestamps: put
# `+timestamp,err+all,fixme-all,trace+obelisk` on the first line of the runtime's linux/winedebug (for instance
# Voile/current/linux/winedebug, which Obelisk reads at each launch), or pass it with --guest WINEDEBUG=...
# Without it, only the wsd, Neptune and fexdarwin lines are there.
set -uo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$HERE/../.." && pwd)"
LIBRARY="${OBELISK_LIBRARY_DIRECTORY:-$HOME/Library/Application Support/com.zagrios.obelisk}"
ID=; STORE=Steam; SAMPLE_AT=0; PROFILE=; ENVLINES=; OUT=
while [[ $# -gt 0 ]]; do
    case "$1" in
        --store) STORE="${2:?}"; shift ;;
        --profile) PROFILE=1; ENVLINES+=$'FEXDARWIN_PROFILE=1\n' ;;
        --guest) PROFILE=1; ENVLINES+="guest ${2:?}"$'\n'; shift ;;
        --host) PROFILE=1; ENVLINES+="${2:?}"$'\n'; shift ;;
        --sample) if [[ "${2:-}" =~ ^[0-9]+$ ]]; then SAMPLE_AT=$2; shift; else SAMPLE_AT=120; fi ;;
        --out) OUT="${2:?}"; shift ;;
        -*) echo "Unknown option $1."; exit 2 ;;
        *) ID="$1" ;;
    esac
    shift
done
[[ -n "$ID" ]] || { sed -n '2,/^set -uo/p' "$0" | sed '$d' | sed 's/^# \{0,1\}//'; exit 2; }
# The runtime Obelisk runs: a development runtime (Voile/current) with a Linux engine wins over every release.
RUNTIME="$LIBRARY/Voile/current"
[[ -f "$RUNTIME/linux/engine/fexdarwin-run" ]] || RUNTIME="$(/bin/ls -d "$LIBRARY"/Voile/v* 2>/dev/null | sort -V | tail -1)"
[[ -f "$RUNTIME/linux/engine/fexdarwin-run" ]] || { echo "No Voile runtime with a Linux engine under $LIBRARY/Voile."; exit 1; }
LOG="$HOME/Library/Logs/Obelisk/$STORE/wine-$ID.log"
OUT="${OUT:-$REPO/build/profiles/$(date +%Y%m%d-%H%M%S)}"
# Absolute, because the runner resolves FEXDARWIN_DUMP_DIR from its own working directory.
mkdir -p "$OUT" && OUT="$(cd "$OUT" && pwd)" || exit 1
echo "Profile -> $OUT (runtime $RUNTIME)"
ENVFILE="$RUNTIME/linux/engine/fexdarwin.env"
remove_env() { if [[ -n "$PROFILE" && -f "$ENVFILE" ]]; then rm -f "$ENVFILE" && echo "Removed $ENVFILE."; fi; }
if [[ -n "$PROFILE" ]]; then
    if pgrep -x fexdarwin-run >/dev/null; then echo "The env file must be in place before the game starts."; exit 1; fi
    if [[ -e "$ENVFILE" ]]; then echo "$ENVFILE exists already; not touching it."; exit 1; fi
    mkdir -p "$OUT/dumps"
    ENVLINES+="FEXDARWIN_DUMP_DIR=$OUT/dumps"$'\n'
    # Removed on any exit from here on: a file left behind would profile every later game.
    trap remove_env EXIT
    printf '%s' "$ENVLINES" > "$ENVFILE"
    echo "Wrote $ENVFILE (removed when this script ends):"; sed 's/^/    /' "$ENVFILE"
elif [[ -e "$ENVFILE" ]]; then
    echo "Note: $ENVFILE exists and applies to this session (copied next to the profile):"; sed 's/^/    /' "$ENVFILE"
    cp "$ENVFILE" "$OUT/"
fi
trap 'exit 130' INT TERM HUP
ETOP_BIN="$REPO/build/engine-top"
if [[ ! -x "$ETOP_BIN" || "$HERE/engine-top.c" -nt "$ETOP_BIN" ]]; then
    mkdir -p "$REPO/build"
    clang -O2 -o "$ETOP_BIN" "$HERE/engine-top.c" -framework IOKit -framework CoreFoundation \
        || echo "engine-top did not build; going on without the per-thread view."
fi
session_up() { pgrep -x fexdarwin-run >/dev/null && pgrep -x wsd >/dev/null; }
if ! session_up; then
    echo "Waiting for the game to start (fexdarwin-run and wsd)..."
    until session_up; do sleep 1; done
fi
START=$(date +%s)
echo "Game running since $(date +%H:%M:%S)."
EXE="$(ps -o comm= -p "$(pgrep -x fexdarwin-run | head -1)" 2>/dev/null)"
if [[ -n "$EXE" && "$(cd "$(dirname "$EXE")" && pwd -P)" != "$(cd "$RUNTIME/linux/engine" && pwd -P)" ]]; then
    echo "Warning: the game runs $EXE, not the runner of $RUNTIME: the env file and the winedebug file there do not apply."
fi
# top in logging mode: a header and the 60 busiest processes every 2 s, filtered to ours at the end.
top -l 0 -s 2 -stats pid,cpu,th,command -o cpu -n 60 > "$OUT/top.raw" 2>/dev/null &
TOP=$!
# ps vsz counts PROT_NONE reservations too, which is what a leak of per-thread host mappings looks like.
( while sleep 10; do ps -axo pid=,vsz=,rss=,comm= | awk -v t="$(date +%H:%M:%S)" '/fexdarwin-run/{print t, $1, $2, $3}'; done ) > "$OUT/vsz.txt" 2>/dev/null &
VSZ=$!
ETOP=
if [[ -x "$ETOP_BIN" ]]; then
    "$ETOP_BIN" 5 > "$OUT/engine-top.txt" 2>&1 &
    ETOP=$!
fi
# The render server that draws: the busy one, not the idle 4 MB helper. The runner to sample: the busiest one, not
# the guest shell that started Wine and waits for it.
render_pid() { ps -axo pid=,rss=,comm= | awk '/virgl_render_server/{if($2>m){m=$2;p=$1}} END{print p}'; }
runner_pid() { ps -axo pid=,%cpu=,comm= | awk '/fexdarwin-run$/{if(p==""||$2>m){m=$2;p=$1}} END{print p}'; }
cleanup() {
    trap '' INT TERM HUP
    kill "$TOP" "$VSZ" ${ETOP:+"$ETOP"} 2>/dev/null
    wait "$TOP" "$VSZ" ${ETOP:+"$ETOP"} 2>/dev/null
    remove_env
    # Host vsz per runner pid, first and last sample (KiB -> GiB).
    awk '{if(!($2 in f))f[$2]=$3; l[$2]=$3; n[$2]++} END{for(p in f) if(n[p]>5) printf "host vsz pid %s: %.1f GiB -> %.1f GiB over %d samples\n", p, f[p]/1048576, l[p]/1048576, n[p]}' "$OUT/vsz.txt" | tee "$OUT/vsz-summary.txt"
    python3 -I - "$OUT/top.raw" > "$OUT/cpu.csv" <<'PY'
import re, sys
# top cuts COMMAND to 16 characters: virgl_render_server shows as virgl_render_ser.
procs = re.compile(r'fexdarwin-run|virgl_render_ser|wsd|WindowServer|Obelisk|obelisk-input')
t = -1
print("sample,pid,cpu,threads,command")
for line in open(sys.argv[1], errors='replace'):
    if line.startswith('Processes:'):
        t += 1; continue
    p = line.split(None, 3)
    # Sample 0 has no CPU yet: top needs two samples to compute it.
    if t > 0 and len(p) == 4 and p[0].isdigit() and procs.fullmatch(p[3].strip()):
        print(f"{t},{p[0]},{p[1]},{p[2]},{p[3].strip()}")
PY
    python3 -I - "$OUT/cpu.csv" <<'PY' | tee "$OUT/cpu-summary.txt"
import csv, statistics, sys
from collections import defaultdict
cpu, procs, threads, runner = (defaultdict(lambda: defaultdict(float)) for _ in range(4))
last = 0
for r in csv.DictReader(open(sys.argv[1])):
    try: s, c, th = int(r['sample']), float(r['cpu']), int(r['threads'].split('/')[0])
    except ValueError: continue
    name = r['command']; last = max(last, s)
    cpu[name][s] += c; procs[name][s] += 1; threads[name][s] += th
    if name == 'fexdarwin-run': runner[r['pid']][s] = c
if not last:
    print("No top samples."); sys.exit()
print(f"CPU per process name over {last} samples (% of one core, 100 = one core busy; the processes of one name are"
      " added up, and a process outside top's 60 busiest counts as 0):")
for name in sorted(cpu, key=lambda k: -sum(cpu[k].values())):
    v = [cpu[name][s] for s in range(1, last + 1)]
    print(f"  {name:17s} mean {statistics.fmean(v):6.1f}  median {statistics.median(v):6.1f}  max {max(v):6.1f}  "
          f"processes {int(min(procs[name].values()))}-{int(max(procs[name].values()))}  "
          f"threads {int(min(threads[name].values()))}-{int(max(threads[name].values()))}")
busy = sorted(runner, key=lambda p: -sum(runner[p].values()))[:5]
print("busiest runners: " + ", ".join(f"pid {p} mean {sum(runner[p].values()) / last:.1f}" for p in busy))
PY
    if [[ -f "$LOG" ]]; then
        cp "$LOG" "$OUT/"
        python3 -I "$HERE/analyze-log.py" "$OUT/wine-$ID.log" | tee "$OUT/analysis.txt"
    else
        echo "No log at $LOG (wrong --store or ID?)."
    fi
    # Thermal pressure from the system log: 0 nominal, 1 elevated, 2 heavy.
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
    echo "Done: $OUT"
}
trap cleanup EXIT
# Stall watch: wsd logs "no frame shown for N s" once per stall while the game holds the pointer. On a new one, ask
# for the guest thread dumps and sample the native side while the freeze lasts. Only the bytes added since the last
# look are read, so that a long log costs nothing; at quit time the processes are gone and the samples come back
# empty, which is fine.
seen=0; stalls=0
stall_check() {
    local size new pid
    size=$(stat -f %z "$LOG" 2>/dev/null) || return
    (( size < seen )) && seen=0
    (( size > seen )) || return
    new=$(tail -c +$((seen + 1)) "$LOG" | head -c $((size - seen)) | /usr/bin/grep -c 'no frame shown for')
    seen=$size
    (( new > 0 )) || return
    stalls=$((stalls + new))
    echo "Stall at +${now}s: runner dumps and host samples -> $OUT"
    [[ -n "$PROFILE" ]] && touch "$OUT/dumps/dump-request"
    pid=$(render_pid); [[ -n "$pid" ]] && sample "$pid" 3 -mayDie -file "$OUT/stall-$stalls.server.sample.txt" >/dev/null 2>&1 &
    pid=$(pgrep -x wsd | head -1); [[ -n "$pid" ]] && sample "$pid" 3 -mayDie -file "$OUT/stall-$stalls.wsd.sample.txt" >/dev/null 2>&1 &
    date "+%H:%M:%S stall $stalls" >> "$OUT/stalls.txt"
}
sampled=; now=0
while pgrep -x fexdarwin-run >/dev/null; do
    sleep 1
    now=$(( $(date +%s) - START ))
    stall_check
    if [[ -z "$sampled" && "$SAMPLE_AT" -gt 0 && "$now" -ge "$SAMPLE_AT" ]]; then
        sampled=1
        echo "5-second samples of the busiest runner and the render server at +${now}s (the frame rate drops meanwhile)."
        pid=$(runner_pid)
        [[ -n "$pid" ]] && sample "$pid" 5 -mayDie -file "$OUT/runner.sample.txt" >/dev/null 2>&1 &
        pid=$(render_pid)
        [[ -n "$pid" ]] && sample "$pid" 5 -mayDie -file "$OUT/server.sample.txt" >/dev/null 2>&1 &
    fi
done
echo "Game exited after ${now}s."
exit 0
