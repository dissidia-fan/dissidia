#!/usr/bin/env python3
"""Self-test for vcdiff.py and port_xdelta_mod.py on synthetic data (no game files).
Run: python3 -I test_port_xdelta_mod.py"""
import os
import random
import struct
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import package_extract as pe  # noqa: E402
import port_xdelta_mod as P  # noqa: E402
import vcdiff  # noqa: E402


# ---------------------------------------------------------------------------------------------
# a tiny VCDIFF encoder (explicit sizes, default code table) to feed the decoder

def varint(v):
    out = [v & 0x7F]
    v >>= 7
    while v:
        out.append(0x80 | (v & 0x7F))
        v >>= 7
    return bytes(reversed(out))


class Enc:
    """ops: ('add', bytes) | ('run', byte, n) | ('copy', addr, size, mode); addr is the absolute address in the
    source+target window space. Mirrors the RFC 3284 address caches."""

    def __init__(self, src_len):
        self.src_len, self.tlen = src_len, 0
        self.data, self.inst, self.addr = b'', b'', b''
        self.near, self.same, self.slot = [0] * 4, [0] * 768, 0

    def add(self, b):
        self.inst += b'\x01' + varint(len(b))
        self.data += b
        self.tlen += len(b)

    def run(self, byte, n):
        self.inst += b'\x00' + varint(n)
        self.data += bytes([byte])
        self.tlen += n

    def copy(self, a, size, mode):
        here = self.src_len + self.tlen
        if mode == 'same':                   # SAME bank = (address % 768) // 256 selects mode 6, 7 or 8
            assert self.same[a % 768] == a, 'SAME needs a cached address'
            mode = 6 + (a % 768) // 256
        if mode == 0:
            self.addr += varint(a)
        elif mode == 1:
            self.addr += varint(here - a)
        elif mode < 6:
            self.addr += varint(a - self.near[mode - 2])
        else:
            self.addr += bytes([a % 256])    # one byte, not a varint
        self.near[self.slot] = a
        self.slot = (self.slot + 1) % 4
        self.same[a % 768] = a
        self.inst += bytes([19 + 16 * mode]) + varint(size)
        self.tlen += size

    def finish(self, src_pos=0):
        body = varint(self.tlen) + b'\x00' + varint(len(self.data)) + varint(len(self.inst)) + varint(len(self.addr))
        body += self.data + self.inst + self.addr
        win = b'\x01' + varint(self.src_len) + varint(src_pos) + varint(len(body)) + body
        return b'\xd6\xc3\xc4\x00\x00' + win


def test_vcdiff():
    # source is 1000 bytes; the target keeps [0,100) (identity), ADDs 3 bytes, RUNs 5, copies source[500:520]
    # (relocated), copies from its own window (HERE), then re-uses a cached address (SAME, one byte)
    e = Enc(1000)
    e.copy(0, 100, 0)
    e.add(b'ABC')
    e.run(0x7F, 5)
    e.copy(500, 20, 0)                       # relocated: target 108, source 500
    e.copy(1000 + 100, 4, 1)                 # from the target window: bytes 100..103 = 'ABC\x7f'
    e.copy(500, 20, 'same')                  # SAME cache hit (mode 7, one address byte: 500 % 256 = 244)
    d = vcdiff.decode(e.finish())
    assert d.target_size == 100 + 3 + 5 + 20 + 4 + 20, d.target_size
    assert d.ranges == [(100, b'ABC' + b'\x7f' * 5), (128, b'ABC\x7f')], d.ranges
    assert d.moved == [(500, 108, 20), (500, 132, 20)], d.moved
    # the code table is the RFC one: spot check the double instructions
    t = vcdiff.CODE_TABLE
    assert t[163] == (vcdiff.ADD, 1, 0, vcdiff.COPY, 4, 0) and t[247] == (vcdiff.COPY, 4, 0, vcdiff.ADD, 1, 0)
    assert t[19] == (vcdiff.COPY, 0, 0, 0, 0, 0) and t[20][1] == 4 and t[234] == (vcdiff.ADD, 4, 0, vcdiff.COPY, 6, 5)
    # secondary compression is refused, not misread
    try:
        vcdiff.decode(b'\xd6\xc3\xc4\x00\x01\x01')
        raise AssertionError('should refuse')
    except ValueError:
        pass


