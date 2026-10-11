#!/usr/bin/env python3
"""gen.py - static recompiler: PowerPC (Gekko/Broadway) code of main.dol -> C.

Usage: python3 -I tools/gen.py game/main.dol game/part/files/LynWiiRetail.elf gen/

Function boundaries, direct-branch targets, function pointers and switch tables come from the
unstripped ELF that ships on the disc (symbols + the relocation sections the linker kept), so no
guessing is needed. The instruction words themselves are read from main.dol (the code that the
console actually runs); gen.py checks that both agree.

Every entry point becomes `void f_XXXXXXXX(CPU *restrict c)`. Branches inside a function become
gotos, `bl` becomes a C call, `blr` a return, `bctr` a switch over the function's jump-table targets
(or an indirect call). Interrupts are delivered by the runtime at function entries and loop
back-edges (FENTER / POLL)."""
import os, sys, struct, re, subprocess
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from elf import Elf

SHARD = 400

# ------------------------------------------------------------------------------------- image
class Dol:
    def __init__(self, path):
        d = self.d = open(path, 'rb').read()
        off = struct.unpack('>18I', d[0:72]); addr = struct.unpack('>18I', d[72:144]); sz = struct.unpack('>18I', d[144:216])
        self.secs = [(addr[i], sz[i], off[i]) for i in range(18) if sz[i]]
        self.text = [(addr[i], sz[i], off[i]) for i in range(7) if sz[i]]
        self.entry = struct.unpack('>I', d[0xE0:0xE4])[0]
    def word(self, a):
        for base, size, off in self.secs:
            if base <= a < base + size:
                return struct.unpack('>I', self.d[off + a - base:off + a - base + 4])[0]
        return None
    def in_text(self, a):
        return any(b <= a < b + s for b, s, _ in self.text)

def sx16(v): return v - 0x10000 if v & 0x8000 else v
def mask(mb, me):
    m = 0
    i = mb
    while True:
        m |= 1 << (31 - i)
        if i == me: break
        i = (i + 1) & 31
    return m

# ------------------------------------------------------------------------------------- HLE list
def load_hle(path, name2addr):
    hle = {}
    if not os.path.exists(path): return hle
    for line in open(path):
        line = line.split('#')[0].split('--')[0].strip()
        if not line: continue
        parts = line.split()
        key = parts[0]
        if re.fullmatch(r'[0-9A-Fa-f]{8}', key): a = int(key, 16); fn = 'hle_%08X' % a
        elif key in name2addr: a = name2addr[key]; fn = 'hle_' + re.sub(r'\W', '_', key)
        else: print('hle: unknown symbol', key, file=sys.stderr); continue
        hle[a] = set(parts[1:]) | {'fn=' + fn}
    return hle

