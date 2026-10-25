#!/usr/bin/env python3
"""Self-test for gmo_arrays_check.py on a tiny synthetic GMO (no game data). Run: python3 -I test_gmo_arrays_check.py"""
import os, struct, sys, tempfile
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import gmo_arrays_check as g


def chunk(t, header, body):
    hs = 8 + len(header)
    return struct.pack('<HHI', t, hs, hs + len(body)) + header + body


def arrays(name, nw, count, retail_style):
    word = 2 | (6 << 2) | (2 << 7) | (1 << 9) | ((nw - 1) << 14)
    stride = g.vertex_stride(word)
    word |= stride << 24
    nm = name.encode() + b'\0'
    nm += b'\0' * (-len(nm) % 4)
    body = struct.pack('<IIII', word, count, 1, 0) + bytes(count * stride)
    hs = 8 + 8 + len(nm)
    w0, w1 = (hs + len(body), hs + 16) if retail_style else (hs, hs)
    return chunk(7, struct.pack('<II', w0, w1) + nm, body), stride


def gmo(*arrs):
    part = chunk(5, struct.pack('<II', 0x18, 0x18) + b'p\0\0\0', b''.join(arrs))
    return b'OMG.00.1PSP\0\0\0\0\0' + chunk(2, struct.pack('<II', 0x10, 0x10), part)


def main():
    assert [g.vertex_stride(w) for w in (0x0E00131A & 0xFFFFFF, 0x1000931A & 0xFFFFFF, 0x1401C31A & 0xFFFFFF, 0x1201431A & 0xFFFFFF)] == [14, 16, 20, 18]
    for nw, want in ((1, 14), (3, 16), (4, 16), (6, 18), (8, 20)):
        a, st = arrays('arrays-0-0000', nw, 5, True)
        assert st == want, (nw, st)
        n, problems, _ = g.check(gmo(a))
        assert n == 1 and not problems, problems
    bad, _ = arrays('arrays-0-0000', 8, 5, False)
    good, _ = arrays('arrays-1-0000', 4, 7, True)
    data = gmo(bad, good)
    n, problems, fixed = g.check(data)
    assert n == 2 and len(problems) == 1 and 'arrays-0-0000' in problems[0], problems
    n, problems, again = g.check(fixed)
    assert not problems and again == fixed
    with tempfile.TemporaryDirectory() as d:
        p = os.path.join(d, 'x.gmo'); q = os.path.join(d, 'y.gmo')
        open(p, 'wb').write(data)
        assert g.main(['x', p]) == 1
        assert g.main(['x', p, '--fix', q]) == 1 and g.main(['x', q]) == 0
    print('ok')


main()
