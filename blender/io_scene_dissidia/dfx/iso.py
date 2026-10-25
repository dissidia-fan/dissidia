"""Read and patch the Dissidia 012 ISO (PACKAGE.BIN / PACKAGE_INFO.BIN) from Python.

PACKAGE_INFO.BIN: 16-byte header ('pack' at 4, u32 count at 8), then sorted 12-byte
entries {u32 hash, u32 sector, u32 size | flag<<28}. hash = BZip2-CRC32 of the
lowercase path. Files in PACKAGE.BIN are raw.
"""
import os, struct, zlib

SECTOR = 2048
_R8 = bytes(int(f'{i:08b}'[::-1], 2) for i in range(256))

ROSTER = [('one100', 'Warrior of Light'), ('one200', 'Garland'), ('two100', 'Firion'), ('two200', 'The Emperor'),
          ('thr100', 'Onion Knight'), ('thr200', 'Cloud of Darkness'), ('for100', 'Cecil'), ('for110', 'Kain'),
          ('for200', 'Golbez'), ('fiv100', 'Bartz'), ('fiv200', 'Exdeath'), ('fiv210', 'Gilgamesh'),
          ('six100', 'Terra'), ('six200', 'Kefka'), ('sev100', 'Cloud'), ('sev110', 'Tifa'), ('sev120', 'Aerith'),
          ('sev200', 'Sephiroth'), ('eht100', 'Squall'), ('eht110', 'Laguna'), ('eht200', 'Ultimecia'),
          ('nin100', 'Zidane'), ('nin200', 'Kuja'), ('ten100', 'Tidus'), ('ten110', 'Yuna'), ('ten200', 'Jecht'),
          ('gst100', 'Shantotto'), ('gst110', 'Prishe'), ('gst120', 'Vaan'), ('gst130', 'Lightning'),
          ('gst200', 'Gabranth'), ('org200', 'Chaos'), ('org210', 'Feral Chaos')]
COSTUMES = [('1p', 'Normal'), ('2p', 'Alternate 1'), ('3p', 'Alternate 2'), ('zako', 'Manikin'),
            ('1p_a', 'Assist'), ('2p_a', 'Assist (alt 1)')]


def dhash(path):
    c = zlib.crc32(path.lower().encode('ascii').translate(_R8))
    return int(f'{c:032b}'[::-1], 2)


