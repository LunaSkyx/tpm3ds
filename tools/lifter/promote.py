"""Stack-slot promotion for lifted functions (used by lift.py).

MSVC code keeps its locals and arguments in ebp-relative stack slots, and the lifted C reads and writes
every one of them through the bounds-checked guest memory accessors. In a function whose frame
provably never escapes, a slot that is only ever touched as one aligned dword can live in a C local
instead: the C compiler then keeps it in a host register (or a plain host stack slot) and the masked
address computation, the range check and the memcpy disappear.

A function is promoted only when all of this holds (any doubt: the function is left alone):
  * it starts with `push ebp; mov ebp, esp` and ebp is never touched again except `pop ebp`,
    `mov esp, ebp`, `lea esp, [ebp+d]` and `leave`;
  * no instruction takes the address of the frame: no `lea` with an ebp base other than the
    epilogue form, no ebp-based indexed operand, no explicit esp-based memory operand, no esp or ebp
    copied to anything, no pushad/popad/enter, no fs/gs segment operand;
  * every memory access is a plain size 1/2/4/8 operand (no fnsave, fld tbyte, ...).
A slot [ebp+D] is promoted when every access that touches any of its four bytes is exactly a dword at
D, it is not one of the dwords the prologue pushes (saved registers), and
  * D <= -4 and D >= -(frame bytes the entry block allocates) - a local, or
  * D >= 8 and the function never writes it - an argument (loaded once on entry).
Locals the function may read before it wrote them (must-definition dataflow) are loaded from memory
on entry as well, so even that undefined-but-deterministic behaviour is kept.
The instruction bodies are rewritten textually: `uint32_t ea = ...;` disappears, gmem_rd32(ea) becomes
the variable, gmem_wr32(ea, X); becomes `var = X;`, and the float forms go through fb2d/d2fb.
"""
import re
from capstone import x86 as X

EBP = (X.X86_REG_EBP, X.X86_REG_BP)
ESP = (X.X86_REG_ESP, X.X86_REG_SP)
REGOP, MEMOP, IMMOP = X.X86_OP_REG, X.X86_OP_MEM, X.X86_OP_IMM

_EA_DECL = 'uint32_t ea = (uint32_t)(R(EBP) + 0x%xu);'
_RD = ('gmem_rd32(ea)', 'frd32(ea)')
_WR = ('gmem_wr32(ea, ', 'fwr32(ea, ')


def _mn(i):
    return i.mnemonic.split()[-1]


def _allowed_reg_use(i):
    mn, ops = _mn(i), i.operands
    if mn == 'pop' and ops[0].type == REGOP and ops[0].reg in EBP:
        return True
    if mn == 'leave':
        return True
    if mn == 'mov' and ops[0].type == REGOP and ops[0].reg in ESP and ops[1].type == REGOP and ops[1].reg in EBP:
        return True
    if mn == 'lea' and ops[0].type == REGOP and ops[0].reg in ESP and ops[1].type == MEMOP \
       and ops[1].mem.base in EBP and not ops[1].mem.index:
        return True
    if mn in ('sub', 'add', 'and') and ops[0].type == REGOP and ops[0].reg in ESP \
       and not (ops[1].type == REGOP and (ops[1].reg in EBP or ops[1].reg in ESP)):
        return True
    return False


def _take_wr(body, tok, name, conv):
    """Replace every `tok EXPR);` with `name = conv(EXPR);`; None if the parentheses do not close."""
    out, pos = [], 0
    while True:
        k = body.find(tok, pos)
        if k < 0:
            out.append(body[pos:])
            return ''.join(out)
        out.append(body[pos:k])
        s, depth, p = k + len(tok), 1, k + len(tok)
        while p < len(body) and depth:
            ch = body[p]
            depth += (ch == '(') - (ch == ')')
            p += 1
        if depth or p >= len(body) or body[p] != ';':
            return None
        expr = body[s:p - 1]
        out.append('%s = %s;' % (name, conv(expr)))
        pos = p + 1


def _rewrite(body, d, name):
    body = body.replace(_EA_DECL % (d & 0xffffffff), '')
    body = body.replace('gmem_rd32(ea)', name).replace('frd32(ea)', 'fb2d(%s)' % name)
    body = _take_wr(body, 'gmem_wr32(ea, ', name, lambda e: e)
    if body is None:
        return None
    body = _take_wr(body, 'fwr32(ea, ', name, lambda e: 'd2fb(%s)' % e)
    if body is None or re.search(r'\bea\b', body):
        return None
    return body


