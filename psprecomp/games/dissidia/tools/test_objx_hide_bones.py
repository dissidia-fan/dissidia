#!/usr/bin/env python3
"""Self-test for objx_hide_bones.py on a synthetic objx (no game data). Run: python3 -I test_objx_hide_bones.py"""
import os, struct, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import objx_hide_bones as h


def timeline(motion, ranged, keyed=()):
    """ranged: [(start, end, op, args)], keyed: [(frame, op, args)]"""
    pre = bytes(4)
    r = b''.join(struct.pack('<HHHHI', s, e, op, 0, 0) + struct.pack('<%dI' % len(a), *a) for s, e, op, a in ranged)
    k = b''.join(struct.pack('<HHI', f, op, 0) + struct.pack('<%dI' % len(a), *a) for f, op, a in keyed)
    off_k = 16 + len(pre) + len(r)
    return struct.pack('<8H', motion, (len(keyed) << 8) | (len(ranged) + 1), off_k, off_k + len(k), 0, 0, 0, 0) + pre + r + k


def objx(blobs):
    return h.arc_write(bytes(8), [[8, b'p_se', h.events_join(3, blobs)], [1, b'comb', bytes(12)]])


def main():
    idle = timeline(0, [(0, 65535, 0x15, [])], [(3, 0x0102, [7])])
    dash = timeline(16, [(0, 52, 0x11B, [122]), (16, 20, 0x11B, [118])])
    weap = timeline(43, [(0, 62, 0x11B, [64])])
    data = objx([idle, dash, weap])
    out, done, bad, owners = h.patch_objx(data, [118, 122])
    assert (done, bad, owners) == (2, [], [16]), (done, bad, owners)
    extra, ents = h.arc_read(out)
    assert [e[1] for e in ents] == [b'p_se', b'comb']                       # other entries survive
    ver, blobs = h.events_split(ents[0][2])
    assert ver == 3 and len(blobs) == 3 and blobs[1] == dash                 # the dash timeline is untouched
    for i in (0, 2):
        pre, evs = h.ranged_events(blobs[i])
        assert [e for e in evs if e[0] == 0x11C] == [(0x11C, 118), (0x11C, 122)], evs
        hd = struct.unpack_from('<8H', blobs[i], 0)
        assert hd[3] == len(blobs[i]) and hd[2] <= hd[3]                      # size and keyed offset follow the insert
    assert blobs[0].endswith(idle[-12:])                                      # keyed events kept
    assert h.ranged_events(blobs[2])[1][0] == (0x11B, 64)                     # original events kept first
    # a motion that is skipped, and a timeline that does not decode
    out2, done2, _, _ = h.patch_objx(data, [118], skip={0})
    assert done2 == 1
    assert h.parse_ranges('118-120,125') == [118, 119, 120, 125]
    print('ok')


if __name__ == '__main__':
    main()
