#!/usr/bin/env python3
"""texpack_id.py - the runtime's texture-pack id of a GIM, offline (no game run, no dump needed).

The recomp names a replacement <mods>/textures/<id>.png (or a PSPRECOMP_TEXPACK dir) after a 64-bit xxHash of what the
GE decodes (runtime/src/psp_tex_pack.cpp psp_texpack_make_id):

    raw_h  = xxh64(texels, seed 0)          texels unswizzled, rows packed to the texture width (CLUT8: w*h bytes)
    clut_h = xxh64(used palette, seed 1)    for every palette index the texels use, ascending: index byte + RGBA
    id     = xxh64(u64 raw_h, clut_h, fmt, w, h  (little endian), seed 0x70737074)      fmt: GE format, CLUT8 = 5

That is the same for a texture wherever the game uploads it, so the id of a menu GIM can be computed from the file.
Covers the 8-bit indexed + RGBA8888 GIMs the menu portraits use (gim_encode.py), swizzled or not.

    texpack_id.py <file.gim> ...                 print '<id>  WxH  file'
    texpack_id.py --pack <menu pak.bin> <index> ...
Standard library only (pure Python xxHash64).
"""
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import gim_encode as ge   # noqa: E402  (parse() only; it imports Pillow at module level)
import menu_pack as mp    # noqa: E402

M = 0xFFFFFFFFFFFFFFFF
P1, P2, P3, P4, P5 = (11400714785074694791, 14029467366897019727, 1609587929392839161,
                      9650029242287828579, 2870177450012600261)
GE_TFMT_CLUT8 = 5


def _rotl(x, r):
    return ((x << r) | (x >> (64 - r))) & M


def _round(acc, v):
    acc = (acc + v * P2) & M
    return (_rotl(acc, 31) * P1) & M


def _merge(acc, v):
    return (((acc ^ _round(0, v)) * P1) + P4) & M


def xxh64(data, seed=0):
    n = len(data)
    p = 0
    if n >= 32:
        v1, v2, v3, v4 = (seed + P1 + P2) & M, (seed + P2) & M, seed & M, (seed - P1) & M
        lim = n - 32
        while p <= lim:
            a, b, c, d = struct.unpack_from("<4Q", data, p)
            v1, v2, v3, v4 = _round(v1, a), _round(v2, b), _round(v3, c), _round(v4, d)
            p += 32
        h = (_rotl(v1, 1) + _rotl(v2, 7) + _rotl(v3, 12) + _rotl(v4, 18)) & M
        for v in (v1, v2, v3, v4):
            h = _merge(h, v)
    else:
        h = (seed + P5) & M
    h = (h + n) & M
    while p + 8 <= n:
        h ^= _round(0, struct.unpack_from("<Q", data, p)[0])
        h = (_rotl(h, 27) * P1 + P4) & M
        p += 8
    if p + 4 <= n:
        h ^= (struct.unpack_from("<I", data, p)[0] * P1) & M
        h = (_rotl(h, 23) * P2 + P3) & M
        p += 4
    while p < n:
        h ^= (data[p] * P5) & M
        h = (_rotl(h, 11) * P1) & M
        p += 1
    h ^= h >> 33
    h = (h * P2) & M
    h ^= h >> 29
    h = (h * P3) & M
    h ^= h >> 32
    return h


def unswizzle(plane, w_bytes, h):
    """PSP 16-byte x 8-row block order -> linear rows."""
    out = bytearray(len(plane))
    bw = w_bytes // 16
    i = 0
    for by in range(0, h, 8):
        for bx in range(bw):
            for y in range(8):
                o = (by + y) * w_bytes + bx * 16
                out[o:o + 16] = plane[i:i + 16]
                i += 16
    return bytes(out)


def make_id(texels, fmt, w, h, clut_rgba=None, clut_bits=8):
    raw_h = xxh64(texels, 0)
    clut_h = 0
    if clut_rgba is not None:
        used = set(texels) if clut_bits == 8 else {b & 15 for b in texels} | {b >> 4 for b in texels}
        buf = bytearray()
        for i in sorted(used):
            buf.append(i)
            buf += clut_rgba[4 * i:4 * i + 4]
        clut_h = xxh64(bytes(buf), 1)
    return xxh64(struct.pack("<5Q", raw_h, clut_h, fmt, w, h), 0x70737074)


def gim_id(d):
    """Texture-pack id of an 8-bit indexed + RGBA8888 GIM (returns (id, w, h))."""
    info = ge.parse(d)
    io, w, h, fmt, order = info["img"]
    po, pw, _, pfmt, _ = info["pal"]
    if fmt != 5 or pfmt != 3:
        raise ValueError("only I8 + RGBA8888 GIMs (fmt %d, palette fmt %d)" % (fmt, pfmt))
    plane = d[io:io + w * h]
    if order == 1:
        plane = unswizzle(plane, w, h)
    pal = bytearray(d[po:po + 4 * pw]) + bytes(4 * max(0, 256 - pw))
    return make_id(plane, GE_TFMT_CLUT8, w, h, bytes(pal)), w, h


def main(argv):
    if not argv:
        print(__doc__)
        return 2
    if argv[0] == "--pack":
        pk = mp.parse(open(argv[1], "rb").read())
        for k in argv[2:]:
            tid, w, h = gim_id(mp.entry(pk, int(k)))
            print("%016x  %dx%d  %s[%s]" % (tid, w, h, argv[1], k))
        return 0
    for f in argv:
        tid, w, h = gim_id(open(f, "rb").read())
        print("%016x  %dx%d  %s" % (tid, w, h, f))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
