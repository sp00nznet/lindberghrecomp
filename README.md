# lindberghrecomp

```
 #       ###  #   #  ####   ####   ####  ####    ###   #   #
 #        #   ##  #  #   #  #   #  #     #   #  #      #   #
 #        #   # # #  #   #  ####   ###   ####   #  ##  #####
 #        #   #  ##  #   #  #   #  #     #   #  #   #  #   #
 #####   ###  #   #  ####   ####   ####  #   #   ###   #   #

 Static Recompilation Toolkit for Sega Lindbergh Arcade Games
```

> A Lindbergh is a 2005 PC in an arcade case. The game is a 32-bit x86 ELF that
> calls libc and OpenGL. So don't emulate the board — recompile the ELF and
> hand it the libraries.

**[Join the sp00nznet recomp Discord](https://discord.gg/CRpzGWZFcu)** — the
community hub for sp00nznet's recomp projects.

**Current version: v0.5.1 (September 2026).** A Lindbergh game boots and
runs a full render loop; the frame it presents is still black. See
[Status](#status).

---

## What is this?

Sega's Lindbergh (2005) is where Sega stopped designing arcade hardware and
started buying it. Under the cabinet art is a Pentium 4, a GeForce, a gigabyte
of DDR2 and MontaVista Linux — see [docs/hardware.md](docs/hardware.md).

That makes it an unusually good static recompilation target. There is no custom
CPU to model and no GPU to emulate. There is a 32-bit x86 ELF, the Linux/i386
ABI it was built against, and a set of shared libraries the board provided that
we have to provide instead. The hard parts of a console recomp — the exotic
instruction set, the bespoke graphics pipeline — simply are not here.

This toolkit is **title-agnostic**. Everything it knows about a game it learns
from that game's ELF.

## The pipeline

```
        YOUR LINDBERGH DVD
               |
               v
    +----------------------+   Find the nested ISO9660 filesystems and pull
    |  1. Carve the disc   |   out disk0.img / su1.dat / frontend.set.
    +----------------------+   tools/disc/          WORKS
               |
               v
    +----------------------+   The payload is AES-encrypted under a key that
    |  2. Decrypt          |   lives in the cabinet. Not this toolkit's job -
    +----------------------+   bring a game tree you can already read.
               |                              NOT OURS
               |
               v
    +----------------------+   PT_LOADs, symbols, PLT imports, DT_NEEDED.
    |  3. Parse the ELF    |   tools/elf/           WORKS
    +----------------------+
               |
               v
    +----------------------+   x86-32 -> C, one C statement per instruction,
    |  4. Lift to C        |   with `int 0x80` and the PLT understood.
    +----------------------+   tools/recomp/        WORKS
               |
               v
    +----------------------+   Map the image where it was linked, answer the
    |  5. Link the runtime |   kernel, answer the libraries.
    +----------------------+   src/runtime/         PARTIAL
               |
               v
         NATIVE EXECUTABLE
```

### The x86 lifter is not ours

Step 4 is [**pcrecomp**](https://github.com/sp00nznet/pcrecomp)'s
`tools/lift/lift32_cpu.py`, vendored here as a git submodule. One x86-32
lifter, shared with every PC-era target, rather than a fork per platform — a
fix to `adc` helps Lindbergh and *Fury³* at once.

What Lindbergh adds on top of it is three things, and they are the whole of
`tools/recomp/driver.py`:

* the image comes from `PT_LOAD` segments, not PE sections
* `int 0x80` and the vDSO's `sysenter` become `linux_syscall(c)` — the stock
  lifter emits `abort()` for both
* a `call` to a PLT stub becomes a named `hle_call(c, HLE_glClear)`, because
  there is no `ld.so` here and no `.so` to jump into

The subclass is 30 lines. Everything else the lifter already did.

## Status

A Lindbergh game boots from its own ELF, answers the cabinet's base board, and
renders its attract mode.

![Two giant spiders mid-leap on a jungle path, both players' rifles and
crosshairs on screen, combo counters running](docs/attract.png)

*Let's Go Jungle*, demo sequence. The attract loop runs end to end — warning
card, logos, title, tutorial, a demo of the rail sequences, and the ranking
board. The frame is `glReadPixels` on the back buffer before the swap.

| | |
|---|---|
| Disc carving | **Works.** Four dumps, both filesystems found in each, payload byte-exact. |
| ELF32 parsing | **Works.** 31,759 functions, 406 PLT imports, 13 `DT_NEEDED`. |
| Lifting | **Works.** All 31,759 in 28 s, 1.7 M lines across 80 translation units. |
| Instruction coverage | **99.96%** on the game — 622 `RECOMP_TODO` lines, all MMX or port I/O, none on a path reached. **100%** on four unrelated binaries, see below. |
| Runtime | Image mapping, kernel, threads, window, GL, Cg, SEGA base board, JVS, NVRAM. |
| Boots and runs | **Yes** — CRT, constructors, `main`, 3 threads. |
| Presents a picture | **Yes.** Attract mode at 1360×768. |
| Input, sound | **No.** |

### Further reading

The detail moved to `docs/`, so this page stays a map rather than a diary.

| | |
|---|---|
| [docs/lifting.md](docs/lifting.md) | coverage across five binaries, what an unstripped ELF gives you, and how the SSE gap was closed |
| [docs/the-black-frame.md](docs/the-black-frame.md) | one mis-lifted x87 instruction, and the five wrong answers before it |
| [docs/instruments.md](docs/instruments.md) | the `LINDBERGH_*` switches, and why each one exists |
| [docs/cabinet.md](docs/cabinet.md) | amLib, the JVS I/O board, and the battery-backed store |
| [docs/disc-format.md](docs/disc-format.md) | what a retail Lindbergh DVD looks like, and why this does not decrypt one |
| [docs/hardware.md](docs/hardware.md) | the machine itself |

### On the discs

The encrypted-disc finding still stands and is still worth reading —
[docs/disc-format.md](docs/disc-format.md) records what was measured, including
the evidence that *Let's Go Jungle* and *Initial D 4* share a key. **This
toolkit does not decrypt Lindbergh media.** Bring a game tree you can already
read; preservation projects have published clean dumps of many Lindbergh
titles, taken from original DVDs and cabinet hard discs with the keys their
owners had.

## Use

```powershell
git clone --recursive https://github.com/sp00nznet/lindberghrecomp
cd lindberghrecomp

# what is on a disc image
py -3.11 -m tools disc "Let's Go Jungle (World) ... (Rev A).iso" -o out\

# what an ELF needs
py -3.11 -m tools elf game.elf

# lift it
py -3.11 -m tools recomp game.elf gen\

# the checks
py -3.11 tools\disc\test_carve.py
py -3.11 tools\recomp\test_driver.py
cmake -S . -B build -A Win32; cmake --build build --config Release
.\build\Release\lindbergh_rt_selftest.exe
```

Needs Python 3.11 with `capstone`, and a 32-bit MSVC toolchain. The 32-bit part
is not negotiable: the CPU model is flat — a guest register holds a real host
address — and the game maps at `0x08048000`, which only exists as an address in
a 32-bit process. The CMake refuses a 64-bit configure rather than let you find
out later.

## Layout

```
tools/
  disc/       carve the nested ISO9660s out of a Lindbergh DVD
  elf/        ELF32: segments, symbols, PLT imports, DT_NEEDED
  recomp/     the ELF end of the lifter - PT_LOAD, syscalls, PLT
src/runtime/
  guest.c     map PT_LOADs where they were linked, build the initial stack
  syscall.c   Linux/i386 syscalls
  hle.c       the shared libraries the board provided
  dispatch.c  original address -> lifted function
pcrecomp/     the x86-32 lifter and CPU model (git submodule)
docs/
  hardware.md     what a Lindbergh is
  disc-format.md  what is actually on the disc, measured
```

## Legal

MIT, and original work throughout. **No game data, no keys, and no
circumvention of anything.** The tools here read a disc's unencrypted
filesystem structure and translate x86 machine code you supply. Lindbergh
titles are © Sega; this is an independent, non-commercial preservation project.
