#!/usr/bin/env python3
"""x86 (PE32) -> C static recompiler, v0.

usage: python -I tools/lifter/lift.py <WMAIN.EXE> <out_dir> [--prefix wmain]

Generated code is derived from the player's own exe: it is written to
source/gen/ (gitignored) and must never be committed or edited by hand.
Fix this generator and regenerate instead.

v0 scope: integer ISA, control flow, switch tables, lazy flags with
liveness. x87 and rare instructions emit cpu_unimpl() so coverage is
measurable. See docs/PLAN.md.
"""
import os
import struct
import re
import sys
from collections import Counter, defaultdict

import capstone as cs
from capstone import x86 as X

# --------------------------------------------------------------------------- PE
class PE:
    def __init__(self, path, base=None):
        self.data = d = open(path, 'rb').read()
        pe = struct.unpack_from('<I', d, 0x3c)[0]
        nsec, optsz = struct.unpack_from('<H', d, pe + 6)[0], struct.unpack_from('<H', d, pe + 20)[0]
        opt = pe + 24
        self.base = struct.unpack_from('<I', d, opt + 28)[0]
        self.entry = self.base + struct.unpack_from('<I', d, opt + 16)[0]
        self.secs = []
        for i in range(nsec):
            o = opt + optsz + 40 * i
            name = d[o:o + 8].rstrip(b'\0').decode()
            vs, va, rs, ra = struct.unpack_from('<IIII', d, o + 8)
            self.secs.append((name, self.base + va, max(vs, rs), ra, rs))
        t = [s for s in self.secs if s[0] == '.text'][0]
        self.text_lo, self.text_hi = t[1], t[1] + t[2]
        imp_rva = struct.unpack_from('<I', d, opt + 104)[0]
        self.iat = {}                       # iat slot va -> 'dll!name'
        o = self.off(self.base + imp_rva)
        while True:
            oft, _, _, name, ft = struct.unpack_from('<IIIII', d, o)
            o += 20
            if not name:
                break
            dll = self.cstr(self.base + name)
            t_off, slot = self.off(self.base + (oft or ft)), self.base + ft
            while True:
                v = struct.unpack_from('<I', d, t_off)[0]
                if not v:
                    break
                fn = '#%d' % (v & 0xffff) if v & 0x80000000 else self.cstr(self.base + v + 2)
                self.iat[slot] = dll + '!' + fn
                t_off += 4
                slot += 4
        self.exports = {}                   # va -> name (or '#ordinal')
        exp_rva = struct.unpack_from('<I', d, opt + 96)[0]
        if exp_rva:
            e = self.off(self.base + exp_rva)
            obase, nfun, nnames, af, an, ao = struct.unpack_from('<IIIIII', d, e + 16)
            names = {}
            for k in range(nnames):
                ordi = struct.unpack_from('<H', d, self.off(self.base + ao) + 2 * k)[0]
                names[ordi] = self.cstr(self.base + struct.unpack_from('<I', d, self.off(self.base + an) + 4 * k)[0])
            for k in range(nfun):
                f = struct.unpack_from('<I', d, self.off(self.base + af) + 4 * k)[0]
                if f:
                    self.exports[self.base + f] = names.get(k, '#%d' % (obase + k))
        if base is not None and base != self.base:
            self.rebase(base, struct.unpack_from('<II', d, opt + 96 + 5 * 8))

    def rebase(self, new, reloc_dir):
        """Apply the base relocations so code is decoded and emitted at the
        address the runtime loader (core/pe_load.c) maps the image to."""
        rva, size = reloc_dir
        if not rva:
            raise SystemExit('lift: image needs rebasing but has no .reloc')
        delta, d = (new - self.base) & 0xffffffff, bytearray(self.data)
        p, end, n = self.off(self.base + rva), self.off(self.base + rva) + size, 0
        while p + 8 <= end:
            page, bsize = struct.unpack_from('<II', d, p)
            if bsize < 8:
                break
            for q in range(p + 8, p + bsize, 2):
                v = struct.unpack_from('<H', d, q)[0]
                if v >> 12 == 3:
                    o = self.off(self.base + page + (v & 0xfff))
                    struct.pack_into('<I', d, o, (struct.unpack_from('<I', d, o)[0] + delta) & 0xffffffff)
                    n += 1
                elif v >> 12:
                    raise SystemExit('lift: relocation type %d not supported' % (v >> 12))
            p += bsize
        sh = lambda va: (va + delta) & 0xffffffff
        self.data, self.base, self.entry = bytes(d), new, sh(self.entry)
        self.secs = [(nm, sh(va), sz, ra, rs) for nm, va, sz, ra, rs in self.secs]
        self.text_lo, self.text_hi = sh(self.text_lo), sh(self.text_hi)
        self.iat = {sh(k): v for k, v in self.iat.items()}
        self.exports = {sh(k): v for k, v in self.exports.items()}
        self.nrelocs = n

    def off(self, va):
        for _, sva, size, ra, rs in self.secs:
            if sva <= va < sva + size and va - sva < rs:
                return ra + va - sva
        return None

    def cstr(self, va):
        o = self.off(va)
        return self.data[o:self.data.index(b'\0', o)].decode('latin1')

    def u32(self, va):
        o = self.off(va)
        return None if o is None or o + 4 > len(self.data) else struct.unpack_from('<I', self.data, o)[0]

    def in_text(self, va):
        return self.text_lo <= va < self.text_hi

# ------------------------------------------------------------------- decoding
md = cs.Cs(cs.CS_ARCH_X86, cs.CS_MODE_32)
md.detail = True
_cache = {}

def insn_at(pe, va):
    if va in _cache:
        return _cache[va]
    o = pe.off(va)
    r = None
    if o is not None and pe.in_text(va):
        r = next(md.disasm(pe.data[o:o + 15], va, 1), None)
    _cache[va] = r
    return r

NORETURN_IMPORTS = {'KERNEL32.dll!ExitProcess', 'KERNEL32.dll!TerminateProcess',
                    'KERNEL32.dll!FatalAppExitA', 'KERNEL32.dll!RaiseException',
                    'KERNEL32.dll!ExitThread'}
JCC = {'jo', 'jno', 'jb', 'jae', 'je', 'jne', 'jbe', 'ja', 'js', 'jns', 'jp', 'jnp',
       'jl', 'jge', 'jle', 'jg'}

def mem_op(i):
    return next((o for o in i.operands if o.type == X.X86_OP_MEM), None)

def iat_target(pe, i):
    """'dll!name' if i is call/jmp dword ptr [iat_slot]."""
    m = mem_op(i)
    if m and m.mem.base == 0 and m.mem.index == 0 and len(i.operands) == 1:
        return pe.iat.get(m.mem.disp & 0xffffffff)
    return None

def switch_targets(pe, i, prev):
    """jmp [idx*4 + table]: recover the possible targets. The dispatch is emitted
    on the loaded target address, so index ranges need not start at 0 (MSVC
    memmove uses negative indices, memcpy skips index 0). Entries are taken
    while they point near the jump; two bad entries in a row end a direction.
    A preceding 'cmp idx, N' caps the forward scan."""
    m = mem_op(i)
    if not m or m.mem.scale != 4 or m.mem.index == 0 or m.mem.base != 0:
        return None
    table, idx = m.mem.disp & 0xffffffff, m.mem.index
    bound, p = None, prev.get(i.address)
    for _ in range(8):
        if p is None:
            break
        pi = insn_at(pe, p)
        if pi is not None and pi.mnemonic == 'cmp' and pi.operands[0].type == X.X86_OP_REG and            pi.operands[0].reg == idx and pi.operands[1].type == X.X86_OP_IMM:
            bound = pi.operands[1].imm & 0xffffffff
            break
        p = prev.get(p)

    def ok(v):
        return v is not None and pe.in_text(v) and abs(v - i.address) < 0x4000

    out = []
    for direction, limit in ((1, (bound + 1) if bound is not None and bound < 4096 else 1024), (-1, 16)):
        k, bad = (0 if direction == 1 else -1), 0
        while abs(k) < limit + (0 if direction == 1 else 1) and bad < 2:
            v = pe.u32(table + 4 * k)
            if ok(v):
                out.append(v)
                bad = 0
            else:
                bad += 1
            k += direction
    return sorted(set(out)) or None

class Func:
    def __init__(self, start):
        self.start = start
        self.insns = {}        # va -> capstone insn
        self.succ = {}         # va -> list of intra-function successor vas
        self.targets = set()   # vas that need a label
        self.switch = {}       # va -> list of targets
        self.calls = set()     # direct call targets
        self.tail = {}         # va -> direct tail-call target
        self.code_refs = set() # immediates/displacements pointing into .text (callbacks)

