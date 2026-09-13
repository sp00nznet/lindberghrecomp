#!/usr/bin/env python3
"""
elf32.py - enough ELF32 to drive the lifter.

A Lindbergh game is a 32-bit little-endian x86 ELF built against MontaVista
Linux 4.0 (glibc 2.3.x), so this handles exactly that shape and refuses
anything else loudly rather than producing wrong addresses quietly.

What the rest of the pipeline needs from here:

    image_base / image_size   the span the lifter treats as "in the image"
    read_va(va, n)            flat reads at original virtual addresses
    functions()               {addr: (size, name)} from the symbol table
    needed()                  the .so list, i.e. the HLE surface the runtime
                              has to provide (libGL, libc, libsegaapi, ...)
"""

import bisect
import struct
from dataclasses import dataclass

ELFCLASS32, ELFDATA2LSB, EM_386 = 1, 1, 3
PT_LOAD, PT_DYNAMIC = 1, 2
SHT_SYMTAB, SHT_DYNSYM, SHT_STRTAB = 2, 11, 3
STT_FUNC = 2
DT_NULL, DT_NEEDED, DT_STRTAB, DT_STRSZ = 0, 1, 5, 10


@dataclass
class Segment:
    type: int
    offset: int
    vaddr: int
    filesz: int
    memsz: int
    flags: int


@dataclass
class Section:
    name: str
    type: int
    addr: int
    offset: int
    size: int
    link: int
    entsize: int


