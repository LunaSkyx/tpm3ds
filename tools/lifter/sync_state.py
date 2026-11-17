"""State synchronisation for lifted functions (used by lift.py).

A lifted function runs on a local copy (lc) of the shared CPU state (cg), so
the C compiler keeps guest registers in host registers. The shared state must
be correct wherever other code can see it: before calls, at returns, at traps.

Only the fields a function touches are copied:
  entry  : load every field the function uses
  call   : spill the fields it writes; after the call reload every field it
           uses (the callee may have changed any of them)
  return : spill the fields it writes
Fields it never mentions stay in cg untouched, which is what the guest sees.

Flags follow the interprocedural facts from flag_fixpoint(): they are loaded on
entry only if the function reads them before writing them (or has indirect
calls/traps, which spill them conservatively), spilled before a direct call
only if the callee reads them on entry, reloaded after a direct call only if
the callee returns them, and spilled at a return only if callers read them.
eflags also carries DF, so it is synced with the flags.

x87 in static mode (fpu_static.py): the slots fs_[k] are guest registers
(ft0 + k) & 7; each instruction line carries its depth before and after as
/*fd=N,M*/: a spill writes TOP from N, and the reload after a call rebases ft0
on the TOP the callee left, at the model's depth M after the call.
"""
import re

RN = ['EAX', 'ECX', 'EDX', 'EBX', 'ESP', 'EBP', 'ESI', 'EDI']
LAZY = ['fa', 'fb', 'fr', 'fcf', 'fop', 'fbits']

_RE_FLAGS = re.compile(r'\bFLAGS\(|\bCC_[a-z]+\(|\bcpu_[a-z]f\(|\bcpu_flags\(|c->(eflags|fcf|fop|fa|fb|fr|fbits)\b')
_RE_FPU = re.compile(r'\bST\(|\bFPUSH\(|\bFPOP\(|\bFCOM\(|\bcpu_f(?!lags)[a-z0-9]*\(|c->(fsw|fcw|ftop|st)\b|\bfs_\[')
_RE_WRITE = re.compile(r'\bR\((E[A-Z]{2})\)\s*(?:[-+*/%&|^]|<<|>>)?=(?!=)|\bR\((E[A-Z]{2})\)(?:\+\+|--)|(?:\+\+|--)R\((E[A-Z]{2})\)'
                       r'|\bset(?:8l|8h|16)\(c, (E[A-Z]{2})\b')
_CALL = re.compile(r'\b(cpu_call|cpu_unimpl|cpu_trap)\(c, ([^;]*)\);|\b([a-z0-9]+)_([0-9a-f]{8})\(c\);')
_FD = re.compile(r'/\*fd=(-?\d+),(-?\d+)\*/ ')


def _off(depth):
    return '- %du' % depth if depth >= 0 else '+ %du' % -depth


class _Sync:
    def __init__(self, used, written, eflags, fpu, fs, slots):
        self.used, self.written = used, written
        self.eflags, self.fpu, self.fs = eflags, fpu, fs
        self.slots = sorted(slots) if slots is not None else None   # None: dynamic x87

    def load(self, flags, rebase=None, only=None, fpu=True):
        """only: registers the callee may write (None: all). fpu: callee may use x87."""
        f = ['r[%s]' % r for r in RN if r in self.used and (only is None or r in only)]
        if self.eflags:
            f.append('eflags')
        if flags:
            f += LAZY
        if self.fs:
            f.append('fs_base')
        out = ['lc.%s = cg->%s;' % (x, x) for x in f]
        if self.fpu and fpu:
            out += ['lc.fsw = cg->fsw;', 'lc.fcw = cg->fcw;']
            if self.slots is None:
                out += ['memcpy(lc.st, cg->st, sizeof lc.st);', 'lc.ftop = cg->ftop;']
            else:
                if rebase is not None:
                    out.append('ft0 = (cg->ftop %s) & 7u;' % _off(-rebase))
                out += ['fs_[%d] = cg->st[(ft0 + %du) & 7u];' % (k, k) for k in self.slots]
        return ' '.join(out)

    def spill(self, flags, depth, only=None, fpu=True):
        """only: registers the callee may read (None: all). fpu: callee may use x87."""
        f = ['r[%s]' % r for r in RN if r in self.written and (only is None or r in only)]
        if self.eflags:
            f.append('eflags')
        if flags:
            f += LAZY
        out = ['cg->%s = lc.%s;' % (x, x) for x in f]
        if self.fpu and fpu:
            out += ['cg->fsw = lc.fsw;', 'cg->fcw = lc.fcw;']
            if self.slots is None:
                out += ['memcpy(cg->st, lc.st, sizeof lc.st);', 'cg->ftop = lc.ftop;']
            else:
                out += ['cg->st[(ft0 + %du) & 7u] = fs_[%d];' % (k, k) for k in self.slots]
                out.append('cg->ftop = (ft0 %s) & 7u;' % _off(depth))
        return ' '.join(out)


def _own_sets(text):
    """Registers mentioned / written, x87 use, indirect-call flag and direct callees of one body."""
    used = set(r for r in RN if re.search(r'\b%s\b' % r, text))
    used.add('ESP')
    written = set(g for m in _RE_WRITE.finditer(text) for g in m.groups() if g)
    written.add('ESP')
    if 'cpu_cpuid(c)' in text:
        used |= {'EAX', 'ECX', 'EDX', 'EBX'}
        written |= {'EAX', 'ECX', 'EDX', 'EBX'}
    fpu = bool(_RE_FPU.search(text))
    cons = bool(re.search(r'\b(cpu_call|cpu_trap|cpu_unimpl)\(c, ', text))
    calls = set(int(m.group(4), 16) for m in _CALL.finditer(text) if m.group(3))
    return used, written, fpu, cons, calls


