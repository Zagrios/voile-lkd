#!/usr/bin/env python3
"""Counters and a timeline from the log of a Windows game run by Obelisk's Linux engine.

Usage: analyze-log.py LOG [--timeline]

Prints what a session did, so that two sessions (before and after a change) compare on numbers:
  - the frame rate from wsd's "120 frames shown, present interval avg" lines (frames actually shown), as median
    and mean, on the window plane that shows the most frames (a swapchain's overlay plane, or the base plane of a
    game that draws into its window);
  - long present gaps on that plane: wsd's "frame held" is the time between two shown frames, and 200 ms or more
    is a visible hitch;
  - D3D12 swapchain bring-ups (Neptune's "WSI up", the first one included), and how many follow a focus loss within
    a minute (a game that minimizes itself when deactivated rebuilds on the way back); a D3D11 swapchain logs its
    creation only, so its rebuilds are not counted;
  - relative mouse transitions, and how many flip back within 200 ms (a driver fighting the game);
  - stalls, windows hidden by macOS, resizes, fatal faults and Wine errors;
  - with FEXDARWIN_PROFILE=1 (docs/V1.md): per process, CPU, syscall rate, time inside LKD, JIT share, the busiest
    guest threads and the costliest syscalls.
--timeline prints the interesting lines in order, with Wine's timestamp as seconds since the first.
The focus loss, resize and relative mouse lines need Wine's obelisk channel, and every time-based count needs Wine's
timestamps: `+timestamp,err+all,fixme-all,trace+obelisk` on the first line of the runtime's linux/winedebug (see
profile-session.sh). Without them, those counts stay at 0 and the output says why.
"""
import re, statistics, sys

