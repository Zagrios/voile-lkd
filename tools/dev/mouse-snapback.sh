#!/bin/sh
# Relative mouse windows of a game's log: how long the driver stayed in relative mode each time, and the jump the
# game's cursor made on the way out. Since voile-wine a73dadee4ed, the driver warps the Mac pointer to Wine's cursor
# when it leaves relative mode, and the jump is the distance from that warp to the first absolute move after it:
# near 0 when the driver behaves. A log of an older driver has no warp line; the jump is then the distance from
# where the raw moves took Wine's cursor (the last absolute position plus their sum, without the clamp to the
# screen) to the first absolute move, which is the snap-back of the view.
# It needs Wine's obelisk channel with timestamps: `+timestamp,err+all,fixme-all,trace+obelisk` on the first line
# of the runtime's linux/winedebug (see profile-session.sh).
# Usage: tools/dev/mouse-snapback.sh ~/Library/Logs/Obelisk/Steam/wine-<app id>.log
[ -f "${1:-}" ] || { sed -n '2,/^# Usage/p' "$0" | sed 's/^# //'; exit 2; }
/usr/bin/grep -E 'update_relative_mouse |send_mouse hwnd 0x[0-9a-f]+ flags 0x(1|8001) ' "$1" | awk '
# Wine timestamp in seconds, or -1 on a line without one
function stamp(  t) { t = $1; sub(/:.*/, "", t); return (t ~ /^[0-9]+\.[0-9]+$/) ? t + 0 : -1 }
# The x,y of "send_mouse hwnd H flags F x,y data D" and of "warp to x,y, ..."
function xy(word,  i) { for (i = 1; i < NF; i++) if ($i == word) { split($(i + (word == "flags" ? 2 : 1)), p, ","); return 1 } return 0 }
function report(jump, how) {
    printf "%10.3f %8.2f s %5d raw moves, raw sum %5.0f px, jump ", on, off - on, n, sqrt(sx * sx + sy * sy)
    if (jump < 0) printf "    ? (%s)\n", how; else printf "%5.0f px (%s)\n", jump, how
    windows++; total += off - on; if (jump >= 20) big++
}
{ t = stamp() }
t < 0 { unstamped++; next }
/relative mouse window/ {
    if ($NF != 0) {
        if (state == "off") report(-1, "no absolute move before the next window")
        state = "on"; on = t; n = 0; sx = 0; sy = 0; bx = lx; by = ly; based = seen; warped = 0
    } else if (state == "on") { state = "off"; off = t }
    next
}
/update_relative_mouse warp to / { if (state == "off" && xy("to")) { wx = p[1]; wy = p[2]; warped = 1 } next }
/ flags 0x1 / { if (state == "on" && xy("flags")) { n++; sx += p[1]; sy += p[2] } next }
/ flags 0x8001 / {
    if (!xy("flags")) next
    if (state == "off") {
        if (warped) report(sqrt((p[1] - wx) ^ 2 + (p[2] - wy) ^ 2), "from the warp")
        else if (based) report(sqrt((p[1] - bx - sx) ^ 2 + (p[2] - by - sy) ^ 2), "no warp: from where the raw moves led")
        else report(-1, "no warp and no absolute move before the window")
        state = ""
    }
    lx = p[1]; ly = p[2]; seen = 1
}
END {
    if (state == "on") printf "%10.3f still in relative mode at the end of the log\n", on
    printf "windows %d, total %.0f s in relative mode, jumps >= 20 px: %d\n", windows, total, big
    if (!NR) print "no relative mouse or send_mouse lines: the obelisk channel of Wine is off (see the header of this script)"
    else if (unstamped && !windows) print "no Wine timestamps: put +timestamp in WINEDEBUG (see the header of this script)"
}'
