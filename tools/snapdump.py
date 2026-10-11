"""snapdump.py - list the records of a renderer snapshot (runtime/gxrender.c RGH_SNAP)
usage: python3 tools/snapdump.py build/snaps/snap_N.bin [--draws] [--bp] [--xf]"""
import struct, sys
NAMES = {1: 'BP', 2: 'XF', 3: 'DRAW', 4: 'TEX', 5: 'TEXUP', 6: 'TEXFREE', 7: 'COPY', 8: 'FRAME', 9: 'CP'}
def main():
    d = open(sys.argv[1], 'rb').read(); w = struct.unpack('<%dI' % (len(d) // 4), d)
    opts = set(sys.argv[2:]); i = 0; frame = 0
    while i < len(w):
        hdr = w[i]; t, n = hdr >> 24, hdr & 0xFFFFFF
        if n == 0: break
        r = w[i:i + n]
        if t == 3 and '--draws' in opts:
            prim, cnt, mask, stride = r[1:5]; pad = stride >> 16; stride &= 0xFFFF
            vals = d[(i + 5 + pad) * 4:(i + n) * 4]
            pos = [struct.unpack_from('<3f', vals, k * stride) for k in range(min(cnt, 4))] if mask & 1 else []
            print('f%d DRAW prim=%02X n=%d mask=%04X stride=%d pos=%s' % (frame, prim, cnt, mask, stride, ' '.join('(%.1f,%.1f,%.2f)' % p for p in pos)))
        elif t == 1 and '--bp' in opts: print('f%d BP %02X = %06X' % (frame, r[1], r[2]))
        elif t == 2 and '--xf' in opts and r[2] < 64: print('f%d XF %04X n=%d %s' % (frame, r[1], r[2], ' '.join('%08X' % x for x in r[3:3 + r[2]])))
        elif t in (4, 5, 6, 7, 9) and '--all' in opts: print('f%d %s %s' % (frame, NAMES[t], ' '.join('%X' % x for x in r[1:min(n, 10)])))
        elif t == 8: frame += 1; print('---- FRAME', r[1])
        i += n
main()
