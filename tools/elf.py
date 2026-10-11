"""Minimal ELF32 big-endian reader for LynWiiRetail.elf (the unstripped build shipped on the disc).
Gives sections, symbols and relocations; used by gen.py to find function boundaries, every
function-pointer / jump-table reference and every direct branch target exactly."""
import struct

class Elf:
    def __init__(self, path):
        d = self.d = open(path, 'rb').read()
        if d[:4] != b'\x7fELF' or d[4] != 1 or d[5] != 2:
            raise ValueError('not an ELF32 big-endian file')
        (self.type, self.machine, _, self.entry, phoff, shoff, _, _, _, _, shentsize, shnum, shstrndx) = \
            struct.unpack('>HHIIIIIHHHHHH', d[16:52])
        self.sections = []
        for i in range(shnum):
            o = shoff + i * shentsize
            name, typ, flags, addr, off, size, link, info, align, entsize = struct.unpack('>10I', d[o:o + 40])
            self.sections.append(dict(idx=i, name_off=name, type=typ, flags=flags, addr=addr, off=off, size=size,
                                      link=link, info=info, entsize=entsize))
        st = self.sections[shstrndx]
        for s in self.sections:
            s['name'] = self._str(st['off'], s['name_off'])
        self.by_name = {s['name']: s for s in self.sections}
        self._symbols()

    def _str(self, base, off):
        e = self.d.index(b'\0', base + off)
        return self.d[base + off:e].decode('latin-1')

    def _symbols(self):
        sym = self.by_name['.symtab']; strt = self.sections[sym['link']]
        self.syms = []
        for i in range(sym['size'] // 16):
            o = sym['off'] + i * 16
            name, value, size, info, other, shndx = struct.unpack('>IIIBBH', self.d[o:o + 16])
            self.syms.append(dict(name=self._str(strt['off'], name), value=value, size=size,
                                  bind=info >> 4, type=info & 15, shndx=shndx))

    def relocs(self):
        """Yield (section_name_patched, r_offset, type, target_address) for every RELA entry."""
        for s in self.sections:
            if s['type'] != 4:  # SHT_RELA
                continue
            tgt = self.sections[s['info']]
            for i in range(s['size'] // 12):
                o = s['off'] + i * 12
                off, info, addend = struct.unpack('>IIi', self.d[o:o + 12])
                sy = self.syms[info >> 8]
                yield tgt['name'], off, info & 0xFF, (sy['value'] + addend) & 0xFFFFFFFF
