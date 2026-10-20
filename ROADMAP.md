# Roadmap

What is next, what is waiting, and what this project will not do. Which game is
at which stage is in [docs/compatibility.md](docs/compatibility.md).

## Next

- **Sound.** `libsegaapi` has no public documentation. It has to be worked out
  from a title's imports and its behaviour.
- **A playable game.** *Let's Go Jungle* takes a coin and starts. Next is
  getting through a stage.
- **The offscreen composite on the Cg titles.** The world draws with
  `LINDBERGH_NO_FBO=1` but not through the engine's framebuffer objects.
- **Headless recording** (`--headless --record out.mp4`), the same flag every
  sibling recomp has, so a run can be filmed with no window.
- **A conformance harness** that reports a pass count against a fixed set of
  lifted binaries and fails CI on a regression. Today the checks are
  pass/fail self-tests.

## Deferred

- **Other board revisions.** Everything here assumes Lindbergh Yellow.
- **A one-click `Setup.cmd`.** The toolkit has nothing to launch on its own, so
  the quick start lives in each game repo (for example
  [letsgojungle-lindbergh-recomp](https://github.com/sp00nznet/letsgojungle-lindbergh-recomp)).
- **Card readers and networking** for *Initial D 4/5*.
- **Upstream pieces of the PS5 port** (letsgojungle-lindbergh-recomp#1): the
  C versions of the Sofdec MMX helpers, and its crash reporter, each as its own
  PR.

## Out of scope

- **Decrypting Lindbergh media.** Bring a game tree you can already read.
- Distributing game data, keys, or lifted source in any form.