def read_dir_records(f):
    f.seek(16 * SECTOR); pvd = f.read(SECTOR)
    out = []

    def walk(rec, path):
        lba, size = struct.unpack_from('<I', rec, 2)[0], struct.unpack_from('<I', rec, 10)[0]
        f.seek(lba * SECTOR); data = f.read(size); i = 0
        while i < len(data):
            l = data[i]
            if l == 0:
                i = (i // SECTOR + 1) * SECTOR; continue
            r = data[i:i + l]; name = r[33:33 + r[32]]
            if name not in (b'\0', b'\1'):
                nm = name.split(b';')[0].decode('latin1')
                out.append((lba * SECTOR + i, r, path + '/' + nm))
                if r[25] & 2:
                    walk(r, path + '/' + nm)
            i += l
    walk(pvd[156:190], '')
    return pvd, out


def rec_extent(r):
    return struct.unpack_from('<I', r, 2)[0], struct.unpack_from('<I', r, 10)[0]


class IsoPackage:
    """Read-only access to the game files inside an ISO."""

    def __init__(self, iso_path):
        self.path = iso_path
        self.f = open(iso_path, 'rb')
        _, recs = read_dir_records(self.f)
        by = {p: rec_extent(r) for _, r, p in recs}
        try:
            self.pkg_lba, _ = by['/PSP_GAME/USRDIR/DATA/PACKAGE.BIN']
            inf_lba, inf_size = by['/PSP_GAME/USRDIR/DATA/PACKAGE_INFO.BIN']
        except KeyError:
            raise ValueError('This ISO has no PSP_GAME/USRDIR/DATA/PACKAGE.BIN; is it Dissidia 012?')
        self.f.seek(inf_lba * SECTOR); info = self.f.read(inf_size)
        if info[4:8] != b'pack':
            raise ValueError('PACKAGE_INFO.BIN is not in the expected format')
        n = struct.unpack_from('<I', info, 8)[0]
        self.index = {}
        for i in range(n):
            h, sec, w = struct.unpack_from('<III', info, 16 + i * 12)
            self.index[h] = (sec, w & 0x0FFFFFFF)

    def has(self, name):
        return dhash(name) in self.index

    def read(self, name):
        sec, size = self.index[dhash(name)]
        self.f.seek(self.pkg_lba * SECTOR + sec * SECTOR)
        return self.f.read(size)

    def close(self):
        self.f.close()


def build_patched_iso(src, out, replace=None, add=None, alias=None, progress=None):
    """Copy `src` to `out` with package files replaced/added/aliased. Returns a log list.
    replace/add: {package_path: bytes}; alias: {new_path: existing_path}."""
    replace, add, alias = replace or {}, add or {}, alias or {}
    if os.path.abspath(src) == os.path.abspath(out):
        raise ValueError('refusing to overwrite the source ISO')
    s = open(src, 'rb')
    pvd, recs = read_dir_records(s)
    by_path = {p: (off, r) for off, r, p in recs}
    pkg_off, pkg_rec = by_path['/PSP_GAME/USRDIR/DATA/PACKAGE.BIN']
    inf_off, inf_rec = by_path['/PSP_GAME/USRDIR/DATA/PACKAGE_INFO.BIN']
    pkg_lba, pkg_size = rec_extent(pkg_rec)
    inf_lba, inf_size = rec_extent(inf_rec)
    files = sorted((rec_extent(r)[0], p) for _, r, p in recs if not r[25] & 2)
    after = [l for l, _ in files if l > pkg_lba]
    next_after_pkg = min(after) if after else None
    next_after_inf = min(l for l, _ in files if l > inf_lba)
    vol = struct.unpack_from('<I', pvd, 80)[0]
    s.seek(inf_lba * SECTOR); info = s.read(inf_size)
    n = struct.unpack_from('<I', info, 8)[0]
    ents = {}
    for i in range(n):
        h, sec, w = struct.unpack_from('<III', info, 16 + i * 12)
        ents[h] = [sec, w & 0x0FFFFFFF, w >> 28]
    pkg_sectors = (pkg_size + SECTOR - 1) // SECTOR
    append = bytearray(); log = []
    for kind, items in (('replace', replace), ('add', add)):
        for name, data in items.items():
            h = dhash(name)
            if kind == 'replace' and h not in ents:
                kind_eff = 'add'
            else:
                kind_eff = kind
            sec = pkg_sectors + len(append) // SECTOR
            flag = ents[h][2] if h in ents else 1
            ents[h] = [sec, len(data), flag]
            append += data + b'\0' * ((-len(data)) % SECTOR)
            log.append(f'{kind_eff} {name} ({len(data):,} bytes)')
    for new, old in alias.items():
        hn, ho = dhash(new), dhash(old)
        if ho in ents and hn not in ents:
            ents[hn] = list(ents[ho]); log.append(f'alias {new} -> {old}')
    new_pkg_size = pkg_sectors * SECTOR + len(append) if append else pkg_size
    new_pkg_sectors = (new_pkg_size + SECTOR - 1) // SECTOR
    shift = 0
    if next_after_pkg is not None and pkg_lba + new_pkg_sectors > next_after_pkg:
        shift = ((pkg_lba + new_pkg_sectors - next_after_pkg) + 15) // 16 * 16
    newinfo = bytearray(info[:16]) + b''.join(struct.pack('<III', h, ents[h][0], (ents[h][2] << 28) | ents[h][1]) for h in sorted(ents))
    struct.pack_into('<I', newinfo, 8, len(ents))
    if inf_lba + (len(newinfo) + SECTOR - 1) // SECTOR > next_after_inf:
        raise ValueError('PACKAGE_INFO.BIN would overrun the next file')
    total = os.path.getsize(src)
    split = (next_after_pkg if next_after_pkg is not None else total // SECTOR) * SECTOR
    CH = 16 * 1024 * 1024
    with open(out, 'wb') as o:
        s.seek(0); left = split; done = 0
        while left:
            b = s.read(min(CH, left)); o.write(b); left -= len(b); done += len(b)
            if progress: progress(done / total)
        o.seek((pkg_lba + pkg_sectors) * SECTOR); o.write(append)
        o.seek(split + shift * SECTOR); s.seek(split)
        while True:
            b = s.read(CH)
            if not b:
                break
            o.write(b); done += len(b)
            if progress: progress(min(1.0, done / total))
        end = o.tell()
        if end % SECTOR:
            o.write(b'\0' * (-end % SECTOR))
        o.seek(inf_lba * SECTOR); o.write(newinfo); o.write(b'\0' * ((-len(newinfo)) % SECTOR))
        for off, r, p in recs:
            lba, size = rec_extent(r)
            nr = bytearray(r)
            if p.endswith('/PACKAGE.BIN'):
                struct.pack_into('<I', nr, 10, new_pkg_size); struct.pack_into('>I', nr, 14, new_pkg_size)
            elif p.endswith('/PACKAGE_INFO.BIN'):
                struct.pack_into('<I', nr, 10, len(newinfo)); struct.pack_into('>I', nr, 14, len(newinfo))
            elif shift and not r[25] & 2 and lba > pkg_lba:
                struct.pack_into('<I', nr, 2, lba + shift); struct.pack_into('>I', nr, 6, lba + shift)
            if bytes(nr) != r:
                o.seek(off); o.write(bytes(nr))
        if shift:
            o.seek(16 * SECTOR + 80); o.write(struct.pack('<I', vol + shift) + struct.pack('>I', vol + shift))
    s.close()
    log.append(f'PACKAGE.BIN {pkg_size:,} -> {new_pkg_size:,} bytes; movies shifted {shift} sectors')
    return log
