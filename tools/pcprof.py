"""Symbolises build-host/pcprof.txt (TPM_PROF=2 host runs, see
source/pal/host/pcprof.c) with nm and prints the hottest functions.

    python -I tools/pcprof.py [build-host/pcprof.txt] [build-host/tpm_host.exe] [top]

Lifted guest functions appear as <module>_<guest address> (e.g. wmain_0047e34d),
so the guest address can be looked up in Ghidra directly."""
import bisect
import collections
import subprocess
import sys


def main():
    prof = sys.argv[1] if len(sys.argv) > 1 else 'build-host/pcprof.txt'
    exe = sys.argv[2] if len(sys.argv) > 2 else 'build-host/tpm_host.exe'
    top = int(sys.argv[3]) if len(sys.argv) > 3 else 40
    out = subprocess.run(['nm', '-n', '--defined-only', exe], capture_output=True, text=True).stdout
    syms = []
    image_base = None
    for line in out.splitlines():
        p = line.split()
        if len(p) == 3 and p[1] in 'tTwW':
            syms.append((int(p[0], 16), p[2]))
        if len(p) == 3 and p[2] == '__image_base__':
            image_base = int(p[0], 16)
    syms.sort()
    addrs = [a for a, _ in syms]
    if image_base is None:
        image_base = 0x140000000
    hist = collections.Counter()
    total = outside = 0
    for line in open(prof):
        if line.startswith('#'):
            f = line.split()
            total, outside = int(f[2]), int(f[4])
            continue
        n, off = line.split()
        a = image_base + int(off, 16)
        k = bisect.bisect_right(addrs, a) - 1
        hist[syms[k][1] if k >= 0 else '?'] += int(n)
    inside = total - outside
    print('%d samples, %d (%.1f%%) outside the exe (waiting in Windows)' % (total, outside, 100.0 * outside / max(total, 1)))
    groups = collections.Counter()
    for name, n in hist.items():
        g = name.split('_')[0] if name.startswith(('wmain_', 'bink_', 'imuse_')) else 'runtime'
        groups[g] += n
    print('busy samples by group: ' + ', '.join('%s %.1f%%' % (g, 100.0 * n / max(inside, 1)) for g, n in groups.most_common()))
    for name, n in hist.most_common(top):
        print('%6.2f%%  %s' % (100.0 * n / max(inside, 1), name))


if __name__ == '__main__':
    main()
