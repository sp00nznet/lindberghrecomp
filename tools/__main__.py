#!/usr/bin/env python3
"""lindberghrecomp command line.

  py -3.11 -m tools disc   <disc.iso> [-o outdir]   carve the DVD payload
  py -3.11 -m tools elf    <game.elf>               what the ELF says it needs
  py -3.11 -m tools recomp <game.elf> <outdir>      lift it to C
"""

import argparse
import sys

from .disc.carve import extract, find_filesystems
from .elf.elf32 import Elf32


def cmd_disc(a):
    found = find_filesystems(a.image)
    if not found:
        sys.exit("no ISO9660 filesystem found - not a Lindbergh disc image?")
    for fs_start, vol, sysid, size in found:
        print("%#010x  sector %-7d %-16s %-22s %9.1f MB"
              % (fs_start, fs_start // 2048, sysid, vol, size / 1e6))
    if not a.out:
        return
    print("\nextracting the last filesystem to %s" % a.out)
    for name, size in extract(a.image, a.out):
        print("  %-16s %12d" % (name, size))
    print("\nThese files are encrypted. See docs/disc-format.md.")


def cmd_elf(a):
    e = Elf32(a.elf)
    print("entry      %#010x" % e.entry)
    print("image      %#010x + %#x" % (e.image_base, e.image_size))
    print("segments   %d PT_LOAD" % len(e.loads))
    funcs = e.functions()
    print("functions  %d sized STT_FUNC symbols%s"
          % (len(funcs), "" if funcs else "   (stripped - bring a bounds file)"))
    plt = e.plt_map()
    print("imports    %d PLT stubs" % len(plt))
    for lib in e.needed():
        print("  needs    %s" % lib)


def cmd_recomp(a):
    from .recomp.driver import recompile
    addrs = [int(x, 16) for x in a.addrs] or None
    done, imports = recompile(a.elf, a.outdir, addrs)
    print("lifted %d functions, %d distinct imports -> %s"
          % (len(done), len(imports), a.outdir))


def main(argv=None):
    p = argparse.ArgumentParser(prog="tools", description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = p.add_subparsers(dest="cmd", required=True)

    d = sub.add_parser("disc", help="carve a Lindbergh DVD image")
    d.add_argument("image")
    d.add_argument("-o", "--out", help="extract the payload here")
    d.set_defaults(fn=cmd_disc)

    e = sub.add_parser("elf", help="report on a game ELF")
    e.add_argument("elf")
    e.set_defaults(fn=cmd_elf)

    r = sub.add_parser("recomp", help="lift a game ELF to C")
    r.add_argument("elf")
    r.add_argument("outdir")
    r.add_argument("addrs", nargs="*", help="lift only these (hex); default all")
    r.set_defaults(fn=cmd_recomp)

    a = p.parse_args(argv)
    a.fn(a)


if __name__ == "__main__":
    main()