def explore(pe, start, funcs):
    f = Func(start)
    work, prev = [start], {}
    while work:
        va = work.pop()
        while va not in f.insns:
            i = insn_at(pe, va)
            if i is None:
                f.insns[va] = None
                f.succ[va] = []
                break
            f.insns[va] = i
            nxt, mn, succ = va + i.size, i.mnemonic, []
            if mn not in JCC and mn not in ('jmp', 'call', 'loop', 'jecxz'):
                for o in i.operands:
                    v = (o.imm if o.type == X.X86_OP_IMM else o.mem.disp if o.type == X.X86_OP_MEM else 0) & 0xffffffff
                    if pe.in_text(v):
                        f.code_refs.add(v)
            op0 = i.operands[0] if i.operands else None
            direct = op0 is not None and op0.type == X.X86_OP_IMM
            if mn in ('ret', 'retf', 'int3', 'hlt', 'ud2'):
                pass
            elif mn == 'jmp':
                if direct:
                    t = op0.imm & 0xffffffff
                    if t in funcs and t != start:
                        f.tail[va] = t
                    else:
                        succ = [t]
                else:
                    tg = switch_targets(pe, i, prev)
                    if tg:
                        f.switch[va] = tg
                        succ = sorted(set(tg))
            elif mn in JCC or mn in ('jecxz', 'loop'):
                succ = [op0.imm & 0xffffffff, nxt]
            elif mn == 'call':
                if direct:
                    f.calls.add(op0.imm & 0xffffffff)
                if iat_target(pe, i) not in NORETURN_IMPORTS:
                    succ = [nxt]
            else:
                succ = [nxt]
            succ = [s for s in succ if pe.in_text(s)]
            f.succ[va] = succ
            for s in succ:
                prev.setdefault(s, va)
            if len(succ) == 1 and succ[0] == nxt and mn not in JCC:
                va = nxt
                continue
            for s in succ:
                if s != nxt or mn in JCC:
                    f.targets.add(s)
                work.append(s)
            break
    return f

# Instructions a 1999 Win32 game never executes: seeing one means we are
# decoding data. String ops with es: are normal and not listed here.
JUNK_MN = {'in', 'out', 'insb', 'insd', 'outsb', 'outsd', 'arpl', 'bound', 'aaa', 'aas', 'aad', 'aam',
           'daa', 'das', 'cli', 'sti', 'hlt', 'ljmp', 'lcall', 'les', 'lds', 'lss', 'into', 'salc',
           'sldt', 'sgdt', 'sidt', 'lgdt', 'lidt', 'iretd', 'retf', 'int1', 'xlatb', 'icebp'}
SEGREGS = {'es', 'cs', 'ss', 'ds', 'fs', 'gs'}


def is_junk(f):
    """True if the function body decodes into data-like instructions."""
    for i in f.insns.values():
        if i is None:
            return True
        mn = i.mnemonic.split()[-1]
        if mn in JUNK_MN:
            return True
        if mn in ('push', 'pop', 'mov') and any(o.type == X.X86_OP_REG and i.reg_name(o.reg) in SEGREGS
                                                for o in i.operands):
            return True
    first = f.insns.get(f.start)
    # "add byte ptr [reg], al" (00 xx) is the classic signature of zero data
    return first is not None and first.bytes[0] == 0x00


def discover(pe):
    """Recursive descent from the entry point. Direct call targets of trusted
    functions are trusted. Code pointers found in data or in instruction
    operands are only seeds: they are kept (and their callees followed)
    only if their body does not decode as junk."""
    funcs, known, rejected = {}, {pe.entry} | {v for v in pe.exports if pe.in_text(v)}, set()

    def run(start, trusted):
        queue = [(start, trusted)]
        while queue:
            s, tr = queue.pop()
            if s in funcs or s in rejected:
                continue
            f = explore(pe, s, known)
            if not tr and is_junk(f):
                rejected.add(s)
                continue
            funcs[s] = f
            for t in f.calls:
                if pe.in_text(t) and t not in known:
                    known.add(t)
                    queue.append((t, True))

    run(pe.entry, True)
    for v in sorted(pe.exports):            # DLL exports are entry points too
        if pe.in_text(v):
            run(v, True)
    covered = set()
    for f in funcs.values():
        covered.update(f.insns)

    def try_seed(v):
        if v in covered or v in known or v in rejected or insn_at(pe, v) is None:
            return False
        known.add(v)
        run(v, False)
        if v in funcs:
            covered.update(funcs[v].insns)
            return True
        known.discard(v)
        return False

    # code pointers in data (vtables, callbacks, function tables)
    for name, sva, size, ra, rs in pe.secs:
        if name == '.text' or not rs:
            continue
        for k in range(0, rs - 3, 4):
            v = struct.unpack_from('<I', pe.data, ra + k)[0]
            if pe.in_text(v):
                try_seed(v)
    # code pointers in instruction operands (push offset callback; mov [x], offset fn)
    changed = True
    while changed:
        changed = False
        for v in sorted(set().union(*(f.code_refs for f in funcs.values()))):
            changed |= try_seed(v)
    discover.rejected = len(rejected)
    return funcs

# ----------------------------------------------------------------- liveness
FLAG_READERS = JCC | {'adc', 'sbb', 'setb', 'setae', 'sete', 'setne', 'setbe', 'seta', 'sets',
                      'setns', 'setp', 'setnp', 'setl', 'setge', 'setle', 'setg', 'seto', 'setno',
                      'pushfd', 'pushf', 'lahf', 'rcl', 'rcr', 'cmc', 'into', 'salc'}
FLAG_KILLERS = {'add', 'sub', 'cmp', 'and', 'or', 'xor', 'test', 'neg', 'adc', 'sbb', 'imul', 'mul',
                'sahf', 'popfd', 'popf', 'call', 'cmpsb', 'cmpsd', 'scasb', 'scasd'}
SHIFTS = {'shl', 'sal', 'shr', 'sar'}

def base_mn(i):
    """Mnemonic without rep/repne prefix words (capstone folds them in)."""
    return i.mnemonic.split()[-1]

def reads_flags(i):
    if base_mn(i) in FLAG_READERS or base_mn(i) in ('inc', 'dec', 'rol', 'ror', 'bt', 'bts', 'btr', 'btc'):
        return True
    if i.mnemonic in SHIFTS and i.operands[1].type != X.X86_OP_IMM:
        return True                       # shift by cl: count 0 keeps old flags
    return i.prefix[0] in (X.X86_PREFIX_REP, X.X86_PREFIX_REPNE) and base_mn(i).startswith(('cmps', 'scas'))

def kills_flags(i):
    if base_mn(i) in FLAG_KILLERS:
        return True
    return i.mnemonic in SHIFTS and i.operands[1].type == X.X86_OP_IMM and (i.operands[1].imm & 31) != 0

# Interprocedural flag facts (MSVC CRT asm returns results in EFLAGS, and some
# helpers read the caller's flags on entry). Filled by flag_fixpoint().
LIVE_ENTRY = set()     # functions that read flags before writing them
RET_LIVE = set()       # functions whose callers read flags after the call returns


def direct_target(i):
    o = i.operands[0] if i.operands else None
    return (o.imm & 0xffffffff) if o is not None and o.type == X.X86_OP_IMM else None


def liveness(f, want_in=False):
    """live_out[va]: may any flag written by insn va be read later?"""
    live_in = {va: False for va in f.insns}
    ret_live = f.start in RET_LIVE
    changed = True
    while changed:
        changed = False
        for va in sorted(f.insns, reverse=True):
            i = f.insns[va]
            if i is None:
                continue
            mn = base_mn(i)
            r = reads_flags(i)
            if mn in ('call', 'jmp') and direct_target(i) in LIVE_ENTRY and direct_target(i) not in f.insns:
                r = True
            if mn == 'ret' and ret_live:
                r = True
            out = any(live_in.get(s, False) for s in f.succ[va])
            new = r or (out and not kills_flags(i))
            if new != live_in[va]:
                live_in[va] = new
                changed = True
    live_out = {va: any(live_in.get(s, False) for s in f.succ[va]) for va in f.insns}
    return (live_out, live_in) if want_in else live_out


def flag_fixpoint(funcs):
    while True:
        n0 = (len(LIVE_ENTRY), len(RET_LIVE))
        for f in funcs.values():
            _, li = liveness(f, True)
            if li.get(f.start):
                LIVE_ENTRY.add(f.start)
            for va, i in f.insns.items():
                if i is None:
                    continue
                t = direct_target(i) if base_mn(i) in ('call', 'jmp') else None
                if t is None or t not in funcs or t in f.insns:
                    continue
                if base_mn(i) == 'call' and li.get(va + i.size):
                    RET_LIVE.add(t)
                if base_mn(i) == 'jmp' and f.start in RET_LIVE:      # tail call returns to our caller
                    RET_LIVE.add(t)
        if (len(LIVE_ENTRY), len(RET_LIVE)) == n0:
            return

