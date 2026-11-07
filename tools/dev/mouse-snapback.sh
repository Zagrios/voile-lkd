#!/bin/sh
# relative mouse windows of a game's log: how long the driver stayed in relative mode each time, and
# the jump the game saw on the way out (the raw counts of the window handed back as one absolute move,
# which is what a snap-back of the view looks like). a driver that behaves shows windows spanning
# whole matches and a snap-back column near 0. needs `trace+obelisk` in the runtime's winedebug file.
# usage: tools/dev/mouse-snapback.sh ~/Library/Logs/Obelisk/Steam/wine-<app id>.log
[ -f "${1:-}" ] || { sed -n '2,6p' "$0" | sed 's/^# //'; exit 2; }
/usr/bin/grep "update_relative_mouse\|send_mouse hwnd 0x[0-9a-f]* flags 0x1 \|send_mouse hwnd 0x[0-9a-f]* flags 0x8001" "$1" | awk -F'[: ,]' '
# a window opens on "relative mouse window 0 -> N" and closes on "-> 0"; flags 0x1 moves are the raw
# deltas inside it, the first 0x8001 (absolute) move after it is the pointer the game got back
/update_relative_mouse/ { if ($0 ~ /-> [1-9]/) { if (wait) { printf "%.3f %7.2f s %4d raw moves, raw sum %4.0f px, no absolute move before the next window\n", on, off-on, n, sqrt(sx*sx+sy*sy); tot++; dur+=off-on; wait=0 } on=$1+0; n=0; sx=0; sy=0; bx=lx; by=ly } else if (on) { off=$1+0; wait=1 } next }
/flags 0x1 / { if (on && !wait) { n++; sx+=$(NF-3); sy+=$(NF-2) } next }
/flags 0x8001/ { x=$(NF-3); y=$(NF-2); if (wait) { jx=x-bx; jy=y-by; raw=sqrt(sx*sx+sy*sy); snap=sqrt((jx+sx)*(jx+sx)+(jy+sy)*(jy+sy));
      printf "%.3f %7.2f s %4d raw moves, raw sum %4.0f px, pointer jump %4.0f px, snap-back %4.0f px\n", on, off-on, n, raw, sqrt(jx*jx+jy*jy), raw-sqrt(jx*jx+jy*jy); tot++; dur+=off-on; if (raw-sqrt(jx*jx+jy*jy)>=20) big++; wait=0; on=0 } lx=x; ly=y }
END { printf "windows %d, total %.0f s in relative mode, snap-backs >= 20 px: %d\n", tot, dur, big+0 }'
