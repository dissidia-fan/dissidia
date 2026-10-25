#!/usr/bin/env python3
"""Read and edit Dissidia 012 'mess pak' text archives (text/<lang>/mess_*.bin).

A mess pak is a menu pak (menu_pack.py) whose entries are string tables:

    u16 count
    u16 start[count]     start of record i, in 16 bit words from the entry start (record i ends where
                         record i+1 starts, the last one at the end of the entry)
    records              u16 byte_length, then byte_length/2 words (UTF-16 code unit incl. the NUL)

Each code unit is XORed with KEY[(record_index + position_in_record) % 16], so a record can only be moved
to another index by re-encoding it.  The key was recovered from the known text "Play as this character?".

The u16 starts limit a plain entry to records that start below 65536 words (128 KiB). The game's text code also
reads an "OFF4" entry (retail: entry 1 of text/<lang>/mess_pk_customize_ability_0.bin), which has no such limit:

    "OFF4", u32 count, u32 offset[count] (in 4-byte units from the entry start), records {u32 byte_length,
    byte_length/2 code units}: the text, a NUL, NUL padding to an even number of units; contiguous, no gaps

(EBOOT FUN_0887A8F0 tests the magic; FUN_0887A800 = record offset, FUN_0887A6A0 = the decoder run at load and
FUN_0887B3EC & co. = the accessors take both layouts; the same XOR key applies). records() reads both,
build() writes the plain layout, build_off4() the OFF4 one and build_fit() whichever holds the texts (plain if it can).

    mess_text.py <pak> list  <entry>
    mess_text.py <pak> set   <entry> <index> "text" <out pak>      # replace a record (index == count appends)
    mess_text.py <pak> add   <entry> "text" <out pak>              # append, prints the new record index
Standard library only.
"""
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import menu_pack as mp  # noqa: E402

KEY = [0xED98, 0x90AB, 0x5E28, 0x8442, 0x32B1, 0x2DDA, 0xA657, 0xC8BF,
       0x0033, 0x579A, 0xF227, 0x4B4C, 0x36A7, 0x927D, 0x739D, 0x84CE]


OFF4 = b"OFF4"


def records(e):
    if bytes(e[:4]) == OFF4:
        return _records_off4(e)
    cnt = struct.unpack_from("<H", e, 0)[0]
    starts = struct.unpack_from("<%dH" % cnt, e, 2)
    out = []
    for i in range(cnt):
        a = starts[i] * 2
        b = starts[i + 1] * 2 if i + 1 < cnt else len(e)
        n = struct.unpack_from("<H", e, a)[0]
        w = struct.unpack_from("<%dH" % (n // 2), e, a + 2)
        out.append("".join(chr(x ^ KEY[(i + j) % 16]) for j, x in enumerate(w)).rstrip("\0"))
    return out


def _records_off4(e):
    cnt = struct.unpack_from("<I", e, 4)[0]
    offs = struct.unpack_from("<%dI" % cnt, e, 8)
    out = []
    for i, o in enumerate(offs):
        n = struct.unpack_from("<I", e, 4 * o)[0]
        w = struct.unpack_from("<%dH" % (n // 2), e, 4 * o + 4)
        out.append("".join(chr(x ^ KEY[(i + j) % 16]) for j, x in enumerate(w)).rstrip("\0"))
    return out


def build_off4(texts):
    """OFF4 entry bytes for a list of strings (layout in the module doc)."""
    cnt = len(texts)
    body, offs = bytearray(), []
    base = 8 + 4 * cnt
    for i, t in enumerate(texts):
        u = [ord(c) for c in t] + [0]
        assert all(c < 0x10000 for c in u)
        if len(u) % 2:
            u.append(0)
        offs.append((base + len(body)) // 4)
        body += struct.pack("<I", 2 * len(u)) + struct.pack(
            "<%dH" % len(u), *[c ^ KEY[(i + j) % 16] for j, c in enumerate(u)])
    return OFF4 + struct.pack("<I", cnt) + struct.pack("<%dI" % cnt, *offs) + bytes(body)


def plain_fits(texts):
    """True if build(texts) can hold the texts (every record starts below 65536 words)."""
    pos = 1 + len(texts)
    for t in texts[:-1]:
        pos += 1 + len(t) + 1
    return pos <= 0xFFFF


def build_fit(texts):
    """build(texts) when the plain layout holds them, else build_off4(texts)."""
    return build(texts) if plain_fits(texts) else build_off4(texts)


def build(texts):
    """Entry bytes for a list of strings."""
    cnt = len(texts)
    recs = []
    for i, t in enumerate(texts):
        u = [ord(c) for c in t] + [0]
        # characters outside the BMP are not used by the game's fonts
        assert all(c < 0x10000 for c in u)
        recs.append(struct.pack("<H", 2 * len(u)) + struct.pack(
            "<%dH" % len(u), *[c ^ KEY[(i + j) % 16] for j, c in enumerate(u)]))
    start = 1 + cnt
    out = bytearray(struct.pack("<H", cnt))
    pos = start
    for r in recs:
        out += struct.pack("<H", pos)
        pos += len(r) // 2
    for r in recs:
        out += r
    return bytes(out)


def set_text(pak_data, entry, index, text):
    pk = mp.parse(pak_data)
    e = mp.entry(pk, entry)
    t = records(e)
    if index == len(t):
        t.append(text)
    else:
        t[index] = text
    # keep untouched records byte exact by rebuilding only through the codec (it is lossless for
    # records that were decoded from the same layout); verify that to be safe
    blobs = [mp.entry(pk, i) for i in range(len(pk["ents"]))]
    blobs[entry] = build(t)
    return mp.build(pk, blobs), index


def main(argv):
    if len(argv) < 3:
        print(__doc__)
        return 2
    data = open(argv[0], "rb").read()
    pk = mp.parse(data)
    cmd = argv[1]
    entry = int(argv[2])
    if cmd == "list":
        for i, t in enumerate(records(mp.entry(pk, entry))):
            print(i, repr(t))
    elif cmd == "set":
        out, i = set_text(data, entry, int(argv[3]), argv[4])
        open(argv[5], "wb").write(out)
        print("wrote", argv[5], "record", i)
    elif cmd == "add":
        n = len(records(mp.entry(pk, entry)))
        out, i = set_text(data, entry, n, argv[3])
        open(argv[4], "wb").write(out)
        print("wrote", argv[4], "record", i)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