# ----------------------------------------------------------------- emission
R32 = ['eax', 'ecx', 'edx', 'ebx', 'esp', 'ebp', 'esi', 'edi']
REG = {}
for k, n in enumerate(R32):
    REG[n] = (k, 32)
for k, n in enumerate(['ax', 'cx', 'dx', 'bx', 'sp', 'bp', 'si', 'di']):
    REG[n] = (k, 16)
for k, n in enumerate(['al', 'cl', 'dl', 'bl']):
    REG[n] = (k, 8)
for k, n in enumerate(['ah', 'ch', 'dh', 'bh']):
    REG[n] = (k, 'h')
RN = ['EAX', 'ECX', 'EDX', 'EBX', 'ESP', 'EBP', 'ESI', 'EDI']
MASK = {8: '0xffu', 16: '0xffffu', 32: '0xffffffffu'}
STYPE = {8: 'int8_t', 16: 'int16_t', 32: 'int32_t'}
UTYPE = {8: 'uint8_t', 16: 'uint16_t', 32: 'uint32_t'}

class Unsupported(Exception):
    pass

class Ctx:
    def __init__(self, i):
        self.i = i
        self.name = lambda r: i.reg_name(r)

    def addr(self, o):
        m, parts = o.mem, []
        if m.base:
            parts.append('R(%s)' % RN[REG[self.name(m.base)][0]])
        if m.index:
            idx = 'R(%s)' % RN[REG[self.name(m.index)][0]]
            parts.append(idx if m.scale == 1 else '%s * %du' % (idx, m.scale))
        if m.disp or not parts:
            parts.append('0x%xu' % (m.disp & 0xffffffff))
        if m.segment and self.name(m.segment) == 'fs':
            parts.append('c->fs_base')
        elif m.segment and self.name(m.segment) not in ('ds', 'ss', 'es', 'cs'):
            raise Unsupported('segment ' + self.name(m.segment))
        return '(uint32_t)(' + ' + '.join(parts) + ')'

    def load(self, o, bits=None):
        bits = bits or o.size * 8
        if o.type == X.X86_OP_REG:
            n = self.name(o.reg)
            if n not in REG:
                raise Unsupported('reg ' + n)
            k, kind = REG[n]
            if kind == 32: return 'R(%s)' % RN[k]
            if kind == 16: return '(R(%s) & 0xffffu)' % RN[k]
            if kind == 8:  return '(R(%s) & 0xffu)' % RN[k]
            return '((R(%s) >> 8) & 0xffu)' % RN[k]
        if o.type == X.X86_OP_IMM:
            return '0x%xu' % (o.imm & int(MASK[bits][:-1], 16))
        return 'gmem_rd%d(ea)' % (o.size * 8)

    def store(self, o, val):
        if o.type == X.X86_OP_REG:
            n = self.name(o.reg)
            if n not in REG:
                raise Unsupported('reg ' + n)
            k, kind = REG[n]
            if kind == 32: return 'R(%s) = %s;' % (RN[k], val)
            if kind == 16: return 'set16(c, %s, %s);' % (RN[k], val)
            if kind == 8:  return 'set8l(c, %s, %s);' % (RN[k], val)
            return 'set8h(c, %s, %s);' % (RN[k], val)
        if o.type == X.X86_OP_MEM:
            return 'gmem_wr%d(ea, (%s)(%s));' % (o.size * 8, UTYPE[o.size * 8], val)
        raise Unsupported('store to imm')

def sx(expr, bits):
    return '(uint32_t)(int32_t)(%s)(%s)' % (STYPE[bits], expr) if bits < 32 else expr

COND_LAZY = {j[1:]: 'CC_%s(c)' % j[1:] for j in JCC}

def fused_cond(cc, kind, a, b, bits):
    """Direct C condition for cmp a,b / test a,b followed by j<cc>; None if not fusable."""
    u, s = UTYPE[bits], STYPE[bits]
    if kind == 'cmp':
        table = {'e': '%s == %s', 'ne': '%s != %s'}
        ops = {'e': '==', 'ne': '!=', 'b': '<', 'ae': '>=', 'be': '<=', 'a': '>',
               'l': '<', 'ge': '>=', 'le': '<=', 'g': '>'}
        if cc in ('e', 'ne', 'b', 'ae', 'be', 'a'):
            return '(%s)(%s) %s (%s)(%s)' % (u, a, ops[cc], u, b)
        if cc in ('l', 'ge', 'le', 'g'):
            return '(%s)(%s) %s (%s)(%s)' % (s, a, ops[cc], s, b)
        if cc in ('s', 'ns'):
            return '(%s)((%s) - (%s)) %s 0' % (s, a, b, '<' if cc == 's' else '>=')
        return None
    # test: r = a & b, CF = OF = 0, so the signed conditions reduce to the sign of r
    r = '(%s)((%s) & (%s))' % (s, a, b)
    fmt = {'e': '%s == 0', 'ne': '%s != 0', 'be': '%s == 0', 'a': '%s != 0',
           's': '%s < 0', 'ns': '%s >= 0', 'l': '%s < 0', 'ge': '%s >= 0',
           'le': '%s <= 0', 'g': '%s > 0'}.get(cc)
    return fmt % r if fmt else None

ALU = {'add': ('+', 'FL_ADD'), 'sub': ('-', 'FL_SUB'), 'cmp': ('-', 'FL_SUB'),
       'and': ('&', 'FL_LOGIC'), 'or': ('|', 'FL_LOGIC'), 'xor': ('^', 'FL_LOGIC'),
       'test': ('&', 'FL_LOGIC')}

def L(va):
    return 'L_%08x' % va

def fn_name(va, prefix):
    return '%s_%08x' % (prefix, va)

# Hand-written guest functions (source/core/native_overrides.c). The lifter still
# analyses them like every other function (liveness, x87 static depth, bodies),
# but the body is emitted under <name>_ref inside TPM_OVERRIDE_REF for the
# differential test, and all call sites and the dispatch table use the
# hand-written <name>. Per module prefix (OVERRIDES_BY_PREFIX).
OVERRIDES = {
    0x00406300,   # scene object -> depth bucket insert (source/core/native_actor.c)
    0x0049c740,   # CRT _CIcos wrapper fast path (source/core/native_actor.c)
    0x0049c7f0,   # CRT _CIsin wrapper fast path
    0x0049a44c,   # CRT __ftol (source/core/native_actor.c)
    0x00494238,   # sincos(angle, *sin, *cos) in floats
    0x004074d4,   # packed 0xAABBGGRR from 3 floats
    0x0040e96b,   # vertex permutation gather
    0x004064b0,   # per-sprite submit: project/clip, vertex+palette buffers
    0x004075c6,   # material floats -> out[0..3] + record depth terms
    0x0041af30,   # sprite/light descriptor
    0x0047ceb9,   # 4x3 inverse: transpose 3x3 + negated translation
    0x0047dad6,   # 4x3 affine multiply
    0x0047e263,   # 3x3 (matrix row) * vector
    0x0047e40a,   # transform 3-vector in place by a 4x3 matrix
    0x0047dee9,   # 4x3 affine multiply, in place
    0x0047e34d,   # 4x3 transform point
    0x00486245,   # vector normalise
    0x00487f90,   # depth comparator
    0x004199b0,   # world_cull (source/core/native_world.c, docs/PERF_PLAN.md section 3)
    0x0040fc10,   # actor vertex lighting (source/core/native_actor.c)
    0x0041ae50,   # animated-texture frame selector (source/core/native_misc.c)
    0x00401eb8,   # object-key list insert (source/core/native_misc.c)
    0x004940ab,   # angle wrap to [0,360) (source/core/native_angle.c)
    0x0049418b,   # angle wrap to (-180,180]
    0x004941c0,   # shortest signed angle difference
    0x0040f170,   # aggregate a byte over an object's parts (source/core/native_scene.c)
    0x0047cac0,   # 3x3 rotation matrix from Euler angles
    0x00494272,   # sin_deg wrapper around CIsin (native_actor.c)
    0x0040efd0,   # actor vertex projection (source/core/native_actor.c)
    0x00402155,   # deferred actor polygon flush (source/core/native_actor.c)
    0x0040f1e0,   # actor transform (source/core/native_actor.c; rare polygon kinds fall back to the lifted body)
    0x00422130,   # actor AI sweep (source/core/native_ai.c; epilogue contract see there)
    0x00407d26,   # nearby-object gather (source/core/native_407d26.c)
    0x004616cc,   # RLE 16-bit sprite blitter, the menu's CPU 2D drawing (source/core/native_4616cc.c)
    0x00404730,   # skeleton/bone-hierarchy update (source/core/native_404730.c)
    0x00483e60,   # skinned-vertex transform (source/core/native_483e60.c)
    0x00487260,   # per-draw wrapper (source/core/native_487260.c)
    0x00406830,   # world_emit (source/core/native_406830.c; its cost is 0x41b070's)
    0x004500b0,   # player death routine, skipped under God / Ghost (source/core/native_4500b0.c)
    0x00418544,   # updateCam: Ghost feeds the camera follow target first (source/core/native_418544.c)
    0x00471c5c,   # extract one LAB resource into save/cache: skipped when the file is still on the card (source/core/native_471c5c.c)
}
ALWAYS_REF = {0x0040f1e0, 0x00402155, 0x00406300, 0x0049c740, 0x0049c7f0, 0x0049a44c, 0x00494238, 0x0041ae50, 0x00401eb8, 0x004940ab, 0x0049418b, 0x004941c0, 0x0040f170, 0x0047cac0, 0x00494272, 0x00422130, 0x00407d26, 0x00404730, 0x00483e60, 0x00487260, 0x00406830, 0x004616cc, 0x004500b0, 0x00418544, 0x00471c5c}   # overrides whose lifted body is also compiled into release builds (runtime fallback)
IMUSE_OVERRIDES = {
    0x00922630,   # VIMA/ADPCM block decoder (source/core/native_imuse.c)
}
# Widescreen (docs/PLAN.md): the level/cutscene flow in FUN_0043eb2a forces 640x480 with
# immediate pushes; they become the screen size the HLE reports (core/video/video.h).
PUSH_SCREEN = {
    0x0043edef: 'g_screen_h', 0x0043edf4: 'g_screen_w',
    0x0043ef0f: 'g_screen_h', 0x0043ef14: 'g_screen_w',
    0x0046c3ac: 'g_screen_h', 0x0046c3b1: 'g_screen_w',   # movie player (FUN_0046c35a)
}
OVERRIDES_BY_PREFIX = {'wmain': OVERRIDES, 'imuse': IMUSE_OVERRIDES,
                      'bink': set()}   # DISABLED: the audio-gate natives hang the movies (see docs/PLAN.md section 38)

