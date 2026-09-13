# Contributing to lindberghrecomp

This toolkit turns a Sega Lindbergh game's ELF into native C. Anything that
gets a real title closer to running is welcome.

## Where the gaps are

Real, scoped, measured against *Let's Go Jungle*'s `lgj_final` — 31,752
functions, all of which lift. None of them need permission to start.

**Start here. `lift32_cpu.py` has no SSE**, and that is 87.7% of every
unlifted instruction in a real Lindbergh title. The work is upstream in
[pcrecomp](https://github.com/sp00nznet/pcrecomp), and it is the highest-value
thing anyone can do for this project:

| Upstream job | Count in one game | Share of the gap | Difficulty |
|---|---:|---:|---|
| **Scalar SSE** — `movss` `mulss` `addss` `subss` `divss` `ucomiss` `cvtsi2ss` `cvttss2si` | 141,081 | **87.7%** | Medium once, mechanical after. The CPU struct needs XMM storage; the ops are one C line each. |
| **`prefetcht0`/`nta`** | 4,859 | 3.0% | Trivial — they are semantically no-ops. Half an hour. |
| **`cmovcc`** | 3,435 | 2.1% | Trivial — the condition helpers already exist for `Jcc`. |
| **SSE packed** — `movaps` `xorps` `andps` `shufps` | 8,966 | 5.6% | Medium. Needs the XMM storage from the scalar job first. |
| **MMX / SSE2 integer** — `pxor` `movq` `pmaddwd` `paddd` | 1,591 | 1.0% | Medium. |
| **x87 cases the FPU path misses** | 699 | 0.4% | Small each. |

The first three together close 92.8% of the gap.

Then, in this repo:

| Area | What is missing | Difficulty |
|---|---|---|
| **glibc** | `hle_call()` aborts on every import. A Lindbergh game links glibc 2.3.x; the string/memory/stdio set is mechanical and unblocks everything after it. | Small each, many of them |
| **OpenGL / GLU / X11** | The game issues real `gl*` calls. Pass them to the host's GL — arguments come off the guest stack, cdecl. Mostly transcription. | Medium |
| **Cg** | NVIDIA's Cg shader runtime, which the game ships its own copy of. Its shaders are on the disc in `shader/Cg`. | Medium |
| **`libsegaapi`** | Sega's sound API, and the only Sega-specific library in the whole import list. No public documentation; it has to be read off a title's imports and its behaviour. | Large |
| **JVS input** | Coins, buttons, guns and pedals arrive over USB to a Sega I/O board, reached through `ioctl` and `libposixdrv`. | Medium |
| **Another board revision** | Everything here assumes Lindbergh Yellow. | Varies |

Reproduce any of these numbers with:

```powershell
py -3.11 -m tools recomp <game.elf> genfindstr /C:"TODO" genecomp_funcs.c | find /c /v ""
```

## Ground rules

**No decryption, no keys, no game data.** Not in the repo, not in an issue, not
in a PR. The toolkit reads a disc's unencrypted filesystem structure and
translates machine code you supply, and that is where the line is. A patch that
moves it will be closed.

**Measure, don't recite.** [docs/disc-format.md](docs/disc-format.md) is what it
is because every number in it came off a disc in hand. If you add to the docs,
say which dump you read and mark inference as inference.

**Instruction fixes go upstream.** The x86-32 lifter is pcrecomp's, vendored
here as a submodule. Fixing `adc` there fixes it for every PC-era target at
once; fixing it here forks the lifter and helps one.

**Leave a check behind.** Non-trivial logic gets the smallest runnable thing
that fails when it breaks — see `tools/disc/test_carve.py`,
`tools/recomp/test_driver.py` and `tests/selftest/`. No frameworks.

## Running the checks

```powershell
py -3.11 tools\disc\test_carve.py
py -3.11 tools\recomp\test_driver.py
cmake -S . -B build -A Win32; cmake --build build --config Release
.\build\Release\lindbergh_rt_selftest.exe
```

All three must pass before a PR.
