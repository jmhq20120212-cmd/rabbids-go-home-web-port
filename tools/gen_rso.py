#!/usr/bin/env python3
"""gen_rso.py - recompile the code of the dynamically loaded module Ai2CppWiiFinal.rso (the game's
AI scripts compiled to C++). The module is relocated by the game at run time, so it is translated
from a memory snapshot taken after the game linked it (tools: RGH_DUMP=1 ./build/rgh game) at the
address where the game always places it; function names come from the module's export table.

Usage: python3 -I tools/gen_rso.py game/part/files/Ai2CppWiiFinal.rso build/mem1.bin gen_rso/ [gen/syms.txt]
The runtime checks at first use that the module really sits at the translated address."""
import os, sys, struct, re
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import gen

class MemImage:
    def __init__(self, mem1):
        self.m = mem1
    def word(self, a):
        p = a & 0x1FFFFFFF
        if a >= 0xE0000000 or p + 4 > len(self.m): return None
        return struct.unpack('>I', self.m[p:p + 4])[0]

def find_header(m, rso):
    nsec, sectab, = struct.unpack('>II', rso[8:16])
    bss = struct.unpack('>I', rso[0x1C:0x20])[0]
    for j in range(0, len(m) - 0x58, 32):
        if struct.unpack('>I', m[j + 8:j + 12])[0] == nsec and struct.unpack('>I', m[j + 0x18:j + 0x1C])[0] == 1 \
           and struct.unpack('>I', m[j + 0x1C:j + 0x20])[0] == bss:
            return 0x80000000 + j
    raise SystemExit('module header not found in the memory snapshot')

class ModGen(gen.Gen):
    def __init__(self, img, main_entries, text_lo, text_hi, funcs, names, data_ranges):
        self.dol = img; self.elf = None
        self.funcs = funcs; self.fstarts = [f[0] for f in funcs]
        self.entries = {f[0]: (f[0], f[1]) for f in funcs}
        self.name = names; self.labels_data = set(); self.hle = {}; self.unimpl = {}
        self.main_entries = main_entries; self.lo, self.hi = text_lo, text_hi
        # pointers stored in the module's data that land inside its code: switch tables / vtables
        for lo, hi in data_ranges:
            for a in range(lo, hi, 4):
                v = img.word(a)
                if v and text_lo <= v < text_hi and not v & 3: self.labels_data.add(v)
        for v in list(self.labels_data):
            f = self.func_of(v)
            if f and f[0] == v: continue
        # bl targets inside the module
        for f in funcs:
            for a in range(f[0], f[1], 4):
                w = img.word(a) or 0
                if w >> 26 == 18 and (w & 1) and not (w & 2):
                    li = w & 0x03FFFFFC
                    if li & 0x02000000: li -= 0x04000000
                    t = (a + li) & 0xFFFFFFFF
                    if text_lo <= t < text_hi: self.add_entry(t)

    def call(self, t, nxt, ret=False):
        if t in self.main_entries and not (self.lo <= t < self.hi):
            if ret: return 'f_%08X(c); return;' % t
            return 'c->lr = 0x%08X; f_%08X(c);' % (nxt, t)
        if not (self.lo <= t < self.hi):
            return ('call_indirect(c, 0x%08X); return;' % t) if ret else ('c->lr = 0x%08X; call_indirect(c, 0x%08X);' % (nxt, t))
        return gen.Gen.call(self, t, nxt, ret)

