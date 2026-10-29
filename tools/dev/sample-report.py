#!/usr/bin/env python3
# Report of the fexdarwin sampler (FEXDARWIN_SAMPLE): sample-<pid>.bin + sample-<pid>.txt.
# Usage: sample-report.py DIR_OR_PREFIX [--pid N] [--thread REGEX] [--from S] [--to S] [--top N] [--disasm N]
#        [--timeline REGEX] [--bucket MS] [--stalls REGEX]
# Times (--from/--to) are seconds since the first sample of the process.
import argparse, collections, glob, os, re, struct, subprocess, sys

REC48 = struct.Struct('<QQQQIIiHBB')
REC64 = struct.Struct('<QQQQIIiHBBQII')
REC = REC48
KINDS = {1: 'jit', 2: 'syscall', 3: 'compile', 4: 'host'}
SYSCALLS = {0: 'read', 1: 'write', 7: 'poll', 9: 'mmap', 10: 'mprotect', 11: 'munmap', 13: 'rt_sigaction', 14: 'rt_sigprocmask',
            16: 'ioctl', 17: 'pread64', 18: 'pwrite64', 19: 'readv', 20: 'writev', 23: 'select', 24: 'sched_yield', 28: 'madvise',
            35: 'nanosleep', 44: 'sendto', 45: 'recvfrom', 46: 'sendmsg', 47: 'recvmsg', 72: 'fcntl', 202: 'futex',
            228: 'clock_gettime', 230: 'clock_nanosleep', 232: 'epoll_wait', 270: 'pselect6', 271: 'ppoll', 281: 'epoll_pwait',
            441: 'epoll_pwait2', 98: 'getrusage', 39: 'getpid', 186: 'gettid', 96: 'gettimeofday', 257: 'openat', 262: 'newfstatat',
            3: 'close', 8: 'lseek', 56: 'clone', 61: 'wait4', 62: 'kill', 234: 'tgkill', 200: 'tkill', 15: 'rt_sigreturn'}
# clang and llvm-objdump that know x86-64 (Voile's llvm-mingw).
TOOLS = os.environ.get('LLVM_BIN', os.path.join(os.path.dirname(os.path.abspath(__file__)), '../../../build/toolchain/llvm-mingw/bin'))


def host_insn_class(w):
    if w == 0:
        return '?'
    if (w & 0xFFFFFFE0) == 0xD5031000:
        return 'wfet (tpause/umwait wait)'
    if w in (0xD503205F, 0xD503203F):
        return 'wfe/yield'
    if (w & 0x3FFFFC00) == 0x089FFC00:
        return 'stlr (TSO store)'
    if (w & 0x3FE00C00) == 0x19000000:
        return 'stlur (TSO store)'
    if (w & 0x3FFFFC00) == 0x08DFFC00:
        return 'ldar (TSO load)'
    if (w & 0x3FFFFC00) == 0x38BFC000:
        return 'ldapr (TSO load)'
    if (w & 0x3F600C00) == 0x19400000:
        return 'ldapur (TSO load)'
    if (w & 0xFFFFF0FF) == 0xD50330BF:
        return 'dmb'
    if (w & 0x3F208C00) == 0x38200000 or (w & 0x3F208C00) == 0x38208000:
        return 'lse atomic'
    if (w & 0x3FA07C00) == 0x08A07C00:
        return 'cas'
    if (w & 0x3F000000) == 0x08000000:
        return 'exclusive/ordered'
    op0 = (w >> 25) & 0xF
    if op0 in (0x4, 0x6, 0xC, 0xE):
        return 'load/store'
    if op0 in (0x8, 0x9):
        return 'int imm'
    if op0 in (0x5, 0xD):
        return 'int reg'
    if op0 in (0x7, 0xF):
        return 'simd/fp'
    if op0 in (0xA, 0xB):
        return 'branch/sys'
    return 'other'


# Host system call stubs that block. macOS reports a thread asleep in a ulock wait as TH_STATE_RUNNING, so the sampler
# records it; such samples are waits, not CPU time. Filled by resolve_blocking().
BLOCKING_NAMES = ('__ulock_wait', '__psynch_cvwait', '__psynch_mutexwait', 'poll', '__select', 'kevent', '__semwait_signal',
                  '__wait4', '__sigsuspend', '__accept', 'recvmsg', '__recvfrom', 'read', 'pread', '__pselect')
BLOCKING_PCS = set()
WAIT_AFTER_RIPS = set()  # guest RIPs that follow a tpause/umwait: a thread woken from wfet resumes there


def resolve_blocking(recs, image_base):
    # Host PCs outside fexdarwin-run (system libraries) are named with atos against this very process: the dyld shared
    # cache has the same slide in every process until the next reboot.
    pcs = collections.Counter(r[1] for r in recs if r[8] in (2, 4) and not (image_base <= r[1] < image_base + (64 << 20)))
    top = [pc for pc, _ in pcs.most_common(300)]
    if not top:
        return {}
    out = subprocess.run(['atos', '-p', str(os.getpid())] + [hex(p) for p in top], capture_output=True, text=True).stdout.splitlines()
    names = {}
    for pc, name in zip(top, out):
        names[pc] = name.split(' (in ')[0]
        if any(name.startswith(b) for b in BLOCKING_NAMES):
            BLOCKING_PCS.add(pc)
    return names