# ---------------------------------------------------------------------------------------------
# a synthetic package

def timeline(motion, ranged):
    pre = bytes(4)
    r = b''.join(struct.pack('<HHHHI', s, e, op, 0, 0) + struct.pack('<%dI' % len(a), *a) for s, e, op, a in ranged)
    off_k = 16 + len(pre) + len(r)
    return struct.pack('<8H', motion, len(ranged) + 1, off_k, off_k, 0, 0, 0, 0) + pre + r


def events(blobs):
    o = 8 + 4 * len(blobs)
    offs = []
    for b in blobs:
        offs.append(o)
        o += len(b)
    return struct.pack('<II', 3, len(blobs)) + struct.pack('<%dI' % len(blobs), *offs) + b''.join(blobs)


def arc(secs):
    n = len(secs)
    o = 16 + 16 * n
    table, body = b'', b''
    for t, tag, data in secs:
        pad = (-o) % 4
        body += b'\0' * pad
        o += pad
        table += struct.pack('<I4sII', t, tag, o, len(data))
        body += data
        o += len(data)
    return b'ARC\x01' + struct.pack('<I', n) + bytes(8) + table + body


def plya(recs):
    out = struct.pack('<II', 0x16, len(recs))
    for act, script, name in recs:
        r = bytearray(116)
        struct.pack_into('<HH', r, 0, act, 0)
        struct.pack_into('<H', r, 0x1C, script)
        r[100:100 + len(name)] = name.encode()
        out += bytes(r)
    return out


def atkp(recs):
    out = struct.pack('<II', 12, len(recs))
    for name, b4 in recs:
        r = bytearray(68)
        r[4] = b4
        r[52:52 + len(name)] = name.encode()
        out += bytes(r)
    return out


def objx(seed=1):
    ev = events([timeline(0, [(0, 0xFFFF, 0x15, [])]),
                 timeline(1, [(40, 0xFFFF, 0x120, [0x14B]), (0, 52, 0x402, [1, 2, 3, 4])])])
    return arc([(1, b'plya', plya([(0x10, 0x2710, 'LAND_A'), (0x11, 0x2711, 'LAND_B')])),
                (1, b'atkp', atkp([('HIT_A', 0), ('HIT_B', 0)])),
                (8, b'p_se', ev),
                (1, b'rand', random.Random(seed).randbytes(4096))])


def mpk_main():
    ablt = struct.pack('<4I', 0x49424241, 0x20100611, 3, 0) + b''.join(
        bytes(16) + struct.pack('<H', cp) + bytes(22) for cp in (15, 30, 15))
    item = struct.pack('<4I', 0x4D455449, 0x20100609, 2, 0) + b''.join(
        bytes(24) + struct.pack('<I', m) + bytes(76) for m in (1, 2))
    sysb = arc([(1, b'ablt', ablt), (1, b'item', item)])
    return mpk([('battle.bin', b'xx'), ('system.bin', sysb)])


def mpk(ents):
    n = len(ents)
    names = b''.join(e[0].encode() + b'\0' for e in ents)
    name_base = 0x10 + 16 * n
    data_base = (name_base + len(names) + 15) & ~15
    table, pos, body = b'', name_base, b''
    for nm, data in ents:
        o = data_base + len(body)
        o = (o + 15) & ~15
        body += b'\0' * (o - data_base - len(body))
        table += struct.pack('<4I', pos, o, len(data), 0)
        pos += len(nm) + 1
        body += data
    out = b'MPK \x06\x06\x07\x20' + struct.pack('<II', n, 0) + table + names
    return out.ljust(data_base, b'\0') + body