# Inclusive timers (build with -DTPM_FN_TIMERS; runtime in core/prof.c): the
# body of each listed function is renamed <name>_tm and <name> becomes a
# wrapper that adds its ticks to timer <index>. Index 0 must be the per-frame
# function: core/prof.c reports all timers every 300 calls of it. Time spent
# in other guest threads while the timed function waits for the run token is
# included (inclusive wall ticks).
FN_TIMERS = [
    ('wmain', 0x0043e9f2, 'frame'),
    ('wmain', 0x00475b75, 'limiter'),
    ('wmain', 0x0043f57a, 'render'),
    ('wmain', 0x004199b0, ' world_cull'),
    ('wmain', 0x00406830, ' world_emit'),
    ('wmain', 0x00404730, ' skeleton'),
    ('wmain', 0x00484901, 'render_tail'),
    ('wmain', 0x00411028, 'obj_update'),
    ('wmain', 0x00402155, ' actor_emit'),
    ('wmain', 0x0040f1e0, ' actor_xform'),
    ('wmain', 0x0040fc10, ' actor_light'),
    ('wmain', 0x00417930, ' obj_17930'),
    ('wmain', 0x0040fe70, '  o_fe70'),
    ('wmain', 0x0040901d, '   x_901d'),
    ('wmain', 0x0040801d, '   x_801d'),
    ('wmain', 0x00483e60, '   x_83e60'),
    ('wmain', 0x00407d26, '   x_7d26'),
    ('wmain', 0x004100a0, '   x_100a0'),
    ('wmain', 0x0047840e, '   x_7840e'),
    ('wmain', 0x004148af, '   u_4148af'),
    ('wmain', 0x0047e14b, '   u_47e14b'),
    ('wmain', 0x0041c910, '   u_41c910'),
    ('wmain', 0x0041401a, '   u_41401a'),
    ('wmain', 0x00412c20, '   u_412c20'),
    ('wmain', 0x0041c4b0, '   u_41c4b0'),
    ('wmain', 0x00483d20, '   u_483d20'),
    ('wmain', 0x0049a660, '   u_qsort'),
    ('wmain', 0x00426ef7, '  o_26ef7'),
    ('wmain', 0x0043a5ff, ' obj_3a5ff'),
    ('wmain', 0x00422130, 'ai_22130'),
    ('wmain', 0x0040a7d1, 'anim_0a7d1'),
    ('imuse', 0x00921c70, 'imuse_decode'),
    # world_emit internals (docs/asm/0041b070_listing.txt); inclusive, callees overlap
    ('wmain', 0x0041b070, ' emit_core'),
    ('wmain', 0x004025b0, '  emit_4025b0'),
    ('wmain', 0x00487260, '  emit_487260'),
    ('wmain', 0x00487c50, '  emit_487c50'),
    ('wmain', 0x0041c620, '  emit_41c620'),
    ('wmain', 0x0041bab0, '  emit_41bab0'),
    ('wmain', 0x00488210, '  emit_488210'),
    ('wmain', 0x00488510, '  emit_488510'),
    ('wmain', 0x00487be0, '  emit_487be0'),
    # frame function direct callees (perf phase 14)
    ('wmain', 0x004181b9, 'f_4181b9'),
    ('wmain', 0x00401d30, 'f_401d30'),
    ('wmain', 0x0046c108, 'f_46c108'),
    ('wmain', 0x004756fc, 'f_4756fc'),
    ('wmain', 0x0046f4a9, 'f_46f4a9'),
    ('wmain', 0x0046c139, 'f_46c139'),
    ('wmain', 0x00487c00, ' f_487c00'),
    ('wmain', 0x0048f052, ' f_48f052'),
    ('wmain', 0x00475be1, 'f_475be1'),
]

