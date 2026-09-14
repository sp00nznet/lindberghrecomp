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

**Current version: v0.4.0 (September 2026).** The recompiled game boots and
reaches `main()`. See [Status](#status) for what runs and what it asks for next.

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

**The recompiled game runs its entire C runtime and reaches `main()`.**
`_start`, every C++ static constructor in the binary, then `main` — all of it
executing lifted x86, on a guest process this runtime builds.

```
[crt] __libc_csu_init at 0x0859fff8
[hle] glXGetProcAddressARB
[crt] main at 0x08411ff0 (argc=1)
...
[hle] XOpenDisplay
```

| | |
|---|---|
| Disc carving | **Works.** Verified on four dumps — both filesystems found in each, payload extracted byte-exact. |
| ELF32 parsing | **Works.** 2 `PT_LOAD`s, **31,759** functions, **406** PLT imports, 13 `DT_NEEDED` libraries. |
| Lifting | **Works.** All 31,759 functions, 28 s, 1,707,769 lines of C across 80 translation units. |
| Instruction coverage | **99.94%** — 962 `/* TODO */ abort()` lines left. |
| Compiles and links | **Yes.** A 25 MB native executable. |
| Boots | **Yes.** CRT, constructors, `main`. |
| Runs | Stops at `XOpenDisplay`, which is the right place — see below. |

### What the game actually asks for

Survey mode (`LINDBERGH_HLE_PERMISSIVE=1`) reports each unbound import once and
carries on, so one run enumerates the startup path in order rather than costing
one 80-TU rebuild per import:

```
glXGetProcAddressARB            <- the only import the constructors need
pthread_mutex_lock / unlock
getcwd, realpath                <- works out where it is installed
pthread_mutexattr_*, pthread_mutex_init, pthread_cond_init
pthread_attr_*, sched_get_priority_max / min
pthread_create                  <- spawns a worker thread
pthread_cond_wait               <- and waits on it
XSetErrorHandler, XOpenDisplay  <- opens the display
```

That is a far smaller startup than 406 imports suggested, and it puts the next
two jobs in order: **pthread** on Win32 threads, then the **window seam**.

### The window seam

`XOpenDisplay` is where this stops, and it is where it should. Reimplementing
50 Xlib calls on Windows so they can hand a GLX context to WGL is absurd. The
game opens a Display, creates a Window and makes a GL context — so the seam is
cut there: a Win32 window with a WGL context behind an opaque `Display *` the
game never looks inside. 68 X11 and GLX imports collapse to about a dozen
shims.

### What is still unlifted

962 lines, and none of it is on the startup path:

| | count |
|---|---:|
| MMX — `movq` `pmaddwd` `paddd` `paddsw` `pshufw` `psrad` | 537 |
| packed SSE arithmetic — `shufps` `mulps` `addps` | 231 |
| x87 cases the FPU path misses | 102 |
| `out` / `in` — port I/O, which userspace has no business doing | 83 |
| everything else | 9 |

MMX is a second register file and a second job; packed SSE was left out
deliberately rather than guessed at, because a plausible-looking wrong lane is
worse than an honest `abort()`. Both belong upstream in
[pcrecomp](https://github.com/sp00nznet/pcrecomp).

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
