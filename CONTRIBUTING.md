# Contributing to lindberghrecomp

This toolkit turns a Sega Lindbergh game's ELF into native C. Anything that
gets a real title closer to running is welcome.

## Where the gaps are

Real, scoped, and none of them need permission to start.

| Area | What is missing | Difficulty |
|---|---|---|
| **glibc** | `hle_call()` aborts on every import. A Lindbergh game links glibc 2.3.x; the string/memory/stdio set is mechanical and unblocks everything after it. | Small each, many of them |
| **OpenGL** | The game issues real `gl*` calls. Pass them to the host's GL — arguments come off the guest stack, cdecl. Mostly transcription. | Medium |
| **Sound** | ALSA and Sega's `libsegaapi`. No public documentation for the latter; it will have to be read off a title's import list and its behaviour. | Large |
| **JVS input** | Coins, buttons, guns, pedals and force feedback arrive over USB to a Sega I/O board, reached through `ioctl` and `libposixdrv`. | Medium |
| **SSE2/SSE3** | The lifter came from Pentium III targets. A Pentium 4 title will use instructions it has never seen — they lift to `/* TODO */ abort()`, which is at least loud. Fixes belong upstream in [pcrecomp](https://github.com/sp00nznet/pcrecomp), not here. | Small each |
| **Another board revision** | Everything here assumes Lindbergh Yellow. | Varies |

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
