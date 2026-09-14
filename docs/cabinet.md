# The cabinet

A Lindbergh game does not talk to its hardware directly. It calls SEGA's
amLib, statically linked into the binary, and without answers it stops on an
error screen before drawing anything.

## The cabinet, not just the CPU

A Lindbergh game reaches its hardware through SEGA's `amLib`, statically linked
into the binary — so there is no import to bind, and the runtime replaces the
lifted functions by symbol name instead. Without it a game stops on **Error 11
— JVS I/O board is not connected to main board** before drawing anything.

`hle_sega.c` answers three things, each at the lowest seam that works:

* **The base board** — `amLibInit`, `amJvsInit`, `amDongleInit`, `amDongleUpdate` and their predicates. `amJvsCheckInit` is a predicate, not a status code; answering it with the library's success value of 0 becomes "−5 JVS node(s) found".
* **A JVS I/O board**, at `amJvsSendRequest` / `amJvsRecvAcknowledge` only. The frames are real JVS, so all sixty of a game's own packet builders and parsers run unmodified above two replaced functions. It reports identity, revisions, and a feature list; nothing is pressed or aimed.
* **The battery-backed store**, at the four wrapper functions under the record layer. Record layout, duplicate copies and CRCs are the game's own code and work untouched. Read takes the offset first and the buffer second; write takes them the other way round — which is not guessable, and the log says so.

What is assumed rather than emulated: the backup records are blank and a game
cannot initialise them without the EEPROM's I2C bus. A real cabinet ships that
store written, so the runtime makes the same statement, suppresses the one
error code it raises, and announces that it has.
