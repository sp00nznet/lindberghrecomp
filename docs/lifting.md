# Lifting

How much of an x86 binary this turns into C, what it does with the parts that
are not code, and what closing the last gaps took.

## Tried on binaries it was not written for

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

## The binary is not stripped

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

## The gap was SSE. It is now closed

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