def main():
    rso = open(sys.argv[1], 'rb').read(); mem = open(sys.argv[2], 'rb').read(); out = sys.argv[3]
    symf = sys.argv[4] if len(sys.argv) > 4 else 'gen/syms.txt'
    main_entries = set(int(l.split()[0], 16) for l in open(symf) if l.strip())
    hdr = find_header(mem, rso)
    img = MemImage(mem)
    nsec, sectab = struct.unpack('>II', rso[8:16])
    # section table in memory (absolute addresses after the game located the module)
    mtab = img.word(hdr + 0x0C)
    secs = [(img.word(mtab + 8 * k), img.word(mtab + 8 * k + 4)) for k in range(nsec)]
    text_lo, text_sz = secs[1]
    text_hi = text_lo + text_sz
    data_ranges = [(a, a + s) for i, (a, s) in enumerate(secs) if i > 1 and a and s and a < 0x81800000]
    # exports (from the file: name offset, section offset, section index, hash)
    et, es, en = struct.unpack('>III', rso[0x40:0x4C])
    names = {}; starts = set()
    for i in range(es // 16):
        no, off, sec, _h = struct.unpack('>4I', rso[et + 16 * i:et + 16 * i + 16])
        if sec != 1: continue
        a = text_lo + off
        nm = rso[en + no:rso.index(b'\0', en + no)].decode('latin-1')
        names[a] = nm; starts.add(a)
    starts.add(text_lo)
    # extra function starts: 'stwu r1,-N(r1)' right after a blr
    for a in range(text_lo + 4, text_hi, 4):
        if (img.word(a) & 0xFFFF8000) == 0x94218000 and img.word(a - 4) == 0x4E800020: starts.add(a)
    ss = sorted(starts)
    funcs = [[a, (ss[i + 1] if i + 1 < len(ss) else text_hi), names.get(a, 'rso_%08X' % a)] for i, a in enumerate(ss)]
    print('module at %08X, text %08X-%08X, %d functions' % (hdr, text_lo, text_hi, len(funcs)))
    g = ModGen(img, main_entries, text_lo, text_hi, funcs, names, data_ranges)
    g.SHARD_PREFIX = 'r'
    # emit
    done = {}
    while True:
        todo = sorted(set(g.entries) - set(done))
        if not todo: break
        for e in todo: done[e] = g.function(e)
    os.makedirs(out, exist_ok=True)
    ents = sorted(done)
    protos = ''.join('void f_%08X(CPU *restrict c);\n' % e for e in ents)
    gen.write_if_changed(os.path.join(out, 'rfuncs.h'), '#pragma once\n#include "funcs.h"\n' + protos)
    n = 0
    for i in range(0, len(ents), gen.SHARD):
        parts = ['#include "rfuncs.h"\n']
        for e in ents[i:i + gen.SHARD]:
            nm, body = done[e]
            cm = ' /* %s */' % nm.replace('*/', '* /') if nm else ''
            parts.append('void f_%08X(CPU *restrict c) {%s\n  FENTER(0x%08X);\n%s}\n' % (e, cm, e, body))
        gen.write_if_changed(os.path.join(out, 'r%03d.c' % n), ''.join(parts)); n += 1
    for fn in os.listdir(out):
        m = re.fullmatch(r'r(\d{3})\.c', fn)
        if m and int(m.group(1)) >= n: os.remove(os.path.join(out, fn))
    # checksum of the translated text so the runtime can verify the module is where we expect
    ck = 0
    for a in range(text_lo, text_hi, 4): ck = (ck * 31 + (img.word(a) or 0)) & 0xFFFFFFFF
    tab = ['#include "rfuncs.h"\n', 'const struct { u32 a; GuestFn f; } g_rso_funcs[] = {\n']
    tab += ['  {0x%08X, f_%08X},\n' % (e, e) for e in ents]
    tab.append('};\nconst int g_rso_nfuncs = %d;\nconst u32 g_rso_text_lo = 0x%08X, g_rso_text_hi = 0x%08X, g_rso_cksum = 0x%08X;\n' % (len(ents), text_lo, text_hi, ck))
    gen.write_if_changed(os.path.join(out, 'rfuncs.c'), ''.join(tab))
    with open(os.path.join(out, 'syms.txt'), 'w') as f:
        for e in ents: f.write('%08X %08X %s\n' % (e, g.entries[e][1], g.name.get(e, '')))
    print('%d entries in %d shards; unimplemented: %s' % (len(ents), n, g.unimpl or 'none'))

if __name__ == '__main__':
    main()
