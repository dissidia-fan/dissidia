#!/usr/bin/env python3
"""Self-test for setup_from_iso.py: builds a tiny synthetic ISO 9660 image (no game data) and checks
directory parsing, nested paths, ;1 suffixes, multi-sector directories, streaming extraction and the
ELF helpers. Run: python3 test_setup_from_iso.py"""
import hashlib, os, struct, sys, tempfile
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import setup_from_iso as S

SEC = 2048

def rec(name, extent, size, is_dir):
    nb = name if isinstance(name, bytes) else name.encode()
    ln = 33 + len(nb); ln += ln & 1
    r = bytearray(ln)
    r[0] = ln; struct.pack_into('<I', r, 2, extent); struct.pack_into('>I', r, 6, extent)
    struct.pack_into('<I', r, 10, size); struct.pack_into('>I', r, 14, size)
    r[25] = 2 if is_dir else 0; r[32] = len(nb); r[33:33 + len(nb)] = nb
    return bytes(r)

def build_iso(tree):
    """tree: nested dict, bytes values are files. Returns ISO bytes."""
    sectors = {}          # sector -> bytes
    nxt = [17 + 1]        # 16 PVD, 17 terminator
    def alloc(n): s = nxt[0]; nxt[0] += n; return s
    # simple recursive builder
    def build(d):
        items = []
        for name, v in d.items():
            if isinstance(v, dict): items.append((name, build(v), True))
            else:
                n = max(1, (len(v) + SEC - 1) // SEC); e = alloc(n)
                for i in range(n): sectors[e + i] = v[i * SEC:(i + 1) * SEC].ljust(SEC, b'\0')
                items.append((name, (e, len(v)), False))
        # directory extent
        recs_est = sum(len(rec(n + ('' if isdir else ';1'), 0, 0, isdir)) for n, _, isdir in items) + 68
        nsec = (recs_est + SEC - 1) // SEC + 0
        ext = alloc(nsec)
        buf = bytearray(); cur = bytearray()
        def put(r):
            nonlocal cur
            if len(cur) + len(r) > SEC:
                buf.extend(cur.ljust(SEC, b'\0')); cur = bytearray()
            cur.extend(r)
        put(rec(b'\x00', ext, nsec * SEC, True)); put(rec(b'\x01', ext, nsec * SEC, True))
        for n, info, isdir in items:
            put(rec(n if isdir else n + ';1', info[0], info[1], isdir))
        buf.extend(cur.ljust(SEC, b'\0'))
        buf = bytes(buf).ljust(nsec * SEC, b'\0')
        assert len(buf) == nsec * SEC, (len(buf), nsec)
        for i in range(nsec): sectors[ext + i] = buf[i * SEC:(i + 1) * SEC]
        return (ext, nsec * SEC)
    root = build(tree)
    pvd = bytearray(SEC)
    pvd[0] = 1; pvd[1:6] = b'CD001'; pvd[6] = 1
    pvd[40:72] = b'TESTDISC'.ljust(32)
    struct.pack_into('<H', pvd, 128, SEC)
    pvd[156:190] = rec(b'\x00', root[0], root[1], True)[:34].ljust(34, b'\0')
    pvd[156] = 34
    term = bytearray(SEC); term[0] = 255; term[1:6] = b'CD001'
    total = max(sectors) + 1
    out = bytearray(total * SEC)
    out[16 * SEC:17 * SEC] = pvd; out[17 * SEC:18 * SEC] = term
    for s, b in sectors.items(): out[s * SEC:(s + 1) * SEC] = b
    return bytes(out)

def fake_elf(etype, vaddr, body, filesz_extra=0, memsz_extra=0):
    hdr = bytearray(0x54)
    hdr[0:4] = b'\x7fELF'; hdr[4:7] = bytes([1, 1, 1])
    struct.pack_into('<HH', hdr, 16, etype, 8)
    struct.pack_into('<I', hdr, 28, 0x34)
    struct.pack_into('<HHH', hdr, 40, 0x34, 0x20, 1)
    struct.pack_into('<8I', hdr, 0x34, 1, 0x54, vaddr, vaddr, len(body), len(body) + memsz_extra, 7, 4)
    return bytes(hdr) + body

def main():
    big = os.urandom(5 * SEC + 123)
    ovl_body = struct.pack('<I', 2) + b'MENU' * 10
    files = {
        'PSP_GAME': {
            'PARAM.SFO': b'sfo',
            'SYSDIR': {'EBOOT.BIN': b'~PSP' + b'\0' * 100},
            'USRDIR': {'DATA': {'PACKAGE.BIN': big, 'OVL_MENU_APP.ELF': fake_elf(0xffa8, S.OVL_BASE, ovl_body),
                                'MODULE': {'LIBFONT.PRX': b'prx'},
                                'BGM': {('B%03d.AT3' % i): b'x' * (i + 1) for i in range(60)}},
                       'MOVIE': {'OP.PMF': b'mov'}}}}
    iso = build_iso(files)
    d = tempfile.mkdtemp(prefix='isotest_')
    p = os.path.join(d, 't.iso'); open(p, 'wb').write(iso)
    I = S.Iso9660(p)
    assert I.volume_id == 'TESTDISC'
    r = I.find('psp_game/usrdir/data/package.bin'); assert r and r['size'] == len(big)
    assert I.find('PSP_GAME/NOPE') is None
    assert len(list(I.walk(I.find('PSP_GAME/USRDIR/DATA/BGM')))) == 60   # multi-sector directory
    out = os.path.join(d, 'w')
    res = S.extract(I, out, False)
    got = open(os.path.join(out, 'disc0/PSP_GAME/USRDIR/DATA/PACKAGE.BIN'), 'rb').read()
    assert got == big and res['package_md5'] == hashlib.md5(big).hexdigest()
    assert res['bgm_count'] == 60 and res['movie_count'] == 1
    assert open(os.path.join(out, 'EBOOT.enc.bin'), 'rb').read(4) == b'~PSP'
    assert os.path.isfile(os.path.join(out, 'disc0/PSP_GAME/USRDIR/MOVIE/OP.PMF'))
    assert 'DATA/OVL_BATTLE_APP.ELF' in res['missing']
    res2 = S.extract(I, out, False); assert res2['files'] == 0     # resume skips existing
    I.close()
    # ELF helpers
    e = fake_elf(0xffa0, 0x08804040, b'ABCD', memsz_extra=16)
    img = S.build_eboot_image(e); assert len(img) == S.OVL_BASE - S.EBOOT_BASE and img[0x40:0x44] == b'ABCD'
    e0 = fake_elf(0xffa0, 0x40, b'WXYZ')                              # PRX linked at 0
    assert S.build_eboot_image(e0)[0x40:0x44] == b'WXYZ'
    assert S.overlay_blob(fake_elf(0xffa8, S.OVL_BASE, ovl_body)) == ovl_body
    try: S.Iso9660(__file__); assert False
    except S.IsoError: pass
    more_tests(iso, d)
    print('ok')


def make_cso(iso_bytes, block=2048, align=0, level=9):
    import zlib
    n = (len(iso_bytes) + block - 1) // block
    blocks, idx, pos = [], [], 0
    hdr_len = 24 + 4 * (n + 1)
    pos = hdr_len
    for i in range(n):
        chunk = iso_bytes[i * block:(i + 1) * block].ljust(block, b'\0')
        c = zlib.compressobj(level, zlib.DEFLATED, -15)
        z = c.compress(chunk) + c.flush()
        if len(z) >= len(chunk) or i % 7 == 3:          # store some blocks plain
            data, flag = chunk, 0x80000000
        else:
            data, flag = z, 0
        pad = (-pos) % (1 << align)
        blocks.append(b'\0' * pad + data)
        pos += pad
        idx.append((pos >> align) | flag)
        pos += len(data)
    idx.append(pos >> align)
    hdr = b'CISO' + struct.pack('<IQIBB2x', 24, len(iso_bytes), block, 1, align)
    return hdr + struct.pack('<%dI' % len(idx), *idx) + b''.join(blocks)


def expect_error(fn, needle):
    try:
        fn()
    except S.IsoError as e:
        assert needle in str(e), (needle, str(e))
        return
    raise AssertionError('no IsoError containing %r' % needle)


def more_tests(iso_bytes, d):
    import shutil
    # --- CSO: transparent read, both alignments, extraction identical to the plain ISO
    for align in (0, 2):
        p = os.path.join(d, 'a%d.cso' % align)
        open(p, 'wb').write(make_cso(iso_bytes, align=align))
        c = S.CsoFile(p)
        assert c.total == len(iso_bytes)
        c.seek(5000); assert c.read(3000) == iso_bytes[5000:8000]
        c.seek(0); assert c.read() == iso_bytes
        c.close()
        I = S.Iso9660(p)
        assert I.volume_id == 'TESTDISC'
        out = os.path.join(d, 'wcso%d' % align)
        res = S.extract(I, out, False); I.close()
        assert res['bgm_count'] == 60 and res['movie_count'] == 1 and res['disc_id'] is None
    # --- unsupported containers give actionable errors
    for magic, needle in ((b'ZISO', 'maxcso'), (b'DAX\0', 'DAX'), (b'\0PBP', 'PBP')):
        p = os.path.join(d, 'bad.img'); open(p, 'wb').write(magic + b'\0' * 4096)
        expect_error(lambda: S.open_image(p), needle)
    raw = bytearray(40 * 2352)
    raw[16 * 2352 + 16 + 1:16 * 2352 + 16 + 6] = b'CD001'
    p = os.path.join(d, 'raw.bin'); open(p, 'wb').write(raw)
    expect_error(lambda: S.open_image(p), '2352')
    hdr = bytearray(make_cso(iso_bytes)[:24]); hdr[20] = 2        # CSO v2
    p = os.path.join(d, 'v2.cso'); open(p, 'wb').write(bytes(hdr) + b'\0' * 4096)
    expect_error(lambda: S.CsoFile(p), 'version 2')
    # --- PARAM.SFO
    keys = b'DISC_ID\0TITLE\0PARENTAL_LEVEL\0'
    ents = [(0, 0x0204, b'ULES01505\0\0\0'), (8, 0x0204, b'DISSIDIA\0'), (14, 0x0404, struct.pack('<I', 3))]
    kt = 20 + 16 * len(ents); dt = kt + len(keys); dt += (-dt) % 4
    sfo = bytearray(b'\0PSF\x01\x01\0\0' + struct.pack('<III', kt, dt, len(ents)))
    off = 0
    for ko, fmt, val in ents:
        sfo += struct.pack('<HHIII', ko, fmt, len(val), len(val), off); off += len(val)
    sfo += keys; sfo = sfo.ljust(dt, b'\0') + b''.join(v for _, _, v in ents)
    info = S.parse_sfo(bytes(sfo))
    assert info == {'DISC_ID': 'ULES01505', 'TITLE': 'DISSIDIA', 'PARENTAL_LEVEL': 3}, info
    assert S.parse_sfo(b'junk') == {}
    # --- disc id note: EU PSN image silent, US UMD known to work, anything else warned about
    assert S.disc_note('ULES01505') == ''
    assert 'US UMD' in S.disc_note('ULUS10566') and 'NOT' not in S.disc_note('ULUS10566')
    assert 'NOT the EU PSN image' in S.disc_note('NPJH50377')
    # --- disk space check
    real = shutil.disk_usage
    shutil.disk_usage = lambda p: shutil._ntuple_diskusage(100, 100, 1000)
    try:
        I = S.Iso9660(os.path.join(d, 't.iso'))
        expect_error(lambda: S.extract(I, os.path.join(d, 'wfull'), False), 'disk space')
        I.close()
    finally:
        shutil.disk_usage = real
    # --- EBOOT handling: plain ELF on disc needs no pspdecrypt; --decrypted-eboot; garbage is rejected
    w = os.path.join(d, 'wpost'); os.makedirs(os.path.join(w, 'disc0/PSP_GAME/USRDIR/DATA'))
    elf = fake_elf(2, S.EBOOT_BASE + 0x40, b'CODE' * 8)
    open(os.path.join(w, 'EBOOT.enc.bin'), 'wb').write(elf)
    def boom(): raise AssertionError('pspdecrypt must not be needed')
    facts = S.postprocess(w, boom)
    assert open(os.path.join(w, 'EBOOT.dec.elf'), 'rb').read() == elf
    assert open(os.path.join(w, 'disc0/PSP_GAME/SYSDIR/BOOT.BIN'), 'rb').read() == elf
    assert facts['eboot_image'] == S.OVL_BASE - S.EBOOT_BASE
    open(os.path.join(w, 'EBOOT.enc.bin'), 'wb').write(b'~PSP' + b'\0' * 64)
    S.postprocess(w, boom, os.path.join(w, 'EBOOT.dec.elf'))
    try: S.postprocess(w, boom, os.path.join(d, 't.iso')); assert False
    except RuntimeError: pass
    # --- end to end through main() with a CSO and an already-plain EBOOT
    tree = {'PSP_GAME': {'SYSDIR': {'EBOOT.BIN': elf},
                         'USRDIR': {'DATA': {'PACKAGE.BIN': b'p' * 5000,
                                             'OVL_MENU_APP.ELF': fake_elf(0xffa8, S.OVL_BASE, struct.pack('<I', 2) + b'x' * 20),
                                             'MOVIE': {'A.PMF': b'm'}}}}}
    p = os.path.join(d, 'e2e.cso'); open(p, 'wb').write(make_cso(build_iso(tree), align=1))
    assert S.main([p, os.path.join(d, 'we2e')]) == 0
    assert os.path.isdir(os.path.join(d, 'we2e/ms0/PSP/SAVEDATA'))
    assert os.path.isfile(os.path.join(d, 'we2e/OVL_MENU_APP.bin'))
    assert S.main([os.path.join(d, 'nope.iso'), os.path.join(d, 'x')]) == 2


if __name__ == '__main__': main()