def build_package(d, files, pad_sectors=3):
    """files: [(path, bytes)] -> PACKAGE.BIN + PACKAGE_INFO.BIN in d; returns {path: package offset}"""
    pkg, idx, offs = bytearray(b'\xAA' * 2048 * pad_sectors), b'', {}
    for path, data in files:
        sector = len(pkg) // 2048
        offs[path] = sector * 2048
        idx += struct.pack('<III', pe.package_crc(path), sector, len(data))
        pkg += data + b'\0' * ((-len(data)) % 2048)
    hdr = struct.pack('<I4sII', 0, b'pack', len(files), 0)
    open(os.path.join(d, 'PACKAGE.BIN'), 'wb').write(pkg)
    open(os.path.join(d, 'PACKAGE_INFO.BIN'), 'wb').write(hdr + idx)
    return offs


def test_port():
    shift = 0x40000
    with tempfile.TemporaryDirectory() as tmp:
        data = os.path.join(tmp, 'DATA')
        os.makedirs(data)
        a, b = objx(1), objx(2)
        offs = build_package(data, [('obj/p_sev110.objx', a), ('obj/p_fiv100.objx', b),
                                    ('general_archive/main/main.bin', mpk_main())])
        pkg = P.Pkg(data)
        secs = {t: (o, s) for _t, t, o, s in P.arc_sections(a)}
        base = shift + offs['obj/p_sev110.objx']
        # (1) literal: plya record 1 script 0x2711 -> 0x2799; (2) literal: event start 40 -> 0xF028 (timeline 1)
        # (3) relocated copy: atkp record 1 byte +4 takes the value of atkp record 0's neighbouring byte, i.e. a COPY
        #     from the package that differs from the target bytes; (4) a literal that equals the EU byte (no-op)
        plya_o = secs[b'plya'][0]
        atkp_o = secs[b'atkp'][0]
        ev_o = secs[b'p_se'][0]
        ev_base = ev_o + struct.unpack_from('<I', a, ev_o + 8 + 4)[0]       # timeline 1
        pr = ev_base + 16 + 4                                                # its first ranged event
        delta = vcdiff.Delta()
        delta.ranges = [(base + plya_o + 8 + 116 + 0x1C, b'\x99\x27'),
                        (base + pr + 1, b'\xF0'),
                        (base + atkp_o + 8, a[atkp_o + 8:atkp_o + 10])]     # no-op
        src_pos = base + atkp_o + 8 + 68 + 4                                 # atkp record 1 +4 (value 0)
        # source: the plya actionId low byte of record 1 (0x11); target: atkp record 0 +4
        delta.moved = [(base + plya_o + 8 + 116, base + atkp_o + 8 + 4, 1)]
        # --- apply
        mods = os.path.join(tmp, 'mods')
        res, unres, stray = P.port(pkg, delta, shift, mods, log=lambda *x: None)
        assert not unres and not stray
        assert [r['label'] for r in res] == ['obj/p_sev110.objx'], [r['label'] for r in res]
        out = open(os.path.join(mods, 'obj', 'p_sev110.objx'), 'rb').read()
        assert len(out) == len(a)
        diff = [i for i in range(len(a)) if a[i] != out[i]]
        assert diff == sorted([plya_o + 8 + 116 + 0x1C, pr + 1, atkp_o + 8 + 4]), diff   # 0x2711 -> 0x2799 changes one byte
        assert out[atkp_o + 8 + 4] == 0x11                                   # byte copied from the source position
        assert out[pr + 1] == 0xF0
        rows = '\n'.join(t for _s, t in res[0]['rows'])
        assert 'scriptId: 0x2711 -> 0x2799' in rows, rows
        assert 'ranged event op 0x0120' in rows and 'never fires' in rows, rows
        assert 'atkp #0 HIT_A: +4 00 -> 11' in rows, rows
        assert not os.path.exists(os.path.join(mods, 'obj', 'p_fiv100.objx'))   # unchanged entries are not written
        # --- the shift search finds the shift from literal bytes that agree with the package
        delta2 = vcdiff.Delta()
        ro = secs[b'rand'][0]
        delta2.ranges = [(base + ro, a[ro:ro + 1500]), (base + pr + 1, b'\xF0')]
        assert P.find_shift(delta2, pkg, log=lambda *x: None) == shift
        # --- ranges outside every entry are counted, not applied
        delta3 = vcdiff.Delta()
        delta3.ranges = [(shift + 10, b'\x01\x02')]
        _res, _u, stray3 = P.port(pkg, delta3, shift, os.path.join(tmp, 'm3'), dry=True, log=lambda *x: None)
        assert stray3 == 2
        # --- literal-only mode ignores the COPY
        res4, _u, _s = P.port(pkg, delta, shift, os.path.join(tmp, 'm4'), dry=True, log=lambda *x: None, literal_only=True)
        assert all('atkp' not in t for _s, t in res4[0]['rows'])
        # --- JSON round trip of the decoded list
        pj = os.path.join(tmp, 'd.json')
        P.dump_delta_json(delta, pj)
        d5 = P.load_delta(pj)
        assert d5.ranges == delta.ranges and [tuple(m) for m in d5.moved] == delta.moved