def call_facts(bodies_text):
    """Transitive facts for lean call sites (docs/PERF_PLAN.md section 10).

    bodies_text: start -> raw C body text (before sync). Returns start -> (R, W, fpu) where R is
    every register the function or a direct callee mentions, W every register one writes, and
    fpu whether any of them uses x87. Indirect calls, traps and calls to functions outside
    bodies_text make the function conservative: all registers, x87 used."""
    ALL = frozenset(RN)
    own = {s: _own_sets(t) for s, t in bodies_text.items()}
    F = {}
    for s, (used, written, fpu, cons, calls) in own.items():
        if cons:
            F[s] = [ALL, ALL, True]
        else:
            F[s] = [frozenset(used), frozenset(written), fpu]
    changed = True
    while changed:
        changed = False
        for s, (used, written, fpu, cons, calls) in own.items():
            if cons:
                continue
            R, W, fu = F[s]
            for t in calls:
                if t in F:
                    R2, W2, fu2 = F[t]
                else:
                    R2, W2, fu2 = ALL, ALL, True
                nR, nW, nf = R | R2, W | W2, fu or fu2
                if (nR, nW, nf) != (R, W, fu):
                    R, W, fu = nR, nW, nf
                    changed = True
            F[s] = [R, W, fu]
    return {s: (frozenset(R), frozenset(W), bool(fu)) for s, (R, W, fu) in F.items()}


def sync_state(text, start, live_entry, ret_live, slots, facts=None):
    used = set(r for r in RN if re.search(r'\b%s\b' % r, text))
    used.add('ESP')
    written = set(g for m in _RE_WRITE.finditer(text) for g in m.groups() if g)
    written.add('ESP')
    if 'cpu_cpuid(c)' in text:
        used |= {'EAX', 'ECX', 'EDX', 'EBX'}
        written |= {'EAX', 'ECX', 'EDX', 'EBX'}
    flags = bool(_RE_FLAGS.search(text))
    fpu = bool(_RE_FPU.search(text))
    fs = 'c->fs_base' in text
    eflags = flags or 'c->eflags' in text or bool(re.search(r'\b(movs|stos|lods|cmps|scas)', text))
    conservative = bool(re.search(r'\b(cpu_call|cpu_trap|cpu_unimpl)\(c, ', text))
    sy = _Sync(used, written, eflags, fpu, fs, slots)
    ret_flags = flags and start in ret_live

    def callee_kw(t):
        """(spill, load) keyword arguments for a direct call to t; full sync when unknown."""
        cf = facts.get(t) if facts else None
        if cf is None:
            return {}, {}
        R, W, fu = cf
        return {'only': R, 'fpu': fu}, {'only': W, 'fpu': fu}

    pc = [start]

    def line(ln):
        mc = re.match(r'    /\* ([0-9a-f]{8}): ', ln)
        if mc:
            pc[0] = int(mc.group(1), 16)
            return ln
        m = _FD.search(ln)
        d = int(m.group(1)) if m else 0
        da = int(m.group(2)) if m else 0
        if m:
            ln = ln[:m.start()] + ln[m.end():]

        def tail(m):
            # call in tail position: the callee's final state is ours; no reload, no return spill
            if m.group(3):
                t = int(m.group(4), 16)
                return '%s %s_%s(cg); return_;' % (sy.spill(flags and t in live_entry, d), m.group(3), m.group(4))
            return '%s %s(cg, %s); return_;' % (sy.spill(flags, d), m.group(1), m.group(2))

        def call(m):
            if m.group(3):
                t = int(m.group(4), 16)
                ks, kl = callee_kw(t)
                return '%s %s_%s(cg); %s' % (sy.spill(flags and t in live_entry, d, **ks), m.group(3), m.group(4),
                                             sy.load(flags and t in ret_live, da, **kl))
            # indirect calls, traps and unimplemented instructions
            return '%s %s(cg, %s); %s' % (sy.spill(flags, d), m.group(1), m.group(2), sy.load(flags, da))

        ln = re.sub('(?:%s) return;' % _CALL.pattern, tail, ln)
        ln = _CALL.sub(call, ln)
        return re.sub(r'\breturn;', sy.spill(ret_flags, d) + ' return;', ln)

    text = '\n'.join(line(ln) for ln in text.split('\n'))
    entry = sy.load(flags and (start in live_entry or conservative))
    if slots is not None:
        entry = 'uint32_t ft0 = cg->ftop; double fs_[8]; ' + entry
    text = text.replace('    cpu_t lc = *cg, *const c = &lc;\n',
                        '    cpu_t lc, *const c = &lc;\n    GMEM_LOCAL;\n    %s\n' % entry, 1)
    # guest memory through the arena base/size cached in locals
    text = re.sub(r'\bgmem_(rd|wr)(8|16|32)\(', lambda m: 'G%s%s(' % (m.group(1).upper(), m.group(2)), text)
    text = re.sub(r'\b(frd32|frd64|fwr32|fwr64|grd64s|gwr64)\(', lambda m: 'L%s(' % m.group(1).upper(), text)
    text = re.sub(r'\bPUSH\(c, ', 'LPUSH(c, ', text)
    text = re.sub(r'\bPOP\(c\)', 'LPOP(c)', text)
    return text.replace('return_;', 'return;')
