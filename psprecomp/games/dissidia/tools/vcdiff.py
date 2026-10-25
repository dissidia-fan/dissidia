#!/usr/bin/env python3
"""Minimal VCDIFF (RFC 3284 / xdelta3) decoder that works WITHOUT the source file.

    vcdiff.py PATCH.xdelta [--json OUT.json]

A delta applied to a source file S gives a target T. Most of T is copied from S unchanged; what the patch
actually *adds* is spelled out in the delta itself (ADD / RUN instructions).  Without S we can therefore
recover every byte the patch wrote, and where, but not the bytes it left alone:

    decode(patch) -> Delta(target_size, ranges=[(target_offset, bytes), ...], moved=[...], notes=[...])

* `ranges`  literal bytes written by ADD/RUN (and by COPYs out of earlier literal bytes of the same window),
            merged when adjacent, sorted by target offset.  Positions the patch writes with bytes that happen
            to equal the source are included (a delta cannot tell), so a range may be a no-op.
* `moved`   COPY instructions out of the source at an address different from the target position
            (source_offset, target_offset, length).  A rebalance patch should not have any; if it does, the
            ranges alone do not describe the change and the caller must not trust them.
* identity COPYs (source address == target position) are the "unchanged" part and are not reported.

Supported: the default code table, SELF/HERE/NEAR/SAME address modes, VCD_SOURCE windows, the xdelta3
adler32 flag and application header. Not supported (raises ValueError): secondary compression, custom code
tables, VCD_TARGET windows, delta-compressed sections.  xdelta3 -S none output (what DeltaPatcher writes by
default) uses none of those.

Gotcha worth remembering: the SAME address modes (6, 7, 8) take ONE byte from the address stream, not a
varint; SELF/HERE/NEAR take a varint.
"""
import json
import struct
import sys
from dataclasses import dataclass, field

RUN, ADD, COPY = 1, 2, 3        # instruction types (0 = NOOP)
NEAR_SIZE, SAME_SIZE = 4, 3


def default_code_table():
    """RFC 3284 section 5.6: 256 entries (type1, size1, mode1, type2, size2, mode2)."""
    t = [(RUN, 0, 0, 0, 0, 0)]
    t += [(ADD, s, 0, 0, 0, 0) for s in range(18)]                       # 1..18
    for mode in range(9):                                                # 19..162
        t += [(COPY, s, mode, 0, 0, 0) for s in [0] + list(range(4, 19))]
    for mode in range(6):                                                # 163..234
        for a in range(1, 5):
            for c in (4, 5, 6):
                t.append((ADD, a, 0, COPY, c, mode))
    for mode in range(6, 9):                                             # 235..246
        for a in range(1, 5):
            t.append((ADD, a, 0, COPY, 4, mode))
    t += [(COPY, 4, mode, ADD, 1, 0) for mode in range(9)]               # 247..255
    assert len(t) == 256
    return t


CODE_TABLE = default_code_table()


class _Reader:
    def __init__(self, b):
        self.b, self.p = b, 0

    def byte(self):
        v = self.b[self.p]
        self.p += 1
        return v

    def varint(self):
        v = 0
        while True:
            c = self.byte()
            v = (v << 7) | (c & 0x7F)
            if not c & 0x80:
                return v

    def take(self, n):
        v = self.b[self.p:self.p + n]
        if len(v) != n:
            raise ValueError('truncated delta')
        self.p += n
        return v

    def eof(self):
        return self.p >= len(self.b)


@dataclass
class Delta:
    target_size: int = 0
    ranges: list = field(default_factory=list)       # [(target_offset, bytes)]
    moved: list = field(default_factory=list)        # [(source_offset, target_offset, length)]
    notes: list = field(default_factory=list)
    windows: int = 0
    app_header: bytes = b''


class _Window:
    """The target window as a list of segments: ('lit', bytes) | ('src', source_offset, length)."""

    def __init__(self):
        self.segs, self.n = [], 0

    def add_lit(self, b):
        if b:
            self.segs.append(('lit', bytes(b)))
            self.n += len(b)

    def add_src(self, off, ln):
        if ln:
            self.segs.append(('src', off, ln))
            self.n += ln

    def slice(self, start, ln):
        out, pos = [], 0
        for s in self.segs:
            sl = len(s[1]) if s[0] == 'lit' else s[2]
            lo, hi = max(start, pos), min(start + ln, pos + sl)
            if lo < hi:
                out.append(('lit', s[1][lo - pos:hi - pos]) if s[0] == 'lit' else ('src', s[1] + lo - pos, hi - lo))
            pos += sl
        return out

    def copy_self(self, addr, ln):
        # overlapping copies repeat the bytes written so far, like memmove byte by byte
        while ln:
            chunk = min(ln, self.n - addr)
            if chunk <= 0:
                raise ValueError('COPY from beyond the written window')
            for s in self.slice(addr, chunk):
                if s[0] == 'lit':
                    self.add_lit(s[1])
                else:
                    self.add_src(s[1], s[2])
            addr += chunk
            ln -= chunk