class Emitter:
    def __init__(self, pe, funcs, prefix):
        self.pe, self.funcs, self.prefix = pe, funcs, prefix
        self.unimpl = Counter()
        self.overrides = OVERRIDES_BY_PREFIX.get(prefix, set())

    def call_target(self, t):
        return '%s(c);' % fn_name(t, self.prefix) if t in self.funcs else 'cpu_call(c, 0x%08xu);' % t

    def insn(self, f, i, live_out, fuse):
        """Returns C statements for one instruction (may raise Unsupported)."""
        mn, ops, cx = base_mn(i), i.operands, Ctx(i)
        nxt = i.address + i.size
        m = mem_op(i)
        pre = ['uint32_t ea = %s;' % cx.addr(m)] if m is not None and mn != 'lea' else []
        if i.prefix[0] in (X.X86_PREFIX_REP, X.X86_PREFIX_REPNE) and not mn.startswith(('movs', 'stos', 'cmps', 'scas', 'lods', 'ret')):
            raise Unsupported('rep ' + mn)
        bits = ops[0].size * 8 if ops else 32

        if mn in ('nop', 'wait', 'fwait'):
            return []
        if mn.startswith('f'):
            return self.fpu(i, mn, pre)
        if mn in ('mov', 'movzx'):
            return pre + [cx.store(ops[0], cx.load(ops[1]))]
        if mn == 'movsx':
            return pre + [cx.store(ops[0], sx(cx.load(ops[1]), ops[1].size * 8))]
        if mn == 'lea':
            return [cx.store(ops[0], cx.addr(ops[1]))]
        if mn in ('push', 'pop') and ops[0].type == X.X86_OP_REG and i.reg_name(ops[0].reg) in SEGREGS:
            # flat model: one code and one data selector (Win32 values); pop discards
            if mn == 'pop':
                return ['R(ESP) += 4u;']
            sel = {'cs': 0x1b, 'fs': 0x3b, 'gs': 0}.get(i.reg_name(ops[0].reg), 0x23)
            return ['PUSH(c, 0x%xu);' % sel]
        if mn == 'push':
            if ops[0].size != 4:
                raise Unsupported('push16')
            v = cx.load(ops[0]) if ops[0].type != X.X86_OP_IMM else '0x%xu' % (ops[0].imm & 0xffffffff)
            if self.prefix == 'wmain' and i.address in PUSH_SCREEN:
                v = PUSH_SCREEN[i.address]
            return pre + ['PUSH(c, %s);' % v]
        if mn == 'pop':
            if m is not None:
                return ['uint32_t v = POP(c);', 'uint32_t ea = %s;' % cx.addr(m), cx.store(ops[0], 'v')]
            return ['uint32_t v = POP(c);', cx.store(ops[0], 'v')]
        if mn in ALU:
            op, fl = ALU[mn]
            a, b = cx.load(ops[0]), cx.load(ops[1], bits)
            out = pre + ['uint32_t a = %s, b = %s, r = (a %s b) & %s;' % (a, b, op, MASK[bits])]
            if mn not in ('cmp', 'test'):
                out.append(cx.store(ops[0], 'r'))
            if fuse:
                f.fused[i.address] = (mn if mn in ('cmp', 'test') else None, a, b, bits)
            if live_out and not fuse:
                out.append('FLAGS(c, %s, %d, a, b, r);' % (fl, bits))
            return out
        if mn in ('adc', 'sbb'):
            op = '+' if mn == 'adc' else '-'
            out = pre + ['uint32_t a = %s, b = %s, ci = (uint32_t)cpu_cf(c), r = (a %s b %s ci) & %s;'
                         % (cx.load(ops[0]), cx.load(ops[1], bits), op, op, MASK[bits]), cx.store(ops[0], 'r')]
            if live_out:
                out += ['c->fcf = ci;', 'FLAGS(c, %s, %d, a, b, r);' % ('FL_ADC' if mn == 'adc' else 'FL_SBB', bits)]
            return out
        if mn in ('inc', 'dec'):
            op = '+' if mn == 'inc' else '-'
            out = pre + ['uint32_t a = %s, r = (a %s 1u) & %s;' % (cx.load(ops[0]), op, MASK[bits]), cx.store(ops[0], 'r')]
            if live_out:
                out += ['c->fcf = (uint32_t)cpu_cf(c);', 'FLAGS(c, %s, %d, a, 1u, r);' % ('FL_INC' if mn == 'inc' else 'FL_DEC', bits)]
            return out
        if mn == 'neg':
            out = pre + ['uint32_t a = %s, r = (0u - a) & %s;' % (cx.load(ops[0]), MASK[bits]), cx.store(ops[0], 'r')]
            if live_out:
                out.append('FLAGS(c, FL_NEG, %d, a, 0u, r);' % bits)
            return out
        if mn == 'not':
            return pre + [cx.store(ops[0], '~%s & %s' % (cx.load(ops[0]), MASK[bits]))]
        if mn in SHIFTS:
            cnt = ('0x%xu' % (ops[1].imm & 31)) if ops[1].type == X.X86_OP_IMM else '(R(ECX) & 31u)'
            if mn in ('shl', 'sal'):
                expr, fl = '(a << n) & %s' % MASK[bits], 'FL_SHL'
            elif mn == 'shr':
                expr, fl = 'a >> n', 'FL_SHR'
            else:
                expr, fl = '(uint32_t)((int32_t)%s >> n) & %s' % (sx('a', bits), MASK[bits]), 'FL_SAR'
            out = pre + ['uint32_t a = %s, n = %s;' % (cx.load(ops[0]), cnt),
                         'if (n) { uint32_t r = %s; %s%s }' % (expr, cx.store(ops[0], 'r'),
                         (' FLAGS(c, %s, %d, a, n, r);' % (fl, bits)) if live_out else '')]
            return out
        if mn in ('rol', 'ror'):
            cnt = ('0x%xu' % (ops[1].imm & 31)) if ops[1].type == X.X86_OP_IMM else '(R(ECX) & 31u)'
            rot = '((a << n) | (a >> (%d - n)))' % bits if mn == 'rol' else '((a >> n) | (a << (%d - n)))' % bits
            cf = 'r & 1u' if mn == 'rol' else '(r >> %d) & 1u' % (bits - 1)
            flag = (' cpu_flags(c); c->eflags = (c->eflags & ~F_CF) | (%s);' % cf) if live_out else ''
            return pre + ['uint32_t a = %s, n = %s %% %du;' % (cx.load(ops[0]), cnt, bits),
                          'if (n) { uint32_t r = %s & %s; %s%s }' % (rot, MASK[bits], cx.store(ops[0], 'r'), flag)]
        if mn in ('imul', 'mul'):
            signed = mn == 'imul'
            if len(ops) == 1:
                src = cx.load(ops[0])
                if bits == 32:
                    t = 'int64_t' if signed else 'uint64_t'
                    cast = '(int64_t)(int32_t)' if signed else '(uint64_t)'
                    out = pre + ['%s p = %sR(EAX) * %s%s;' % (t, cast, cast, src),
                                 'R(EAX) = (uint32_t)p; R(EDX) = (uint32_t)((uint64_t)p >> 32);']
                    ovf = 'p != (int64_t)(int32_t)p' if signed else '(p >> 32) != 0'
                elif bits == 8:
                    if signed:
                        out = pre + ['int32_t p = (int32_t)(int8_t)R(EAX) * (int32_t)(int8_t)%s;' % src]
                        ovf = 'p != (int32_t)(int8_t)p'
                    else:
                        out = pre + ['uint32_t p = (R(EAX) & 0xffu) * %s;' % src]
                        ovf = '(p >> 8) != 0'
                    out.append('set16(c, EAX, (uint32_t)p);')
                else:
                    raise Unsupported(mn + '16')
            else:
                if not signed:
                    raise Unsupported('mul 2op')
                a, b = (cx.load(ops[0]), cx.load(ops[1])) if len(ops) == 2 else (cx.load(ops[1]), '0x%xu' % (ops[2].imm & 0xffffffff))
                if bits != 32:
                    raise Unsupported('imul16')
                out = pre + ['int64_t p = (int64_t)(int32_t)%s * (int64_t)(int32_t)%s;' % (a, b),
                             cx.store(ops[0], '(uint32_t)p')]
                ovf = 'p != (int64_t)(int32_t)p'
            if live_out:
                out += ['c->fcf = (uint32_t)(%s);' % ovf, 'FLAGS(c, FL_MUL, %d, 0u, 0u, (uint32_t)p);' % bits]
            return out
        if mn in ('div', 'idiv'):
            src = cx.load(ops[0])
            if bits == 32:
                if mn == 'div':
                    body = ['uint64_t n = ((uint64_t)R(EDX) << 32) | R(EAX); uint32_t d = %s;' % src,
                            'if (!d || n / d > 0xffffffffu) { cpu_trap(c, 0x%08xu, "divide error"); return; }' % i.address,
                            'R(EAX) = (uint32_t)(n / d); R(EDX) = (uint32_t)(n % d);']
                else:
                    body = ['int64_t n = (int64_t)(((uint64_t)R(EDX) << 32) | R(EAX)); int32_t d = (int32_t)%s;' % src,
                            'if (!d || (d == -1 && n == INT64_MIN) || n / d > INT32_MAX || n / d < INT32_MIN) { cpu_trap(c, 0x%08xu, "divide error"); return; }' % i.address,
                            'R(EAX) = (uint32_t)(int32_t)(n / d); R(EDX) = (uint32_t)(int32_t)(n % d);']
                return pre + body
            raise Unsupported(mn + str(bits))
        if mn == 'cdq':
            return ['R(EDX) = (uint32_t)((int32_t)R(EAX) >> 31);']
        if mn == 'cwde':
            return ['R(EAX) = (uint32_t)(int32_t)(int16_t)R(EAX);']
        if mn == 'cbw':
            return ['set16(c, EAX, (uint32_t)(int16_t)(int8_t)R(EAX));']
        if mn == 'xchg':
            a, b = cx.load(ops[0]), cx.load(ops[1])
            return pre + ['uint32_t a = %s, b = %s;' % (a, b), cx.store(ops[0], 'b'), cx.store(ops[1], 'a')]
        if mn == 'leave':
            return ['R(ESP) = R(EBP);', 'R(EBP) = POP(c);']
        if mn.startswith('set') and mn[3:] in COND_LAZY:
            return pre + [cx.store(ops[0], '(uint32_t)(%s)' % self.cond(f, i, mn[3:]))]
        if mn in ('cld', 'std'):
            return ['c->eflags = %s;' % ('c->eflags & ~F_DF' if mn == 'cld' else 'c->eflags | F_DF')]
        if mn in ('clc', 'stc', 'cmc'):
            e = {'clc': 'c->eflags & ~F_CF', 'stc': 'c->eflags | F_CF', 'cmc': 'c->eflags ^ F_CF'}[mn]
            return ['cpu_flags(c);', 'c->eflags = %s;' % e]
        if mn == 'sahf':
            return ['cpu_flags(c);', 'c->eflags = (c->eflags & ~0xd5u) | ((R(EAX) >> 8) & 0xd5u);']
        if mn == 'lahf':
            return ['set8h(c, EAX, cpu_flags(c) & 0xd7u);']
        if mn in ('pushfd', 'pushf'):
            return ['PUSH(c, cpu_flags(c));']
        if mn in ('popfd', 'popf'):
            return ['c->eflags = POP(c) & 0x0cd5u; c->fop = FL_RAW;']
        if mn in ('pushal', 'pushad'):
            return ['uint32_t s = R(ESP);'] + ['PUSH(c, %s);' % ('s' if k == 4 else 'R(%s)' % RN[k]) for k in range(8)]
        if mn in ('popal', 'popad'):
            return ['%s' % ('POP(c);' if k == 4 else 'R(%s) = POP(c);' % RN[k]) for k in reversed(range(8))]
        if mn in ('bt', 'bts', 'btr', 'btc') and ops[0].type == X.X86_OP_REG:
            n = '(%s & %du)' % (cx.load(ops[1]), bits - 1)
            out = ['uint32_t a = %s, n = %s;' % (cx.load(ops[0]), n),
                   'cpu_flags(c); c->eflags = (c->eflags & ~F_CF) | ((a >> n) & 1u);']
            if mn != 'bt':
                o = {'bts': 'a | (1u << n)', 'btr': 'a & ~(1u << n)', 'btc': 'a ^ (1u << n)'}[mn]
                out.append(cx.store(ops[0], o))
            return out
        if mn in ('shld', 'shrd') and bits == 32:
            cnt = ('0x%xu' % (ops[2].imm & 31)) if ops[2].type == X.X86_OP_IMM else '(R(ECX) & 31u)'
            e = '(a << n) | (b >> (32 - n))' if mn == 'shld' else '(a >> n) | (b << (32 - n))'
            fl = (' FLAGS(c, %s, 32, a, n, r);' % ('FL_SHL' if mn == 'shld' else 'FL_SHR')) if live_out else ''
            return pre + ['uint32_t a = %s, b = %s, n = %s;' % (cx.load(ops[0]), cx.load(ops[1]), cnt),
                          'if (n) { uint32_t r = %s; %s%s }' % (e, cx.store(ops[0], 'r'), fl)]
        if mn == 'xlatb':
            return ['set8l(c, EAX, gmem_rd8(R(EBX) + (R(EAX) & 0xffu)));']
        if mn == 'cpuid':
            return ['cpu_cpuid(c);']
        if mn == 'rdtsc':
            return ['{ uint64_t t = cpu_rdtsc(c); R(EAX) = (uint32_t)t; R(EDX) = (uint32_t)(t >> 32); }']
        if mn in ('rcl', 'rcr'):
            cnt = ('0x%xu' % (ops[1].imm & 31)) if len(ops) > 1 and ops[1].type == X.X86_OP_IMM else                   ('1u' if len(ops) == 1 else '(R(ECX) & 31u)')
            step = ('{ uint32_t o = (a >> %d) & 1u; a = ((a << 1) | cf) & %s; cf = o; }' % (bits - 1, MASK[bits])
                    if mn == 'rcl' else
                    '{ uint32_t o = a & 1u; a = (a >> 1) | (cf << %d); cf = o; }' % (bits - 1))
            return pre + ['uint32_t a = %s, n = %s %% %du, cf = (uint32_t)cpu_cf(c);' % (cx.load(ops[0]), cnt, bits + 1),
                          'if (n) { while (n--) %s %s cpu_flags(c); c->eflags = (c->eflags & ~F_CF) | cf; }'
                          % (step, cx.store(ops[0], 'a'))]
        if mn.startswith(('movs', 'stos', 'lods', 'cmps', 'scas')):
            return self.string_op(i, live_out)
        if mn in ('int3', 'hlt', 'ud2'):
            return ['cpu_trap(c, 0x%08xu, "%s"); return;' % (i.address, mn)]
        raise Unsupported(mn)

    # ------------------------------------------------------------- x87
    FARITH = {'add': 'a + b', 'mul': 'a * b', 'sub': 'a - b', 'subr': 'b - a', 'div': 'a / b', 'divr': 'b / a'}
    FCONST = {'fld1': '1.0', 'fldz': '0.0', 'fldpi': '3.14159265358979323846',
              'fldl2e': '1.44269504088896340736', 'fldl2t': '3.32192809488736234787',
              'fldlg2': '0.30102999566398119521', 'fldln2': '0.69314718055994530942'}
    FSIMPLE = {'fchs': 'ST(0) = -ST(0);', 'fabs': 'ST(0) = fabs(ST(0));', 'fsqrt': 'ST(0) = sqrt(ST(0));',
               'fsin': 'ST(0) = sin(ST(0)); c->fsw &= (uint16_t)~FSW_C2;',
               'fcos': 'ST(0) = cos(ST(0)); c->fsw &= (uint16_t)~FSW_C2;',
               'fptan': 'ST(0) = tan(ST(0)); FPUSH(c, 1.0); c->fsw &= (uint16_t)~FSW_C2;',
               'fsincos': '{ double v = ST(0); ST(0) = sin(v); FPUSH(c, cos(v)); } c->fsw &= (uint16_t)~FSW_C2;',
               'fpatan': 'ST(1) = atan2(ST(1), ST(0)); FPOP(c);',
               'fyl2x': 'ST(1) = ST(1) * log2(ST(0)); FPOP(c);',
               'fyl2xp1': 'ST(1) = ST(1) * log2(1.0 + ST(0)); FPOP(c);',
               'f2xm1': 'ST(0) = exp2(ST(0)) - 1.0;',
               'fscale': 'ST(0) = ldexp(ST(0), (int)trunc(ST(1)));',
               'frndint': 'ST(0) = cpu_fround(c->fcw, ST(0));',
               'fprem': 'ST(0) = fmod(ST(0), ST(1)); c->fsw &= (uint16_t)~FSW_C2;',
               'fprem1': 'ST(0) = remainder(ST(0), ST(1)); c->fsw &= (uint16_t)~FSW_C2;',
               'fxam': 'cpu_fxam(c);', 'fnclex': 'c->fsw &= 0x7f00u;', 'fninit': 'cpu_finit(c);',
               'fdecstp': 'c->ftop = (c->ftop - 1) & 7u;', 'fincstp': 'c->ftop = (c->ftop + 1) & 7u;',
               'ffree': '', 'fnop': ''}

    def fpu(self, i, mn, pre):
        ops = i.operands

        def st(o):
            n = i.reg_name(o.reg)
            if not n.startswith('st('):
                raise Unsupported('fpu reg ' + n)
            return int(n[3])

        def fload(o):
            if o.type == X.X86_OP_REG:
                return 'ST(%d)' % st(o)
            if o.size in (4, 8, 10):
                return {4: 'frd32(ea)', 8: 'frd64(ea)', 10: 'cpu_f80_rd(ea)'}[o.size]
            raise Unsupported('fp mem%d' % o.size)

        def iload(o):
            return {2: '(double)(int16_t)gmem_rd16(ea)', 4: '(double)(int32_t)gmem_rd32(ea)',
                    8: '(double)grd64s(ea)'}[o.size]

        def fstore(o, v):
            if o.type == X.X86_OP_REG:
                return 'ST(%d) = %s;' % (st(o), v)
            return {4: 'fwr32(ea, %s);', 8: 'fwr64(ea, %s);', 10: 'cpu_f80_wr(ea, %s);'}[o.size] % v

        if mn in self.FCONST:
            return ['FPUSH(c, %s);' % self.FCONST[mn]]
        if mn == 'fld':
            return pre + ['{ double v = %s; FPUSH(c, v); }' % fload(ops[0])]
        if mn == 'fild':
            return pre + ['FPUSH(c, %s);' % iload(ops[0])]
        if mn in ('fst', 'fstp'):
            return pre + [fstore(ops[0], 'ST(0)')] + (['FPOP(c);'] if mn == 'fstp' else [])
        if mn in ('fist', 'fistp'):
            w = {2: 'gmem_wr16(ea, (uint16_t)cpu_fist16(c->fcw, ST(0)));', 4: 'gmem_wr32(ea, cpu_fist32(c->fcw, ST(0)));',
                 8: 'gwr64(ea, cpu_fist64(c->fcw, ST(0)));'}[ops[0].size]
            return pre + [w] + (['FPOP(c);'] if mn == 'fistp' else [])
        if mn == 'fxch':
            k = st(ops[-1]) if ops else 1
            return ['{ double t = ST(0); ST(0) = ST(%d); ST(%d) = t; }' % (k, k)]
        base, pop = mn[1:], 0
        if base.endswith('p') and base[:-1] in self.FARITH:
            base, pop = base[:-1], 1
        integer = base not in self.FARITH and base.startswith('i') and base[1:] in self.FARITH
        if base in self.FARITH or integer:
            e = self.FARITH[base[1:] if integer else base]
            if integer:
                return pre + ['{ double a = ST(0), b = %s; ST(0) = %s; }' % (iload(ops[0]), e)]
            if len(ops) == 1 and ops[0].type == X.X86_OP_MEM:
                return pre + ['{ double a = ST(0), b = %s; ST(0) = %s; }' % (fload(ops[0]), e)]
            if pop:                                    # f<op>p st(i): st(i) = st(i) op st(0); pop
                k = st(ops[0]) if ops else 1
                return ['{ double a = ST(%d), b = ST(0); ST(%d) = %s; } FPOP(c);' % (k, k, e)]
            if len(ops) == 1:                          # f<op> st(i): st(0) = st(0) op st(i)
                return ['{ double a = ST(0), b = ST(%d); ST(0) = %s; }' % (st(ops[0]), e)]
            d, s2 = st(ops[0]), st(ops[1])            # f<op> st(d), st(s): st(d) = st(d) op st(s)
            return ['{ double a = ST(%d), b = ST(%d); ST(%d) = %s; }' % (d, s2, d, e)]
        if mn in ('fcom', 'fcomp', 'fcompp', 'fucom', 'fucomp', 'fucompp', 'ficom', 'ficomp', 'ftst'):
            if mn == 'ftst':
                src = '0.0'
            elif mn.startswith('fi'):
                src = iload(ops[0])
            elif ops:
                src = fload(ops[-1])
            else:
                src = 'ST(1)'
            npop = 2 if mn.endswith('pp') else 1 if mn.endswith('p') else 0
            return pre + ['FCOM(c, ST(0), %s);' % src] + ['FPOP(c);'] * npop
        if mn in self.FSIMPLE:
            return [self.FSIMPLE[mn]] if self.FSIMPLE[mn] else []
        if mn == 'fnstsw':
            if ops[0].type == X.X86_OP_REG:
                return ['set16(c, EAX, cpu_fsw(c));']
            return pre + ['gmem_wr16(ea, (uint16_t)cpu_fsw(c));']
        if mn == 'fnstcw':
            return pre + ['gmem_wr16(ea, c->fcw);']
        if mn == 'fldcw':
            return pre + ['c->fcw = gmem_rd16(ea);']
        if mn == 'fnsave':
            return pre + ['cpu_fsave(c, ea);']
        if mn == 'frstor':
            return pre + ['cpu_frstor(c, ea);']
        raise Unsupported(mn)

    def string_op(self, i, live_out):
        mn = base_mn(i)
        rep = i.prefix[0]
        size = {'b': 1, 'w': 2, 'd': 4}[mn[-1]]
        bits, kind = size * 8, mn[:4]
        step = 'int32_t st = (c->eflags & F_DF) ? -%d : %d;' % (size, size)
        acc = {8: '(R(EAX) & 0xffu)', 16: '(R(EAX) & 0xffffu)', 32: 'R(EAX)'}[bits]
        if kind == 'movs':
            one = 'gmem_wr%d(R(EDI), gmem_rd%d(R(ESI))); R(ESI) += st; R(EDI) += st;' % (bits, bits)
        elif kind == 'stos':
            one = 'gmem_wr%d(R(EDI), (%s)%s); R(EDI) += st;' % (bits, UTYPE[bits], acc)
        elif kind == 'lods':
            one = '%s R(ESI) += st;' % {8: 'set8l(c, EAX, gmem_rd8(R(ESI)));', 16: 'set16(c, EAX, gmem_rd16(R(ESI)));',
                                         32: 'R(EAX) = gmem_rd32(R(ESI));'}[bits]
        elif kind == 'cmps':
            one = 'a = gmem_rd%d(R(ESI)); b = gmem_rd%d(R(EDI)); R(ESI) += st; R(EDI) += st;' % (bits, bits)
        else:
            one = 'a = %s; b = gmem_rd%d(R(EDI)); R(EDI) += st;' % (acc, bits)
        cmp = kind in ('cmps', 'scas')
        out = [step]
        if cmp:
            out.append('uint32_t a = 0, b = 0, done = 0;')
        if rep == 0:
            out.append(one)
            if cmp:
                out.append('done = 1;')
        elif not cmp:
            out.append('while (R(ECX)) { %s R(ECX)--; }' % one)
        else:
            stop = 'a != b' if rep == X.X86_PREFIX_REP else 'a == b'
            out.append('while (R(ECX)) { %s R(ECX)--; done = 1; if (%s) break; }' % (one, stop))
        if cmp:
            out.append('if (done) FLAGS(c, FL_SUB, %d, a, b, (a - b) & %s);' % (bits, MASK[bits]))
        return out

    def cond(self, f, i, cc):
        prev = f.fusable.get(i.address)
        if prev is not None and prev in f.fused:
            kind, a, b, bits = f.fused[prev]
            fc = fused_cond(cc, kind, 'fa_%x' % prev, 'fb_%x' % prev, bits)
            if fc:
                return fc
        return COND_LAZY[cc]

    def bodies(self, f):
        """Phase 1: C body text of every instruction (cached for phase 2)."""
        live = liveness(f)
        vas = sorted(va for va, i in f.insns.items() if i is not None)
        # fusion candidates: cmp/test whose only flag consumer is the next jcc/setcc
        f.fused, f.fusable = {}, {}
        fuse_ok = set()
        for k, va in enumerate(vas[:-1]):
            i, n = f.insns[va], f.insns[vas[k + 1]]
            if i.mnemonic in ('cmp', 'test') and n.address == va + i.size and n.address not in f.targets \
               and (n.mnemonic in JCC or (n.mnemonic.startswith('set') and n.mnemonic[3:] in COND_LAZY)) \
               and not live[n.address] and fused_cond(n.mnemonic[1:] if n.mnemonic in JCC else n.mnemonic[3:],
                                                      i.mnemonic, 'x', 'y', i.operands[0].size * 8):
                fuse_ok.add(va)
                f.fusable[n.address] = va
        bod, decls = {}, []
        for va in vas:
            i = f.insns[va]
            try:
                body = self.control(f, i) if self.is_control(i) else self.insn(f, i, live[va], va in fuse_ok)
                if va in fuse_ok:
                    kind, a, b, bits = f.fused[va]
                    m = mem_op(i)
                    pre = ['uint32_t ea = %s;' % Ctx(i).addr(m)] if m is not None else []
                    body = pre + ['fa_%x = %s; fb_%x = %s;' % (va, a, va, Ctx(i).load(i.operands[1], bits))]
                    decls.append('    uint32_t fa_%x, fb_%x;' % (va, va))
            except Unsupported as e:
                self.unimpl[str(e)] += 1
                body = ['cpu_unimpl(c, 0x%08xu, "%s");' % (va, i.mnemonic)]
            bod[va] = ' '.join(body)
        f.cached = (vas, bod, decls)
        return bod

    def function(self, f):
        vas, bod, decls = f.cached
        # x87: static slots when the stack depth is known everywhere (fpu_static.py)
        fpu = None
        if fpu_static.uses_fpu(bod) and not os.environ.get('TPM_LIFT_DYNAMIC_X87'):   # switch for A/B checks
            r = fpu_static.analyze(f, bod, self.fdelta)
            if r is not None:
                fpu = (r, set())
                self.fpu_stats[0] += 1
            else:
                self.fpu_stats[1] += 1
        # The function works on a local copy of the CPU state (lc, reached through c):
        # it cannot alias guest memory, so the C compiler keeps registers and flags
        # in host registers across guest loads/stores. The shared state (cg) is
        # synced only where other code can see it: calls, returns, traps.
        oname = fn_name(f.start, self.prefix)
        override = f.start in self.overrides
        if override:
            oname += '_ref'          # lifted body: differential-test reference only
        lines = ['void %s(cpu_t *cg)' % oname, '{', '    cpu_t lc = *cg, *const c = &lc;',
                 '    CPU_FN_ENTER(c, 0x%08xu);' % f.start]
        lines[3:3] = decls
        for k, va in enumerate(vas):
            i = f.insns[va]
            if va in f.targets and va != f.start:
                lines.append('%s:;' % L(va))
            lines.append('    /* %08x: %s %s */' % (va, i.mnemonic, i.op_str.replace('*/', '* /')))
            nxt = va + i.size
            body = bod[va]
            if fpu is not None:
                d = fpu[0].get(va, 0)
                body, used = fpu_static.rewrite(body, d)
                fpu[1].update(used)
                # depth after a call = the model's depth at the next instruction
                body = '/*fd=%d,%d*/ %s' % (d, fpu[0].get(nxt, d), body)
            if body:
                lines.append('    { ' + body + ' }')
            # fallthrough to a non-adjacent address
            if not self.ends_flow(f, i) and nxt in f.insns and (k + 1 >= len(vas) or vas[k + 1] != nxt):
                lines.append('    goto %s;' % L(nxt))
                f.targets.add(nxt)
        lines.append('}')
        # labels may have been added late (fallthrough gotos): insert missing ones
        text = '\n'.join(lines)
        import re
        # every goto target that is an instruction of this function needs a label
        # (switch-table cases are only known from the emitted gotos)
        for u in set(re.findall(r'goto L_([0-9a-f]{8});', text)):
            t = int(u, 16)
            if t in f.insns and f.insns[t] is not None:
                f.targets.add(t)
        for t in f.targets:
            if t in f.insns and f.insns[t] is not None and t != f.start and '%s:;' % L(t) not in text:
                text = text.replace('    /* %08x:' % t, '%s:;\n    /* %08x:' % (L(t), t), 1)
        if vas and vas[0] != f.start:
            # code below the entry (shared tails): enter at the entry, not at the lowest address
            entry = '    CPU_FN_ENTER(c, 0x%08xu);\n' % f.start
            text = text.replace(entry, entry + '    goto %s;\n' % L(f.start), 1)
            if '%s:;' % L(f.start) not in text:
                text = text.replace('    /* %08x:' % f.start, '%s:;\n    /* %08x:' % (L(f.start), f.start), 1)
        elif f.start in f.targets:
            entry = '    CPU_FN_ENTER(c, 0x%08xu);\n' % f.start
            text = text.replace(entry, entry + '%s:;\n' % L(f.start), 1)
        # any label still undefined (undecodable bytes, target outside .text): trap there
        used = set(re.findall(r'goto (L_[0-9a-f]{8});', text))
        missing = sorted(u for u in used if '%s:;' % u not in text)
        if missing:
            stubs = ''.join('%s:; cpu_trap(c, 0x%su, "jump to non-code"); return;\n' % (u, u[2:]) for u in missing)
            text = text[:-1] + stubs + '}'
        text = sync_state(text, f.start, LIVE_ENTRY, RET_LIVE, fpu[1] if fpu else None,
                          facts=getattr(self, 'call_facts', None))
        if override and f.start not in ALWAYS_REF:
            # the lifted semantics stay available for tests/test_override.c only
            text = '#ifdef TPM_OVERRIDE_REF\n' + text + '\n#endif /* TPM_OVERRIDE_REF */'
        for k, (pfx, va, _label) in enumerate(FN_TIMERS):
            if pfx == self.prefix and va == f.start and not override:
                n = fn_name(va, pfx)
                text = ('#ifdef TPM_FN_TIMERS\nvoid %s_tm(cpu_t *cg);\n'
                        'void %s(cpu_t *cg) { uint64_t t0_ = fn_timer_begin(); %s_tm(cg); fn_timer_end(%d, t0_); }\n'
                        '#define %s %s_tm\n#endif\n' % (n, n, n, k, n, n)
                        + text + '\n#ifdef TPM_FN_TIMERS\n#undef %s\n#endif' % n)
        return text

    def is_control(self, i):
        return i.mnemonic in ('jmp', 'call', 'ret', 'jecxz', 'loop') or i.mnemonic in JCC

    def ends_flow(self, f, i):
        return i.mnemonic in ('jmp', 'ret', 'int3', 'hlt', 'ud2') or not f.succ.get(i.address)

    def control(self, f, i):
        mn, ops, cx, nxt = i.mnemonic, i.operands, Ctx(i), i.address + i.size
        if mn == 'ret':
            n = (ops[0].imm & 0xffff) if ops else 0
            return ['R(ESP) += %du; return;' % (4 + n)]
        if mn in JCC or mn in ('jecxz', 'loop'):
            t = ops[0].imm & 0xffffffff
            if mn == 'jecxz':
                cond = 'R(ECX) == 0'
            elif mn == 'loop':
                return ['if (--R(ECX) != 0) goto %s;' % L(t)]
            else:
                cond = self.cond(f, i, mn[1:])
            return ['if (%s) goto %s;' % (cond, L(t))]
        if mn == 'call':
            imp = iat_target(self.pe, i)
            if ops[0].type == X.X86_OP_IMM:
                return ['PUSH(c, 0x%08xu);' % nxt, self.call_target(ops[0].imm & 0xffffffff)]
            m = mem_op(i)
            pre = ['uint32_t ea = %s;' % cx.addr(m)] if m is not None else []
            note = ' /* %s */' % imp if imp else ''
            return pre + ['uint32_t t = %s;' % cx.load(ops[0]), 'PUSH(c, 0x%08xu);' % nxt, 'cpu_call(c, t);' + note]
        # jmp
        if ops[0].type == X.X86_OP_IMM:
            t = ops[0].imm & 0xffffffff
            if i.address in f.tail:
                return [self.call_target(t), 'return;']
            return ['goto %s;' % L(t)]
        if i.address in f.switch:
            m = mem_op(i)
            cases = ' '.join('case 0x%08xu: goto %s;' % (t, L(t)) for t in f.switch[i.address])
            return ['uint32_t ea = %s; uint32_t t = gmem_rd32(ea);' % cx.addr(m),
                    'switch (t) { %s default: cpu_call(c, t); return; }' % cases]
        m = mem_op(i)
        pre = ['uint32_t ea = %s;' % cx.addr(m)] if m is not None else []
        return pre + ['cpu_call(c, %s); return;' % cx.load(ops[0])]

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from sync_state import sync_state, call_facts, RN   # noqa: E402
import fpu_static                   # noqa: E402

