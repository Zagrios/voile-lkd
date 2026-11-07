#!/usr/bin/env python3
"""counters and a timeline from the log of a windows game run by obelisk's linux engine.

usage: analyze-log.py LOG [--timeline]

prints what a session did, so two sessions (before and after a change) compare on numbers:
  - frame pacing from wsd's "120 frames shown, present interval avg" lines (frames actually shown), as
    median and mean, and the fps they mean;
  - long present gaps: wsd's "frame held" is the time between two shown frames of the game's plane;
    200 ms and more is a visible hitch;
  - swapchain rebuilds ("WSI up"), and how many follow a focus loss within a minute (a game that
    minimizes itself when deactivated rebuilds on the way back);
  - relative mouse transitions, and how many flip back within 200 ms (a driver fighting the game);
  - stalls, windows hidden by macOS, resizes, fatal faults and wine errors;
  - with FEXDARWIN_PROFILE=1 (docs/V1.md): per process, cpu, syscall rate, time inside LKD, JIT share,
    the busiest guest threads and the costliest syscalls.
--timeline prints the interesting lines in order, with the wine timestamp as seconds since the first.
the wine lines ("WSI up", "app deactivated", the relative mouse ones) need `trace+obelisk` in the
runtime's winedebug file; without it those counters stay at 0.
"""
import re, statistics, sys