def decode(patch: bytes) -> Delta:
    r = _Reader(patch)
    if r.take(3) != b'\xd6\xc3\xc4':
        raise ValueError('not a VCDIFF file')
    r.byte()                                            # version
    hind = r.byte()
    if hind & 1:
        raise ValueError('secondary compression is not supported')
    if hind & 2:
        raise ValueError('custom code table is not supported')
    d = Delta()
    if hind & 4:
        d.app_header = r.take(r.varint())
    tpos = 0
    while not r.eof():
        wind = r.byte()
        if wind & 2:
            raise ValueError('VCD_TARGET windows are not supported')
        src_len = src_pos = 0
        if wind & 1:
            src_len, src_pos = r.varint(), r.varint()
        dl = r.varint()
        body = _Reader(r.take(dl))
        tlen = body.varint()
        dind = body.byte()
        if dind:
            raise ValueError('delta-compressed sections are not supported')
        n_add, n_inst, n_addr = body.varint(), body.varint(), body.varint()
        if wind & 4:
            body.take(4)                                # adler32 of the target window
        add = _Reader(body.take(n_add))
        inst = _Reader(body.take(n_inst))
        addr = _Reader(body.take(n_addr))
        near, same, slot = [0] * NEAR_SIZE, [0] * (SAME_SIZE * 256), 0
        w = _Window()

        def do_copy(size, mode):
            nonlocal slot
            here = src_len + w.n
            if mode == 0:
                a = addr.varint()
            elif mode == 1:
                a = here - addr.varint()
            elif mode < 2 + NEAR_SIZE:
                a = near[mode - 2] + addr.varint()
            else:
                a = same[(mode - 2 - NEAR_SIZE) * 256 + addr.byte()]       # SAME: one byte
            near[slot] = a
            slot = (slot + 1) % NEAR_SIZE
            same[a % (SAME_SIZE * 256)] = a
            if a < src_len:
                if a + size > src_len:
                    raise ValueError('COPY crosses the end of the source segment')
                w.add_src(src_pos + a, size)
            else:
                w.copy_self(a - src_len, size)

        while not inst.eof():
            code = CODE_TABLE[inst.byte()]
            for k in (0, 3):
                typ, size, mode = code[k:k + 3]
                if not typ:
                    continue
                if size == 0:
                    size = inst.varint()
                if typ == ADD:
                    w.add_lit(add.take(size))
                elif typ == RUN:
                    w.add_lit(add.take(1) * size)
                else:
                    do_copy(size, mode)
        if w.n != tlen:
            raise ValueError('window decoded to %d bytes, header says %d' % (w.n, tlen))
        pos = tpos
        for s in w.segs:
            if s[0] == 'lit':
                d.ranges.append((pos, s[1]))
                pos += len(s[1])
            else:
                if s[1] != pos:
                    d.moved.append((s[1], pos, s[2]))
                pos += s[2]
        tpos += tlen
        d.windows += 1
    d.target_size = tpos
    # merge adjacent literal runs
    merged = []
    for off, b in d.ranges:
        if merged and merged[-1][0] + len(merged[-1][1]) == off:
            merged[-1] = (merged[-1][0], merged[-1][1] + b)
        else:
            merged.append((off, b))
    d.ranges = merged
    return d


def main(argv):
    if not argv:
        print(__doc__)
        return 2
    d = decode(open(argv[0], 'rb').read())
    print('%d window(s), target %d B, %d literal ranges (%d B), %d relocated copies'
          % (d.windows, d.target_size, len(d.ranges), sum(len(b) for _, b in d.ranges), len(d.moved)))
    if '--json' in argv:
        out = argv[argv.index('--json') + 1]
        json.dump({'target_size': d.target_size, 'ranges': [[o, b.hex()] for o, b in d.ranges],
                   'moved': d.moved}, open(out, 'w'), indent=0)
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