# ------------------------------------------------------------------------------------- emitter
class Gen:
    def __init__(self, dol, elf):
        self.dol, self.elf = dol, elf
        self.funcs = []           # (start, end, name)
        self.entries = {}         # addr -> (func_start, func_end)
        self.labels_data = set()  # code addresses referenced from data (jump tables / fn pointers)
        self.name = {}
        self.hle = {}
        self.unimpl = {}

    # ---- discovery
    def discover(self):
        text_secs = [s for s in self.elf.sections if s['flags'] & 4 and s['size']]  # SHF_EXECINSTR
        def is_code(a): return any(s['addr'] <= a < s['addr'] + s['size'] for s in text_secs)
        fs = sorted({(s['value'], s['size'], s['name']) for s in self.elf.syms
                     if s['type'] == 2 and s['size'] and is_code(s['value'])})
        out = []; last = 0
        for a, sz, n in fs:
            if a < last: continue  # nested/aliased symbol
            out.append([a, a + sz, n]); last = a + sz
        for s in self.elf.syms:  # every named code address (incl. _savefpr_NN labels)
            if is_code(s['value']) and s['name'] and s['value'] not in self.name:
                self.name[s['value']] = s['name']
        for a, e, n in out: self.name[a] = n
        # gaps between symbols that contain code -> anonymous functions
        filled = []
        for s in text_secs:
            lo, hi = s['addr'], s['addr'] + s['size']
            inside = [f for f in out if lo <= f[0] < hi]
            cur = lo
            for f in inside + [[hi, hi, None]]:
                g = cur
                while g < f[0] and (self.dol.word(g) or 0) == 0: g += 4
                if g < f[0]:
                    filled.append([g, f[0], 'anon_%08X' % g])
                cur = max(cur, f[1])
        self.funcs = sorted(out + filled)
        self.fstarts = [f[0] for f in self.funcs]
        # verify ELF text == DOL text
        bad = 0
        for s in text_secs:
            for a in range(s['addr'], s['addr'] + s['size'], 4):
                o = s['off'] + a - s['addr']
                if struct.unpack('>I', self.elf.d[o:o + 4])[0] != (self.dol.word(a) or 0): bad += 1
        if bad: print('warning: %d words differ between ELF and DOL' % bad, file=sys.stderr)
        for f in self.funcs: self.entries[f[0]] = (f[0], f[1])
        self.add_entry(self.dol.entry)
        # relocations: direct branches and code pointers
        for sec, off, typ, tgt in self.elf.relocs():
            if not is_code(tgt): continue
            if typ == 10:   # R_PPC_REL24: b / bl - entry when it leaves the function or is a call
                f, g = self.func_of(tgt), self.func_of(off)
                w = self.dol.word(off) or 0
                if not f or not g or f[0] != g[0] or (w & 1): self.add_entry(tgt)
            elif typ == 1:  # R_PPC_ADDR32 from data: function pointer or switch-table entry
                self.labels_data.add(tgt)
                if self.func_of(tgt) and self.func_of(tgt)[0] == tgt: pass
            elif typ in (4, 6):  # lis/addi of a code address (callbacks)
                if typ == 4: self.labels_data.add(tgt)
        # function pointers loaded by code / stored in data that point at a function start are fine;
        # pointers to the middle of a function from *code* (not switch tables) become entries as well.
        for sec, off, typ, tgt in self.elf.relocs():
            if typ == 4 and is_code(tgt) and is_code(off):
                f = self.func_of(tgt); g = self.func_of(off)
                if f and g and f[0] != g[0]: self.add_entry(tgt)

    def func_of(self, a):
        import bisect
        i = bisect.bisect_right(self.fstarts, a) - 1
        if i < 0: return None
        f = self.funcs[i]
        return f if f[0] <= a < f[1] else None

    def add_entry(self, a):
        if a in self.entries: return
        f = self.func_of(a)
        if not f: return
        self.entries[a] = (f[0], f[1])
        if a not in self.name: self.name[a] = '%s+0x%X' % (f[2], a - f[0])

    # ---- branch condition
    @staticmethod
    def bcond(BO, BI):
        pre = ''; conds = []
        if not BO & 4:
            pre = '--c->ctr; '
            conds.append('c->ctr == 0' if BO & 2 else 'c->ctr != 0')
        if not BO & 16:
            conds.append(('CRB(c,%d)' % BI) if BO & 8 else ('!CRB(c,%d)' % BI))
        return pre, (' && '.join(conds) if conds else '1')

    def call(self, t, nxt, ret=False):
        """C for 'bl t' (or tail branch when ret)."""
        if t not in self.entries: self.add_entry(t)
        fn = 'f_%08X' % t if t in self.entries else None
        if fn is None:
            return 'call_indirect(c, 0x%08X);%s' % (t, ' return;' if ret else '')
        if ret: return '%s(c); return;' % fn
        flags = self.hle.get(t, set())
        if 'load' in flags and getattr(self, 'cur_resume', None):
            # OSLoadContext only returns here when this thread is resumed: continue as if the
            # OSSaveContext call in this function had returned (r3 = 1).
            self.cur_labels.add(self.cur_resume)
            return 'c->lr = 0x%08X; %s(c); goto L_%08X;' % (nxt, fn, self.cur_resume)
        return 'c->lr = 0x%08X; %s(c);' % (nxt, fn)

    # ---- one instruction
    def insn(self, a, w, lo, hi, labels, jt):
        op = w >> 26
        rD = (w >> 21) & 31; rA = (w >> 16) & 31; rB = (w >> 11) & 31; rC = (w >> 6) & 31
        simm = sx16(w & 0xFFFF); uimm = w & 0xFFFF
        rc = w & 1
        G = lambda n: 'c->gpr[%d]' % n
        A0 = lambda: ('c->gpr[%d]' % rA) if rA else '0'
        F0 = lambda n: 'c->ps0[%d]' % n
        F1 = lambda n: 'c->ps1[%d]' % n
        nxt = a + 4
        def cr0s(r): return (' cr0(c, %s);' % r) if rc else ''
        def goto(t, back):
            return ('POLL(0x%08X); goto L_%08X;' % (t, t)) if back else ('goto L_%08X;' % t)

        if op == 18:  # b / bl / ba
            li = w & 0x03FFFFFC
            if li & 0x02000000: li -= 0x04000000
            t = (li if w & 2 else a + li) & 0xFFFFFFFF
            if w & 1: return self.call(t, nxt)
            if lo <= t < hi: labels.add(t); return goto(t, t <= a)
            return self.call(t, nxt, ret=True)
        if op == 16:  # bc
            BO, BI = rD, rA
            bd = sx16(w & 0xFFFC)
            t = (bd if w & 2 else a + bd) & 0xFFFFFFFF
            pre, cond = self.bcond(BO, BI)
            if w & 1:
                return '%sc->lr = 0x%08X; if (%s) { %s }' % (pre, nxt, cond, self.call(t, nxt).replace('c->lr = 0x%08X; ' % nxt, ''))
            if lo <= t < hi:
                labels.add(t)
                return '%sif (%s) { %s }' % (pre, cond, goto(t, t <= a))
            return '%sif (%s) { %s }' % (pre, cond, self.call(t, nxt, ret=True))
        if op == 19:
            xo = (w >> 1) & 0x3FF
            if xo == 16:  # bclr
                pre, cond = self.bcond(rD, rA)
                if w & 1:
                    return '{ %su32 t = c->lr; c->lr = 0x%08X; if (%s) call_indirect(c, t); }' % (pre, nxt, cond)
                return '%sif (%s) return;' % (pre, cond) if cond != '1' else pre + 'return;'
            if xo == 528:  # bcctr
                pre, cond = self.bcond(rD | 4, rA)
                if w & 1:
                    return 'c->lr = 0x%08X; if (%s) call_indirect(c, c->ctr);' % (nxt, cond)
                targets = sorted(set(t for t in jt if lo <= t < hi) | set(self.scan_table(a, lo, hi)))
                body = ''
                if targets:
                    for t in targets: labels.add(t)
                    body = 'switch (c->ctr) { %s default: call_indirect(c, c->ctr); return; }' % \
                        ' '.join('case 0x%08X: goto L_%08X;' % (t, t) for t in targets)
                else:
                    body = 'call_indirect(c, c->ctr); return;'
                return ('if (%s) { %s }' % (cond, body)) if cond != '1' else body
            if xo == 0: return 'CRF_SET(c, %d, (c->cr >> (28 - %d*4)) & 15);' % (rD >> 2, rA >> 2)
            crops = {257: '&', 449: '|', 193: '^', 225: 'nand', 33: 'nor', 289: 'eqv', 129: 'andc', 417: 'orc'}
            if xo in crops:
                o = crops[xo]; x, y = 'CRB(c,%d)' % rA, 'CRB(c,%d)' % rB
                e = {'&': f'{x} & {y}', '|': f'{x} | {y}', '^': f'{x} ^ {y}', 'nand': f'!({x} & {y})', 'nor': f'!({x} | {y})',
                     'eqv': f'!({x} ^ {y})', 'andc': f'{x} & !{y}', 'orc': f'{x} | !{y}'}[o]
                return 'crset(c, %d, %s);' % (rD, e)
            if xo == 150: return ';'  # isync
            if xo == 50: return 'rt_rfi(c); return;'
            return None
        if op == 7: return '%s = (u32)((s32)%s * %d);' % (G(rD), G(rA), simm)
        if op == 8: return '%s = addc(c, ~%s, 0x%08X, 1);' % (G(rD), G(rA), simm & 0xFFFFFFFF)
        if op == 10: return 'cmpu(c, %d, %s, 0x%X);' % (rD >> 2, G(rA), uimm)
        if op == 11: return 'cmps(c, %d, (s32)%s, %d);' % (rD >> 2, G(rA), simm)
        if op == 12: return '%s = addc(c, %s, 0x%08X, 0);' % (G(rD), G(rA), simm & 0xFFFFFFFF)
        if op == 13: return '%s = addc(c, %s, 0x%08X, 0); cr0(c, %s);' % (G(rD), G(rA), simm & 0xFFFFFFFF, G(rD))
        if op == 14: return '%s = %s + 0x%08X;' % (G(rD), A0(), simm & 0xFFFFFFFF) if rA else '%s = 0x%08X;' % (G(rD), simm & 0xFFFFFFFF)
        if op == 15: return '%s = %s + 0x%08X;' % (G(rD), A0(), (uimm << 16)) if rA else '%s = 0x%08X;' % (G(rD), uimm << 16)
        if op == 17: return 'rt_sc(c);'
        if op in (20, 21, 23):
            sh, mb, me = rB, rC, (w >> 1) & 31
            m = mask(mb, me)
            if op == 21: e = '(rotl(%s, %d) & 0x%08X)' % (G(rD), sh, m)
            elif op == 23: e = '(rotl(%s, %s & 31) & 0x%08X)' % (G(rD), G(rB), m)
            else: e = '((rotl(%s, %d) & 0x%08X) | (%s & 0x%08X))' % (G(rD), sh, m, G(rA), (~m) & 0xFFFFFFFF)
            return '%s = %s;%s' % (G(rA), e, cr0s(G(rA)))
        if op == 24: return '%s = %s | 0x%X;' % (G(rA), G(rD), uimm) if (rA, rD, uimm) != (0, 0, 0) else ';'
        if op == 25: return '%s = %s | 0x%08X;' % (G(rA), G(rD), uimm << 16)
        if op == 26: return '%s = %s ^ 0x%X;' % (G(rA), G(rD), uimm)
        if op == 27: return '%s = %s ^ 0x%08X;' % (G(rA), G(rD), uimm << 16)
        if op == 28: return '%s = %s & 0x%X; cr0(c, %s);' % (G(rA), G(rD), uimm, G(rA))
        if op == 29: return '%s = %s & 0x%08X; cr0(c, %s);' % (G(rA), G(rD), uimm << 16, G(rA))
        # ---------------- D-form loads/stores
        dmem = {32: ('R32', 4), 34: ('R8', 1), 40: ('R16', 2), 42: ('(u32)(s32)(s16)R16', 2)}
        if op in (32, 33, 34, 35, 40, 41, 42, 43):
            base = op & ~1; upd = op & 1
            fn = dmem[base][0]
            if upd:
                return '{ u32 ea = %s + 0x%08X; %s = %s(ea); %s = ea; }' % (G(rA), simm & 0xFFFFFFFF, G(rD), fn, G(rA))
            return '%s = %s(%s + 0x%08X);' % (G(rD), fn, A0(), simm & 0xFFFFFFFF)
        smem = {36: 'W32', 38: 'W8', 44: 'W16'}
        if op in (36, 37, 38, 39, 44, 45):
            base = op & ~1; upd = op & 1
            if upd:
                return '{ u32 ea = %s + 0x%08X; %s(ea, %s); %s = ea; }' % (G(rA), simm & 0xFFFFFFFF, smem[base], G(rD), G(rA))
            return '%s(%s + 0x%08X, %s);' % (smem[base], A0(), simm & 0xFFFFFFFF, G(rD))
        if op == 46:
            return '{ u32 ea = %s + 0x%08X; for (int i = %d; i < 32; i++, ea += 4) c->gpr[i] = R32(ea); }' % (A0(), simm & 0xFFFFFFFF, rD)
        if op == 47:
            return '{ u32 ea = %s + 0x%08X; for (int i = %d; i < 32; i++, ea += 4) W32(ea, c->gpr[i]); }' % (A0(), simm & 0xFFFFFFFF, rD)
        if op in (48, 49, 50, 51, 52, 53, 54, 55):
            upd = op & 1
            ea = ('%s + 0x%08X' % (G(rA), simm & 0xFFFFFFFF)) if upd else ('%s + 0x%08X' % (A0(), simm & 0xFFFFFFFF))
            st = {48: 'c->ps0[{d}] = c->ps1[{d}] = RFS(ea);', 50: 'c->ps0[{d}] = RFD(ea);',
                  52: 'WFS(ea, c->ps0[{d}]);', 54: 'WFD(ea, c->ps0[{d}]);'}[op & ~1].format(d=rD)
            return '{ u32 ea = %s; %s%s }' % (ea, st, (' %s = ea;' % G(rA)) if upd else '')
        if op in (56, 57, 60, 61):
            d12 = w & 0xFFF
            if d12 & 0x800: d12 -= 0x1000
            W = (w >> 15) & 1; I = (w >> 12) & 7
            upd = op & 1
            ea = ('%s + 0x%08X' % (G(rA), d12 & 0xFFFFFFFF)) if upd else ('%s + 0x%08X' % (A0(), d12 & 0xFFFFFFFF))
            fn = 'rt_psq_l' if op < 60 else 'rt_psq_st'
            return '{ u32 ea = %s; %s(c, %d, ea, %d, %d);%s }' % (ea, fn, rD, W, I, (' %s = ea;' % G(rA)) if upd else '')
        if op == 31: return self.op31(a, w, rD, rA, rB, rc, G, A0)
        if op == 59: return self.op59(w, rD, rA, rB, rC, rc)
        if op == 63: return self.op63(w, rD, rA, rB, rC, rc)
        if op == 4: return self.op4(w, rD, rA, rB, rC, rc, G, A0)
        if op == 3:  # twi
            TO = rD
            return self.trap(a, TO, '(s32)%s' % G(rA), '(s32)%d' % simm)
        return None

    def trap(self, a, TO, x, y):
        if TO == 31: return 'rt_trap(c, 0x%08X);' % a
        conds = []
        if TO & 16: conds.append('%s < %s' % (x, y))
        if TO & 8: conds.append('%s > %s' % (x, y))
        if TO & 4: conds.append('%s == %s' % (x, y))
        if TO & 2: conds.append('(u32)%s < (u32)%s' % (x, y))
        if TO & 1: conds.append('(u32)%s > (u32)%s' % (x, y))
        if not conds: return ';'
        return 'if (%s) rt_trap(c, 0x%08X);' % (' || '.join(conds), a)

    def op31(self, a, w, rD, rA, rB, rc, G, A0):
        xo = (w >> 1) & 0x3FF
        oe = (w >> 10) & 1
        xo9 = xo & 0x1FF
        def cr(r): return (' cr0(c, %s);' % r) if rc else ''
        D, Ra, Rb = G(rD), G(rA), G(rB)
        S = D  # for logical ops rD field is rS
        def ov(expr_a, expr_b, res):  # signed add overflow
            return (' addo(c, %s, %s, %s);' % (expr_a, expr_b, res)) if oe else ''
        # arithmetic (XO-form with OE)
        if xo9 == 266: return '{ u32 x = %s, y = %s; %s = x + y;%s%s }' % (Ra, Rb, D, ov('x', 'y', D), cr(D))
        if xo9 == 10: return '{ u32 x = %s, y = %s; %s = addc(c, x, y, 0);%s%s }' % (Ra, Rb, D, ov('x', 'y', D), cr(D))
        if xo9 == 138: return '{ u32 x = %s, y = %s; %s = addc(c, x, y, c->xer_ca);%s%s }' % (Ra, Rb, D, ov('x', 'y', D), cr(D))
        if xo9 == 202: return '{ u32 x = %s; %s = addc(c, x, 0, c->xer_ca);%s%s }' % (Ra, D, ov('x', '0', D), cr(D))
        if xo9 == 234: return '{ u32 x = %s; %s = addc(c, x, 0xFFFFFFFFu, c->xer_ca);%s%s }' % (Ra, D, ov('x', '0xFFFFFFFFu', D), cr(D))
        if xo9 == 40: return '{ u32 x = %s, y = %s; %s = y - x;%s%s }' % (Ra, Rb, D, ov('~x', 'y', D), cr(D))
        if xo9 == 8: return '{ u32 x = %s, y = %s; %s = addc(c, ~x, y, 1);%s%s }' % (Ra, Rb, D, ov('~x', 'y', D), cr(D))
        if xo9 == 136: return '{ u32 x = %s, y = %s; %s = addc(c, ~x, y, c->xer_ca);%s%s }' % (Ra, Rb, D, ov('~x', 'y', D), cr(D))
        if xo9 == 200: return '{ u32 x = %s; %s = addc(c, ~x, 0, c->xer_ca);%s%s }' % (Ra, D, ov('~x', '0', D), cr(D))
        if xo9 == 232: return '{ u32 x = %s; %s = addc(c, ~x, 0xFFFFFFFFu, c->xer_ca);%s%s }' % (Ra, D, ov('~x', '0xFFFFFFFFu', D), cr(D))
        if xo9 == 104: return '{ u32 x = %s; %s = (u32)-(s32)x;%s%s }' % (Ra, D, (' c->xer_ov = (x == 0x80000000u); c->xer_so |= c->xer_ov;' if oe else ''), cr(D))
        if xo9 == 235: return '{ s64 p = (s64)(s32)%s * (s32)%s; %s = (u32)p;%s%s }' % (Ra, Rb, D, (' c->xer_ov = (p != (s32)p); c->xer_so |= c->xer_ov;' if oe else ''), cr(D))
        if xo9 == 75: return '%s = (u32)(((s64)(s32)%s * (s32)%s) >> 32);%s' % (D, Ra, Rb, cr(D))
        if xo9 == 11: return '%s = (u32)(((u64)%s * %s) >> 32);%s' % (D, Ra, Rb, cr(D))
        if xo9 == 491:
            return ('{ s32 x = (s32)%s, y = (s32)%s; int bad = (y == 0) || (x == (s32)0x80000000 && y == -1); '
                    '%s = bad ? ((x < 0) ? 0xFFFFFFFFu : 0) : (u32)(x / y);%s%s }') % (Ra, Rb, D,
                    (' c->xer_ov = bad; c->xer_so |= bad;' if oe else ''), cr(D))
        if xo9 == 459:
            return '{ u32 x = %s, y = %s; %s = y ? x / y : 0;%s%s }' % (Ra, Rb, D, (' c->xer_ov = !y; c->xer_so |= !y;' if oe else ''), cr(D))
        # logical (rS = rD field, result in rA)
        logic = {28: '%s & %s', 444: '%s | %s', 316: '%s ^ %s', 476: '~(%s & %s)', 124: '~(%s | %s)', 284: '~(%s ^ %s)',
                 60: '%s & ~%s', 412: '%s | ~%s'}
        if xo in logic:
            if xo == 444 and rD == rB and not rc: return '%s = %s;' % (Ra, S)
            return '%s = %s;%s' % (Ra, logic[xo] % (S, Rb), cr(Ra))
        if xo == 24: return '{ u32 n = %s & 63; %s = n > 31 ? 0 : %s << n; }%s' % (Rb, Ra, S, cr(Ra))
        if xo == 536: return '{ u32 n = %s & 63; %s = n > 31 ? 0 : %s >> n; }%s' % (Rb, Ra, S, cr(Ra))
        if xo == 792:
            return ('{ u32 n = %s & 63; s32 v = (s32)%s; if (n > 31) { %s = v < 0 ? 0xFFFFFFFFu : 0; c->xer_ca = v < 0; } '
                    'else { %s = (u32)(v >> n); c->xer_ca = (v < 0) && n && ((u32)v & ((1u << n) - 1)); } }%s') % (Rb, S, Ra, Ra, cr(Ra))
        if xo == 824:
            sh = rB
            if sh == 0: return '%s = %s; c->xer_ca = 0;%s' % (Ra, S, cr(Ra))
            return '{ s32 v = (s32)%s; %s = (u32)(v >> %d); c->xer_ca = (v < 0) && ((u32)v & 0x%X); }%s' % (S, Ra, sh, (1 << sh) - 1, cr(Ra))
        if xo == 26: return '%s = cntlz(%s);%s' % (Ra, S, cr(Ra))
        if xo == 922: return '%s = (u32)(s32)(s16)%s;%s' % (Ra, S, cr(Ra))
        if xo == 954: return '%s = (u32)(s32)(s8)%s;%s' % (Ra, S, cr(Ra))
        if xo == 0: return 'cmps(c, %d, (s32)%s, (s32)%s);' % (rD >> 2, Ra, Rb)
        if xo == 32: return 'cmpu(c, %d, %s, %s);' % (rD >> 2, Ra, Rb)
        if xo == 4: return self.trap(a, rD, '(s32)%s' % Ra, '(s32)%s' % Rb)
        # indexed loads / stores
        EA = '%s + %s' % (A0(), Rb)
        EAU = '%s + %s' % (Ra, Rb)
        ld = {23: 'R32', 87: 'R8', 279: 'R16', 343: '(u32)(s32)(s16)R16'}
        if xo in ld: return '%s = %s(%s);' % (D, ld[xo], EA)
        ldu = {55: 'R32', 119: 'R8', 311: 'R16', 375: '(u32)(s32)(s16)R16'}
        if xo in ldu: return '{ u32 ea = %s; %s = %s(ea); %s = ea; }' % (EAU, D, ldu[xo], Ra)
        st = {151: 'W32', 215: 'W8', 407: 'W16'}
        if xo in st: return '%s(%s, %s);' % (st[xo], EA, S)
        stu = {183: 'W32', 247: 'W8', 439: 'W16'}
        if xo in stu: return '{ u32 ea = %s; %s(ea, %s); %s = ea; }' % (EAU, stu[xo], S, Ra)
        if xo == 534: return '%s = bs32(R32(%s));' % (D, EA)
        if xo == 790: return '%s = bs16((u16)R16(%s));' % (D, EA)
        if xo == 662: return 'W32(%s, bs32(%s));' % (EA, S)
        if xo == 918: return 'W16(%s, bs16((u16)%s));' % (EA, S)
        if xo == 20: return '%s = R32(%s);' % (D, EA)                                     # lwarx
        if xo == 150: return 'W32(%s, %s); CRF_SET(c, 0, 2 | c->xer_so);' % (EA, S)      # stwcx.
        fl = {535: ('c->ps0[{d}] = c->ps1[{d}] = RFS(ea);', 0), 567: ('c->ps0[{d}] = c->ps1[{d}] = RFS(ea);', 1),
              599: ('c->ps0[{d}] = RFD(ea);', 0), 631: ('c->ps0[{d}] = RFD(ea);', 1),
              663: ('WFS(ea, c->ps0[{d}]);', 0), 695: ('WFS(ea, c->ps0[{d}]);', 1),
              727: ('WFD(ea, c->ps0[{d}]);', 0), 759: ('WFD(ea, c->ps0[{d}]);', 1),
              983: ('W32(ea, (u32)ps_bits(c, {d}));', 0)}
        if xo in fl:
            s, u = fl[xo]
            return '{ u32 ea = %s; %s%s }' % (EAU if u else EA, s.format(d=rD), (' %s = ea;' % Ra) if u else '')
        if xo == 597: return 'rt_lswi(c, %d, %s, %d);' % (rD, A0(), rB or 32)
        if xo == 725: return 'rt_stswi(c, %d, %s, %d);' % (rD, A0(), rB or 32)
        if xo == 533: return 'rt_lswi(c, %d, %s, c->xer_bc & 127);' % (rD, EA)
        if xo == 661: return 'rt_stswi(c, %d, %s, c->xer_bc & 127);' % (rD, EA)
        # cache / sync
        if xo == 1014: return 'rt_dcbz(%s);' % EA
        if xo == 470: return 'rt_dcbi(%s);' % EA
        if xo == 86: return 'rt_dcbf(%s);' % EA
        if xo == 982: return 'rt_icbi(%s);' % EA
        if xo in (54, 278, 246, 598, 854, 306, 566): return ';'
        # special registers
        spr = ((w >> 16) & 31) | (((w >> 11) & 31) << 5)
        if xo == 339:
            if spr == 8: return '%s = c->lr;' % D
            if spr == 9: return '%s = c->ctr;' % D
            if spr == 1: return '%s = (c->xer_so << 31) | (c->xer_ov << 30) | (c->xer_ca << 29) | c->xer_bc;' % D
            if 912 <= spr <= 919: return '%s = c->gqr[%d];' % (D, spr - 912)
            return '%s = rt_mfspr(c, %d);' % (D, spr)
        if xo == 467:
            if spr == 8: return 'c->lr = %s;' % D
            if spr == 9: return 'c->ctr = %s;' % D
            if spr == 1: return '{ u32 v = %s; c->xer_so = v >> 31; c->xer_ov = (v >> 30) & 1; c->xer_ca = (v >> 29) & 1; c->xer_bc = v & 127; }' % D
            if 912 <= spr <= 919: return 'c->gqr[%d] = %s;' % (spr - 912, D)
            return 'rt_mtspr(c, %d, %s);' % (spr, D)
        if xo == 371: return '%s = rt_mftb(c, %d);' % (D, 1 if spr == 269 else 0)
        if xo == 19: return '%s = c->cr;' % D
        if xo == 144:
            crm = (w >> 12) & 0xFF
            m = 0
            for i in range(8):
                if crm & (0x80 >> i): m |= 0xF0000000 >> (i * 4)
            return 'c->cr = (c->cr & 0x%08X) | (%s & 0x%08X);' % ((~m) & 0xFFFFFFFF, D, m)
        if xo == 512: return 'CRF_SET(c, %d, (c->xer_so << 3) | (c->xer_ov << 2) | (c->xer_ca << 1)); c->xer_so = c->xer_ov = c->xer_ca = 0;' % (rD >> 2)
        if xo == 83: return '%s = c->msr;' % D
        if xo == 146: return 'rt_mtmsr(c, %s);' % D
        if xo in (210, 242): return ';'
        if xo in (595, 659): return '%s = 0;' % D
        if xo in (310,): return '%s = 0;' % D
        if xo in (438,): return ';'
        return None

    def op59(self, w, d, a, b, cc, rc):
        xo = (w >> 1) & 31
        A, B, C = 'c->ps0[%d]' % a, 'c->ps0[%d]' % b, 'c->ps0[%d]' % cc
        e = {18: f'{A} / {B}', 20: f'{A} - {B}', 21: f'{A} + {B}', 25: f'{A} * {C}',
             28: f'{A} * {C} - {B}', 29: f'{A} * {C} + {B}', 30: f'-({A} * {C} - {B})', 31: f'-({A} * {C} + {B})',
             24: f'1.0 / {B}'}.get(xo)
        if e is None: return None
        r = ' CRF_SET(c, 1, c->fpscr >> 28);' if rc else ''
        return '{ f64 r = rnd_s(%s); c->ps0[%d] = c->ps1[%d] = r; }%s' % (e, d, d, r)

    def op63(self, w, d, a, b, cc, rc):
        xo10 = (w >> 1) & 0x3FF; xo = (w >> 1) & 31
        A, B, C = 'c->ps0[%d]' % a, 'c->ps0[%d]' % b, 'c->ps0[%d]' % cc
        D = 'c->ps0[%d]' % d
        r = ' CRF_SET(c, 1, c->fpscr >> 28);' if rc else ''
        e = {18: f'{A} / {B}', 20: f'{A} - {B}', 21: f'{A} + {B}', 25: f'{A} * {C}',
             28: f'{A} * {C} - {B}', 29: f'{A} * {C} + {B}', 30: f'-({A} * {C} - {B})', 31: f'-({A} * {C} + {B})',
             26: f'frsqrte({B})', 23: f'fsel({A}, {C}, {B})'}.get(xo)
        if e is not None: return '%s = %s;%s' % (D, e, r)
        if xo10 == 0 or xo10 == 32: return 'fcmp(c, %d, %s, %s);' % (d >> 2, A, B)
        if xo10 == 12: return '{ f64 v = rnd_s(%s); c->ps0[%d] = c->ps1[%d] = v; }%s' % (B, d, d, r)
        if xo10 == 14: return 'ps_set_bits(c, %d, 0xFFF8000000000000ull | (u32)fctiw_r(%s, c->fpscr & 3));%s' % (d, B, r)
        if xo10 == 15: return 'ps_set_bits(c, %d, 0xFFF8000000000000ull | (u32)fctiw_r(%s, 1));%s' % (d, B, r)
        if xo10 == 40: return '%s = -%s;%s' % (D, B, r)
        if xo10 == 72: return '%s = %s;%s' % (D, B, r)
        if xo10 == 136: return '%s = -fabs(%s);%s' % (D, B, r)
        if xo10 == 264: return '%s = fabs(%s);%s' % (D, B, r)
        if xo10 == 583: return 'ps_set_bits(c, %d, 0xFFF8000000000000ull | c->fpscr);%s' % (d, r)
        if xo10 == 711:
            fm = (w >> 17) & 0xFF; m = 0
            for i in range(8):
                if fm & (0x80 >> i): m |= 0xF0000000 >> (i * 4)
            return 'c->fpscr = (c->fpscr & 0x%08X) | ((u32)ps_bits(c, %d) & 0x%08X);%s' % ((~m) & 0xFFFFFFFF, b, m, r)
        if xo10 == 134:
            crf = d >> 2; imm = (w >> 12) & 15
            sh = 28 - 4 * crf
            return 'c->fpscr = (c->fpscr & ~(0xFu << %d)) | (%du << %d);%s' % (sh, imm, sh, r)
        if xo10 == 38: return 'c->fpscr |= 0x%08X;%s' % (0x80000000 >> d, r)
        if xo10 == 70: return 'c->fpscr &= ~0x%08Xu;%s' % (0x80000000 >> d, r)
        if xo10 == 64:
            return 'CRF_SET(c, %d, (c->fpscr >> (28 - %d*4)) & 15);' % (d >> 2, a >> 2)
        return None

    def op4(self, w, d, a, b, cc, rc, G, A0):
        xo10 = (w >> 1) & 0x3FF; xo5 = (w >> 1) & 31; xo6 = (w >> 1) & 63
        if xo6 in (6, 7, 38, 39):  # psq_lx / psq_stx / psq_lux / psq_stux
            W = (w >> 10) & 1; I = (w >> 7) & 7
            upd = xo6 >= 38
            ea = ('%s + %s' % (G(a), G(b))) if upd else ('%s + %s' % (A0(), G(b)))
            fn = 'rt_psq_l' if xo6 in (6, 38) else 'rt_psq_st'
            return '{ u32 ea = %s; %s(c, %d, ea, %d, %d);%s }' % (ea, fn, d, W, I, (' %s = ea;' % G(a)) if upd else '')
        a0, a1, b0, b1, c0, c1 = ('c->ps0[%d]' % a, 'c->ps1[%d]' % a, 'c->ps0[%d]' % b, 'c->ps1[%d]' % b,
                                  'c->ps0[%d]' % cc, 'c->ps1[%d]' % cc)
        r = ' CRF_SET(c, 1, c->fpscr >> 28);' if rc else ''
        def pair(e0, e1, rnd=True):
            if rnd: return '{ f64 x0 = rnd_s(%s), x1 = rnd_s(%s); c->ps0[%d] = x0; c->ps1[%d] = x1; }%s' % (e0, e1, d, d, r)
            return '{ f64 x0 = %s, x1 = %s; c->ps0[%d] = x0; c->ps1[%d] = x1; }%s' % (e0, e1, d, d, r)
        if xo10 == 0 or xo10 == 32: return 'fcmp(c, %d, %s, %s);' % (d >> 2, a0, b0)
        if xo10 == 64 or xo10 == 96: return 'fcmp(c, %d, %s, %s);' % (d >> 2, a1, b1)
        if xo10 == 40: return pair('-' + b0, '-' + b1, False)
        if xo10 == 72: return pair(b0, b1, False)
        if xo10 == 136: return pair('-fabs(%s)' % b0, '-fabs(%s)' % b1, False)
        if xo10 == 264: return pair('fabs(%s)' % b0, 'fabs(%s)' % b1, False)
        if xo10 == 528: return pair(a0, b0)
        if xo10 == 560: return pair(a0, b1)
        if xo10 == 592: return pair(a1, b0)
        if xo10 == 624: return pair(a1, b1)
        if xo10 == 1014: return 'rt_dcbz(%s + %s);' % (A0(), G(b))  # dcbz_l
        m = {18: (f'{a0} / {b0}', f'{a1} / {b1}'), 20: (f'{a0} - {b0}', f'{a1} - {b1}'), 21: (f'{a0} + {b0}', f'{a1} + {b1}'),
             25: (f'{a0} * {c0}', f'{a1} * {c1}'), 23: (f'fsel({a0}, {c0}, {b0})', f'fsel({a1}, {c1}, {b1})'),
             24: (f'1.0 / {b0}', f'1.0 / {b1}'), 26: (f'frsqrte({b0})', f'frsqrte({b1})'),
             28: (f'{a0} * {c0} - {b0}', f'{a1} * {c1} - {b1}'), 29: (f'{a0} * {c0} + {b0}', f'{a1} * {c1} + {b1}'),
             30: (f'-({a0} * {c0} - {b0})', f'-({a1} * {c1} - {b1})'), 31: (f'-({a0} * {c0} + {b0})', f'-({a1} * {c1} + {b1})'),
             10: (f'{a0} + {b1}', c1), 11: (c0, f'{a0} + {b1}'),
             12: (f'{a0} * {c0}', f'{a1} * {c0}'), 13: (f'{a0} * {c1}', f'{a1} * {c1}'),
             14: (f'{a0} * {c0} + {b0}', f'{a1} * {c0} + {b1}'), 15: (f'{a0} * {c1} + {b0}', f'{a1} * {c1} + {b1}')}
        if xo5 in m:
            e0, e1 = m[xo5]
            return pair(e0, e1, xo5 != 23)
        return None

    @staticmethod
    def gpr_writes(w):
        op = w >> 26; d, a = (w >> 21) & 31, (w >> 16) & 31
        if op in (7, 8, 12, 13, 14, 15, 32, 34, 40, 42): return {d}
        if op in (33, 35, 41, 43): return {d, a}
        if op in (37, 39, 45, 47, 49, 51, 53, 55, 57, 61): return {a}
        if op == 46: return set(range(d, 32))
        if op in (20, 21, 23, 24, 25, 26, 27, 28, 29): return {a}
        if op == 31:
            xo = (w >> 1) & 0x3FF
            if xo in (28, 444, 316, 476, 124, 284, 60, 412, 24, 536, 792, 824, 26, 922, 954): return {a}
            if xo in (151, 215, 407, 662, 918, 150, 663, 727, 983, 725, 661, 0, 32, 4, 1014, 470, 86, 982, 54, 278, 246, 598, 854, 467, 144, 146):
                return set()
            if xo in (55, 119, 311, 375, 183, 247, 439, 567, 631, 695, 759): return {d, a}
            return {d}
        if op == 18 and (w & 1): return set(range(0, 13))   # calls clobber volatile regs
        return set()

    def scan_table(self, a, lo, hi):
        """Switch tables without relocations: find 'lwzx rT,rA,rB / mtctr rT / bctr', get the table base
        by constant propagation of lis/addi/ori from the function start, the size from 'cmplwi rI,N'."""
        f = self.func_of(a); flo, fhi = (f[0], f[1]) if f else (lo, hi)
        lw = None
        for k in range(1, 6):
            w = self.dol.word(a - 4 * k) or 0
            if w >> 26 == 31 and ((w >> 1) & 0x3FF) == 23: lw = ((w >> 16) & 31, (w >> 11) & 31, a - 4 * k); break
        if not lw: return []
        known = {}
        for x in range(flo, lw[2], 4):
            w = self.dol.word(x) or 0
            op = w >> 26; d, ra = (w >> 21) & 31, (w >> 16) & 31
            val = None
            if op == 15 and ra == 0: val = (w & 0xFFFF) << 16
            elif op == 14 and ra and ra in known: val = (known[ra] + sx16(w & 0xFFFF)) & 0xFFFFFFFF
            elif op == 24 and d in known: val = known[d] | (w & 0xFFFF); d = ra
            for r in self.gpr_writes(w): known.pop(r, None)
            if val is not None: known[d] = val
        count = None
        for k in range(1, 16):
            w = self.dol.word(lw[2] - 4 * k) or 0
            if w >> 26 == 10: count = (w & 0xFFFF) + 1; break
        cands = [known[r] for r in lw[:2] if r in known]
        out = []
        for t in cands:
            for i in range(count if count else 1024):
                v = self.dol.word(t + 4 * i)
                if v is None or not (flo <= v < fhi): break
                out.append(v)
            if out: break
        return [x for x in out if lo <= x < hi]

    # ---- one C function for entry e (covers [e, func_end))
    def function(self, e):
        lo = e; hi = self.entries[e][1]
        labels = set(); body = []
        self.cur_labels = labels; self.cur_resume = None
        for a in range(lo, hi, 4):   # a call to a 'save' function in this body: its return address is the resume point
            w = self.dol.word(a) or 0
            if w >> 26 == 18 and (w & 3) == 1:
                li = w & 0x03FFFFFC
                if li & 0x02000000: li -= 0x04000000
                if 'save' in self.hle.get((a + li) & 0xFFFFFFFF, set()): self.cur_resume = a + 4
        jt = [t for t in self.labels_data if lo <= t < hi]
        for a in range(lo, hi, 4):
            w = self.dol.word(a)
            if w is None: break
            s = self.insn(a, w, lo, hi, labels, jt)
            if s is None:
                self.unimpl[w >> 26] = self.unimpl.get(w >> 26, 0) + 1
                s = 'rt_unimpl(c, 0x%08X, 0x%08X); return;' % (a, w)
            body.append((a, w, s))
        out = []
        for a, w, s in body:
            lab = ('L_%08X: ' % a) if a in labels else ''
            out.append('  %s%s\n' % (lab, s))
        # falling off the end (unreachable after a final blr/b): continue into whatever follows
        out.append('  %s\n' % ('f_%08X(c); return;' % hi if hi in self.entries else 'return;'))
        nm = self.name.get(e, '')
        # labels referenced must exist; any label outside emitted range -> should not happen
        return nm, ''.join(out)

    def run(self, outdir, hle):
        self.hle = hle
        # entries can be added while emitting (cross-function targets); iterate to fixpoint
        done = {}
        while True:
            todo = sorted(set(self.entries) - set(done))
            if not todo: break
            for e in todo: done[e] = self.function(e)
        os.makedirs(outdir, exist_ok=True)
        ents = sorted(done)
        protos = ''.join('void f_%08X(CPU *restrict c);\n' % e for e in ents)
        hfn = lambda e: [x[3:] for x in hle[e] if x.startswith('fn=')][0]
        protos += ''.join('void o_%08X(CPU *restrict c);\nvoid %s(CPU *restrict c);\n' % (e, hfn(e)) for e in sorted(hle) if e in done)
        write_if_changed(os.path.join(outdir, 'funcs.h'), '#pragma once\n#include "rt.h"\n' + protos)
        nshards = 0
        for i in range(0, len(ents), SHARD):
            parts = ['#include "funcs.h"\n#pragma GCC diagnostic ignored "-Wunused-label"\n']
            for e in ents[i:i + SHARD]:
                nm, body = done[e]
                cm = ' /* %s */' % nm.replace('*/', '* /') if nm else ''
                if e in hle:
                    parts.append('void f_%08X(CPU *restrict c) { c->pc_hint = 0x%08X; %s(c); }%s\n' % (e, e, hfn(e), cm))
                    parts.append('void o_%08X(CPU *restrict c) {%s\n%s}\n' % (e, cm, body))
                else:
                    parts.append('void f_%08X(CPU *restrict c) {%s\n  FENTER(0x%08X);\n%s}\n' % (e, cm, e, body))
            write_if_changed(os.path.join(outdir, 'f%03d.c' % nshards), ''.join(parts))
            nshards += 1
        # remove stale shards from earlier runs
        for fn in os.listdir(outdir):
            m = re.fullmatch(r'f(\d{3})\.c', fn)
            if m and int(m.group(1)) >= nshards: os.remove(os.path.join(outdir, fn))
        tab = ['#include "funcs.h"\n', 'const struct { u32 a; GuestFn f; } g_funcs[] = {\n']
        tab += ['  {0x%08X, f_%08X},\n' % (e, e) for e in ents]
        tab.append('};\nconst int g_nfuncs = %d;\n' % len(ents))
        write_if_changed(os.path.join(outdir, 'funcs.c'), ''.join(tab))
        # symbol table for diagnostics
        with open(os.path.join(outdir, 'syms.txt'), 'w') as f:
            for e in ents: f.write('%08X %08X %s\n' % (e, self.entries[e][1], self.name.get(e, '')))
        print('%d functions (%d entries) in %d shards; unimplemented opcodes: %s' %
              (len(self.funcs), len(ents), nshards, self.unimpl or 'none'))

def write_if_changed(path, text):
    if os.path.exists(path) and open(path).read() == text: return
    open(path, 'w').write(text)

def main():
    dol = Dol(sys.argv[1]); elf = Elf(sys.argv[2]); out = sys.argv[3]
    g = Gen(dol, elf)
    g.discover()
    name2addr = {}
    for a, n in g.name.items(): name2addr.setdefault(n, a)
    here = os.path.dirname(os.path.abspath(__file__))
    hle = load_hle(os.path.join(here, '..', 'runtime', 'hle.list'), name2addr)
    for a in hle: g.add_entry(a)
    g.run(out, hle)

if __name__ == '__main__':
    main()
