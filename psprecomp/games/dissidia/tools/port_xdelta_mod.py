#!/usr/bin/env python3
"""Port a community xdelta patch made against the USA ISO onto this project's loose-file mod format.

    port_xdelta_mod.py <disc0 or DATA dir> <mods dir> (--xdelta PATCH | --changes FILE.json)
                       [--shift 0x16C80000 | --shift auto] [--map CHANGES.md] [--manifest OUT.json]
                       [--main-bin-base FILE] [--dfx DIR] [--dry-run]

Why this is not a plain byte patch: the patch is written against the USA ISO (ULUS10566), we only have the EU
disc. Both PACKAGE.BIN files hold the same game-data entries at the same *relative* offsets, so a byte of the
patch at ISO offset O belongs to package offset O - SHIFT, and from there to one entry (an .objx, main.bin...)
found through PACKAGE_INFO.BIN. SHIFT is the ISO offset of the USA PACKAGE.BIN (a multiple of 2048); the default
(0x16C80000) was measured for "012 Rebalance v0.2", `--shift auto` re-measures it for another patch.

How the patch is read without the USA ISO (see vcdiff.py): ADD/RUN literals give the bytes the patch wrote. The
encoder also copies *from other places of the source* when that is cheaper (a record 116 bytes further on, a
neighbouring table row), and those COPYs change bytes too; they are resolved with the EU package standing in
for the USA source (valid where both hold the same data, which the alignment check below confirms). A decoded
list that only has the literals (the plain `--changes` list) misses them - for v0.2 that is 657 item rows and
~110 moveset fields.

Alignment check (`--shift auto`, and printed for the given shift): the number of literal patch bytes that equal
the EU byte at that position peaks sharply at the right SHIFT (v0.2: 342 of 951 bytes, 80-120 anywhere else), and
every changed byte then lands on a recurring field of a known record (plya +0x1C scriptId, atkp +4, ablt +16 ...).

Output, per changed entry: <mods>/<package path> (or <mods>/by_crc/<crc>.bin when no path is known), same size as
the EU original, only the patched bytes differ. Every result is parsed back (ARC sections, plya/atkp record
counts, event timelines still decode, comb commands) before it is written. `--map` writes the per-character
change map, `--manifest` a JSON list of every changed byte run.

main.bin: its tables are addressed by (MPK entry, ARC tag, offset), so `--main-bin-base FILE` applies the main.bin
changes to a main.bin built by tools/roster_mainbin.py instead of the retail one (the roster tool rebuilds the
archive; absolute offsets move, table offsets do not).

Only the standard library is used (dfx is used for an extra round-trip check when --dfx / $DFX_PATH points to it).
"""
import argparse
import bisect
import json
import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import package_extract as pe  # noqa: E402
import vcdiff  # noqa: E402

DEFAULT_SHIFT = 0x16C80000          # ISO offset of PACKAGE.BIN in the USA disc (measured, see above)
MAIN_CRC = 0x93307C1C               # general_archive/main/main.bin

# code -> character. Certain: the ones named in the project notes (fiv100, sev100, ten110, gst130) and the
# 012 Rebalance changelog (one100 WoL, sev110 Tifa, nin200 Kuja, for200 Golbez). The rest follow the
# FF-number convention and are marked '?' where the variant is a guess.
CHARS = {
    'one100': 'Warrior of Light', 'one200': 'Garland', 'two100': 'Firion', 'two200': 'Emperor',
    'thr100': 'Onion Knight', 'thr200': 'Cloud of Darkness', 'for100': 'Cecil (Dark Knight)',
    'for110': 'Cecil/Kain? (FF4 variant)', 'for200': 'Golbez', 'fiv100': 'Bartz', 'fiv200': 'Exdeath',
    'fiv210': 'Gilgamesh?', 'six100': 'Terra', 'six200': 'Kefka', 'sev100': 'Cloud', 'sev110': 'Tifa',
    'sev120': 'Aerith (unused slot)', 'sev200': 'Sephiroth', 'eht100': 'Squall', 'eht110': 'Laguna?',
    'eht200': 'Ultimecia', 'nin100': 'Zidane', 'nin200': 'Kuja', 'ten100': 'Tidus', 'ten110': 'Yuna',
    'ten200': 'Jecht', 'gst100': 'gst100 (guest slot, generic LAND_/AIR_ names)', 'gst110': 'gst110 (guest slot)',
    'gst120': 'gst120 (guest slot)', 'gst130': 'Lightning', 'gst200': 'gst200 (guest slot)',
    'org210': 'Chaos (org210)',
}

# event opcodes (see docs/moves-and-events.md; '?' = this tool's guess from the rebalance itself)
OPS = {0x011B: 'show weapon', 0x011C: 'hide weapon', 0x042A: 'hitbox', 0x0200: 'attack active',
       0x0101: 'recovery window', 0x0402: 'movement', 0x0315: 'effect', 0x0925: 'weapon trail',
       0x060F: 'effect end', 0x0311: 'sound', 0x0303: 'sound', 0x0422: 'face/blink', 0x0219: 'own effect',
       0x0120: 'dodge-motion event (arg = id; the patch disables it)?', 0x0022: 'cancel gate?',
       0x0037: 'cancel gate (variant)?', 0x0D10: 'keyed param block?', 0x0518: 'slow/speed ramp?',
       0x0811: 'draw/aim param?'}


