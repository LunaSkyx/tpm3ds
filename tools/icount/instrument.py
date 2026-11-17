"""Instruction-count build of the lifted code (host only).

    python -I tools/icount/instrument.py            # writes build-icount/gensrc/*.c

Every lifted x86 instruction (the '/* 00xxxxxx: insn */' comment the lifter
emits before its C) gets an ICNT(addr) counter in front of it. Linked with
tools/icount/icount.c into build-icount/tpm_icount.exe by tools/icount/Makefile.
Counts are per x86 instruction address; tools/icount/report.py aggregates them
per lifted function. Counters reset on the scenario's prof_reset."""
import os
import re
import sys

SRC = 'source/gen'
DST = 'build-icount/gensrc'
pat = re.compile(r'^(\s+)/\* ([0-9a-f]{8}): ')


def main():
    os.makedirs(DST, exist_ok=True)
    n = 0
    for name in sorted(os.listdir(SRC)):
        if not name.endswith('.c'):
            continue
        out = []
        with open(os.path.join(SRC, name), encoding='utf-8') as f:
            for line in f:
                out.append(line)
                m = pat.match(line)
                if m:
                    out.append('%sICNT(0x%su);\n' % (m.group(1), m.group(2)))
                    n += 1
        text = ''.join(out)
        text = '#include "icount.h"\n' + text
        with open(os.path.join(DST, name), 'w', encoding='utf-8') as f:
            f.write(text)
    print('instrumented %d instructions' % n)


if __name__ == '__main__':
    sys.exit(main())
