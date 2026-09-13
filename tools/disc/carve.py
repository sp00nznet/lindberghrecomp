#!/usr/bin/env python3
"""
carve.py - pull the payload files out of a Lindbergh game DVD image.

A Lindbergh DVD is not a plain ISO, so `7z x disc.iso` fails on it outright.
Measured across the Let's Go Jungle / House of the Dead 4 / Initial D 4 /
Virtua Tennis 3 dumps, the layout is:

    0x000000   encrypted region, 2.8-3.9 MB. Byte-identical across all four
               discs for at least its first 0x60 bytes, so it is one fixed
               structure under one fixed key, not per-disc data.
    ~0x2B8000  ISO9660 #1, volume "SEGA_LINDBERGH" / "<GAME NAME>", ~1 MB
    ~0x3C0000  ISO9660 #2, volume "LINUX" / "CDROM", the rest of the disc

Both filesystems are plain, unencrypted ISO9660. The second one is the payload:

    disk0.img      the game's root filesystem, 1.0-3.3 GB   (encrypted)
    disk1/9.img    372,736 bytes each, boilerplate           (encrypted)
    su1.dat        31-37 MB updater payload                  (encrypted)
    su2.dat        ~330 KB                                   (encrypted)
    frontend.set   ~850 bytes                                (encrypted)

So carving gets you correctly named, correctly sized payload files and stops
there: their *contents* are encrypted under a key that lives in the cabinet,
not on the disc. This tool does not decrypt anything - see docs/disc-format.md
for what is known about the encryption and what you have to bring yourself.
"""

import os
import struct

SECTOR = 2048
PVD_MAGIC = b"\x01CD001"


def _le32(buf, off):
    """ISO9660 stores 32-bit fields twice, little-endian then big-endian.
    Read the little-endian half."""
    return struct.unpack_from("<I", buf, off)[0]


def find_filesystems(path, scan_bytes=16 << 20):
    """Locate every ISO9660 primary volume descriptor in the head of an image.

    Returns [(fs_start, volume_id, system_id, size_bytes)] in disc order. A PVD
    always sits 16 sectors into its own filesystem, which is what lets us
    recover fs_start - the nested filesystems here do not start at offset 0."""
    with open(path, "rb") as f:
        data = f.read(scan_bytes)
    found = []
    for off in range(16 * SECTOR, len(data) - SECTOR, SECTOR):
        if data[off:off + 6] != PVD_MAGIC:
            continue
        found.append((
            off - 16 * SECTOR,
            data[off + 40:off + 72].decode("latin-1").strip(),
            data[off + 8:off + 40].decode("latin-1").strip(),
            _le32(data, off + 80) * SECTOR,
        ))
    return found


class Iso9660:
    """Just enough ISO9660 to list and extract the root directory.

    Lindbergh payload filesystems are flat - a handful of files, no
    subdirectories, no Joliet - so there is nothing here to recurse through."""

    def __init__(self, f, fs_start):
        self.f = f
        self.base = fs_start
        f.seek(fs_start + 16 * SECTOR)
        pvd = f.read(SECTOR)
        if pvd[:6] != PVD_MAGIC:
            raise ValueError("no ISO9660 primary volume descriptor at %#x" % fs_start)
        self.volume_id = pvd[40:72].decode("latin-1").strip()
        self.system_id = pvd[8:40].decode("latin-1").strip()
        root = pvd[156:156 + 34]
        self.root_lba = _le32(root, 2)
        self.root_len = _le32(root, 10)

    def files(self):
        """[(name, absolute byte offset in the image, size)] for the root dir."""
        self.f.seek(self.base + self.root_lba * SECTOR)
        buf = self.f.read(self.root_len)
        out = []
        p = 0
        while p < len(buf):
            rec_len = buf[p]
            if rec_len == 0:
                # A directory record never straddles a sector; the tail of the
                # current one is zero padding. Skip to the next sector.
                p = (p // SECTOR + 1) * SECTOR
                continue
            rec = buf[p:p + rec_len]
            p += rec_len
            if rec[25] & 0x02:                     # directory flag: '.' and '..'
                continue
            name = rec[33:33 + rec[32]].decode("latin-1").split(";")[0]
            out.append((name, self.base + _le32(rec, 2) * SECTOR, _le32(rec, 10)))
        return out


def extract(image, outdir, fs_start=None):
    """Carve the payload files out of `image` into `outdir`.

    With no fs_start, takes the last filesystem on the disc - the big "LINUX"
    one. Returns [(name, size)]."""
    if fs_start is None:
        found = find_filesystems(image)
        if not found:
            raise ValueError("no ISO9660 filesystem in %s - not a Lindbergh disc?" % image)
        fs_start = found[-1][0]
    os.makedirs(outdir, exist_ok=True)
    got = []
    with open(image, "rb") as f:
        iso = Iso9660(f, fs_start)
        for name, off, size in iso.files():
            f.seek(off)
            with open(os.path.join(outdir, name), "wb") as o:
                left = size
                while left:
                    chunk = f.read(min(left, 1 << 22))
                    if not chunk:
                        raise EOFError("%s: image ends %d bytes early" % (name, left))
                    o.write(chunk)
                    left -= len(chunk)
            got.append((name, size))
    return got
