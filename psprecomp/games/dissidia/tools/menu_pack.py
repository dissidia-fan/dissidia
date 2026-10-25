#!/usr/bin/env python3
"""Read and rebuild Dissidia 012 'menu pak' (and the identically laid out 'mess pak' text) archives (menu/*_gim_N.bin, *_seq_N.bin).

Layout (little endian):
    0x00  "menu pak"
    0x08  u16 version (1)
    0x0A  u16 entry count N
    0x0C  u32 id base (0x7000 for the free-battle gim packs: the menu engine makes one entity per
          entry with id base + running index across all packs of the screen)
    0x10  N x { u32 offset, u32 size }     (size 0 = empty slot, kept so later entries keep their index)
    ...   entry data, each starting on a 16-byte boundary; entries are GIM files (gim packs) or
          'SEQ ' sprite sequence files (seq packs; every second entry there is a 4 byte terminator)

Used to put a new character's textures into free (size 0) slots without moving any other entry's index.

    menu_pack.py <pack.bin> list
    menu_pack.py <pack.bin> extract <index> <out>
    menu_pack.py <pack.bin> set <index> <in.gim> <out pack.bin>   # fill/replace one entry
Standard library only.
"""
import struct
import sys


def parse(data):
    assert data[:8] in (b"menu pak", b"mess pak"), "not a menu/mess pak"
    ver, n, base = struct.unpack_from("<HHI", data, 8)
    ents = [struct.unpack_from("<II", data, 0x10 + 8 * i) for i in range(n)]
    return {"magic": data[:8], "ver": ver, "base": base, "ents": ents, "data": data}


def entry(pk, i):
    off, size = pk["ents"][i]
    return pk["data"][off:off + size]


def build(pk, blobs):
    """Rebuild with entry contents `blobs` (list of bytes, empty = free slot)."""
    n = len(blobs)
    head = bytearray(pk.get("magic", b"menu pak") + struct.pack("<HHI", pk["ver"], n, pk["base"]))
    pos = 0x10 + 8 * n
    table = bytearray()
    body = bytearray(b"\x77" * ((-pos) % 16))
    pos += (-pos) % 16
    for b in blobs:
        if len(b) == 0:
            table += struct.pack("<II", (pos + 15) & ~15, 0)
            continue
        pad = (-pos) % 16
        body += b"\x77" * pad
        pos += pad
        table += struct.pack("<II", pos, len(b))
        body += b
        pos += len(b)
    body += b"\x77" * ((-pos) % 16)
    return bytes(head + table + body)


def set_entry(data, index, blob):
    pk = parse(data)
    blobs = [entry(pk, i) for i in range(len(pk["ents"]))]
    blobs[index] = bytes(blob)
    return build(pk, blobs)


def main(argv):
    if len(argv) < 2:
        print(__doc__)
        return 2
    data = open(argv[0], "rb").read()
    pk = parse(data)
    if argv[1] == "list":
        for i, (o, s) in enumerate(pk["ents"]):
            print(i, hex(o), hex(s))
    elif argv[1] == "extract":
        open(argv[3], "wb").write(entry(pk, int(argv[2])))
    elif argv[1] == "set":
        out = set_entry(data, int(argv[2]), open(argv[3], "rb").read())
        open(argv[4], "wb").write(out)
        print("wrote", argv[4], len(out), "bytes")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
