#!/usr/bin/env python3
"""Self-check for the Lindbergh disc carver.

Builds a disc shaped like a real one - junk, then an ISO9660 filesystem at a
non-zero offset - and checks the carver finds it and pulls the file back out
byte for byte. Run: py -3.11 tools/disc/test_carve.py
"""

import os
import struct
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))))

from tools.disc.carve import SECTOR, extract, find_filesystems

PAYLOAD = b"disk0.img contents, encrypted on a real disc" * 100


def _both32(v):
    return struct.pack("<I", v) + struct.pack(">I", v)


def _dirrec(lba, size, name, flags=0):
    name_b = name.encode("ascii")
    rec = bytearray(33 + len(name_b))
    rec[1] = 0
    rec[2:10] = _both32(lba)
    rec[10:18] = _both32(size)
    rec[25] = flags
    rec[32] = len(name_b)
    rec[33:] = name_b
    if len(rec) % 2:                       # records are padded to even length
        rec += b"\0"
    rec[0] = len(rec)
    return bytes(rec)


def make_disc(junk_sectors, volume_id, filename):
    """junk | [fs_start: 16 empty sectors | PVD | rootdir | file data]"""
    fs = bytearray(16 * SECTOR)
    pvd = bytearray(SECTOR)
    pvd[0:7] = b"\x01CD001\x01"
    pvd[8:40] = b"LINUX".ljust(32)
    pvd[40:72] = volume_id.encode("ascii").ljust(32)
    file_lba, root_lba = 18, 17
    total = file_lba + (len(PAYLOAD) + SECTOR - 1) // SECTOR
    pvd[80:88] = _both32(total)
    root = _dirrec(root_lba, SECTOR, filename)
    pvd[156:156 + len(root)] = root        # root record's own name is ignored
    fs += pvd

    rootdir = bytearray(SECTOR)
    recs = _dirrec(root_lba, SECTOR, "\x00", flags=2) + \
           _dirrec(root_lba, SECTOR, "\x01", flags=2) + \
           _dirrec(file_lba, len(PAYLOAD), filename)
    rootdir[:len(recs)] = recs
    fs += rootdir
    fs += PAYLOAD.ljust(((len(PAYLOAD) + SECTOR - 1) // SECTOR) * SECTOR, b"\0")
    return bytes(bytearray(os.urandom(junk_sectors * SECTOR))) + bytes(fs)


def main():
    junk = 1376                            # where Let's Go Jungle's first fs sits
    disc = make_disc(junk, "CDROM", "DISK0.IMG;1")
    with tempfile.TemporaryDirectory() as td:
        img = os.path.join(td, "disc.iso")
        with open(img, "wb") as f:
            f.write(disc)

        found = find_filesystems(img)
        assert len(found) == 1, "expected one filesystem, got %r" % (found,)
        fs_start, vol, sysid, size = found[0]
        assert fs_start == junk * SECTOR, "fs_start %#x" % fs_start
        assert vol == "CDROM", vol
        assert sysid == "LINUX", sysid

        got = extract(img, os.path.join(td, "out"))
        assert got == [("DISK0.IMG", len(PAYLOAD))], got
        with open(os.path.join(td, "out", "DISK0.IMG"), "rb") as f:
            assert f.read() == PAYLOAD, "payload mismatch"

    print("ok: carve finds a nested ISO9660 and extracts it byte-exact")


if __name__ == "__main__":
    main()