# Cross-check of the patch against its own changelog ("Changes from 0.1.txt"); interpretation, not format fact.
NOTES = {
    'one100': "Changelog: 'sword thrust 2nd hit is now also melee-mid in EX'. Matches atkp #40 MIDDLE2 and #78 EX_MIDDLE2, "
              "+4: 0 -> 1 (atkp +4 is a small hit-class flag; LONG_FIRE 7 -> 8 is the same field). The remaining edits are not "
              "in the v0.2 notes.",
    'sev100': "Changelog: 'Double Cut can no longer cancel on hit'. Matches motions 44 and 56: the ranged gate 0x0022 becomes "
              "0x0037 and opens at tick 36 instead of 16, and two 0x041e events become 0x047a with their first and third "
              "argument swapped (1/0 -> 0/1). Reading of the opcodes (0x22 = cancel allowed, 0x37 = not after a hit) is a guess.",
    'sev110': "Changelog: 'feint dodge-cancel window 30f -> 37f'. Motions 23 and 24 (the two feint animations): the cancel "
              "gate events (ops 0x0022 and 0x0028, open to the end of the motion) start at tick 74 in the USA original and "
              "at tick 37 (0x25) after the patch; 37 is the number the changelog quotes (v0.1 presumably had 30 = 0x1E there). "
              "The motion is 47 frames = 94 ticks long, so the cancel opens 18.5 frames in.",
    'for200': "Changelog: 'Golbez attack system fixed to prevent infinites, no cancel with attacks after any part of the attack "
              "hits'. Matches motion 42/48: the 0x041e gate events are re-typed (0x041a / 0x047a / 0x0428) and start at tick "
              "64 instead of 100, plus atkp AIR_NEAR1 and AIR_NEAR1_2 +4: 3 -> 10 and plya AIR_NEAR1_2 script +1.",
    'nin200': "Changelog: 'Kuja burst energy size reduced, ring holy speed reduced'. The only numeric edits are two entries of "
              "the magi parameter table, both going UP (1.5 -> 2.25, 0.1 -> 0.2). They may be durations/reciprocals or the "
              "records that use them may have been repointed in a part of the patch this map does not see; unresolved.",
    'two200': "Timeline 47 is rewritten: a 20-byte ranged event (op 0x0214) becomes op 0x0018 with args {0, 0.8f}, and the first "
              "keyed event's op 0x0123 becomes 0x0000. The dfx rule `op >> 8 = argument count` no longer walks this timeline; "
              "the byte layout is what the patch has, so it is applied as is. Needs an in-game look at the Emperor.",
}


# ---------------------------------------------------------------------------------------------
# package access

class Pkg:
    def __init__(self, data_dir):
        self.pk = pe.Package(data_dir)
        self.ents = sorted((s * 2048, size, crc) for crc, (s, size, _f) in self.pk.entries.items())
        self.starts = [e[0] for e in self.ents]
        self.size = os.path.getsize(os.path.join(self.pk.dir, 'PACKAGE.BIN'))
        self.names = {}
        for line in open(os.path.join(HERE, '..', 'package_paths.txt')):
            p = line.strip()
            if not p:
                continue
            c = pe.package_crc(p)
            old = self.names.get(c)
            # the list holds a few gibberish alias names whose CRC collides with a real asset
            # (menu/.../mqnmqn == obj/p_two200.objx): prefer a path with a file extension
            if old is None or ('.' not in os.path.basename(old) and '.' in os.path.basename(p)):
                self.names[c] = p
        # entries the list lacks (p_two200.objx is only present as a gibberish CRC alias)
        for code in CHARS:
            for suf in ('', '_a', '_w', '_zako_w'):
                p = 'obj/p_%s%s.objx' % (code, suf)
                c = pe.package_crc(p)
                if c in self.pk.entries and ('.' not in os.path.basename(self.names.get(c, '')) or c not in self.names):
                    self.names[c] = p

    def locate(self, pos):
        i = bisect.bisect_right(self.starts, pos) - 1
        if i >= 0 and pos < self.ents[i][0] + self.ents[i][1]:
            return self.ents[i]
        return None

    def read_abs(self, pos, n):
        self.pk.pkg.seek(pos)
        return self.pk.pkg.read(n)

    def read(self, crc):
        return self.pk.read_crc(crc)

    def label(self, crc):
        return self.names.get(crc) or 'by_crc/%08x' % crc


# ---------------------------------------------------------------------------------------------
# delta input

def load_delta(path):
    if path.lower().endswith('.json'):
        j = json.load(open(path))
        d = vcdiff.Delta()
        if isinstance(j, list):                         # [[offset, hex], ...]: literals only
            d.ranges = [(o, bytes.fromhex(h)) for o, h in j]
            d.notes.append('literal-only list: relocated COPY edits are not included')
        else:
            d.ranges = [(o, bytes.fromhex(h)) for o, h in j['ranges']]
            d.moved = [tuple(m) for m in j.get('moved', [])]
            d.target_size = j.get('target_size', 0)
        return d
    return vcdiff.decode(open(path, 'rb').read())


