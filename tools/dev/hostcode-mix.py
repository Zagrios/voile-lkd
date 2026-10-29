#!/usr/bin/env python3
# Composition of the host code FEX made for the hottest blocks (sampler "hostcode" lines, FEXDARWIN_SAMPLE with 64-byte
# records): static share of each kind of arm64 instruction, and the same weighted by where the samples landed.
# Usage: hostcode-mix.py DIR_OR_PREFIX [--thread REGEX] [--from S] [--to S]
import argparse, collections, importlib.util, os, re, subprocess, sys, tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
spec = importlib.util.spec_from_file_location('sr', os.path.join(HERE, 'sample-report.py'))
sr = importlib.util.module_from_spec(spec)
spec.loader.exec_module(sr)


def disasm_arm(begin, blob):
    # The block starts with its JIT header (OffsetToBlockTail, 4 bytes); the code runs up to the tail.
    tail = int.from_bytes(blob[0:4], 'little')
    if 4 < tail <= len(blob):
        blob = blob[:tail]
    with tempfile.TemporaryDirectory() as d:
        src, obj = os.path.join(d, 'b.s'), os.path.join(d, 'b.o')
        with open(src, 'w') as f:
            f.write('.text\n')
            for i in range(0, len(blob) - len(blob) % 4, 4):
                f.write('.word 0x%08x\n' % int.from_bytes(blob[i:i + 4], 'little'))
        subprocess.run([sr.TOOLS + '/clang', '-c', '-target', 'arm64-apple-macos', '-o', obj, src], check=True, capture_output=True)
        out = subprocess.run([sr.TOOLS + '/llvm-objdump', '-d', '--no-show-raw-insn', obj], capture_output=True, text=True).stdout
    res = {}
    for l in out.splitlines():
        m = re.match(r'\s*([0-9a-f]+):\s+(.*)', l)
        if m and int(m.group(1), 16) >= 4:  # offsets (Mach-O objects ignore --adjust-vma); skip the header word
            res[begin + int(m.group(1), 16)] = m.group(2).split('//')[0].strip()
    return res


def classify(t):
    m = t.split()[0] if t else '?'
    ops = t[len(m):]
    if m == 'nop':
        return 'nop (TSO slot)'
    if m in ('ldapur', 'ldapurb', 'ldapurh', 'ldapr', 'ldaprb', 'ldaprh', 'ldar', 'ldarb', 'ldarh', 'ldapursw', 'ldapursh', 'ldapursb'):
        return 'TSO load'
    if m in ('stlur', 'stlurb', 'stlurh', 'stlr', 'stlrb', 'stlrh'):
        return 'TSO store'
    if m == 'add' and 'x24' in ops:
        return 'add guest base (x24)'
    if m in ('ldr', 'ldur', 'ldrb', 'ldrh', 'ldrsw', 'ldrsb', 'ldrsh', 'ldp', 'ldurb', 'ldurh', 'ldursw') and 'x28' in ops:
        return 'load CPU state (x28)'
    if m in ('str', 'stur', 'strb', 'strh', 'stp', 'sturb', 'sturh') and 'x28' in ops:
        return 'store CPU state (x28)'
    if m.startswith(('ld', 'st')) and ('v' in ops.split(',')[0] or 'q' in ops.split(',')[0]):
        return 'vector load/store'
    if m.startswith(('ldr', 'ldu', 'ldp', 'ldx', 'ldax', 'ldad', 'lda')):
        return 'other load'
    if m.startswith(('str', 'stu', 'stp', 'stx', 'stlx')):
        return 'other store'
    if m in ('adr', 'adrp'):
        return 'adr/adrp'
    if m in ('cset', 'csetm', 'csel', 'csinc', 'csinv', 'csneg', 'ccmp', 'ccmn', 'cinc', 'cneg', 'cinv'):
        return 'conditional select/compare'
    if m in ('rmif', 'setf8', 'setf16', 'cfinv', 'axflag', 'xaflag', 'mrs', 'msr') :
        return 'flags (rmif/setf/cfinv/nzcv)'
    if m in ('cmp', 'cmn', 'tst', 'subs', 'adds', 'ands', 'bics', 'negs', 'sbcs', 'adcs'):
        return 'flag-setting ALU'
    if m in ('b', 'bl') or m.startswith('b.') or m in ('cbz', 'cbnz', 'tbz', 'tbnz', 'br', 'blr', 'ret'):
        return 'branch'
    if m in ('udf', 'brk', 'hlt'):
        return 'trap'
    if m.startswith(('f', 'mov.', 'dup', 'ins', 'umov', 'smov', 'ext', 'zip', 'uzp', 'trn', 'tbl', 'tbx', 'rev', 'cnt', 'addp', 'uaddlv')) or 'v' in ops[:4]:
        return 'SIMD/FP'
    if m.startswith(('mov', 'orr', 'and', 'eor', 'bic', 'orn', 'eon', 'lsl', 'lsr', 'asr', 'ror', 'ubfx', 'sbfx', 'ubfm', 'sbfm', 'bfi', 'bfxil', 'extr', 'sxt', 'uxt')):
        return 'move/logic/bitfield'
    if m.startswith(('add', 'sub', 'neg', 'mul', 'madd', 'msub', 'umulh', 'smulh', 'udiv', 'sdiv', 'adc', 'sbc', 'umaddl', 'smaddl')):
        return 'arith'
    return 'other ' + m


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('path')
    ap.add_argument('--thread')
    ap.add_argument('--from', dest='t0', type=float)
    ap.add_argument('--to', dest='t1', type=float)
    a = ap.parse_args()
    import glob
    prefix = a.path
    if os.path.isdir(a.path):
        prefix = sorted(glob.glob(os.path.join(a.path, 'sample-*.bin')), key=os.path.getsize)[-1][:-4]
    threads, vmas, code, recs, meta = sr.load(prefix)
    hc = meta.get('hostcode', {})
    if not hc:
        sys.exit('no hostcode lines (needs a sampler with 64-byte records)')
    t0 = recs[0][0]
    insn = {}
    static = collections.Counter()
    per_block_insns = []
    for begin, (rip, blob) in hc.items():
        d = disasm_arm(begin, blob)
        insn.update(d)
        per_block_insns.append(len(d))
        for t in d.values():
            static[classify(t)] += 1
    st = sum(static.values())
    weighted = collections.Counter()
    n = covered = 0
    for r in recs:
        if r[8] != 1:
            continue
        t = (r[0] - t0) / 1e9
        if a.t0 is not None and t < a.t0 or a.t1 is not None and t > a.t1:
            continue
        if a.thread and not re.search(a.thread, f'{threads.get(r[6], "?")}:{r[6]}'):
            continue
        n += 1
        if len(r) > 10 and r[10] in hc:
            covered += 1
            weighted[classify(insn.get(r[1], ''))] += 1
    print(f'{len(hc)} host blocks dumped, {st} host instructions; JIT samples {n}, in dumped blocks {covered}')
    print(f'{"kind":34s}{"static":>9s}{"samples":>9s}')
    for k in sorted(set(static) | set(weighted), key=lambda k: -static[k]):
        print(f'{k:34s}{100 * static[k] / max(1, st):8.1f}%{100 * weighted[k] / max(1, covered):8.1f}%')


if __name__ == '__main__':
    main()
