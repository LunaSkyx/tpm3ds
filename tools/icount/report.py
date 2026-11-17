"""Per-function report of build-icount/icount.txt (see instrument.py).

    python -I tools/icount/report.py [top]

Counts are executed x86 instructions of lifted code per rendered frame,
exclusive (callees not included). Native overrides are not counted."""
import collections
import os
import re
import sys

fn_re = re.compile(r'^void ((?:wmain|bink|imuse)_([0-9a-f]{8}))\(cpu_t \*cg\)')
ins_re = re.compile(r'^\s+/\* ([0-9a-f]{8}): (.*?) \*/')


def main():
    top = int(sys.argv[1]) if len(sys.argv) > 1 else 40
    owner, text = {}, {}
    for name in sorted(os.listdir('source/gen')):
        if not name.endswith('.c'):
            continue
        cur = None
        for line in open(os.path.join('source/gen', name), encoding='utf-8'):
            m = fn_re.match(line)
            if m:
                cur = m.group(1)
                continue
            m = ins_re.match(line)
            if m and cur:
                a = int(m.group(1), 16)
                owner.setdefault(a, cur)
                text[a] = m.group(2)
    frames = 1
    per_fn = collections.Counter()
    per_ins = collections.Counter()
    total = 0
    for line in open('build-icount/icount.txt'):
        if line.startswith('#'):
            frames = int(line.split()[2])
            continue
        n, a = line.split()
        n, a = int(n), int(a, 16)
        total += n
        per_fn[owner.get(a, '?')] += n
        per_ins[a] += n
    print('%d frames, %.0f x86 instructions per frame (lifted code only)' % (frames, total / frames))
    acc = 0
    for f, n in per_fn.most_common(top):
        acc += n
        print('%6.2f%% %6.2f%% cum %10.0f/frame  %s' % (100.0 * n / total, 100.0 * acc / total, n / frames, f))
    if '-i' in sys.argv:
        print('\nhottest instructions:')
        for a, n in per_ins.most_common(60):
            print('%6.2f%% %08x %-24s %s' % (100.0 * n / total, a, owner.get(a, '?'), text.get(a, '')))


if __name__ == '__main__':
    sys.exit(main())
