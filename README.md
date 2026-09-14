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

### Tried on binaries it was not written for

A toolkit that works on one executable has not been shown to work. The disc
ships eight more 32-bit x86 ELFs beside the game — NVIDIA's Cg compiler, the
Apache Xerces XML parser, libpng — built by different people, at different
times, with different compilers. They lift too:

| | functions | imports | instructions | unlifted |
|---|---:|---:|---:|---:|
| `libxerces-c.so` | 9,019 | 4,448 | 409,207 | 0 |
| `libpng.so` | 339 | 234 | 28,374 | 0 |
| `libCg.so` | 284 | 125 | 18,520 | 48 |
| `libCgGL.so` | 79 | 202 | 4,145 | 0 |

460,246 instructions, and the 48 are not instructions. They disassemble as
`rcr dword ptr [edx], 0` — a rotate by zero, thirty-five times, at eight-byte
spacing — which no compiler emits. The bytes are `1C C2 1A 00 08 C2 1A 00 F4
C1 1A 00 …`: little-endian pointers descending by 0x14, a switch jump table
inside a function body, and the other site sits directly after a `jmp eax`.
Data read as code, in a region nothing can branch to.

So the answer is 100% on all four, and `libCg.so` lifting cleanly matters on
its own: compiling the game's shaders properly rather than matching them
against the disc's precompiled output needs exactly that library.

### The black frame was one instruction

Worth writing down, because nothing about it was visible from where it hurt.

`fxch st(N)` swaps the top of the x87 stack with the N-th register down.
Capstone reports it with **both** registers and the implicit `st(0)` first, so
the meaningful operand is `ops[-1]` — and the lifter read `ops[0]`. Every
`fxch` became a swap of `st(0)` with itself: valid C, no crash, no warning, and
the stack left in precisely the order the original code used `fxch` to avoid.
906 of them in one game binary.

There is no wrong-looking value at the point of the bug. Three correct floats
get stored into three wrong places, and the damage surfaces hundreds of frames
later as a single zero in a projection matrix, which collapses an entire scene
onto a one-pixel line. The fix and its test are
[in pcrecomp](https://github.com/sp00nznet/pcrecomp); the test fails on the old
code with the self-swap it emitted.

The lesson that stuck is in the instruments below.

### Measuring the picture, not the calls

A previous version of this file claimed a game rendered, on the strength of GL
call counts, without reading a pixel. Five diagnoses of the resulting black
frame were wrong before one was right. Every instrument here exists because a
guess stood in for a measurement, and several were wrong before they were
right — the first framebuffer probe read a fixed 64×64 corner of an 800×600
target and called a healthy buffer empty. **Check the instrument against a
known answer before believing it.**

| Variable | Question it answers |
|---|---|
| `LINDBERGH_FBSTATS=1` | how much of the frame is lit, and what each offscreen target holds at its own size |
| `LINDBERGH_TRACE_FRAME=N` | every draw of one frame — bound programs, viewport, scissor, bound textures with their filter state, and the target after each |
| `LINDBERGH_FP_SOLID=1\|2\|3` | replace every fragment program with flat green, a raw texture fetch, or the texture coordinates: does it rasterise, is the texture black, are the coordinates zero |
| `LINDBERGH_DUMP_FP=dir` | the shader text the Cg seam actually supplied, named by program object |
| `LINDBERGH_GLERR=1` | which entry point first raises a GL error |
| `LINDBERGH_CONSOLE=1` | the game's own debug console — what hardware it looked for and why it gave up |
| `LINDBERGH_SHOT=path` | one frame as a BMP |

The most useful of those is the last-but-one. A Lindbergh game narrates its own
startup through `_sDebug::putConsole`, which goes to a console the cabinet has
and a desktop does not. Binding it turns the whole base-board problem from
guesswork into reading.

### The cabinet, not just the CPU

A Lindbergh game reaches its hardware through SEGA's `amLib`, statically linked
into the binary — so there is no import to bind, and the runtime replaces the
lifted functions by symbol name instead. Without it a game stops on **Error 11
— JVS I/O board is not connected to main board** before drawing anything.

`hle_sega.c` answers three things, each at the lowest seam that works:

* **The base board** — `amLibInit`, `amJvsInit`, `amDongleInit`, `amDongleUpdate` and their predicates. `amJvsCheckInit` is a predicate, not a status code; answering it with the library's success value of 0 becomes "−5 JVS node(s) found".
* **A JVS I/O board**, at `amJvsSendRequest` / `amJvsRecvAcknowledge` only. The frames are real JVS, so all sixty of a game's own packet builders and parsers run unmodified above two replaced functions. It reports identity, revisions, and a feature list; nothing is pressed or aimed.
* **The battery-backed store**, at the four wrapper functions under the record layer. Record layout, duplicate copies and CRCs are the game's own code and work untouched. Read takes the offset first and the buffer second; write takes them the other way round — which is not guessable, and the log says so.

What is assumed rather than emulated: the backup records are blank and a game
cannot initialise them without the EEPROM's I2C bus. A real cabinet ships that
store written, so the runtime makes the same statement, suppresses the one
error code it raises, and announces that it has.

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

### The gap was SSE. It is now closed

Measuring the first full lift, every unlifted instruction fell into one family:
scalar SSE was **87.7%** of the gap and `movss` alone was 46% of it. That is
the predicted failure — the lifter came from Pentium III targets and *Let's Go
Jungle* is a Pentium 4 title that keeps its floats in XMM registers.

So it was fixed **upstream in
[pcrecomp](https://github.com/sp00nznet/pcrecomp)**, where it belongs: one x86
lifter, and every PC-era target gets SSE out of it. `lift32_cpu.py` now covers
scalar single and double arithmetic, `sqrt`, `min`/`max`, both kinds of
compare, the conversions, 128-bit moves and the bitwise ops — plus `cmovcc`,
and the prefetch hints as the no-ops they are.

| | before | after |
|---|---:|---:|
| Instruction coverage | 90.55% | **99.91%** |
| `/* TODO */ abort()` lines | 160,820 | **1,558** |

What is still unlifted, in full:

| | count |
|---|---:|
| x87 cases the FPU path misses | 699 |
| MMX `movq` (a separate register file, not SSE) | 192 |
| MMX integer — `pmaddwd` `paddd` `paddsw` `pshufw` `psrad` … | 345 |
| packed float — `shufps` `mulps` `addps` | 231 |
| `out` / `in` — port I/O, which userspace has no business doing | 83 |
| `lock` `bts` `bt` `pushal` `pinsrw` `movntps` | 8 |

Packed arithmetic was left out deliberately rather than guessed at: it needs
per-lane code, and a plausible-looking wrong lane is worse than an honest
`abort()`. MMX is a second register file and a second job.

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