def kind_of(r):
    if r[1] in BLOCKING_PCS or r[8] == 5:
        return 'blocked'
    # A thread suspended in wfet (guest tpause) resumes at the next instruction: those samples are waits.
    if r[8] == 1 and (host_insn_class(r[5]).startswith(('wfet', 'wfe')) or r[2] in WAIT_AFTER_RIPS):
        return 'jit-wait'
    return KINDS.get(r[8], '?')


def load(prefix):
    txt, binf = prefix + '.txt', prefix + '.bin'
    threads, vmas, code, meta = {}, [], {}, {}
    hostcode = {}
    gen = 0
    for line in open(txt, errors='replace'):
        p = line.rstrip('\n').split(' ', 4)
        if p[0] == 'thread':
            threads[int(p[1])] = ' '.join(p[2:])
        elif p[0] == 'vma':
            vmas.append((gen, int(p[1], 16), int(p[2], 16), int(p[3], 16), p[4] if len(p) > 4 else '?'))
        elif p[0] == 'code':
            code[int(p[1], 16)] = bytes.fromhex(p[2])
        elif p[0] == 'hostcode':
            q = line.split()
            hostcode[int(q[2], 16)] = (int(q[1], 16), bytes.fromhex(q[3]))
        elif p[0] == 'exec':
            gen += 1
        elif p[0] == 'fexdarwin-run':
            meta['base'] = int(p[1], 16)
            if 'rec 64' in line:
                meta['rec'] = 64
    vmas.sort(key=lambda v: v[1])
    data = open(binf, 'rb').read()
    rs = REC64 if meta.get('rec') == 64 else REC48
    # Records are used by index 0..9 everywhere; 64-byte records add host begin (10) and host size (11).
    recs = [rs.unpack_from(data, o) for o in range(0, len(data) - rs.size + 1, rs.size)]
    meta['hostcode'] = hostcode
    return threads, vmas, code, recs, meta


def module_of(vmas, addr):
    # Latest generation wins (vmas are sorted by start; few enough for a scan of candidates).
    best = None
    for g, s, e, off, name in vmas:
        if s > addr:
            break
        if s <= addr < e and (best is None or g >= best[0]):
            best = (g, s, off, name)
    if not best:
        return '?', addr
    return os.path.basename(best[3]), addr - best[1] + best[2]


def pct(n, t):
    return f'{100.0 * n / t:5.1f}%' if t else '  -  '