def dump_delta_json(d, path):
    json.dump({'target_size': d.target_size, 'ranges': [[o, b.hex()] for o, b in d.ranges],
               'moved': [list(m) for m in d.moved]}, open(path, 'w'), separators=(',', ':'))


# ---------------------------------------------------------------------------------------------
# shift

def agreement(delta, pkg, shift):
    """(literal bytes equal to the EU byte at that position, literal bytes inside the package)"""
    n = m = 0
    for o, b in delta.ranges:
        if shift <= o and o + len(b) <= shift + pkg.size:
            a = pkg.read_abs(o - shift, len(b))
            n += sum(1 for i in range(len(b)) if a[i] == b[i])
            m += len(b)
    return n, m


def find_shift(delta, pkg, log=print):
    lo = max(0, max(o + len(b) for o, b in delta.ranges) - pkg.size)
    hi = min(o for o, b in delta.ranges)
    cands = []
    for x in range((lo + 2047) & ~2047, hi + 1, 2048):
        cover = 0
        for o, b in delta.ranges:
            e = pkg.locate(o - x)
            if e and o - x + len(b) <= e[0] + e[1]:
                cover += 1
        cands.append((cover, x))
    cands.sort(reverse=True)
    top = cands[:400]
    scored = sorted(((agreement(delta, pkg, x)[0], x) for _c, x in top), reverse=True)
    best, second = scored[0], scored[1]
    log('shift search: %d candidates, best %#x (%d literal bytes agree with EU), runner-up %#x (%d)'
        % (len(cands), best[1], best[0], second[1], second[0]))
    if best[0] < 1.5 * max(second[0], 1):
        raise SystemExit('shift is ambiguous; pass --shift explicitly')
    return best[1]


# ---------------------------------------------------------------------------------------------
# resolving and applying

def resolve(delta, pkg, shift, literal_only=False):
    """-> (pieces [(abs_target_offset, bytes, kind)], unresolved [(src, tgt, len)])"""
    pieces = [(o, b, 'add') for o, b in delta.ranges]
    unresolved = []
    for s, t, ln in ([] if literal_only else delta.moved):
        if shift <= s and s + ln <= shift + pkg.size:
            pieces.append((t, pkg.read_abs(s - shift, ln), 'copy'))
        else:
            unresolved.append((s, t, ln))
    return pieces, unresolved


def apply_pieces(pieces, pkg, shift):
    """-> ({crc: [old, new(bytearray)]}, stray_bytes)"""
    out, stray = {}, 0
    for t, b, _kind in sorted(pieces, key=lambda p: p[0]):
        pos = t - shift
        while b:
            if pos < 0:
                stray += len(b)
                break
            e = pkg.locate(pos)
            if e is None:
                nxt = bisect.bisect_right(pkg.starts, pos)
                skip = (pkg.starts[nxt] - pos) if nxt < len(pkg.starts) else len(b)
                skip = min(skip, len(b))
                stray += skip
                b, pos = b[skip:], pos + skip
                continue
            take = min(len(b), e[0] + e[1] - pos)
            if e[2] not in out:
                old = pkg.read(e[2])
                out[e[2]] = [old, bytearray(old)]
            out[e[2]][1][pos - e[0]:pos - e[0] + take] = b[:take]
            b, pos = b[take:], pos + take
    return out, stray


def diff_runs(old, new, gap=0):
    """[(start, end)] of differing bytes; runs separated by <= gap equal bytes are merged"""
    runs = []
    for i, (a, b) in enumerate(zip(old, new)):
        if a != b:
            if runs and i - runs[-1][1] <= gap:
                runs[-1][1] = i + 1
            else:
                runs.append([i, i + 1])
    return [tuple(r) for r in runs]


# ---------------------------------------------------------------------------------------------
# container parsing (stdlib copies of the dfx ARC / MPK layouts)

def arc_sections(d):
    """-> [(type, tag, offset, size)] or None when d is not an ARC"""
    if d[:4] != b'ARC\x01':
        return None
    n = struct.unpack_from('<I', d, 4)[0]
    return [struct.unpack_from('<I4sII', d, 16 + 16 * i) for i in range(n)]


def mpk_entries(d):
    cnt = struct.unpack_from('<I', d, 8)[0]
    out = []
    for i in range(cnt):
        no, do, sz, _x = struct.unpack_from('<4I', d, 0x10 + 16 * i)
        out.append((d[no:d.index(b'\0', no)].decode(), do, sz))
    return out


def decode_events(b):
    """Same layout as dfx.objx.decode_events: -> (ok, [(kind, offset, size, fields)])"""
    if len(b) < 16:
        return False, []
    h = list(struct.unpack_from('<8H', b, 0))
    n_r, n_k, off_k = (h[1] & 0xFF) - 1, h[1] >> 8, h[2]
    if off_k > len(b):
        return False, []
    keyed, p = [], off_k
    for _ in range(n_k):
        if p + 4 > len(b):
            return False, []
        op = struct.unpack_from('<H', b, p + 2)[0]
        sz = 8 + 4 * (op >> 8)
        keyed.append(('keyed', p, sz))
        p += sz
    for pre in range(4, off_k - 16 + 1, 4):
        p, ranged, good = 16 + pre, [], True
        for _ in range(max(n_r, 0)):
            if p + 6 > off_k:
                good = False
                break
            op = struct.unpack_from('<H', b, p + 4)[0]
            sz = 12 + 4 * (op >> 8)
            ranged.append(('ranged', p, sz))
            p += sz
        if good and p == off_k:
            return True, ranged + keyed
    return False, []


