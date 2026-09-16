# Compatibility

What has actually been run, and how far it got. Every number here was
measured; nothing is inferred from a title looking similar to another.

## What the stages mean

A recompiled game passes through five of them, and each is a different kind of
problem, so "it works" on its own says very little.

| stage | means |
|---|---|
| **lifts** | every instruction translated to C, and the coverage figure is what was left |
| **builds** | the lifted C compiles and links against the runtime |
| **boots** | reaches `main` and gets through the game's own start up |
| **renders** | puts a correct frame on screen |
| **plays** | takes input and reaches gameplay |

## Tested

| title | board | funcs | coverage | lifts | builds | boots | renders | plays |
|---|---|---:|---:|:-:|:-:|:-:|:-:|:-:|
| Let's Go Jungle | Yellow | 31,749 | 99.96% | ✅ | ✅ | ✅ | ✅ attract | ❌ no input yet |
| Ghost Squad Evolution | Red | 6,033 | 99.975% | ✅ | ✅ | ✅ | ✅ | ⚠️ coin and start taken, then a fail-fast |
| OutRun 2 SP SDX | Yellow | 7,543 | 99.93% | ✅ | ✅ | ⚠️ through its backup and EEPROM init | ❌ | ❌ |
| Virtua Tennis 3 | Yellow | 20,783 | 99.93% | ✅ | ✅ | ⚠️ reaches `main` | ❌ | ❌ |
| After Burner Climax | Yellow | 8,529 | 99.86% | ✅ | ✅ | ❌ dies in static construction | ❌ | ❌ |

### What each one wants

The platform splits cleanly in two, and the split is not by year or by genre
but by how the game opens a window.

| title | window | Cg | SEGA libs | libstdc++ imports |
|---|---|---|---|---:|
| Let's Go Jungle | GLX | yes | yes | 10 |
| Virtua Tennis 3 | GLX | yes | yes | 34 |
| Ghost Squad Evolution | **GLUT** | no | **no** | 7 |
| After Burner Climax | **GLUT** | no | yes | 42 |
| OutRun 2 SP SDX | **GLUT** | no | yes | 12 |

Three of the five drive GLUT, which inverts control and needed a seam of its
own; the two that do not are the two that use Cg. Ghost Squad Evolution is the
only one with no SEGA libraries at all, which is why it was the cheapest first
target on the GLUT side despite being a later game.

The unlifted remainder is the same in every case: port I/O, data that
disassembles as code, and one MMX-optimised audio decoder that every title
links statically - the instruction counts for it are identical across Let's Go
Jungle, Virtua Tennis 3 and After Burner Climax, which is how we know it is
the same library.

## Untested

The rest of the Lindbergh library, from the same decrypted set the five above
came from. None has been lifted yet, so there is nothing to report beyond the
title - listed so the corpus is written down rather than remembered.

**Yellow** — Hummer · Hummer Extreme Edition · Initial D Arcade Stage 4
(Export, Japan) · Initial D Arcade Stage 5 · Let's Go Jungle Special ·
OutRun 2 SP SDX (Initial D music) · R-Tuned: Ultimate Street Racing ·
SEGA Race TV · The House of the Dead 4 · The House of the Dead 4 Special ·
Virtua Fighter 5 (Version B, Version C) · Virtua Fighter 5 R ·
Virtua Fighter 5 Final Showdown

**Red** — 2Spicy · Primeval Hunt · The House of the Dead EX

**Red EX** — Harley Davidson: King of the Road · Rambo

### Where to expect trouble

Not a prediction of results, but of which seam each will land on, which is
worth writing down before the work rather than after:

* **The House of the Dead 4 / 4 Special / EX, Primeval Hunt, 2Spicy** are gun
  games, so the JVS analog and trigger path already built for Ghost Squad
  applies directly.
* **Initial D 4 and 5, R-Tuned, SEGA Race TV, Harley Davidson, Hummer** are
  driving games with a wheel and pedals, which is JVS analog again but wants
  axes a mouse does not have.
* **Virtua Fighter 5** in all four versions is the largest thing on the
  platform and the most likely to want parts of libstdc++ that nothing here
  has needed yet.
* **Initial D 4 and 5** expect a card reader and a network, neither of which
  this runtime has any answer for.

## Reading the table

None of the five is playable, and the table says so in the column that
matters rather than in a footnote. Two render, one takes a coin, and the two
that do not boot fail in places that are understood and named. That is the
honest state of it.