def disasm(rip, blob):
    # The block bytes are assembled into an object file (.byte) and disassembled with llvm-objdump at their guest address.
    import tempfile
    with tempfile.TemporaryDirectory() as d:
        src, obj = os.path.join(d, 'b.s'), os.path.join(d, 'b.o')
        with open(src, 'w') as f:
            f.write('.text\n')
            for i in range(0, len(blob), 32):
                f.write('.byte ' + ','.join(str(b) for b in blob[i:i + 32]) + '\n')
        try:
            subprocess.run([TOOLS + '/clang', '-c', '-target', 'x86_64-linux-gnu', '-o', obj, src], check=True, capture_output=True)
            out = subprocess.run([TOOLS + '/llvm-objdump', '-d', '--no-show-raw-insn', '-M', 'intel', f'--adjust-vma={rip:#x}', obj],
                                 capture_output=True, text=True, check=True).stdout
        except Exception as e:
            return [(rip, f'(disassembly failed: {e})')]
    res = []
    for l in out.splitlines():
        m = re.match(r'\s*([0-9a-f]+):\s+(.*)', l)
        if m:
            res.append((int(m.group(1), 16), m.group(2).strip()))
    return res


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('path')
    ap.add_argument('--pid', type=int)
    ap.add_argument('--thread', help='regex on the thread name or tid')
    ap.add_argument('--from', dest='t0', type=float)
    ap.add_argument('--to', dest='t1', type=float)
    ap.add_argument('--top', type=int, default=25)
    ap.add_argument('--disasm', type=int, default=6, help='hottest blocks to disassemble')
    ap.add_argument('--timeline', help='regex on thread names: per bucket, share of samples of those threads by kind')
    ap.add_argument('--bucket', type=int, default=250, help='timeline bucket in ms')
    ap.add_argument('--cpu', action='store_true', help='drop the samples that are waits (blocked syscalls, tpause)')
    args = ap.parse_args()

    if os.path.isdir(args.path):
        cands = sorted(glob.glob(os.path.join(args.path, 'sample-*.bin')), key=os.path.getsize, reverse=True)
        if args.pid:
            cands = [c for c in cands if c.endswith(f'-{args.pid}.bin')]
        if not cands:
            sys.exit('no sample files')
        prefix = cands[0][:-4]
        others = [(os.path.basename(c), os.path.getsize(c) // REC.size) for c in cands[1:6]]
    else:
        prefix, others = args.path, []
    threads, vmas, code, recs, meta = load(prefix)
    if not recs:
        sys.exit('no samples')
    hostnames = resolve_blocking(recs, meta.get('base', 0))
    for brip, blob in code.items():
        prev = None
        for at, txt in disasm(brip, blob):
            if prev and prev.startswith(('tpause', 'umwait')):
                WAIT_AFTER_RIPS.add(at)
            prev = txt
    t_first = recs[0][0]
    def tname(tid):
        return f'{threads.get(tid, "?")}:{tid}'
    sel = []
    for r in recs:
        ns, pc, rip, brip, bsize, insn, tid, sysnr, kind = r[:9]
        t = (ns - t_first) / 1e9
        if args.t0 is not None and t < args.t0:
            continue
        if args.t1 is not None and t > args.t1:
            continue
        if args.thread and not re.search(args.thread, tname(tid)):
            continue
        if args.cpu and kind_of(r) in ('blocked', 'jit-wait'):
            continue
        sel.append(r)
    n = len(sel)
    span = (recs[-1][0] - t_first) / 1e9
    print(f'{os.path.basename(prefix)}: {len(recs)} samples over {span:.1f} s, {n} selected' + (f'; other processes: {others}' if others else ''))

    kinds = collections.Counter(kind_of(r) for r in sel)
    print('\n== by kind\n' + '\n'.join(f'  {pct(c, n)} {k}' for k, c in kinds.most_common()))

    th = collections.Counter(tname(r[6]) for r in sel)
    print('\n== by thread (samples = time on CPU)')
    for k, c in th.most_common(args.top):
        sub = collections.Counter(kind_of(r) for r in sel if tname(r[6]) == k)
        print(f'  {pct(c, n)} {k:40s} ' + ' '.join(f'{kk}={pct(cc, c).strip()}' for kk, cc in sub.most_common()))

    sysc = collections.Counter(SYSCALLS.get(r[7], str(r[7])) + (' (blocked)' if kind_of(r) == 'blocked' else '') for r in sel if r[8] == 2)
    if sysc:
        print('\n== system calls (share of all selected samples)')
        for k, c in sysc.most_common(12):
            print(f'  {pct(c, n)} {k}')

    hp = collections.Counter(hostnames.get(r[1], hex(r[1])) for r in sel if r[8] in (2, 4) and r[1] in hostnames)
    if hp:
        print('\n== system library functions at the sampled PC (syscall + host samples)')
        for k, c in hp.most_common(12):
            print(f'  {pct(c, n)} {k}')

    mods = collections.Counter(module_of(vmas, r[2])[0] for r in sel if r[8] in (1, 4, 3))
    print('\n== guest module of the code running (jit + host + compile)')
    for k, c in mods.most_common(args.top):
        print(f'  {pct(c, n)} {k}')

    jit = [r for r in sel if kind_of(r) == 'jit']
    ic = collections.Counter(host_insn_class(r[5]) for r in jit)
    print(f'\n== host instruction at the sampled PC, JIT samples ({len(jit)}; the PC is where the core waits, often the slow instruction or the one after it)')
    for k, c in ic.most_common():
        print(f'  {pct(c, len(jit))} {k}')

    rips = collections.Counter(r[2] for r in jit)
    print('\n== hottest guest instructions (JIT samples)')
    for rip, c in rips.most_common(args.top):
        m, off = module_of(vmas, rip)
        print(f'  {pct(c, n)} {rip:#x} {m}+{off:#x}')

    blocks = collections.Counter(r[3] for r in jit if r[3])
    print('\n== hottest blocks (JIT samples)')
    for brip, c in blocks.most_common(args.top):
        m, off = module_of(vmas, brip)
        print(f'  {pct(c, n)} {brip:#x} {m}+{off:#x}' + ('' if brip in code else ' (no code dumped)'))

    shown = 0
    for brip, c in blocks.most_common():
        if shown >= args.disasm:
            break
        if brip not in code:
            continue
        shown += 1
        m, off = module_of(vmas, brip)
        per = collections.Counter(r[2] for r in jit if r[3] == brip)
        print(f'\n-- block {brip:#x} {m}+{off:#x}: {c} samples ({pct(c, n).strip()})')
        for at, txt in disasm(brip, code[brip]):
            h = per.get(at, 0)
            print(f'  {h:6d} {at:#x}  {txt}')

    if args.timeline:
        b = args.bucket / 1000.0
        buckets = collections.defaultdict(collections.Counter)
        for r in recs:
            if not re.search(args.timeline, tname(r[6])):
                continue
            t = (r[0] - t_first) / 1e9
            key = 'yield' if r[8] == 2 and r[7] == 24 else ('sys:' + SYSCALLS.get(r[7], str(r[7])) if r[8] == 2 else kind_of(r))
            buckets[int(t / b)][key] += 1
        print(f'\n== timeline of /{args.timeline}/ ({args.bucket} ms buckets; samples per bucket by state)')
        for i in sorted(buckets):
            c = buckets[i]
            tot = sum(c.values())
            print(f'  {i * b:8.2f}s {tot:5d} ' + ' '.join(f'{k}={v}' for k, v in c.most_common(5)))


if __name__ == '__main__':
    main()
