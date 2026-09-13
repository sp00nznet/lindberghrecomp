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

**Current version: v0.2.0 (September 2026).** The pipeline has now been run
end to end against a real Lindbergh title. See [Status](#status) for the
numbers.

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

**The whole pipeline has been run against a real game binary.** *Let's Go
Jungle*'s `lgj_final` — 12.7 MB, `ET_EXEC`, `EM_386`, entry `0x08072d70` — goes
in, and 1.7 million lines of C come out.

| | |
|---|---|
| Disc carving | **Works.** Verified against *Let's Go Jungle*, *House of the Dead 4*, *Initial D 4*, *Virtua Tennis 3* — four dumps, both filesystems found in each, payload extracted byte-exact. |
| ELF32 parsing | **Works.** 2 `PT_LOAD`s, **31,752** sized `STT_FUNC` symbols, **406** PLT imports, 13 `DT_NEEDED` libraries. |
| Lifting | **Works.** All 31,752 functions lift in **28 seconds**, producing 1,702,616 lines of C. Not one function failed outright. |
| Instruction coverage | **90.6%.** 160,820 of the emitted lines are `/* TODO */ abort()`. The gap is one family — see below. |
| Runtime | **Partial.** Image mapping, initial stack, startup syscalls, dispatch and the import boundary build and run 32-bit. The 406 imports have no bodies yet. |

### The binary is not stripped

Worth saying on its own, because it changes what this project is. `lgj_final`
ships its full symbol table: **31,752 function symbols, with names and sizes**.
There is no function-discovery problem, no bounds file to export from Ghidra,
no heuristic carving. The ELF says where every function starts and how long it
is, and the lifter takes it from there.

```
$ py -3.11 -m tools elf lgj_final
entry      0x08072d70
image      0x08048000 + 0xc23f44
segments   2 PT_LOAD
functions  31752 sized STT_FUNC symbols
imports    406 PLT stubs
  needs    libCg.so          libCgGL.so        libxerces-c.so.26
  needs    libGLU.so.1       libGL.so.1        libsegaapi.so
  needs    libpthread.so.0   libm.so.6         libgcc_s.so.1
  needs    libc.so.6         libXext.so.6      libX11.so.6
  needs    libdl.so.2
```

That import list is also the work plan, and it is shorter than it looks:
stock glibc, stock OpenGL/GLU, stock X11, NVIDIA's Cg shader runtime, Xerces,
and exactly one Sega library — `libsegaapi.so`, the sound API.

### The gap is SSE, and almost nothing else

Every unlifted instruction across all 31,752 functions, by family:

| Family | Count | Share of gap |
|---|---:|---:|
| SSE scalar float — `movss` `mulss` `addss` `subss` `divss` `ucomiss` `cvt*ss*` | 141,081 | **87.7%** |
| SSE packed / logical — `movaps` `xorps` `andps` `shufps` `mulps` | 8,966 | 5.6% |
| `prefetcht0` and friends — semantically a no-op | 4,859 | 3.0% |
| `cmovcc` | 3,435 | 2.1% |
| MMX / SSE2 integer — `pxor` `movq` `pmaddwd` `paddd` | 1,591 | 1.0% |
| x87 cases the FPU path misses | 699 | 0.4% |
| everything else (`out`, `in`, `bt`, `lock`, …) | 189 | 0.1% |

`movss` alone is 74,438 of them — 46% of the whole gap.

This is exactly the predicted failure: the lifter came from Pentium III targets
and *Let's Go Jungle* is a Pentium 4 title that keeps its floats in XMM
registers. Three mechanical pieces of work — scalar SSE, `cmovcc`, and making
the prefetches no-ops — close **92.8%** of the gap between them.

**That work belongs upstream in
[pcrecomp](https://github.com/sp00nznet/pcrecomp)**, in `lift32_cpu.py`, not
here. Every PC-era target benefits, and forking the lifter to fix one game is
how you end up maintaining four of them.

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
