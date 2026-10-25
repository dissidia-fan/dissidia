#!/usr/bin/env python3
"""Extract / list entries of Dissidia 012's DATA/PACKAGE.BIN by asset path or CRC.

    package_extract.py <disc0 or DATA dir> list  <substring>            # paths from package_paths.txt
    package_extract.py <disc0 or DATA dir> get   <asset path> [out]     # extract one entry
    package_extract.py <disc0 or DATA dir> crc   <hex crc> [out]
    package_extract.py <disc0 or DATA dir> alias <src path> <dst path> <mods dir>
                                                                         # copy src entry to <mods>/<dst path>

`alias` is how a new character borrows another character's files: the game requests
`obj/p_noc100_1p.gmo`, the loose-file layer serves <mods>/obj/p_noc100_1p.gmo, which here is a
byte copy of `obj/p_fiv100_1p.gmo`.  The index format is described in games/dissidia/README.md.
Only the standard library is used.
"""
import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
PATHS = os.path.join(HERE, "..", "package_paths.txt")


def crc_table():
    t = []
    for i in range(256):
        c = i << 24
        for _ in range(8):
            c = ((c << 1) ^ 0x04C11DB7) & 0xFFFFFFFF if c & 0x80000000 else (c << 1) & 0xFFFFFFFF
        t.append(c)
    return t


_T = crc_table()


def package_crc(path: str) -> int:
    c = 0xFFFFFFFF
    for b in path.lower().replace("\\", "/").encode():
        c = ((c << 8) & 0xFFFFFFFF) ^ _T[((c >> 24) ^ b) & 0xFF]
    return c ^ 0xFFFFFFFF


class Package:
    def __init__(self, data_dir: str):
        for cand in (data_dir, os.path.join(data_dir, "PSP_GAME", "USRDIR", "DATA"),
                     os.path.join(data_dir, "disc0", "PSP_GAME", "USRDIR", "DATA")):
            if os.path.exists(os.path.join(cand, "PACKAGE_INFO.BIN")):
                data_dir = cand
                break
        self.dir = data_dir
        idx = open(os.path.join(data_dir, "PACKAGE_INFO.BIN"), "rb").read()
        magic, tag, count, _ = struct.unpack_from("<I4sII", idx, 0)
        assert tag == b"pack", "not a PACKAGE_INFO.BIN"
        self.entries = {}
        for i in range(count):
            crc, sector, sf = struct.unpack_from("<III", idx, 16 + 12 * i)
            self.entries[crc] = (sector, sf & 0x0FFFFFFF, sf >> 28)
        self.pkg = open(os.path.join(data_dir, "PACKAGE.BIN"), "rb")

    def read_crc(self, crc: int) -> bytes:
        sector, size, _ = self.entries[crc]
        self.pkg.seek(sector * 2048)
        return self.pkg.read(size)

    def read(self, path: str) -> bytes:
        return self.read_crc(package_crc(path))


def main(argv):
    if len(argv) < 3:
        print(__doc__)
        return 2
    pk = Package(argv[0])
    cmd = argv[1]
    if cmd == "list":
        sub = argv[2].lower()
        for line in open(PATHS):
            p = line.strip()
            if sub in p.lower():
                c = package_crc(p)
                if c in pk.entries:
                    print("%08x %9d %s" % (c, pk.entries[c][1], p))
    elif cmd == "get":
        d = pk.read(argv[2])
        out = argv[3] if len(argv) > 3 else os.path.basename(argv[2])
        open(out, "wb").write(d)
        print("%s: %d bytes -> %s" % (argv[2], len(d), out))
    elif cmd == "crc":
        c = int(argv[2], 16)
        d = pk.read_crc(c)
        out = argv[3] if len(argv) > 3 else "%08x.bin" % c
        open(out, "wb").write(d)
        print("%08x: %d bytes -> %s" % (c, len(d), out))
    elif cmd == "alias":
        src, dst, mods = argv[2], argv[3], argv[4]
        d = pk.read(src)
        out = os.path.join(mods, dst)
        os.makedirs(os.path.dirname(out), exist_ok=True)
        open(out, "wb").write(d)
        print("%s -> %s (%d bytes)" % (src, out, len(d)))
    else:
        print(__doc__)
        return 2
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