def event_fields(kind, raw):
    """named fields of one event: [(name, width, value)]"""
    if kind == 'ranged':
        f = [('start', 2), ('end', 2), ('op', 2), ('pad', 2), ('x', 4)]
        base = 12
    else:
        f = [('frame', 2), ('op', 2), ('x', 4)]
        base = 8
    out, p = [], 0
    for nm, w in f:
        out.append((nm, p, w, int.from_bytes(raw[p:p + w], 'little')))
        p += w
    k = 0
    while base + 4 * k + 4 <= len(raw):
        out.append(('arg%d' % k, base + 4 * k, 4, int.from_bytes(raw[base + 4 * k:base + 4 * k + 4], 'little')))
        k += 1
    return out


def fmt_f32(v):
    f = struct.unpack('<f', struct.pack('<I', v))[0]
    return '%g' % f


# ---------------------------------------------------------------------------------------------
# verification

class VerifyError(Exception):
    pass


def verify_objx(old, new, label):
    """Structural check; raises VerifyError for a broken container, returns warnings for event timelines that
    decoded before and no longer do (the patch rewrote an event with an opcode whose size the dfx rule
    `op >> 8 = argument count` does not describe - see CHANGES.md)."""
    warnings = []
    so, sn = arc_sections(old), arc_sections(new)
    if so is None:
        return warnings
    if so != sn or len(old) != len(new):
        raise VerifyError('%s: ARC table or size changed' % label)
    for t, tag, off, size in sn:
        a, b = old[off:off + size], new[off:off + size]
        if tag == b'plya':
            if struct.unpack_from('<II', a)[1] != struct.unpack_from('<II', b)[1]:
                raise VerifyError('%s: plya count changed' % label)
        elif tag == b'atkp':
            n = struct.unpack_from('<I', b, 4)[0]
            if 8 + 68 * n > size:
                raise VerifyError('%s: atkp count overruns the section' % label)
        elif tag == b'comb':
            ver, n = struct.unpack_from('<HH', b, 0)
            offs = struct.unpack_from('<%dI' % n, b, 4)
            for o in offs:
                cid, ns = struct.unpack_from('<2H', b, o)
                if o + 8 + 8 * ns > size:
                    raise VerifyError('%s: comb command overruns the section' % label)
        elif t == 8:
            ver, n = struct.unpack_from('<II', b, 0)
            offs = list(struct.unpack_from('<%dI' % n, b, 8)) + [size]
            for i in range(n):
                ok_o, ev_o = decode_events(a[offs[i]:offs[i + 1]])
                ok_n, ev_n = decode_events(b[offs[i]:offs[i + 1]])
                if ok_o and (not ok_n or [e[1:] for e in ev_o] != [e[1:] for e in ev_n]):
                    warnings.append('%s: event timeline (motion) %d no longer decodes with the same event layout as before the patch'
                                    % (label, i))
    return warnings


def verify_dfx(old, new, label, dfx_dir):
    """Round trip through dfx.objx.Moveset (when importable): the patched file must parse and re-serialise
    to exactly the same bytes (as the unpatched one does)."""
    sys.path.insert(0, dfx_dir)
    from dfx import objx as O  # type: ignore
    try:
        a = O.Moveset.load(bytes(old)).to_bytes()
    except Exception:
        return 'skipped (original does not load)'
    b = O.Moveset.load(bytes(new)).to_bytes()
    if (a == bytes(old)) != (b == bytes(new)):
        raise VerifyError('%s: dfx round trip differs between original and patched' % label)
    return 'round trip %s' % ('identical' if b == bytes(new) else 'n/a (original is not canonical either)')


# ---------------------------------------------------------------------------------------------
# change descriptions (the change map)

PLYA_FIELDS = [(0, 2, 'actionId'), (2, 2, 'phase'), (4, 2, 'ground/air'), (0x1C, 2, 'scriptId'), (0x64, 16, 'name')]
ABLT_FIELDS = {16: (2, 'CP cost'), 10: (2, 'u16@+10 (1 -> 0xFFFF)'), 2: (2, 'name id'), 4: (2, 'text id'),
               22: (1, 'u8@+22'), 30: (1, 'u8@+30')}
ATKP_FLOATS = range(24, 52, 4)


def _hex(b):
    return '0x' + bytes(reversed(b)).hex() if len(b) <= 4 else b.hex()


def _field_span(table, off):
    for s, w, nm in table:
        if s <= off < s + w:
            return s, w, nm
    return off, 1, '+0x%02X' % off


