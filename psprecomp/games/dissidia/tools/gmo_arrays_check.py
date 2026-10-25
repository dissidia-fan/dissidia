#!/usr/bin/env python3
"""Check (and optionally fix) the ARRAYS chunk headers of a Dissidia 012 character GMO.

Why: the game's model loader does not take the vertex data address from the vertex-array payload. It
reads two words from the *chunk header* of every Arrays block and uses them:

    chunk +0   u16 type (7)   u16 header size HS   u32 chunk size
    chunk +8   u32 W0         must be the chunk size (retail: always)
    chunk +12  u32 W1         must be HS + 16: the offset of the first vertex from the chunk start
                              (the 16 bytes in front are the array payload header: vtype, count, ...)
    chunk +16  name ("arrays-0-0000")

Retail files satisfy this for all 89 arrays of p_fiv100_1p.gmo. A tool that writes W0 = W1 = HS (as the
generic "block header" of the other chunk types) makes the loader start every vertex stream 16 bytes
early: the streams are shifted by one vertex for 16-byte vertices (4 weights, subtly wrong triangles)
and by a fraction of a vertex for the others (6 or 8 weights, strides 18 and 20), which renders as
a few huge, smoothly shaded triangles. It has nothing to do with the number of weights.

usage: gmo_arrays_check.py FILE.gmo            report only (exit 1 when an array is wrong)
       gmo_arrays_check.py FILE.gmo --fix OUT  write a corrected copy
"""
import struct, sys

MAGIC = b'OMG.00.1PSP'
ARRAYS = 7
LEAF = {7, 0x000C}   # Arrays and FCurve blocks carry a raw body, every other block carries child chunks


def walk(data, start, end, depth=0):
    """Yield (offset, type, header_size, size, depth) for every chunk."""
    o = start
    while o + 8 <= end:
        t, hs, sz = struct.unpack_from('<HHI', data, o)
        if sz < 8 or o + sz > end:
            raise ValueError('bad chunk 0x%04x at 0x%x (size %d)' % (t, o, sz))
        yield o, t, hs, sz, depth
        if not (t & 0x8000) and t not in LEAF:
            yield from walk(data, o + hs, o + sz, depth + 1)
        o += sz


def vertex_stride(word):
    """Stride of a GE vertex type word (weights, texcoord, colour, normal, position; each aligned to its size)."""
    sz = {0: 0, 1: 1, 2: 2, 3: 4}
    csz = {0: 0, 4: 2, 5: 2, 6: 2, 7: 4}
    off, al = 0, 1

    def place(n, s):
        nonlocal off, al
        if s:
            off = (off + s - 1) // s * s
            off += n * s
            al = max(al, s)
    wt = (word >> 9) & 3
    place(((word >> 14) & 7) + 1 if wt else 0, sz[wt])
    place(2, sz[word & 3])
    place(1, csz[(word >> 2) & 7])
    place(3, sz[(word >> 5) & 3])
    place(3, sz[(word >> 7) & 3])
    return (off + al - 1) // al * al


def check(data, fix=False):
    """Return (problems, data); with fix the returned data has W0/W1 corrected."""
    if data[:11] != MAGIC:
        raise ValueError('not a GMO file')
    buf = bytearray(data)
    problems, n = [], 0
    for o, t, hs, sz, _ in list(walk(data, 16, len(data))):
        if t != ARRAYS:
            continue
        n += 1
        w0, w1 = struct.unpack_from('<II', data, o + 8)
        name = bytes(data[o + 16:o + hs]).split(b'\0')[0].decode('latin1')
        want0, want1 = sz, hs + 16
        vt, count = struct.unpack_from('<II', data, o + hs)
        if (w0, w1) != (want0, want1):
            problems.append('%s at 0x%x: header words 0x%x, 0x%x (want 0x%x, 0x%x)' % (name, o, w0, w1, want0, want1))
            struct.pack_into('<II', buf, o + 8, want0, want1)
        if vt >> 24 != vertex_stride(vt):
            problems.append('%s at 0x%x: vtype 0x%08x stride byte %d, computed stride %d' % (name, o, vt, vt >> 24, vertex_stride(vt)))
        if count * (vt >> 24) + 16 > sz - hs:
            problems.append('%s at 0x%x: %d vertices x stride %d do not fit in the chunk' % (name, o, count, vt >> 24))
    return n, problems, bytes(buf)


def main(argv):
    if len(argv) < 2:
        print(__doc__)
        return 2
    data = open(argv[1], 'rb').read()
    n, problems, fixed = check(data)
    print('%s: %d Arrays chunks, %d problem(s)' % (argv[1], n, len(problems)))
    for p in problems[:20]:
        print('  ' + p)
    if len(problems) > 20:
        print('  ... %d more' % (len(problems) - 20))
    if '--fix' in argv:
        out = argv[argv.index('--fix') + 1]
        open(out, 'wb').write(fixed)
        print('wrote', out)
    return 1 if problems else 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
