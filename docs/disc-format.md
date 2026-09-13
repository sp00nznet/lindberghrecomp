# The Lindbergh game DVD

Everything here was measured from four dumps in hand — *Let's Go Jungle* (Rev A,
2007), *The House of the Dead 4* (Rev A, 2005), *Initial D 4* Export (Rev D,
2008) and *Virtua Tennis 3* (Rev A, 2006) — not read off a wiki. Where a claim
is inference rather than measurement it says so.

## Layout

A Lindbergh disc image is not an ISO, which is why `7z x disc.iso` fails on one
outright. It is three regions back to back:

```
0x000000   encrypted header region, 2.8–3.9 MB
~0x2B0000  ISO9660  system "SEGA_LINDBERGH"  volume "<GAME NAME>"   ~1 MB
~0x3B8000  ISO9660  system "LINUX"           volume "CDROM"         the rest
```

Both ISO9660 filesystems are ordinary and unencrypted — plain directory
records, no Joliet, no subdirectories. `tools/disc/carve.py` finds them by
scanning for primary volume descriptors and recovering each filesystem's start
from the 16-sector offset a PVD always sits at:

```
$ py -3.11 -m tools disc "Let's Go Jungle (World) ... (Rev A).iso"
0x002b0000  sector 1376    SEGA_LINDBERGH   LETS_GO_JUNGLE               1.0 MB
0x003b8000  sector 1904    LINUX            CDROM                     1068.4 MB
```

The first 0x60 bytes of the encrypted header region are **byte-identical across
all four discs**, including discs three years apart. One fixed structure under
one fixed key, not per-disc data.

## The payload

The "LINUX" filesystem holds the game. Two naming conventions appear:

| | Let's Go Jungle | Initial D 4 | Virtua Tennis 3 | House of the Dead 4 |
|---|---|---|---|---|
| main image | `disk0.img` 1,034,084,352 | `disk0.img` 3,329,839,104 | `disk0.img` 1,775,333,376 | `hod4data.img` 2,936,012,800 |
| | `disk1.img` 372,736 | — | — | `hod4prog.img` 16,777,216 |
| | `disk9.img` 372,736 | `disk9.img` 372,736 | `disk9.img` 372,736 | `hod4drv.img` 67,108,864 |
| updater | `su1.dat` 32,891,331 | `su1.dat` 37,171,658 | `su1.dat` 31,092,053 | `su1.dat` 30,806,596 |
| | `su2.dat` 340,450 | `su2.dat` 326,327 | `su2.dat` 328,302 | `su2.dat` 363,431 |
| frontend | `frontend.set` 878 | `frontend.set` 840 | `frontend.set` 858 | `frontend` 3,146 |

The 2005 title names its images after itself and splits program, driver and
data; the 2006–2008 titles use the generic `disk0`/`disk9` scheme. `disk9.img`
is the same 372,736 bytes on every disc that has one.

## Encryption

Every payload file is encrypted. Comparing the first 16 bytes of each:

```
disk0.img   Let's Go Jungle   1fa2d29a44ed92a5215dd535ad9e389d
            Initial D 4       1fa2d29a44ed92a5215dd535ad9e389d     <- identical
            Virtua Tennis 3   d6315f6683b6a4d7d281653c470eae78

disk9.img   Let's Go Jungle   4d9368e07489fecefc26207d507e3d6b
            Initial D 4       4d9368e07489fecefc26207d507e3d6b     <- identical
            Virtua Tennis 3   f3b38096d95ed613e5d816cc459e691b

su1.dat     all four          all different
su2.dat     all four          all different
frontend    all four          all different
```

What that says:

* **A 128-bit block cipher, deterministic per position.** Two different games
  produce the same ciphertext for block 0 of `disk0.img`, so the same plaintext
  went in under the same key with the same starting state. A filesystem image
  begins with a zeroed boot block, which is the plaintext both discs share.
* **The key is not per-disc.** *Let's Go Jungle* (2007) and *Initial D 4* (2008)
  share one. *Virtua Tennis 3* (2006) does not match them, so there is more than
  one key in play — plausibly per era or per hardware revision, though four
  discs is not enough to say which.
* **`su*.dat` and `frontend` differ everywhere**, which is expected: those hold
  real per-game content in their first block, so identical ciphertext would be
  the surprise.

Inference, not measurement: the key lives in the cabinet — the security dongle
and the board's own BIOS — and not on the disc. That is the whole point of the
scheme, and it is consistent with everything above.

## What this means for recompilation

Carving gets you correctly named, correctly sized files whose contents you
cannot read. **This toolkit does not decrypt them and will not grow the
ability to.**

To recompile a Lindbergh title you need the game's root filesystem in the
clear — a `disk0.img` you can mount, or the directory tree off a board's own
hard drive — and you bring that yourself, the same way every other recomp
project here expects you to bring your own disc or ROM. Once you have it, the
game is a 32-bit ELF and `tools/elf` and `tools/recomp` take it from there.
