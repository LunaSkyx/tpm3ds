"""Static x87 stack allocation for lifted functions (used by lift.py).

The x87 register stack is a ring of 8 registers addressed relative to TOP.
Within a stretch of code without calls, the TOP change of every instruction is
known at lift time, so ST(i) resolves to a fixed slot: with ft0 the physical
register of slot 0 and d the static depth (pushes minus pops since the
function started), ST(i) is slot (i - d) & 7 = physical (ft0 + i - d) & 7.
Constant slot numbers let the C compiler keep the values in host FP registers
instead of indexing a memory array with a run-time TOP.

Calls: the slots are spilled to the shared state, and afterwards ft0 is
rebased on the TOP the callee actually left (ft0 = TOP + d), then the slots are
reloaded. So ST(i) always names the same physical register as on hardware,
whatever the callee did - including float functions whose result the caller
ignores without popping (unprototyped calls in the original leak x87 slots)
and functions that return a value on some paths only. Correctness needs no
assumption about callees; their usual stack effect (fixpoint(): +1 for float
functions) only keeps the depth model consistent where paths with and
without such a call merge.

A function stays on the dynamic scheme when its own instructions do not give
every instruction one depth (paths that push different amounts and then
merge, as in float functions without a return value on some path), or when it
uses instructions that depend on the physical layout (fxam, fsave/frstor,
fninit, fincstp/fdecstp).
"""
import re

_PUSH = re.compile(r'\bFPUSH\(c, ')
_POP = re.compile(r'\bFPOP\(c\)')
_TOK = re.compile(r'\bST\((\d)\)|\bFPUSH\(c, |\bFPOP\(c\)|\bcpu_fsw\(c\)')
_DCALL = re.compile(r'\b([a-z0-9]+)_([0-9a-f]{8})\(c\);')
_BAD = ('cpu_fxam(c)', 'cpu_finit(c)', 'c->ftop', 'cpu_fsave(c', 'cpu_frstor(c', 'c->st')
FPU_TOKEN = re.compile(r'\bST\(|\bFPUSH\(|\bFPOP\(|\bcpu_fsw\(c\)')


def uses_fpu(bodies):
    return any(FPU_TOKEN.search(b) for b in bodies.values())


def analyze(f, bodies, fdelta, want_ret=False):
    """bodies: va -> emitted C body text. Returns the static depth before
    every reachable instruction, or None (dynamic scheme). fdelta: modelled
    stack effect of callees (missing: 0). want_ret: return (depths, depth at
    the returns or None if they differ or there are none)."""
    delta = {}
    for va, b in bodies.items():
        if any(t in b for t in _BAD):
            return None
        if re.search(r'\bif \([^{}]*\{[^{}]*\bFP(USH|OP)\(', b):
            return None                       # conditional push/pop
        delta[va] = len(_PUSH.findall(b)) - len(_POP.findall(b))
        m = _DCALL.search(b)
        if m:
            delta[va] += fdelta.get(int(m.group(2), 16), 0)
    depth = {f.start: 0}
    work = [f.start]
    rets = set()
    while work:
        va = work.pop()
        if va not in bodies:
            continue
        out = depth[va] + delta[va]
        if re.search(r'\breturn;', bodies[va]):
            rets.add(out)
        for s in f.succ.get(va, ()):
            if s not in bodies:
                continue
            if s in depth:
                if depth[s] != out:
                    return None
            else:
                depth[s] = out
                work.append(s)
    if want_ret:
        return depth, (rets.pop() if len(rets) == 1 else None)
    return depth


def fixpoint(funcs, bodies_of):
    """Usual x87 stack effect of every function whose returns agree (only
    non-zero effects are recorded)."""
    fdelta = {}
    for _ in range(30):
        changed = False
        for s, f in funcs.items():
            r = analyze(f, bodies_of[s], fdelta, True)
            if r is not None and r[1] is not None and r[1] != fdelta.get(s, 0):
                fdelta[s] = r[1]
                changed = True
        if not changed:
            break
    return fdelta


def rewrite(body, d):
    """Resolves ST/FPUSH/FPOP/cpu_fsw in one instruction body starting at
    depth d. Returns (text, slots used)."""
    slots = set()
    out, pos, cur = [], 0, d
    for m in _TOK.finditer(body):
        out.append(body[pos:m.start()])
        tok = m.group(0)
        if m.group(1) is not None:
            k = (int(m.group(1)) - cur) & 7
            slots.add(k)
            out.append('fs_[%d]' % k)
        elif tok.startswith('FPUSH'):
            cur += 1
            k = (-cur) & 7
            slots.add(k)
            out.append('FPUSH_S(fs_[%d], ' % k)
        elif tok.startswith('FPOP'):
            cur -= 1
            out.append('((void)0)')
        else:
            out.append('cpu_fsw_at(c, (ft0 %s) & 7u)' % off(cur))
        pos = m.end()
    out.append(body[pos:])
    return ''.join(out), slots


def off(depth):
    """C text for '- depth' on an unsigned TOP value."""
    return '- %du' % depth if depth >= 0 else '+ %du' % -depth