def promote(f, vas, bod, name_of=lambda d: ('sl_m%x' % -d) if d < 0 else ('sl_p%x' % d)):
    """Returns (new bodies, declaration lines, number of promoted slots) or None."""
    insns = f.insns
    i0 = insns.get(f.start)
    if i0 is None or f.start in f.targets or _mn(i0) != 'push' or i0.operands[0].type != REGOP or i0.operands[0].reg not in EBP:
        return None
    i1 = insns.get(f.start + i0.size)
    if i1 is None or _mn(i1) != 'mov' or i1.operands[0].type != REGOP or i1.operands[0].reg not in EBP \
       or i1.operands[1].type != REGOP or i1.operands[1].reg not in ESP:
        return None
    frame_vas = {f.start, f.start + i0.size}

    # ---- escape analysis + access collection
    acc = []                    # (disp, size) of every access that touches the frame
    touch = {}                  # va -> disp of its ebp-based operand
    for va in vas:
        i = insns[va]
        mn = _mn(i)
        if va in frame_vas:
            continue
        if mn in ('pushad', 'pushal', 'popad', 'popal', 'enter', 'pusha', 'popa'):
            return None
        ok_regs = _allowed_reg_use(i)
        for o in i.operands:
            if o.type == REGOP:
                if (o.reg in EBP or o.reg in ESP) and not ok_regs:
                    return None
            elif o.type == MEMOP:
                m = o.mem
                if m.segment and i.reg_name(m.segment) not in ('ds', 'ss', 'es', 'cs'):
                    return None
                if m.base in ESP or m.index in ESP or m.index in EBP:
                    return None
                if m.base in EBP:
                    if mn == 'lea':
                        if not ok_regs:
                            return None
                        continue
                    if m.index or o.size not in (1, 2, 4, 8):
                        return None
                    d = m.disp if m.disp < 0x80000000 else m.disp - (1 << 32)
                    acc.append((d, o.size))
                    touch[va] = d
        if mn.startswith(('fnsave', 'frstor', 'fxsave', 'fxrstor', 'fnstenv', 'fldenv')) and va in touch:
            return None

    # ---- the frame the entry block allocates, and the dwords its pushes write
    delta, pushed = 0, set()
    va = f.start + i0.size + i1.size
    while va in insns and insns[va] is not None and va not in f.targets:
        i = insns[va]
        mn = _mn(i)
        if mn in ('call', 'jmp', 'ret', 'loop', 'jecxz') or mn.startswith('j'):
            break
        ops = i.operands
        if mn == 'push':
            delta -= 4
            pushed.add(delta)
            acc.append((delta, 4))
        elif mn == 'sub' and ops[0].type == REGOP and ops[0].reg in ESP:
            if ops[1].type != IMMOP:
                break
            delta -= ops[1].imm & 0xffffffff
        elif any(o.type == REGOP and o.reg in ESP for o in ops) or mn in ('pop', 'leave'):
            break
        va += i.size
    alloc = -delta

    # ---- candidate slots
    exact = {}
    for d, sz in acc:
        exact.setdefault(d, set()).add(sz)
    cand = set()
    for d, szs in exact.items():
        if szs != {4} or d in pushed or d % 4:
            continue
        if not (d >= 8 or -alloc <= d <= -4):
            continue
        if any((d2 != d and d2 < d + 4 and d2 + sz2 > d) for d2, sz2 in acc):
            continue
        cand.add(d)
    if not cand:
        return None
    reads, writes = {}, {}
    for va, d in touch.items():
        if d not in cand:
            continue
        b = bod[va]
        reads[va] = any(t in b for t in _RD)
        writes[va] = any(t in b for t in _WR)
    for d in list(cand):
        if d >= 8 and any(writes[va] for va, dd in touch.items() if dd == d and va in writes):
            cand.discard(d)
    if not cand:
        return None
    slots = sorted(cand)
    bit = {d: 1 << k for k, d in enumerate(slots)}
    ALL = (1 << len(slots)) - 1

    # ---- must-definition dataflow: locals that may be read before written are loaded on entry
    DIN = {va: ALL for va in vas}
    DIN[f.start] = 0
    preds = {}
    for va in vas:
        for s in f.succ.get(va, []):
            preds.setdefault(s, []).append(va)

    def out_of(va, din):
        d = touch.get(va)
        if d in bit and writes.get(va):
            return din | bit[d]
        return din

    changed = True
    while changed:
        changed = False
        for va in vas:
            if va == f.start:
                continue
            ps = [p for p in preds.get(va, []) if p in DIN]
            if not ps:
                if DIN[va]:
                    DIN[va] = 0                  # no known path in: assume nothing is defined
                    changed = True
                continue
            v = ALL
            for p in ps:
                v &= out_of(p, DIN[p])
            if v != DIN[va]:
                DIN[va] = v
                changed = True
    init = set()
    for d in slots:
        if d >= 8:
            init.add(d)
    for va, d in touch.items():
        if d in bit and reads.get(va) and not (DIN[va] & bit[d]):
            init.add(d)

    # ---- rewrite
    new = dict(bod)
    for va, d in touch.items():
        if d not in bit:
            continue
        nb = _rewrite(bod[va], d, name_of(d))
        if nb is None:
            return None
        new[va] = nb
    decls = ['    uint32_t %s;' % ', '.join(name_of(d) for d in slots)]
    pre = ' '.join('%s = gmem_rd32((uint32_t)(R(ESP) + 0x%xu));' % (name_of(d), (d - 4) & 0xffffffff) for d in sorted(init))
    if pre:
        new[f.start] = pre + ' ' + new[f.start]
    return new, decls, len(slots)