# --------------------------------------------------------------------- main
def main():
    exe, out = sys.argv[1], sys.argv[2]
    prefix = sys.argv[sys.argv.index('--prefix') + 1] if '--prefix' in sys.argv else 'wmain'
    per_file = 64
    base = int(sys.argv[sys.argv.index('--base') + 1], 0) if '--base' in sys.argv else None
    pe = PE(exe, base)
    funcs = discover(pe)
    flag_fixpoint(funcs)
    em = Emitter(pe, funcs, prefix)
    em.fpu_stats = [0, 0]
    em.fdelta = fpu_static.fixpoint(funcs, {st: em.bodies(funcs[st]) for st in funcs})
    # lean call sites (docs/PERF_PLAN.md section 10); --no-lean keeps the full sync for A/B checks
    if '--no-lean' not in sys.argv:
        em.call_facts = call_facts({st: ' '.join(em.bodies(funcs[st]).values()) for st in funcs})
    # Hand-written natives replace the lifted body at every call site. The facts above are
    # derived from the lifted body, which no longer runs, so be conservative for them: the
    # native may read and write any register and use x87. Without this the callers drop the
    # sync around those calls and the natives see (and leave) stale registers.
    _ALL = frozenset(RN)
    for _s in OVERRIDES_BY_PREFIX.get(prefix, set()):
        if _s in em.call_facts:
            em.call_facts[_s] = (_ALL, _ALL, True)
    starts = sorted(funcs)
    n_insn = sum(1 for f in funcs.values() for i in f.insns.values() if i is not None)
    hdr = os.path.join(out, prefix + '_gen.h')
    with open(hdr, 'w') as h:
        h.write('/* GENERATED by tools/lifter/lift.py - do not edit. */\n#pragma once\n#include "core/cpu.h"\n'
                'extern uint32_t g_screen_w, g_screen_h;   /* PUSH_SCREEN (core/video/video.c) */\n')
        for s in starts:
            h.write('void %s(cpu_t *c);\n' % fn_name(s, prefix))
            if s in OVERRIDES_BY_PREFIX.get(prefix, set()):
                # lifted body, compiled only with TPM_OVERRIDE_REF (differential tests)
                h.write('void %s_ref(cpu_t *c);\n' % fn_name(s, prefix))
    if prefix == 'wmain':
        with open(os.path.join(out, 'fn_timers_tab.h'), 'w') as h:
            h.write('/* GENERATED by tools/lifter/lift.py: labels of FN_TIMERS (core/prof.c). */\n')
            h.write('static const char *const k_fn_timer_names[] = {\n')
            for pfx, va, label in FN_TIMERS:
                h.write('    "%-14s %s_%08x",\n' % (label, pfx, va))
            h.write('};\n')
    files = 0
    for k in range(0, len(starts), per_file):
        with open(os.path.join(out, '%s_gen_%03d.c' % (prefix, files)), 'w') as fo:
            fo.write('/* GENERATED by tools/lifter/lift.py from the player\'s own exe - do not edit or commit. */\n')
            fo.write('#include <math.h>\n#include <stdint.h>\n#include "%s_gen.h"\n\n' % prefix)
            for s in starts[k:k + per_file]:
                fo.write(em.function(funcs[s]) + '\n\n')
        files += 1
    with open(os.path.join(out, prefix + '_gen_tab.c'), 'w') as fo:
        fo.write('/* GENERATED - dispatch table, sorted by guest address. */\n#include "%s_gen.h"\n' % prefix)
        fo.write('const guest_fn_entry %s_fn_tab[] = {\n' % prefix)
        for s in starts:
            fo.write('    { 0x%08xu, %s },\n' % (s, fn_name(s, prefix)))
        fo.write('};\nconst uint32_t %s_fn_count = %d;\n' % (prefix, len(starts)))
    total_unimpl = sum(em.unimpl.values())
    print('lift: %d functions, %d instructions, %d files (%d junk seeds rejected)' % (len(funcs), n_insn, files, discover.rejected))
    print('lift: unimplemented %d instructions (%.1f%%)' % (total_unimpl, 100.0 * total_unimpl / max(n_insn, 1)))
    print('lift: flags live across calls: %d functions read them on entry, %d return them'
          % (len(LIVE_ENTRY), len(RET_LIVE)))
    print('lift: x87 static in %d functions, dynamic in %d' % (em.fpu_stats[0], em.fpu_stats[1]))
    for k, v in em.unimpl.most_common(25):
        print('   %6d %s' % (v, k))

if __name__ == '__main__':
    main()
