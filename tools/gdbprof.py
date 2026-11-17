"""PC-sampling profiler for the 3DS build running in Azahar, through the
emulator's GDB stub (Settings: use_gdbstub=true, port 24689; Azahar then waits
for this script before it starts the app).

    python -I tools/gdbprof.py [--start S] [--secs S] [--every MS] [--elf tpm3ds.elf] [--port 24689]

Waits --start seconds of wall time (boot, menu), then interrupts the emulated
CPU every --every ms for --secs seconds, reads the PC and lets it continue.
The histogram is symbolised with arm-none-eabi-nm from the ELF: lifted guest
functions show as <module>_<guest address>. Interrupting costs emulation time,
so absolute frame times are lower during sampling; shares are what count.
Every thread is read per sample; threads waiting in an svc are skipped."""
import argparse
import bisect
import collections
import os
import socket
import subprocess
import time

NM = r'C:\devkitPro\devkitARM\bin\arm-none-eabi-nm.exe'


class Gdb:
    def __init__(self, port):
        self.s = socket.create_connection(('127.0.0.1', port), timeout=60)
        self.buf = b''
        self.stops = 0

    def _read_packet(self):
        while True:
            i = self.buf.find(b'$')
            j = self.buf.find(b'#', i + 1) if i >= 0 else -1
            if i >= 0 and j >= 0 and len(self.buf) >= j + 3:
                pkt = self.buf[i + 1:j]
                self.buf = self.buf[j + 3:]
                self.s.sendall(b'+')
                return pkt
            d = self.s.recv(65536)
            if not d:
                raise EOFError('gdb stub closed')
            self.buf += d

    def send(self, cmd, reply=True):
        data = cmd.encode()
        pkt = b'$' + data + b'#' + ('%02x' % (sum(data) & 0xff)).encode()
        self.s.sendall(pkt)
        if not reply:
            return None
        while True:                     # late stop replies (T/S) are not this command's answer
            r = self._read_packet()
            if cmd == '?' or r[:1] not in (b'T', b'S'):
                return r

    def run_for(self, secs):
        """Let the target run; it stops by itself on events (thread start/exit
        etc.) - answer those with continue so it keeps running."""
        end = time.time() + secs
        while True:
            left = end - time.time()
            if left <= 0:
                return
            self.s.settimeout(left)
            try:
                d = self.s.recv(65536)
            except socket.timeout:
                self.s.settimeout(60)
                return
            finally:
                self.s.settimeout(60)
            if not d:
                raise EOFError('gdb stub closed')
            self.buf += d
            while b'#' in self.buf:
                r = self._read_packet()
                if r[:1] in (b'T', b'S'):
                    self.stops += 1
                    self.send('c', reply=False)

    def interrupt(self):
        self.s.sendall(b'\x03')
        while True:
            r = self._read_packet()
            if r[:1] in (b'T', b'S'):
                return r


def symbols(elf):
    out = subprocess.run([NM, '-n', '--defined-only', elf], capture_output=True, text=True).stdout
    syms = []
    for line in out.splitlines():
        p = line.split()
        if len(p) == 3 and p[1] in 'tTwW' and not p[2].startswith('$'):
            syms.append((int(p[0], 16), p[2]))
    syms.sort()
    return [a for a, _ in syms], [n for _, n in syms]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--start', type=float, default=130)
    ap.add_argument('--secs', type=float, default=30)
    ap.add_argument('--every', type=float, default=10)
    ap.add_argument('--elf', default='tpm3ds.elf')
    ap.add_argument('--port', type=int, default=24689)
    ap.add_argument('--top', type=int, default=60)
    ap.add_argument('--debug', action='store_true')
    ap.add_argument('--out', default=os.path.join(os.environ.get('TEMP', '.'), 'gdbprof.txt'))
    a = ap.parse_args()
    addrs, names = symbols(a.elf)
    for _ in range(120):
        try:
            g = Gdb(a.port)
            break
        except OSError:
            time.sleep(0.5)
    else:
        raise SystemExit('no gdb stub on port %d' % a.port)
    g.send('?')
    g.send('c', reply=False)
    g.run_for(a.start)
    pcs = collections.Counter()
    t_end = time.time() + a.secs
    while time.time() < t_end:
        g.run_for(a.every / 1000.0)
        g.interrupt()
        tids, r = [], g.send('qfThreadInfo')
        while r.startswith(b'm'):
            tids += r[1:].decode().split(',')
            r = g.send('qsThreadInfo')
        for tid in tids:
            hg = g.send('Hg' + tid)
            r = g.send('p f')
            if a.debug and len(pcs) < 3:
                print('tid', tid, 'Hg', hg, 'pc', r)
            try:
                pc = int.from_bytes(bytes.fromhex(r.decode()), 'little')
            except ValueError:
                continue
            k = bisect.bisect_right(addrs, pc) - 1
            if k >= 0 and names[k].startswith('svc'):
                continue                       # waiting in the kernel: not CPU time
            pcs[pc] += 1
        g.send('c', reply=False)
    hist = collections.Counter()
    for pc, n in pcs.items():
        k = bisect.bisect_right(addrs, pc) - 1
        hist[names[k] if k >= 0 else hex(pc)] += n
    total = sum(pcs.values())
    with open(a.out, 'w') as f:
        for pc, n in pcs.most_common():
            f.write('%d %08x\n' % (n, pc))
    groups = collections.Counter()
    for name, n in hist.items():
        groups[name.split('_')[0] if name.startswith(('wmain_', 'bink_', 'imuse_')) else 'runtime'] += n
    print('%d samples, %d self-stops; ' % (total, g.stops) + ', '.join('%s %.1f%%' % (k, 100.0 * n / total) for k, n in groups.most_common()))
    for name, n in hist.most_common(a.top):
        print('%6.2f%%  %s' % (100.0 * n / total, name))
    g.s.close()


if __name__ == '__main__':
    main()
