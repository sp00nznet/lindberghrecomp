#!/usr/bin/env python3
"""End-to-end self-check: build a real i386 ELF, lift it, read the C back.

The three things this pipeline adds on top of pcrecomp's x86 lifter are the
three things asserted here - PT_LOAD addressing, the PLT becoming named HLE
calls, and `int 0x80` becoming a syscall. If they break, the recompiled game
is silently wrong rather than loudly broken, so they get a test.

Run: py -3.11 tools/recomp/test_driver.py
"""

import os
import struct
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))))

from tools.elf.elf32 import Elf32
from tools.recomp.driver import recompile

BASE = 0x08048000
BS = chr(92)


class Builder:
    """Appends blobs, hands back (file offset, virtual address)."""

    def __init__(self, start):
        self.buf = bytearray(start)

    def add(self, blob, align=4):
        while len(self.buf) % align:
            self.buf.append(0)
        off = len(self.buf)
        self.buf += blob
        return off, BASE + off


def build_elf(path):
    b = Builder(0x60)                       # room for Ehdr + one Phdr

    plt_off, plt_va = b.add(b"\x00" * 32, align=16)   # PLT0 + one 16-byte stub
    stub_va = plt_va + 16

    text_start = BASE + ((len(b.buf) + 15) & ~15)
    call_site = text_start + 3              # push ebp; mov ebp,esp; then call
    code = (b"\x55"                          # push ebp
            b"\x89\xe5"                      # mov  ebp, esp
            + b"\xe8" + struct.pack("<i", stub_va - (call_site + 5))
            + b"\xb8\x01\x00\x00\x00"        # mov  eax, 1
            b"\xcd\x80"                      # int  0x80
            b"\x5d"                          # pop  ebp
            b"\xc3")                         # ret
    text_off, text_va = b.add(code, align=16)
    assert text_va == text_start, (hex(text_va), hex(text_start))

    dynstr = b"\0write\0"
    dynstr_off, _ = b.add(dynstr, align=1)
    dynsym = struct.pack("<IIIBBH", 0, 0, 0, 0, 0, 0) + \
             struct.pack("<IIIBBH", 1, 0, 0, 0x12, 0, 0)      # "write", GLOBAL FUNC
    dynsym_off, _ = b.add(dynsym)
    relplt_off, _ = b.add(struct.pack("<II", 0, (1 << 8) | 7))

    strtab = b"\0testfn\0"
    strtab_off, _ = b.add(strtab, align=1)
    symtab = struct.pack("<IIIBBH", 0, 0, 0, 0, 0, 0) + \
             struct.pack("<IIIBBH", 1, text_va, len(code), 0x12, 0, 1)
    symtab_off, _ = b.add(symtab)

    names = [b"", b".plt", b".text", b".dynstr", b".dynsym", b".rel.plt",
             b".strtab", b".symtab", b".shstrtab"]
    shstr = b"\0".join(names) + b"\0"
    shstr_off, _ = b.add(shstr, align=1)
    noff = {}
    pos = 0
    for n in names:
        noff[n.decode()] = pos
        pos += len(n) + 1

    def shdr(name, typ, addr, off, size, link=0, entsize=0):
        return struct.pack("<10I", noff[name], typ, 0, addr, off, size,
                           link, 0, 4, entsize)

    sections = [
        shdr("", 0, 0, 0, 0),
        shdr(".plt", 1, plt_va, plt_off, 32),
        shdr(".text", 1, text_va, text_off, len(code)),
        shdr(".dynstr", 3, 0, dynstr_off, len(dynstr)),
        shdr(".dynsym", 11, 0, dynsym_off, len(dynsym), link=3, entsize=16),
        shdr(".rel.plt", 9, 0, relplt_off, 8, link=4, entsize=8),
        shdr(".strtab", 3, 0, strtab_off, len(strtab)),
        shdr(".symtab", 2, 0, symtab_off, len(symtab), link=6, entsize=16),
        shdr(".shstrtab", 3, 0, shstr_off, len(shstr)),
    ]
    sh_off, _ = b.add(b"".join(sections))

    total = len(b.buf)
    b.buf[0:16] = b"\x7fELF\x01\x01\x01" + b"\0" * 9
    struct.pack_into("<HHIIIIIHHHHHH", b.buf, 16,
                     2, 3, 1, text_va, 0x34, sh_off, 0,
                     52, 32, 1, 40, len(sections), 8)
    struct.pack_into("<8I", b.buf, 0x34,
                     1, 0, BASE, BASE, total, total, 5, 0x1000)

    with open(path, "wb") as f:
        f.write(b.buf)
    return text_va, stub_va


def main():
    with tempfile.TemporaryDirectory() as td:
        elf_path = os.path.join(td, "test.elf")
        text_va, stub_va = build_elf(elf_path)

        e = Elf32(elf_path)
        assert e.image_base == BASE, hex(e.image_base)
        assert e.entry == text_va, hex(e.entry)
        assert e.read_va(text_va, 1) == b"\x55", "read_va missed .text"
        assert e.plt_map() == {stub_va: "write"}, e.plt_map()
        assert list(e.functions().values())[0][1] == "testfn", e.functions()

        out = os.path.join(td, "gen")
        done, imports = recompile(elf_path, out)
        assert done == [text_va], done
        assert imports == ["write"], imports
        c = open(os.path.join(out, "recomp_funcs_0000.c")).read()

        for want in ("void L_%08X(CPU *c)" % text_va,   # lifted at its real VA
                     "hle_call(c, HLE_write);",         # PLT call became named
                     "linux_syscall(c);"):              # int 0x80 became syscall
            assert want in c, "missing %r in:\n%s" % (want, c)
        assert "abort()" not in c, "something did not lift:\n%s" % c

        # Both generated headers are one long X-macro held together by line
        # continuations. A substring check passes happily while every
        # continuation is the two characters backslash-n instead of a
        # backslash ending the line - which is a header no compiler accepts,
        # and which shipped once already because nothing here looked.
        for fn in ("recomp_imports.h", "recomp_funcs_list.h"):
            text = open(os.path.join(out, fn)).read()
            assert BS + "n" not in text, fn + ": literal backslash-n, not a line continuation"
            for line in text.splitlines()[:-1]:
                if line.startswith("    X("):
                    assert line.endswith(BS), fn + ": unterminated continuation: " + line
        imports_h = open(os.path.join(out, "recomp_imports.h")).read()
        assert 'X(HLE_write, "write")' in imports_h, imports_h

    print("ok: ELF32 -> PLT/syscall-aware x86 lift -> C")


if __name__ == "__main__":
    main()