def _desc_plya(a, b, runs):
    n = struct.unpack_from('<I', b, 4)[0]
    rows, seen, per = [], set(), {}
    for lo, hi in runs:
        for p in range(lo, hi):
            if p < 8 or p >= 8 + 116 * n:
                rows.append('plya tail +0x%x: %s -> %s' % (p, a[p:p + 1].hex(), b[p:p + 1].hex()))
            else:
                per.setdefault((p - 8) // 116, []).append((p - 8) % 116)
    nm = lambda x: x.split(b'\0')[0].decode('latin1')  # noqa: E731
    for rec in sorted(per):
        base = 8 + 116 * rec
        ra, rb = a[base:base + 116], b[base:base + 116]
        act = struct.unpack_from('<H', ra)[0]
        if len(per[rec]) >= 6:
            # a record rewritten wholesale (the encoder rebuilt it from pieces of other records)
            rows.append("plya #%d '%s' REWRITTEN (%d of 116 bytes differ): action 0x%04x -> 0x%04x, phase 0x%04x -> 0x%04x, "
                        "scriptId 0x%04x -> 0x%04x, name '%s' -> '%s'" % (
                            rec, nm(ra[100:]), len(per[rec]), act, struct.unpack_from('<H', rb)[0],
                            struct.unpack_from('<H', ra, 2)[0], struct.unpack_from('<H', rb, 2)[0],
                            struct.unpack_from('<H', ra, 0x1C)[0], struct.unpack_from('<H', rb, 0x1C)[0],
                            nm(ra[100:]), nm(rb[100:])))
            continue
        for off in per[rec]:
            s_, w, fn = _field_span(PLYA_FIELDS, off)
            if (rec, s_) in seen:
                continue
            seen.add((rec, s_))
            ov, nv = ra[s_:s_ + w], rb[s_:s_ + w]
            if fn == 'name':
                rows.append("plya #%d (action 0x%04x) name: '%s' -> '%s'" % (rec, act, nm(ov), nm(nv)))
            elif w == 2:
                rows.append('plya #%d \'%s\' (action 0x%04x) %s: 0x%04x -> 0x%04x' % (
                    rec, nm(ra[100:]), act, fn, int.from_bytes(ov, 'little'), int.from_bytes(nv, 'little')))
            else:
                rows.append('plya #%d \'%s\' (action 0x%04x) %s: 0x%02x -> 0x%02x' % (rec, nm(ra[100:]), act, fn, ov[0], nv[0]))
    return rows


def _desc_atkp(a, b, runs):
    n = struct.unpack_from('<I', b, 4)[0]
    byrec, rows = {}, []
    for lo, hi in runs:
        for p in range(lo, hi):
            if p < 8 or p >= 8 + 68 * n:
                rows.append('atkp tail +0x%x: %s -> %s' % (p, a[p:p + 1].hex(), b[p:p + 1].hex()))
            else:
                rec, off = divmod(p - 8, 68)
                byrec.setdefault(rec, []).append(off)
    for rec in sorted(byrec):
        base = 8 + 68 * rec
        name = a[base + 52:base + 68].split(b'\0')[0].decode('latin1')
        parts, done = [], set()
        for off in byrec[rec]:
            if off >= 52:
                if 'name' not in done:
                    done.add('name')
                    parts.append("name '%s' -> '%s'" % (name, b[base + 52:base + 68].split(b'\0')[0].decode('latin1')))
            elif off in ATKP_FLOATS or (off - 2 in ATKP_FLOATS) or (off - 1 in ATKP_FLOATS) or (off - 3 in ATKP_FLOATS):
                w = off & ~3
                if w not in done:
                    done.add(w)
                    ov, nv = struct.unpack_from('<I', a, base + w)[0], struct.unpack_from('<I', b, base + w)[0]
                    parts.append('+%d f32 %s -> %s' % (w, fmt_f32(ov), fmt_f32(nv)))
            else:
                parts.append('+%d %02x -> %02x' % (off, a[base + off], b[base + off]))
        rows.append('atkp #%d %s: %s' % (rec, name, ', '.join(parts)))
    return rows


def _fmt_time(nm, v):
    if v == 0xFFFF:
        return 'none'
    if v >= 0xF000:
        return '0x%04X (never)' % v
    return '%d ticks (%gf)' % (v, v / 2.0)


def _desc_events(a, b, runs, size):
    ver, n = struct.unpack_from('<II', b, 0)
    offs = list(struct.unpack_from('<%dI' % n, b, 8)) + [size]
    rows, seen, disabled = [], set(), {}
    for lo, hi in runs:
        for p in range(lo, hi):
            bi = bisect.bisect_right(offs, p) - 1
            if bi < 0 or bi >= n:
                rows.append('events header +0x%x: %s -> %s' % (p, a[p:p + 1].hex(), b[p:p + 1].hex()))
                continue
            blob_o, blob_n = a[offs[bi]:offs[bi + 1]], b[offs[bi]:offs[bi + 1]]
            ok, evs = decode_events(blob_o)
            q = p - offs[bi]
            hit = next((e for e in evs if e[1] <= q < e[1] + e[2]), None) if ok else None
            if hit is None:
                w = q & ~3
                key = (bi, 'raw', w)
                if key not in seen:
                    seen.add(key)
                    ow, nw = blob_o[w:w + 4], blob_n[w:w + 4]
                    if ow[2:4] == b'\xff\xff' and nw[2:4] == b'\xff\xff' and nw[1] >= 0xF0:
                        # start/end pair of a ranged event in a timeline the op>>8 size rule cannot walk
                        disabled.setdefault((int.from_bytes(ow[:2], 'little'), 'undecoded timeline'), []).append(bi)
                    else:
                        rows.append('motion %d timeline +0x%x (timeline not decodable by the op>>8 rule): %s -> %s' % (
                            bi, w, ow.hex(), nw.hex()))
                continue
            kind, eo, es = hit
            key = (bi, eo)
            if key in seen:
                continue
            seen.add(key)
            fo, fn = event_fields(kind, blob_o[eo:eo + es]), event_fields(kind, blob_n[eo:eo + es])
            opv = fo[2][3] if kind == 'ranged' else fo[1][3]
            idx = [e for e in evs if e[0] == kind].index(hit)
            diffs = []
            for (nm, fp, w, ov), (_n2, _p2, _w2, nv) in zip(fo, fn):
                if ov != nv:
                    diffs.append((nm, w, ov, nv))
            if kind == 'ranged' and opv == 0x0120 and [d[0] for d in diffs] == ['start'] and diffs[0][3] >= 0xF000:
                disabled.setdefault((diffs[0][2], 'op 0x0120'), []).append(bi)
                continue
            parts = []
            for nm, w, ov, nv in diffs:
                if nm in ('start', 'end', 'frame'):
                    parts.append('%s %s -> %s' % (nm, _fmt_time(nm, ov), _fmt_time(nm, nv)))
                elif nm == 'op':
                    parts.append('op 0x%04x -> 0x%04x' % (ov, nv))
                else:
                    parts.append('%s 0x%x -> 0x%x%s' % (nm, ov, nv, ' (f32 %s -> %s)' % (fmt_f32(ov), fmt_f32(nv))
                                                          if w == 4 and nm != 'x' else ''))
            rows.append('motion %d %s[%d] op 0x%04x (%s): %s' % (
                bi, kind, idx, opv, OPS.get(opv, 'unknown'), '; '.join(parts) or 'bytes in padding'))
    for (start, what), ms in sorted(disabled.items(), key=lambda kv: kv[1]):
        rows.append('motions %s: ranged event %s (start %d ticks, to end of motion) disabled by moving its start to 0x%04X+ (never fires)'
                    % (','.join(map(str, sorted(set(ms)))), what, start, 0xF000 | start))
    return rows


def _desc_magi(a, b, runs):
    """MAGI: 16 B header {magic, version, n1, n2}, n1 records of 204 B, then n2 f32 parameters (the records index them)."""
    n1, n2 = struct.unpack_from('<II', b, 8)
    tab = 16 + 204 * n1
    rows, seen = [], set()
    for lo, hi in runs:
        for p in range(lo, hi):
            w = p & ~3
            if w in seen or w + 4 > len(a):
                continue
            seen.add(w)
            ov, nv = struct.unpack_from('<I', a, w)[0], struct.unpack_from('<I', b, w)[0]
            if tab <= w < tab + 4 * n2:
                where = 'magi param[%d] (+0x%04x)' % ((w - tab) // 4, w)
            elif w >= 16 and w < tab:
                where = 'magi record %d +%d' % ((w - 16) // 204, (w - 16) % 204)
            else:
                where = 'magi +0x%04x' % w
            rows.append('%s: f32 %s -> %s (0x%08x -> 0x%08x)' % (where, fmt_f32(ov), fmt_f32(nv), ov, nv))
    return rows


def describe_arc(old, new):
    secs = arc_sections(old)
    rows = []
    for t, tag, off, size in secs:
        runs = diff_runs(old[off:off + size], new[off:off + size])
        if not runs:
            continue
        a, b = old[off:off + size], new[off:off + size]
        name = tag.decode('latin1')
        if tag == b'plya':
            rows += [('plya', r) for r in _desc_plya(a, b, runs)]
        elif tag == b'atkp':
            rows += [('atkp', r) for r in _desc_atkp(a, b, runs)]
        elif t == 8:
            rows += [('events', r) for r in _desc_events(a, b, runs, size)]
        elif tag == b'magi':
            rows += [('magi', r) for r in _desc_magi(a, b, runs)]
        else:
            rows += [(name, '%s +0x%x: %s -> %s' % (name, lo, a[lo:hi].hex(), b[lo:hi].hex())) for lo, hi in runs]
    return rows


def main_locator(d, off):
    """offset in main.bin -> (mpk entry, arc tag or None, offset inside that tag/entry)"""
    for name, do, sz in mpk_entries(d):
        if do <= off < do + sz:
            ent = d[do:do + sz]
            secs = arc_sections(ent)
            if secs:
                for _t, tag, so, ss in secs:
                    if so <= off - do < so + ss:
                        return name, tag.decode('latin1'), off - do - so
            return name, None, off - do
    return None, None, off


def main_resolve(d, loc):
    name, tag, rel = loc
    for n, do, sz in mpk_entries(d):
        if n == name:
            if tag is None:
                return do + rel
            for _t, t2, so, ss in arc_sections(d[do:do + sz]):
                if t2.decode('latin1') == tag:
                    return do + so + rel
    return None


def describe_main(old, new):
    """main.bin: MPK -> system.bin (ARC) -> ablt (790 x 40 B after a 16 B header) / item (677 x 104 B).
    Rows are aggregated: one line per (field, old, new) for abilities, one for the item mask."""
    sysb = next(e for e in mpk_entries(old) if e[0] == 'system.bin')
    tags = {t.decode(): so for _t, t, so, _ss in arc_sections(old[sysb[1]:sysb[1] + sysb[2]])}
    abl, masks, rows, seen = {}, {}, [], set()
    for lo, hi in diff_runs(old, new):
        for p in range(lo, hi):
            name, tag, rel = main_locator(old, p)
            if (name, tag) == ('system.bin', 'ablt') and rel >= 16:
                rec, off = divmod(rel - 16, 40)
                w, nm = 1, 'u8 +%d' % off
                for fo, (fw, fn) in ABLT_FIELDS.items():
                    if fo <= off < fo + fw:
                        off, w, nm = fo, fw, '+%d %s' % (fo, fn)
                if ('ablt', rec, off) in seen:
                    continue
                seen.add(('ablt', rec, off))
                rb = sysb[1] + tags['ablt'] + 16 + 40 * rec
                ov, nv = (int.from_bytes(x[rb + off:rb + off + w], 'little') for x in (old, new))
                abl.setdefault((nm, ov, nv), []).append((rec, struct.unpack_from('<H', old, rb)[0]))
            elif (name, tag) == ('system.bin', 'item') and rel >= 16 and (rel - 16) // 104 < 677 and 24 <= (rel - 16) % 104 < 28:
                rec = (rel - 16) // 104
                if rec in masks:
                    continue
                rb = sysb[1] + tags['item'] + 16 + 104 * rec + 24
                masks[rec] = (struct.unpack_from('<I', old, rb)[0], struct.unpack_from('<I', new, rb)[0])
            else:
                rows.append(('raw', '%s/%s +0x%x: %02x -> %02x' % (name, tag, rel, old[p], new[p])))
    out = []
    for (nm, ov, nv), recs in sorted(abl.items(), key=lambda kv: (kv[0][0], -len(kv[1]))):
        out.append(('ablt', 'ablt %s: %d -> %d on %d abilities (record #/id): %s' % (
            nm, ov, nv, len(recs), ', '.join('#%d/0x%04x' % r for r in sorted(recs)))))
    if masks:
        news = sorted(set(v[1] for v in masks.values()))
        out.append(('item', 'item +24 u32 (character-usability mask?): %d of 677 item rows set to %s (previously %d different '
                    'values such as %s); rows: %s' % (
                        len(masks), ', '.join('0x%08x' % x for x in news), len(set(v[0] for v in masks.values())),
                        ', '.join('0x%08x' % x for x in sorted(set(v[0] for v in masks.values()))[:6]),
                        ','.join('#%d' % r for r in sorted(masks)))))
    return out + rows


def char_of(label):
    base = os.path.basename(label)
    for pre in ('p_', 'w_'):
        if base.startswith(pre):
            code = base[len(pre):].split('.')[0].split('_')[0]
            return code
    return None


def write_map(path, results, meta):
    L = ['# 012 Rebalance port: change map', '',
         'Generated by `tools/port_xdelta_mod.py`; %s' % meta['summary'], '',
         'Conventions: event times are ticks (60 per second, 2 per 30 fps motion frame); `plya`/`atkp` fields are',
         'named only where the format is known (see docs/moves-and-events.md); everything else is shown as the byte offset.',
         '`add` = literal bytes of the patch, `copy` = bytes the xdelta copies from elsewhere in the USA source (resolved',
         'from the EU package).', '']
    groups = {}
    for r in results:
        groups.setdefault(r['group'], []).append(r)
    for g in sorted(groups):
        L.append('## %s' % g)
        L.append('')
        for r in groups[g]:
            L.append('### `%s`  (%d bytes changed, %d runs)' % (r['label'], r['bytes'], r['runs']))
            L.append('')
            code = char_of(r['label'])
            if code in NOTES and os.path.basename(r['label']) == 'p_%s.objx' % code:
                L.append('> ' + NOTES[code])
                L.append('')
            for w in r.get('warnings', []):
                L.append('> warning: ' + w)
                L.append('')
            cur = None
            for sec, text in r['rows']:
                if sec != cur:
                    cur = sec
                L.append('- %s' % text)
            L.append('')
    open(path, 'w').write('\n'.join(L))


# ---------------------------------------------------------------------------------------------
# driver

def port(pkg, delta, shift, mods, main_base=None, dfx=None, dry=False, log=print, literal_only=False):
    pieces, unresolved = resolve(delta, pkg, shift, literal_only)
    out, stray = apply_pieces(pieces, pkg, shift)
    n_add, n_copy = sum(1 for p in pieces if p[2] == 'add'), sum(1 for p in pieces if p[2] == 'copy')
    log('%d literal ranges + %d resolved copies; %d unresolved copies (source outside PACKAGE.BIN), %d stray bytes outside entries'
        % (n_add, n_copy, len(unresolved), stray))
    results = []
    for crc, (old, new) in sorted(out.items(), key=lambda kv: pkg.label(kv[0])):
        runs = diff_runs(old, new)
        if not runs:
            continue
        label = pkg.label(crc)
        old, new = bytes(old), bytes(new)
        try:
            warns = verify_objx(old, new, label)
            for w in warns:
                log('  warning: ' + w)
            extra = verify_dfx(old, new, label, dfx) if (dfx and arc_sections(old)) else ''
        except VerifyError as e:
            raise SystemExit('verification failed: %s' % e)
        rows = describe_main(old, new) if crc == MAIN_CRC else (describe_arc(old, new) if arc_sections(old) else
                                                               [('raw', 'bytes +0x%x: %s -> %s' % (lo, old[lo:hi].hex(), new[lo:hi].hex())) for lo, hi in runs])
        code = char_of(label)
        group = ('%s (%s)' % (CHARS.get(code[:6] if code else '', code), code)) if code and code[:6] in CHARS else \
            ('Shared tables (main.bin)' if crc == MAIN_CRC else 'Other: ' + label)
        results.append({'crc': crc, 'label': label, 'group': group, 'old': old, 'new': new,
                        'bytes': sum(h - l for l, h in runs), 'runs': len(runs), 'rows': rows, 'warnings': warns if arc_sections(old) else []})
        data = new
        if crc == MAIN_CRC and main_base:
            data = rebase_main(old, new, open(main_base, 'rb').read(), log)
        if not dry:
            dest = os.path.join(mods, 'by_crc', '%08x.bin' % crc) if crc == MAIN_CRC or crc not in pkg.names \
                else os.path.join(mods, pkg.names[crc])
            os.makedirs(os.path.dirname(dest), exist_ok=True)
            open(dest, 'wb').write(data)
        log('%-48s %4d bytes in %3d runs %s' % (label, results[-1]['bytes'], results[-1]['runs'], extra))
    return results, unresolved, stray


def rebase_main(old, new, base, log):
    """Apply the main.bin edits to another build of main.bin (roster tool output) by table position."""
    out = bytearray(base)
    done = skipped = 0
    for lo, hi in diff_runs(old, new):
        for p in range(lo, hi):
            loc = main_locator(old, p)
            q = main_resolve(base, loc)
            if q is None or base[q] != old[p]:
                skipped += 1
                continue
            out[q] = new[p]
            done += 1
    log('main.bin rebased onto %s: %d bytes applied, %d skipped (table moved or already edited)' % (len(base), done, skipped))
    return bytes(out)


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('disc', help='disc0 or DATA directory of the EU disc (PACKAGE.BIN + PACKAGE_INFO.BIN)')
    ap.add_argument('mods', help='output mods directory')
    g = ap.add_mutually_exclusive_group(required=True)
    g.add_argument('--xdelta', help='the .xdelta / VCDIFF patch (preferred: includes relocated copies)')
    g.add_argument('--changes', help='decoded change list (.json): [[offset, hex], ...] or the vcdiff.py --json form')
    ap.add_argument('--shift', default=hex(DEFAULT_SHIFT), help="ISO offset of the USA PACKAGE.BIN, or 'auto'")
    ap.add_argument('--map', help='write the per-character change map (markdown)')
    ap.add_argument('--manifest', help='write every changed byte run as JSON')
    ap.add_argument('--main-bin-base', help='apply the main.bin edits to this main.bin (roster tool output)')
    ap.add_argument('--dfx', default=os.environ.get('DFX_PATH'), help='dir holding the dfx package (extra round-trip check)')
    ap.add_argument('--save-delta', help='write the decoded patch as JSON (input for --changes)')
    ap.add_argument('--literal-only', action='store_true', help='ignore relocated COPYs (what a literal-only decoder sees)')
    ap.add_argument('--dry-run', action='store_true')
    a = ap.parse_args(argv)
    pkg = Pkg(a.disc)
    delta = load_delta(a.xdelta or a.changes)
    for n in delta.notes:
        print('warning:', n)
    if a.save_delta:
        dump_delta_json(delta, a.save_delta)
    shift = find_shift(delta, pkg) if a.shift == 'auto' else int(a.shift, 0)
    n, m = agreement(delta, pkg, shift)
    print('shift %#x: %d of %d literal bytes equal the EU byte at that position' % (shift, n, m))
    if m and n < 0.15 * m:
        print('warning: low agreement; the shift is probably wrong (try --shift auto)')
    results, unresolved, stray = port(pkg, delta, shift, a.mods, a.main_bin_base, a.dfx, a.dry_run, literal_only=a.literal_only)
    summary = '%d files changed, %d bytes, shift %#x.' % (len(results), sum(r['bytes'] for r in results), shift)
    print(summary)
    if a.map:
        write_map(a.map, results, {'summary': summary})
    if a.manifest:
        json.dump([{'label': r['label'], 'crc': '%08x' % r['crc'],
                    'runs': [[lo, r['old'][lo:hi].hex(), r['new'][lo:hi].hex()] for lo, hi in diff_runs(r['old'], r['new'])]}
                   for r in results], open(a.manifest, 'w'), indent=0)
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