def test_main_bin():
    old = mpk_main()
    sysb = next(e for e in P.mpk_entries(old) if e[0] == 'system.bin')
    tags = {t: so for _t, t, so, _s in P.arc_sections(old[sysb[1]:sysb[1] + sysb[2]])}
    new = bytearray(old)
    cp = sysb[1] + tags[b'ablt'] + 16 + 40 * 2 + 16          # ablt record 2, +16 CP cost 15 -> 5
    assert new[cp] == 15
    new[cp] = 5
    mk = sysb[1] + tags[b'item'] + 16 + 104 * 1 + 24         # item record 1 mask -> 0xffbfffff
    new[mk:mk + 4] = struct.pack('<I', 0xFFBFFFFF)
    rows = [t for _s, t in P.describe_main(old, bytes(new))]
    assert any('ablt +16 CP cost: 15 -> 5 on 1 abilities' in r for r in rows), rows
    assert any('item +24' in r and '0xffbfffff' in r for r in rows), rows
    # a rebuilt main.bin (extra leading ARC section moves every table) takes the same edits by table position
    ablt = old[sysb[1] + tags[b'ablt']:sysb[1] + tags[b'ablt'] + 16 + 120]
    item = old[sysb[1] + tags[b'item']:sysb[1] + tags[b'item'] + 16 + 208]
    other = arc([(1, b'chrp', bytes(333)), (1, b'ablt', ablt), (1, b'item', item)])
    base = mpk([('battle.bin', b'xx'), ('system.bin', other)])
    got = P.rebase_main(old, bytes(new), base, log=lambda *x: None)
    bsys = next(e for e in P.mpk_entries(got) if e[0] == 'system.bin')
    btags = {t: so for _t, t, so, _s in P.arc_sections(got[bsys[1]:bsys[1] + bsys[2]])}
    assert got[bsys[1] + btags[b'ablt'] + 16 + 80 + 16] == 5
    assert struct.unpack_from('<I', got, bsys[1] + btags[b'item'] + 16 + 104 + 24)[0] == 0xFFBFFFFF
    assert len(got) == len(base) and sum(1 for x, y in zip(got, base) if x != y) == 5   # 1 CP byte + 4 mask bytes


def test_verify():
    a = objx()
    secs = {t: (o, s) for _t, t, o, s in P.arc_sections(a)}
    ok = bytearray(a)
    ok[secs[b'plya'][0] + 8 + 0x1C] ^= 1
    assert P.verify_objx(a, bytes(ok), 'x') == []
    # breaking a timeline (op 0x120 -> 0x2120 makes the event 12 + 4*0x21 bytes long) is reported, not fatal
    bad = bytearray(a)
    ev_o = secs[b'p_se'][0]
    t1 = ev_o + struct.unpack_from('<I', a, ev_o + 12)[0]
    struct.pack_into('<H', bad, t1 + 16 + 4 + 4, 0x2120)
    w = P.verify_objx(a, bytes(bad), 'x')
    assert len(w) == 1 and 'motion) 1' in w[0], w
    # a changed container is fatal
    try:
        P.verify_objx(a, a + b'\0', 'x')
        raise AssertionError('should fail')
    except P.VerifyError:
        pass


if __name__ == '__main__':
    for t in (test_vcdiff, test_port, test_main_bin, test_verify):
        t()
        print('ok', t.__name__)
