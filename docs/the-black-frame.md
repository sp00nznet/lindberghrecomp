# The black frame

A game that submitted a complete frame every sixty-fifth of a second and
presented nothing. Five diagnoses were wrong before one was right, which is
why the instruments in [instruments.md](instruments.md) exist.

## The black frame was one instruction

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
