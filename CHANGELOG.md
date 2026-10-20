# Changelog

All notable changes to this project. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and versions follow
[SemVer](https://semver.org/). Versions before this file existed were recorded
in commit subjects only; the entries below for them are taken from those
commits and were never tagged.

## [Unreleased]

### Added
- GLUT windowing and input: one queue, read both as GLUT callbacks and as a JVS
  board reporting switches, coins and analog.
- libstdc++, the cabinet EEPROM, and the libc that *Virtua Tennis 3* and
  *House of the Dead 4* need.
- The SEGA base board seam, and diagnostics that survive a process being
  switched off.
- `docs/compatibility.md`: each game's stage, kept apart from the toolkit's.
- `CHANGELOG.md`, `ROADMAP.md`, `CONTRIBUTORS.md`, and CI on push and PR.

### Changed
- The Cg shader table holds 2048 precompiled programs instead of 512, and says
  so when it fills rather than dropping the rest silently. (#1, found by
  [@Redmoneydash](https://github.com/Redmoneydash) in their Initial D 4 port)

### Fixed
- The black frame: one mis-lifted x87 instruction
  ([docs/the-black-frame.md](docs/the-black-frame.md)). *Let's Go Jungle*'s
  attract mode now renders.
- Large allocations are page aligned, as glibc's are.
- An ELF function symbol inside the PLT is treated as a stub.
- `.gitignore` covers extension-less Lindbergh game binaries (`*_final`).
- A fresh clone builds the runtime self-check: two of its stand-in headers were
  ignored by duplicate `.gitignore` lines and had never been committed.

## [0.5.1] - 2026-09-14
### Fixed
- The rendering claim in 0.5.0 was measured by draw calls, not pixels. The
  frame was black; the README says so.

## [0.5.0] - 2026-09-14
### Added
- Shaders on the GPU, texture upload, and a render loop at ~32 fps.

## [0.4.0] - 2026-09-13
### Added
- The game reaches `main`: CRT entry, guest callbacks, three guest threads, a
  window, and a crash reporter.

## [0.3.0] - 2026-09-13
### Added
- 99.91% instruction coverage on *Let's Go Jungle*; libc split into its own
  translation unit.

## [0.2.0] - 2026-09-13
### Added
- The pipeline runs end to end against a real title.

## [0.1.0] - 2026-09-13
### Added
- Disc carving, ELF32 parsing, x86-32 lift via pcrecomp, and the runtime
  skeleton.
