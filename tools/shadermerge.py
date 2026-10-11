"""shadermerge.py out.bin in1.bin [in2.bin ...] - merges shader config files written by RGH_SHADERS
(records of 353 little-endian words: mask, BP[256], XF[0x1000..0x105F]) keeping one record per shader key
(the same key as runtime/gxrender.c shader_rec / web/gx-renderer.js)."""
import struct, sys
def key(r):
    mask, bp, xf = r[0], r[1:257], r[257:353]
    X = lambda a: xf[a - 0x1000]
    gen = bp[0]; nst = ((gen >> 10) & 15) + 1; nind = (gen >> 16) & 7; ntg = min(X(0x103F) & 15, 8)
    p = [mask, nst, ntg, X(0x1009) & 3, nind, X(0x100E), X(0x100F), X(0x1010), X(0x1011), X(0x1012) & 1,
         (bp[0xF3] >> 16) & 255, bp[0xF1] >> 21]
    for i in range(nst):
        p += [bp[0xC0 + 2 * i] & 0xFFFFFF, bp[0xC1 + 2 * i] & 0xFFFFFF, (bp[0x28 + (i >> 1)] >> ((i & 1) * 12)) & 0xFFF, bp[0x10 + i] & 0x1FFFFF]
    p += [bp[0xF6 + i] & 0xFFFFFF for i in range(8)]
    for i in range(ntg): p += [X(0x1040 + i), X(0x1050 + i) & 0x13F]
    p += [bp[0x27] & 0xFFFFFF, bp[0x25] & 0xFFFFFF, bp[0x26] & 0xFFFFFF]
    return tuple(p)
seen, out = set(), []
for fn in sys.argv[2:]:
    d = open(fn, 'rb').read(); n = len(d) // 1412; new = 0
    for i in range(n):
        r = struct.unpack_from('<353I', d, i * 1412); k = key(r)
        if k not in seen: seen.add(k); out.append(d[i * 1412:(i + 1) * 1412]); new += 1
    print(f'{fn}: {n} records, {new} new')
open(sys.argv[1], 'wb').write(b''.join(out)); print(f'{sys.argv[1]}: {len(out)} shaders')
