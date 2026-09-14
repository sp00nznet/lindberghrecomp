"""A STT_FUNC symbol inside the PLT is a stub, not a function.

Virtua Tennis 3 declares `memalign@@GLIBC_2.0` at 0x0804E258 with a size of
486 bytes. That address is inside .plt and 486 bytes spans thirty PLT entries,
so taking the symbol at its word produces a "function" whose body is a run of
`jmp *[got]` stubs. Lifting it looks fine and even runs - right up to the
first call, where the GOT slot still holds its lazy-binding value of PLT+6,
control lands in the push-and-resolve half of a stub, and there is no lifted
function there for dispatch to find. Nor could there be.

.dynsym records an imported symbol's PLT slot as its value in some link
configurations, which is where these come from. Let's Go Jungle carries ten
and Ghost Squad Evolution two; they were lifted too, and simply never called.

This builds a small ELF rather than checking in a game binary.

Run: py -3.11 tools/elf/test_elf32.py
"""

import os
import struct
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from elf.elf32 import Elf32

EM_386, SHT_PROGBITS, SHT_SYMTAB, SHT_STRTAB, PT_LOAD = 3, 1, 2, 3, 1
STT_FUNC = 2
PLT_ADDR, PLT_SIZE = 0x08049000, 0x40
TEXT_ADDR = 0x0804A000


def build_elf(path):
    """A minimal ET_EXEC with .plt, .text and a symtab naming two functions:
    one inside the PLT, one real."""
    shstr = b"\0.plt\0.text\0.symtab\0.strtab\0.shstrtab\0"
    off = {n: shstr.index(b"\0" + n.encode() + b"\0") + 1
           for n in (".plt", ".text", ".symtab", ".strtab", ".shstrtab")}

    strtab = b"\0memalign@@GLIBC_2.0\0real_function\0"
    sym_memalign = strtab.index(b"memalign")
    sym_real = strtab.index(b"real_function")

    def sym(name_off, value, size, info):
        return struct.pack("<IIIBBH", name_off, value, size, info, 0, 1)

    symtab = (sym(0, 0, 0, 0)                                    # index 0, unused
              # the lie: a function symbol six bytes into the PLT, sized to
              # cover many stubs
              + sym(sym_memalign, PLT_ADDR + 6, 486, (1 << 4) | STT_FUNC)
              + sym(sym_real, TEXT_ADDR, 0x20, (1 << 4) | STT_FUNC))

    ehsize, phentsize, shentsize = 52, 32, 40
    phoff = ehsize
    body_off = phoff + phentsize
    plt_off = body_off
    text_off = plt_off + PLT_SIZE
    symtab_off = text_off + 0x20
    strtab_off = symtab_off + len(symtab)
    shstr_off = strtab_off + len(strtab)
    shoff = shstr_off + len(shstr)

    blob = bytearray()
    blob += struct.pack("<4sBBBBB7s", b"\x7fELF", 1, 1, 1, 0, 0, b"\0" * 7)
    blob += struct.pack("<HHIIIIIHHHHHH", 2, EM_386, 1, TEXT_ADDR, phoff, shoff,
                        0, ehsize, phentsize, 1, shentsize, 6, 5)
    blob += struct.pack("<8I", PT_LOAD, 0, PLT_ADDR & ~0xFFF, 0,
                        0x3000, 0x3000, 5, 0x1000)
    blob += b"\xff\x25\x00\x00\x00\x00" + b"\x90" * (PLT_SIZE - 6)   # .plt
    blob += b"\xc3" * 0x20                                           # .text
    blob += symtab + strtab + shstr

    def sh(name, typ, addr, offset, size, link=0, entsize=0):
        return struct.pack("<IIIIIIIIII", name, typ, 0, addr, offset, size,
                           link, 0, 1, entsize)

    sections = (sh(0, 0, 0, 0, 0)
                + sh(off[".plt"], SHT_PROGBITS, PLT_ADDR, plt_off, PLT_SIZE)
                + sh(off[".text"], SHT_PROGBITS, TEXT_ADDR, text_off, 0x20)
                + sh(off[".symtab"], SHT_SYMTAB, 0, symtab_off, len(symtab), 4, 16)
                + sh(off[".strtab"], SHT_STRTAB, 0, strtab_off, len(strtab))
                + sh(off[".shstrtab"], SHT_STRTAB, 0, shstr_off, len(shstr)))
    assert len(blob) == shoff, (len(blob), shoff)
    blob += sections
    open(path, "wb").write(bytes(blob))


def main():
    with tempfile.TemporaryDirectory() as d:
        p = os.path.join(d, "synthetic")
        build_elf(p)
        e = Elf32(p)

        assert e.section(".plt") is not None, "test fixture has no .plt"
        fns = e.functions()

        # The real one survives, with its own bounds.
        assert TEXT_ADDR in fns, fns
        assert fns[TEXT_ADDR][1] == "real_function", fns[TEXT_ADDR]

        # The PLT-resident one is gone, and nothing at all lands in the PLT.
        assert PLT_ADDR + 6 not in fns, "PLT stub was taken for a function"
        inside = [hex(a) for a in fns if PLT_ADDR <= a < PLT_ADDR + PLT_SIZE]
        assert not inside, "functions inside .plt: %s" % inside
        assert not any(n.startswith("memalign") for _s, n in fns.values()), fns

    print("elf32 PLT symbols: ok")


if __name__ == "__main__":
    main()
