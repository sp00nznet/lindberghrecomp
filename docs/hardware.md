# The board

Sega Lindbergh, 2005. The successor to Chihiro, and the point at which Sega
stopped building arcade hardware and started buying it: a Lindbergh is a
mid-range 2005 desktop PC in an arcade case, running Linux.

That is the whole reason this toolkit is small. There is no custom CPU to
model, no GPU to emulate, no bespoke DSP. A Lindbergh game is a 32-bit x86 ELF
that calls libc and OpenGL. The work is not "emulate the hardware", it is
"provide the libraries" — which is a much better problem to have.

> Everything in the table is from published specifications and cabinet
> teardowns, not from these disc dumps. The discs are encrypted (see
> [disc-format.md](disc-format.md)), so nothing here has been read back off
> a binary. Treat it as the target to aim at, and correct it when a decrypted
> game says otherwise.

| | Red | Yellow | Blue |
|---|---|---|---|
| CPU | Pentium 4 3.0 GHz | Pentium 4 3.0 GHz | Celeron D |
| GPU | GeForce 6800 GT, 256 MB | GeForce 7600 GS, 256 MB | GeForce 7300-class |
| RAM | 1 GB DDR2 | 1 GB DDR2 | 512 MB |
| Chipset | Intel 915-series | Intel 915-series | Intel 915-series |

All four dumps in this repo's sight are **Lindbergh Yellow**, so that is the
reference configuration.

Common to every variant:

* **OS** — MontaVista Linux Professional Edition 4.0, a 2.6-series kernel with
  glibc 2.3.x. That is what fixes the ABI the recompiled code has to meet:
  Linux/i386 syscalls (`int 0x80`, and `sysenter` via the vDSO), the SysV i386
  PLT, cdecl.
* **Graphics** — the stock NVIDIA Linux driver and OpenGL 2.x. A game issues
  real `gl*` calls; there is no Sega-specific graphics layer to reverse.
* **Storage** — a DVD-ROM for distribution and an internal hard disc the game
  is installed onto. The DVD is what gets dumped; the installed tree is what
  you actually want.
* **I/O** — JVS over USB to a Sega I/O board. Coins, buttons, guns, pedals and
  the cabinet's test/service switches all arrive this way, as does force
  feedback on the driving titles.
* **Security** — a USB dongle plus a per-game key. The game's ELF is encrypted
  and decrypted at load by Sega's own loader.

## What the runtime has to provide

Working out from the above, in the order a booting game needs them:

| | What | Where it lives |
|---|---|---|
| 1 | Linux/i386 syscalls | `src/runtime/syscall.c` — the startup set works |
| 2 | glibc | `hle_call()` — nothing written yet |
| 3 | OpenGL 2.x | `hle_call()` — pass through to the host's GL |
| 4 | ALSA / `libsegaapi` sound | `hle_call()` — nothing written yet |
| 5 | JVS input | `ioctl` on the I/O device, plus its `libposixdrv` wrapper |
| 6 | the security dongle | answer it, do not emulate it |

Items 2–5 are the actual work, and none of it can be written speculatively:
which functions a title imports is a fact about that title's ELF, and
`py -3.11 -m tools elf <game.elf>` prints the list. That is why the toolkit
ships the loader, the CPU boundary and the kernel, and leaves the library
surface to be filled in per title as the imports show up.