def main():
    if len(sys.argv) < 2:
        print(__doc__); sys.exit(2)
    path = sys.argv[1]; timeline = '--timeline' in sys.argv
    ts_re = re.compile(r'^(\d+\.\d+):')
    # wsd planes: 'overlay' is the game's swapchain, 'base' the window's own surface, which a game rarely
    # redraws (holds of 10-70 s there are normal): only overlay holds count
    held_re = re.compile(r'window (\d+) (base|overlay): frame held (\d+\.\d+) ms')
    wsd_re = re.compile(r'120 frames shown, present interval avg (\d+\.\d+) min [\d.]+ max (\d+\.\d+) ms')
    rel_re = re.compile(r'relative mouse window (\d+) -> (\d+)')
    # fexdarwin's profile (HitchLog.cpp): one header per process per period, then its threads by cpu and
    # its costliest syscalls
    prof_re = re.compile(r'\[fexdarwin\] pid (\d+) profile \((\w+)\) (\d+\.\d+) s to \S+ "([^"]*)": cpu user (\d+\.\d+) ms '
                         r'sys (\d+\.\d+) ms, syscalls (\d+) \((\d+\.\d+) ms inside\), JIT (\d+\.\d+) ms \((\d+) blocks\), '
                         r'SMC faults (\d+) \((\d+\.\d+) ms\), invalidations (\d+) \((\d+\.\d+) ms\)')
    pthr_re = re.compile(r'\[fexdarwin\] pid (\d+)   thread (\d+) "([^"]*)": cpu (\d+\.\d+) ms, syscalls (\d+) \((\d+\.\d+) ms inside\), '
                         r'JIT (\d+\.\d+) ms; ?(.*)')
    psys_re = re.compile(r'\[fexdarwin\] pid (\d+)   syscall (\S+): (\d+) calls, (\d+\.\d+) ms \((\d+\.\d+) us each\)')
    prof = {}  # pid -> {'comm', 'periods': [...], 'threads': {name: [...]}, 'sys': {name: [calls, ms]}}
    tags = [('WSI up', 'WSI'), ('app deactivated', 'DEACT'), ('not shown by macOS', 'HIDDEN'),
            ('shown by macOS', 'SHOWN'), ('no frame shown', 'STALL'), ('frames shown again', 'AGAIN'),
            ('comes back', 'BACK'), ('resized to', 'RESIZE'), ('FATAL', 'FATAL'), ('err:', 'ERR')]
    ts = None; first = None
    n = {k: 0 for k in ('lines', 'wsi', 'wsi_after_deact', 'deact', 'hidden', 'stall', 'held200', 'held1000',
                        'rel', 'rel_fast', 'held_base')}
    wsd_iv, wsd_max, holds = [], [], []
    last_deact = None; last_rel = None; events = []
    for line in open(path, errors='replace'):
        n['lines'] += 1
        m = ts_re.match(line)
        if m:
            ts = float(m.group(1)); first = first if first is not None else ts
        rel = ts - first if (ts is not None and first is not None) else None
        if '[fexdarwin] pid' in line and ('profile (' in line or '   thread ' in line or '   syscall ' in line):
            m = prof_re.search(line)
            if m:
                pr = prof.setdefault(m.group(1), {'comm': m.group(4), 'periods': [], 'threads': {}, 'sys': {}})
                pr['comm'] = m.group(4) or pr['comm']
                pr['periods'].append(dict(why=m.group(2), s=float(m.group(3)), user=float(m.group(5)), sys=float(m.group(6)),
                                          calls=int(m.group(7)), inside=float(m.group(8)), jit=float(m.group(9)),
                                          blocks=int(m.group(10)), smc=int(m.group(11)), inval=int(m.group(13))))
                continue
            m = pthr_re.search(line)
            if m and m.group(1) in prof and prof[m.group(1)]['periods']:
                pr = prof[m.group(1)]; per = pr['periods'][-1]
                name = m.group(3) or f'tid {m.group(2)}'
                pr['threads'].setdefault(name, []).append((float(m.group(4)), float(m.group(6)), float(m.group(7)), per['s'], m.group(8)))
                continue
            m = psys_re.search(line)
            if m and m.group(1) in prof:
                e = prof[m.group(1)]['sys'].setdefault(m.group(2), [0, 0.0])
                e[0] += int(m.group(3)); e[1] += float(m.group(4))
                continue
        h = held_re.search(line)
        if h:
            if h.group(2) != 'overlay':
                n['held_base'] += 1; continue
            v = float(h.group(3)); holds.append(v)
            if v >= 200: n['held200'] += 1; events.append((rel, 'HELD', line.strip()[:140]))
            if v >= 1000: n['held1000'] += 1
            continue
        m = wsd_re.search(line)
        if m: wsd_iv.append(float(m.group(1))); wsd_max.append(float(m.group(2))); continue
        m = rel_re.search(line)
        if m:
            n['rel'] += 1
            if last_rel is not None and ts is not None and ts - last_rel < 0.2: n['rel_fast'] += 1
            last_rel = ts; events.append((rel, 'REL', line.strip()[:140])); continue
        for key, tag in tags:
            if key in line:
                if tag == 'WSI':
                    n['wsi'] += 1
                    if last_deact is not None and ts is not None and ts - last_deact < 60: n['wsi_after_deact'] += 1
                elif tag == 'DEACT': n['deact'] += 1; last_deact = ts
                elif tag == 'HIDDEN': n['hidden'] += 1
                elif tag == 'STALL': n['stall'] += 1
                events.append((rel, tag, line.strip()[:140]))
                break
    def fps(ms): return 1000.0 / ms if ms else 0
    print(f"{path}: {n['lines']} lines, {len(wsd_iv)} wsd samples")
    if wsd_iv:
        md = statistics.median(wsd_iv)
        print(f"wsd shown interval: median {md:.1f} ms ({fps(md):.0f} fps), mean {statistics.fmean(wsd_iv):.1f} ms, "
              f"120-frame windows with a gap >= 200 ms: {sum(1 for x in wsd_max if x >= 200)}/{len(wsd_max)}")
    print(f"present gaps on the game's plane: >= 200 ms {n['held200']}, >= 1000 ms {n['held1000']} "
          f"(largest {max(holds) if holds else 0:.0f} ms); base-plane holds ignored: {n['held_base']}")
    print(f"swapchain rebuilds (WSI up): {n['wsi']}, of which within 60 s after a focus loss: {n['wsi_after_deact']}; "
          f"focus losses: {n['deact']}; window hidden by macOS: {n['hidden']}; stalls (no frame shown): {n['stall']}")
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
        for name, v in pr['threads'].items():
            secs = sum(x[3] for x in v)
            rows.append((sum(x[0] for x in v) / secs / 10, sum(x[1] for x in v) / secs / 10, sum(x[2] for x in v) / secs / 10, name, v[-1][4]))
        rows.sort(reverse=True)
        print("  threads (% of a core, mean over the periods the thread appeared in; inside = wall time in LKD syscalls, waits included):")
        for cpu, inside, jit, name, calls in rows[:10]:
            print(f"    {name:18s} cpu {cpu:5.0f}%  inside {inside:5.0f}%  JIT {jit:4.1f}%   {calls[:90]}")
        top = sorted(pr['sys'].items(), key=lambda kv: -kv[1][1])[:10]
        print("  syscalls by time: " + ", ".join(f"{k} {v[0]/tot:.0f}/s {v[1]/v[0]*1000:.1f} us" for k, v in top if v[0]))
    if timeline:
        print("\n   t(s)  tag       line")
        for rel, tag, txt in events:
            t = f'{rel:8.3f}' if rel is not None else '       ?'
            print(f"{t}  {tag:8s}  {txt}")

if __name__ == '__main__':
    main()
