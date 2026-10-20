# Contributors

Everyone whose work or reports shaped this toolkit. Code contributions keep
their own authorship in git; this file records what each person found or
fixed.

## Code

### Ned Heller ([@sp00nznet](https://github.com/sp00nznet))
Maintainer.

## Issue reports and testing

### [@Redmoneydash](https://github.com/Redmoneydash)
Built a native PS5 port of *Initial D Arcade Stage 4* on this toolkit
([initiald4-lindbergh-ps5-port](https://github.com/Redmoneydash/initiald4-lindbergh-ps5-port)),
plus a Linux X11/EGL debug build. It was the first title brought up on the
toolkit by someone other than the maintainer. Reported it in
letsgojungle-lindbergh-recomp#1, with a patch against `f5dc1b4`.

- Found that the Cg seam's 512-entry shader table fills during Initial D 4's
  init. Shaders after that point (`dof` among them) were never indexed, and
  nothing reported it. The table is now 2048 and reports when it fills (#1).
- Narrowed the Cg titles' 3D pass down to the offscreen composite: with
  `LINDBERGH_NO_FBO=1` the world draws, so geometry and vertex programs are
  fine.
- Wrote C versions of the Sofdec MMX helpers that every title links
  statically. They're on the roadmap to upstream.

## A note on AI-assisted contributions

PRs written with AI help are welcome, provided a person understood the change
and checked it on a real title.