class Elf32:
    def __init__(self, path):
        self.path = path
        with open(path, "rb") as f:
            self.data = f.read()
        d = self.data
        if d[:4] != b"\x7fELF":
            raise ValueError("%s: not an ELF" % path)
        if d[4] != ELFCLASS32 or d[5] != ELFDATA2LSB:
            raise ValueError("%s: not 32-bit little-endian ELF" % path)
        (self.e_type, self.e_machine, _ver, self.entry, e_phoff, e_shoff,
         _flags, _ehsize, e_phentsize, e_phnum, e_shentsize, e_shnum,
         e_shstrndx) = struct.unpack_from("<HHIIIIIHHHHHH", d, 16)
        if self.e_machine != EM_386:
            raise ValueError("%s: machine %d, expected EM_386" % (path, self.e_machine))

        self.segments = []
        for i in range(e_phnum):
            (p_type, p_offset, p_vaddr, _paddr, p_filesz, p_memsz, p_flags,
             _align) = struct.unpack_from("<8I", d, e_phoff + i * e_phentsize)
            self.segments.append(
                Segment(p_type, p_offset, p_vaddr, p_filesz, p_memsz, p_flags))

        raw = [struct.unpack_from("<IIIIIIIIII", d, e_shoff + i * e_shentsize)
               for i in range(e_shnum)]
        shstr = raw[e_shstrndx][4] if e_shnum else 0
        self.sections = [
            Section(self._cstr(shstr + r[0]), r[1], r[3], r[4], r[5], r[6], r[9])
            for r in raw
        ]

    # ---- helpers ----
    def _cstr(self, off):
        end = self.data.index(b"\0", off)
        return self.data[off:end].decode("latin-1")

    def section(self, name):
        for s in self.sections:
            if s.name == name:
                return s
        return None

    @property
    def loads(self):
        return [s for s in self.segments if s.type == PT_LOAD]

    @property
    def image_base(self):
        """Lowest mapped page. A Linux i386 EXEC links at 0x08048000; a PIE
        (ET_DYN) links at 0, and the runtime relocates it."""
        return min(s.vaddr for s in self.loads) & ~0xFFF

    @property
    def image_size(self):
        return max(s.vaddr + s.memsz for s in self.loads) - self.image_base

    def read_va(self, va, n):
        """Flat read at an original virtual address. Bytes past a segment's
        filesz are .bss - zero, not an error."""
        for s in self.loads:
            if s.vaddr <= va < s.vaddr + s.memsz:
                out = bytearray(n)
                have = max(0, min(n, s.vaddr + s.filesz - va))
                if have:
                    fo = s.offset + (va - s.vaddr)
                    out[:have] = self.data[fo:fo + have]
                return bytes(out)
        raise ValueError("VA %#x is not in any PT_LOAD" % va)

    def functions(self):
        """{addr: (size, name)} for every STT_FUNC symbol.

        Symbols with no size get bounds synthesised from the next function
        symbol, clamped to the end of their section. That is not a nicety: the
        handful of functions an ELF carries with size 0 are the hand-written
        assembly out of crt1.o - `_start`, `call_gmon_start`, `frame_dummy` -
        because assembly rarely bothers with a .size directive. They are also
        the entire boot path, so dropping them leaves a binary whose entry
        point does not exist.

        A stripped game ELF has no .symtab and this comes back nearly empty.
        That is the expected case for some titles, and the driver then wants a
        funcs.txt from Ghidra or IDA instead."""
        syms = []
        for sh in self.sections:
            if sh.type not in (SHT_SYMTAB, SHT_DYNSYM) or not sh.entsize:
                continue
            strtab = self.sections[sh.link].offset
            for off in range(sh.offset, sh.offset + sh.size, sh.entsize):
                name_off, value, size, info = struct.unpack_from("<IIIB", self.data, off)
                if (info & 0xF) != STT_FUNC or not value:
                    continue
                syms.append((value, size, self._cstr(strtab + name_off)))

        bounds = sorted({v for v, _s, _n in syms})
        out = {}
        for value, size, name in syms:
            if not size:
                i = bisect.bisect_right(bounds, value)
                nxt = bounds[i] if i < len(bounds) else 0
                end = self._section_end(value)
                if nxt and (not end or nxt < end):
                    end = nxt
                size = end - value if end > value else 0
                if not size:
                    continue
            out[value] = (size, name)
        return out

    def _section_end(self, va):
        """End VA of the allocated section containing va, or 0."""
        for sh in self.sections:
            if sh.addr and sh.addr <= va < sh.addr + sh.size:
                return sh.addr + sh.size
        return 0

    def needed(self):
        """DT_NEEDED shared libraries - the HLE surface the runtime must cover."""
        dyn = next((s for s in self.segments if s.type == PT_DYNAMIC), None)
        if dyn is None:
            return []
        tags, off = [], dyn.offset
        while True:
            tag, val = struct.unpack_from("<II", self.data, off)
            off += 8
            if tag == DT_NULL:
                break
            tags.append((tag, val))
        strtab_va = next((v for t, v in tags if t == DT_STRTAB), None)
        if strtab_va is None:
            return []
        strtab_fo = next(s.offset + (strtab_va - s.vaddr)
                         for s in self.loads
                         if s.vaddr <= strtab_va < s.vaddr + s.filesz)
        return [self._cstr(strtab_fo + v) for t, v in tags if t == DT_NEEDED]

    def plt_map(self):
        """{plt stub address: imported symbol name}.

        Every libc / libGL / libsegaapi call in the game goes through a PLT
        stub, so without this map the recompiled code has nowhere to send them
        and the output is inert. Assumes the stock i386 PLT the SysV ABI
        specifies and every linker of the era emitted: a 16-byte PLT0 header
        followed by one 16-byte stub per .rel.plt entry, in order."""
        rel = self.section(".rel.plt")
        plt = self.section(".plt")
        dynsym = self.section(".dynsym")
        if not (rel and plt and dynsym):
            return {}
        strtab = self.sections[dynsym.link].offset
        out = {}
        for n in range(rel.size // 8):
            _off, info = struct.unpack_from("<II", self.data, rel.offset + n * 8)
            sym = dynsym.offset + (info >> 8) * dynsym.entsize
            name_off = struct.unpack_from("<I", self.data, sym)[0]
            out[plt.addr + (n + 1) * 16] = self._cstr(strtab + name_off)
        return out