def main():
    if len(sys.argv) < 2:
        print(__doc__); sys.exit(2)
    path = sys.argv[1]; timeline = '--timeline' in sys.argv
    # Wine's "+timestamp" prefix: "%3u.%03u:" of the Mac's uptime.
    ts_re = re.compile(r'^\s*(\d+\.\d+):')
    held_re = re.compile(r'window (\d+) (base|overlay): frame held (\d+\.\d+) ms')
    wsd_re = re.compile(r'window (\d+) (base|overlay): 120 frames shown, present interval avg (\d+\.\d+) min [\d.]+ '
                        r'max (\d+\.\d+) ms')
    rel_re = re.compile(r'relative mouse window (\d+) -> (\d+)')
    # fexdarwin's profile (HitchLog.cpp): one header per process per period, then its threads by CPU and its
    # costliest syscalls. Only periods ("period", 1 s or more) count: the short "exec" and "exit" ones would turn a
    # burst into a busy thread.
    prof_re = re.compile(r'\[fexdarwin\] pid (\d+) profile \((\w+)\) (\d+\.\d+) s to \S+ "([^"]*)": cpu user (\d+\.\d+) ms '
                         r'sys (\d+\.\d+) ms, syscalls (\d+) \((\d+\.\d+) ms inside\), JIT (\d+\.\d+) ms \((\d+) blocks\), '
                         r'SMC faults (\d+) \((\d+\.\d+) ms\), invalidations (\d+) \((\d+\.\d+) ms\)')
    pthr_re = re.compile(r'\[fexdarwin\] pid (\d+)   thread (\d+) "([^"]*)": cpu (\d+\.\d+) ms, syscalls (\d+) \((\d+\.\d+) ms inside\), '
                         r'JIT (\d+\.\d+) ms; ?(.*)')
    psys_re = re.compile(r'\[fexdarwin\] pid (\d+)   syscall (\S+): (\d+) calls, (\d+\.\d+) ms \((\d+\.\d+) us each\)')
    prof = {}  # pid -> {'comm', 'periods': [...], 'counting', 'threads': {tid: {'name', 'rows'}}, 'sys': {name: [calls, ms]}}
    tags = [('WSI up', 'WSI'), ('guest swapchain: created', 'D3D11'), ('app deactivated', 'DEACT'),
            ('not shown by macOS', 'HIDDEN'), ('shown by macOS', 'SHOWN'), ('no frame shown', 'STALL'),
            ('frames shown again', 'AGAIN'), ('comes back', 'BACK'), ('resized to', 'RESIZE'), ('FATAL', 'FATAL'),
            ('err:', 'ERR')]
    ts = None; first = None
    n = {k: 0 for k in ('lines', 'stamped', 'obelisk', 'wsi', 'wsi_after_deact', 'd3d11', 'deact', 'hidden', 'stall',
                        'resize', 'fatal', 'err', 'rel', 'rel_fast')}
    shown = {}  # (window, plane) -> ([interval avg], [interval max])
    held = {}   # (window, plane) -> [frame held ms]
    last_deact = None; last_rel = None; events = []
    for line in open(path, errors='replace'):
        n['lines'] += 1
        m = ts_re.match(line)
        if m:
            n['stamped'] += 1
            ts = float(m.group(1)); first = first if first is not None else ts
        rel = ts - first if (ts is not None and first is not None) else None
        if ':trace:obelisk:' in line: n['obelisk'] += 1
        if '[fexdarwin] pid' in line and ('profile (' in line or '   thread ' in line or '   syscall ' in line):
            m = prof_re.search(line)
            if m:
                pr = prof.setdefault(m.group(1), {'comm': m.group(4), 'periods': [], 'threads': {}, 'sys': {}})
                pr['comm'] = m.group(4) or pr['comm']
                per = dict(why=m.group(2), s=float(m.group(3)), user=float(m.group(5)), sys=float(m.group(6)),
                           calls=int(m.group(7)), inside=float(m.group(8)), jit=float(m.group(9)),
                           blocks=int(m.group(10)), smc=int(m.group(11)), inval=int(m.group(13)))
                pr['periods'].append(per)
                pr['counting'] = per['why'] == 'period' and per['s'] >= 1
                continue
            m = pthr_re.search(line)
            if m and prof.get(m.group(1), {}).get('counting'):
                pr = prof[m.group(1)]
                th = pr['threads'].setdefault(m.group(2), {'name': '', 'rows': []})
                th['name'] = m.group(3) or th['name']
                th['rows'].append((float(m.group(4)), float(m.group(6)), float(m.group(7)), pr['periods'][-1]['s'], m.group(8)))
                continue
            m = psys_re.search(line)
            if m and prof.get(m.group(1), {}).get('counting'):
                e = prof[m.group(1)]['sys'].setdefault(m.group(2), [0, 0.0])
                e[0] += int(m.group(3)); e[1] += float(m.group(4))
                continue
        h = held_re.search(line)
        if h:
            v = float(h.group(3)); held.setdefault((h.group(1), h.group(2)), []).append(v)
            if timeline and v >= 200: events.append((rel, 'HELD', line.strip()[:140]))
            continue
        m = wsd_re.search(line)
        if m:
            iv, mx = shown.setdefault((m.group(1), m.group(2)), ([], []))
            iv.append(float(m.group(3))); mx.append(float(m.group(4)))
            continue
        m = rel_re.search(line)
        if m:
            n['rel'] += 1
            if last_rel is not None and ts is not None and ts - last_rel < 0.2: n['rel_fast'] += 1
            last_rel = ts
            if timeline: events.append((rel, 'REL', line.strip()[:140]))
            continue
        for key, tag in tags:
            if key in line:
                if tag == 'WSI':
                    n['wsi'] += 1
                    if last_deact is not None and ts is not None and ts - last_deact < 60: n['wsi_after_deact'] += 1
                elif tag == 'DEACT': n['deact'] += 1; last_deact = ts
                elif tag in ('D3D11', 'HIDDEN', 'STALL', 'RESIZE', 'FATAL', 'ERR'): n[tag.lower()] += 1
                if timeline: events.append((rel, tag, line.strip()[:140]))
                break
    def fps(ms): return 1000.0 / ms if ms else 0
    print(f"{path}: {n['lines']} lines, {sum(len(v[0]) for v in shown.values())} wsd samples")
    if not n['obelisk']:
        print("note: no trace:obelisk lines (Wine's obelisk channel is off): focus losses, resizes and relative mouse "
              "transitions read 0")
    elif not n['stamped']:
        print("note: Wine lines without timestamps (+timestamp is missing from WINEDEBUG): the counts \"within\" a time "
              "read 0 and --timeline has no times")
    # The game's plane: the one that shows the most frames.
    planes = sorted(set(shown) | set(held), key=lambda k: (-len(shown.get(k, ([],))[0]), -len(held.get(k, []))))
    if planes:
        game = planes[0]; iv, mx = shown.get(game, ([], [])); holds = held.get(game, [])
        if iv:
            md = statistics.median(iv)
            print(f"wsd shown interval, window {game[0]} {game[1]}: median {md:.1f} ms ({fps(md):.0f} fps), mean "
                  f"{statistics.fmean(iv):.1f} ms, 120-frame windows with a gap >= 200 ms: {sum(1 for x in mx if x >= 200)}/{len(mx)}")
        print(f"present gaps on that plane: >= 200 ms {sum(1 for v in holds if v >= 200)}, >= 1000 ms "
              f"{sum(1 for v in holds if v >= 1000)} (largest {max(holds) if holds else 0:.0f} ms)")
        others = [f"window {k[0]} {k[1]} ({len(shown.get(k, ([],))[0])} samples, {len(held.get(k, []))} holds)" for k in planes[1:]]
        if others: print("other planes: " + ", ".join(others))
    print(f"D3D12 swapchain bring-ups (WSI up): {n['wsi']}, of which within 60 s after a focus loss: {n['wsi_after_deact']}; "
          f"D3D11 swapchains created: {n['d3d11']} (their rebuilds are not logged)")
    print(f"focus losses: {n['deact']}; window hidden by macOS: {n['hidden']}; stalls (no frame shown): {n['stall']}; "
          f"resizes: {n['resize']}; fatal faults: {n['fatal']}; Wine errors: {n['err']}")
    print(f"relative mouse transitions: {n['rel']}, flipping back within 200 ms: {n['rel_fast']}")
    for pid, pr in sorted(prof.items(), key=lambda kv: -sum(p['user'] for p in kv[1]['periods'])):
        pers = [p for p in pr['periods'] if p['why'] == 'period' and p['s'] >= 1]
        if len(pers) < 2 or sum(p['user'] for p in pers) < 1000: continue  # only processes that matter
        tot = sum(p['s'] for p in pers)
        print(f"\nrunner profile pid {pid} \"{pr['comm']}\": {len(pers)} periods, {tot:.0f} s; cpu user {sum(p['user'] for p in pers)/tot/10:.0f}% "
              f"sys {sum(p['sys'] for p in pers)/tot/10:.0f}% of a core; syscalls {sum(p['calls'] for p in pers)/tot:.0f}/s "
              f"(wall time inside LKD, all threads, waits included: {sum(p['inside'] for p in pers)/tot/10:.0f}% of a core); JIT {sum(p['jit'] for p in pers)/tot/10:.1f}% "
              f"({sum(p['blocks'] for p in pers)/tot:.0f} blocks/s); SMC faults {sum(p['smc'] for p in pers)/tot:.1f}/s, "
              f"invalidations {sum(p['inval'] for p in pers)/tot:.1f}/s")
        rows = []
        for tid, th in pr['threads'].items():
            v = th['rows']; secs = sum(x[3] for x in v)
            rows.append((sum(x[0] for x in v) / secs / 10, sum(x[1] for x in v) / secs / 10, sum(x[2] for x in v) / secs / 10,
                         f"{th['name'] or 'tid'} ({tid})", v[-1][4]))
        rows.sort(reverse=True)
        print("  threads (% of a core, mean over the periods the thread appeared in; inside = wall time in LKD syscalls, waits included):")
        for cpu, inside, jit, name, calls in rows[:10]:
            print(f"    {name:24s} cpu {cpu:5.0f}%  inside {inside:5.0f}%  JIT {jit:4.1f}%   {calls[:90]}")
        top = sorted(pr['sys'].items(), key=lambda kv: -kv[1][1])[:10]
        print("  syscalls by time: " + ", ".join(f"{k} {v[0]/tot:.0f}/s {v[1]/v[0]*1000:.1f} us" for k, v in top if v[0]))
    if timeline:
        print("\n   t(s)  tag       line")
        for rel, tag, txt in events:
            t = f'{rel:8.3f}' if rel is not None else '       ?'
            print(f"{t}  {tag:8s}  {txt}")

if __name__ == '__main__':
    main()
