"""Dissidia 012 character moveset bundle (obj/p_<code>.objx) — read, edit, write.

Container ('ARC\\x01'): u32 magic, u32 count, 8 zero bytes, then count x
{u32 type, char[4] tag, u32 offset, u32 size}. Entries are 4-byte aligned,
except type 14 (sound bank, 64) and types 2/3 (GMO, GIM/mlng, 16).

Decoded sections (others are kept as raw bytes):
  comb  commands: u16 ver, u16 n, u32 offsets[n]; record = u16 id, u16 nsteps,
        u16 prereq_a, u16 prereq_b, then steps x {u16 action, u16 b, u16 c, u16 flags}
  plya  actions:  u32 0x16, u32 n, n x 116-byte records (u16 action_id @0, u16 script_id @0x1C)
  type7 script->motion map: u32 2, u32 n, n x {u16 script, u16 motion, u32 phase}
  type8 per-motion event scripts: u32 3, u32 n, u32 offsets[n], blobs
  atkp  hit profiles: u32 hdr, u32 n, n x 68-byte records (name char[16] @52)
"""
from __future__ import annotations
import struct
from dataclasses import dataclass, field

MAGIC = b'ARC\x01'
ALIGN = {14: 64, 2: 16, 3: 16}


@dataclass
class Entry:
    type: int
    tag: str
    data: bytes


class Arc:
    def __init__(self, entries, header_extra=b'\0' * 8):
        self.entries = entries
        self.header_extra = header_extra

    @classmethod
    def from_bytes(cls, d: bytes):
        if d[:4] != MAGIC:
            raise ValueError('not an ARC')
        n = struct.unpack_from('<I', d, 4)[0]
        ents = []
        for i in range(n):
            t, tag, o, s = struct.unpack_from('<I4sII', d, 16 + i * 16)
            ents.append(Entry(t, tag.decode('latin1'), d[o:o + s]))
        return cls(ents, d[8:16])

    def to_bytes(self) -> bytes:
        n = len(self.entries)
        table = bytearray()
        body = bytearray()
        o = 16 + n * 16
        for e in self.entries:
            al = ALIGN.get(e.type, 4)
            pad = (-o) % al
            body += b'\0' * pad
            o += pad
            table += struct.pack('<I4sII', e.type, e.tag.encode('latin1')[:4].ljust(4, b'\0'), o, len(e.data))
            body += e.data
            o += len(e.data)
        return MAGIC + struct.pack('<I', n) + self.header_extra + bytes(table) + bytes(body)

    def find(self, tag=None, type=None):
        return [e for e in self.entries if (tag is None or e.tag == tag) and (type is None or e.type == type)]

    def get(self, tag=None, type=None):
        r = self.find(tag, type)
        return r[0] if r else None


# Event timelines count at 60 fps: two ticks per (30 fps) motion frame. Confirmed on Bartz: the
# end-of-move marker (keyed op 2) sits at ~1.85x the motion length in every timeline, and his
# weapon-dismiss sparkle lands inside the motion only when read in ticks.
EVENT_RATE = 2


def to_ticks(frame):
    return 0xFFFF if frame == 0xFFFF else int(round(frame * EVENT_RATE))


def to_frame(ticks):
    return 0xFFFF if ticks == 0xFFFF else ticks / EVENT_RATE


# ------------------------------------------------------------------ comb
@dataclass
class Command:
    id: int
    prereq_a: int
    prereq_b: int
    steps: list          # list of [action, b, c, flags]


def parse_comb(d: bytes):
    ver, n = struct.unpack_from('<HH', d, 0)
    offs = struct.unpack_from(f'<{n}I', d, 4)
    cmds = []
    for o in offs:
        cid, ns, pa, pb = struct.unpack_from('<4H', d, o)
        steps = [list(struct.unpack_from('<4H', d, o + 8 + k * 8)) for k in range(ns)]
        cmds.append(Command(cid, pa, pb, steps))
    return ver, cmds


def build_comb(ver, cmds) -> bytes:
    n = len(cmds)
    o = 4 + 4 * n
    offs, body = [], bytearray()
    for c in cmds:
        offs.append(o + len(body))
        body += struct.pack('<4H', c.id, len(c.steps), c.prereq_a, c.prereq_b)
        for s in c.steps:
            body += struct.pack('<4H', *s)
    return struct.pack('<HH', ver, n) + struct.pack(f'<{n}I', *offs) + bytes(body)


# ------------------------------------------------------------------ plya
PLYA_SIZE = 116


@dataclass
class Action:
    raw: bytearray

    @property
    def action_id(self): return struct.unpack_from('<H', self.raw, 0)[0]
    @action_id.setter
    def action_id(self, v): struct.pack_into('<H', self.raw, 0, v)
    @property
    def script_id(self): return struct.unpack_from('<H', self.raw, 0x1C)[0]
    @script_id.setter
    def script_id(self, v): struct.pack_into('<H', self.raw, 0x1C, v)
    @property
    def phase(self): return struct.unpack_from('<H', self.raw, 2)[0]
    @property
    def ground_air(self): return struct.unpack_from('<H', self.raw, 4)[0]


def parse_plya(d: bytes):
    hdr, n = struct.unpack_from('<II', d, 0)
    acts = [Action(bytearray(d[8 + i * PLYA_SIZE:8 + (i + 1) * PLYA_SIZE])) for i in range(n)]
    tail = d[8 + n * PLYA_SIZE:]
    return hdr, acts, tail


def build_plya(hdr, acts, tail=b'') -> bytes:
    return struct.pack('<II', hdr, len(acts)) + b''.join(bytes(a.raw) for a in acts) + tail


# ------------------------------------------------------------------ script -> motion map (type 7)
def parse_motion_map(d: bytes):
    ver, n = struct.unpack_from('<II', d, 0)
    rows = [list(struct.unpack_from('<HHI', d, 8 + i * 8)) for i in range(n)]
    return ver, rows, d[8 + n * 8:]


def build_motion_map(ver, rows, tail=b'') -> bytes:
    return struct.pack('<II', ver, len(rows)) + b''.join(struct.pack('<HHI', *r) for r in rows) + tail


# ------------------------------------------------------------------ per-motion event scripts (type 8)
def parse_events(d: bytes):
    ver, n = struct.unpack_from('<II', d, 0)
    offs = list(struct.unpack_from(f'<{n}I', d, 8)) + [len(d)]
    return ver, [d[offs[i]:offs[i + 1]] for i in range(n)]


def build_events(ver, blobs) -> bytes:
    n = len(blobs)
    o = 8 + 4 * n
    offs = []
    for b in blobs:
        offs.append(o); o += len(b)
    return struct.pack('<II', ver, n) + struct.pack(f'<{n}I', *offs) + b''.join(blobs)


def parse_event_blob(b: bytes):
    """Header (8 u16) + events {u16 start, u16 end, u16 opcode, args...}. Event length is
    not self-describing for every opcode yet, so we expose the header and keep raw bytes."""
    hdr = struct.unpack_from('<8H', b, 0) if len(b) >= 16 else ()
    return {'motion': hdr[0] if hdr else None, 'header': list(hdr), 'size': len(b)}


# ------------------------------------------------------------------ atkp
ATKP_SIZE = 68


@dataclass
class Attack:
    raw: bytearray

    @property
    def name(self):
        return bytes(self.raw[52:68]).split(b'\0')[0].decode('latin1')

    @name.setter
    def name(self, v):
        self.raw[52:68] = v.encode('latin1')[:15].ljust(16, b'\0')


def parse_atkp(d: bytes):
    hdr, n = struct.unpack_from('<II', d, 0)
    atks = [Attack(bytearray(d[8 + i * ATKP_SIZE:8 + (i + 1) * ATKP_SIZE])) for i in range(n)]
    return hdr, atks, d[8 + n * ATKP_SIZE:]


def build_atkp(hdr, atks, tail=b'') -> bytes:
    return struct.pack('<II', hdr, len(atks)) + b''.join(bytes(a.raw) for a in atks) + tail


# ------------------------------------------------------------------ high level
class Moveset:
    """Editable view over a character objx."""

    def __init__(self, arc: Arc):
        self.arc = arc
        e = arc.get('comb'); self.comb_ver, self.commands = parse_comb(e.data) if e else (6, [])
        e = arc.get('plya'); self.plya_hdr, self.actions, self.plya_tail = parse_plya(e.data) if e else (0x16, [], b'')
        e = arc.get(type=7); self.map_ver, self.motion_map, self.map_tail = parse_motion_map(e.data) if e else (2, [], b'')
        e = arc.get(type=8); self.ev_ver, self.events = parse_events(e.data) if e else (3, [])
        e = arc.get('atkp'); self.atkp_hdr, self.attacks, self.atkp_tail = parse_atkp(e.data) if e else (12, [], b'')

    @classmethod
    def load(cls, path_or_bytes):
        d = open(path_or_bytes, 'rb').read() if isinstance(path_or_bytes, str) else path_or_bytes
        return cls(Arc.from_bytes(d))

    def to_bytes(self) -> bytes:
        for tag, typ, data in (('comb', None, build_comb(self.comb_ver, self.commands)),
                               ('plya', None, build_plya(self.plya_hdr, self.actions, self.plya_tail)),
                               (None, 7, build_motion_map(self.map_ver, self.motion_map, self.map_tail)),
                               (None, 8, build_events(self.ev_ver, self.events)),
                               ('atkp', None, build_atkp(self.atkp_hdr, self.attacks, self.atkp_tail))):
            e = self.arc.get(tag, typ)
            if e is not None:
                e.data = data
        return self.arc.to_bytes()

    # ---- navigation helpers
    def action(self, action_id):
        return next((a for a in self.actions if a.action_id == action_id), None)

    def motions_for_script(self, script_id):
        return [(m, ph) for s, m, ph in self.motion_map if s == script_id]

    def resolve(self, motion_names=None):
        """command -> steps -> action -> script -> [motion index/name]"""
        out = []
        for c in self.commands:
            steps = []
            for s in c.steps:
                a = self.action(s[0])
                sid = a.script_id if a else None
                mot = self.motions_for_script(sid) if sid is not None else []
                steps.append({'action': s[0], 'flags': s[3], 'script': sid,
                              'motions': [(m, motion_names[m] if motion_names and m < len(motion_names) else None, ph) for m, ph in mot]})
            out.append({'command': c.id, 'prereq': [c.prereq_a, c.prereq_b], 'steps': steps})
        return out

    def to_json(self, motion_names=None):
        return {
            'commands': [{'id': c.id, 'prereq_a': c.prereq_a, 'prereq_b': c.prereq_b, 'steps': c.steps} for c in self.commands],
            'actions': [{'action_id': a.action_id, 'script_id': a.script_id, 'raw': bytes(a.raw).hex()} for a in self.actions],
            'motion_map': self.motion_map,
            'attacks': [{'name': a.name, 'raw': bytes(a.raw).hex()} for a in self.attacks],
            'events': [b.hex() for b in self.events],
            'resolved': self.resolve(motion_names),
        }

    def apply_json(self, j):
        self.commands = [Command(c['id'], c['prereq_a'], c['prereq_b'], [list(s) for s in c['steps']]) for c in j['commands']]
        self.actions = [Action(bytearray.fromhex(a['raw'])) for a in j['actions']]
        for a, src in zip(self.actions, j['actions']):
            a.action_id = src['action_id']; a.script_id = src['script_id']
        self.motion_map = [list(r) for r in j['motion_map']]
        self.attacks = [Attack(bytearray.fromhex(a['raw'])) for a in j['attacks']]
        for a, src in zip(self.attacks, j['attacks']):
            a.name = src['name']
        self.events = [bytes.fromhex(b) for b in j['events']]


# ------------------------------------------------------------------ event timeline decoding
def _entry_size_ranged(op):   # section A: u16 start, u16 end, u16 op, u16 pad, u32 x, u32 args[op>>8]
    return 12 + 4 * (op >> 8)


def _entry_size_keyed(op):    # section B: u16 frame, u16 op, u32 x, u32 args[op>>8]
    return 8 + 4 * (op >> 8)


def decode_events(b: bytes):
    """Decode a per-motion event script into ranged + keyed event lists.

    Returns dict(motion, header, preamble(bytes), ranged[...], keyed[...], ok).
    Each event keeps its raw bytes so encode_events() is lossless.
    A timeline with no ranged events has only an 8-byte header (motion, counts, keyed offset, size): 'short'."""
    if len(b) >= 8 and not (struct.unpack_from('<H', b, 2)[0] & 0xFF):
        motion, cnt, off_k, size = struct.unpack_from('<4H', b, 0)
        res = {'motion': motion, 'header': [motion, cnt, off_k, size, 0, 0, 0, 0], 'short': True, 'ok': False,
               'preamble': '', 'ranged': []}
        keyed, p = [], off_k
        for _ in range(cnt >> 8):
            if p + 4 > len(b):
                return res
            fr, op = struct.unpack_from('<HH', b, p)
            sz = _entry_size_keyed(op)
            keyed.append({'frame': fr, 'op': op, 'raw': b[p:p + sz].hex()})
            p += sz
        res['keyed'] = keyed
        res['ok'] = p == len(b) and off_k == 8
        return res
    h = list(struct.unpack_from('<8H', b, 0))
    n_r, n_k, off_k, size = (h[1] & 0xFF) - 1, h[1] >> 8, h[2], h[3]
    res = {'motion': h[0], 'header': h, 'ok': False}
    keyed, p = [], off_k
    for _ in range(n_k):
        fr, op = struct.unpack_from('<HH', b, p)
        sz = _entry_size_keyed(op)
        keyed.append({'frame': fr, 'op': op, 'raw': b[p:p + sz].hex()})
        p += sz
    res['keyed'] = keyed
    for pre in range(4, off_k - 16 + 1, 4):
        p, ranged, good = 16 + pre, [], True
        for _ in range(max(n_r, 0)):
            if p + 6 > off_k:
                good = False; break
            s, e, op = struct.unpack_from('<HHH', b, p)
            sz = _entry_size_ranged(op)
            ranged.append({'start': s, 'end': e, 'op': op, 'raw': b[p:p + sz].hex()})
            p += sz
        if good and p == off_k:
            res.update(preamble=b[16:16 + pre].hex(), ranged=ranged, ok=True)
            return res
    res.update(preamble=b[16:off_k].hex(), ranged=[], ok=False)
    return res


def encode_events(ev) -> bytes:
    """Inverse of decode_events (recomputes counts, offsets and size)."""
    if not ev.get('ok'):
        raise ValueError('event script was not fully decoded; edit raw bytes instead')
    if not ev['ranged'] and (ev.get('short') or not ev.get('preamble')):
        keyed = b''.join(bytes.fromhex(e['raw']) for e in ev['keyed'])
        return struct.pack('<4H', ev['header'][0], len(ev['keyed']) << 8, 8, 8 + len(keyed)) + keyed
    pre = bytes.fromhex(ev['preamble'])
    ranged = b''.join(bytes.fromhex(e['raw']) for e in ev['ranged'])
    keyed = b''.join(bytes.fromhex(e['raw']) for e in ev['keyed'])
    h = list(ev['header'])
    h[1] = (len(ev['keyed']) << 8) | (len(ev['ranged']) + 1)
    h[2] = 16 + len(pre) + len(ranged)
    h[3] = h[2] + len(keyed)
    return struct.pack('<8H', *h) + pre + ranged + keyed


# ------------------------------------------------------------------ extra draw bones start hidden
HIDE_OP = 0x011C


KEYED_HIDE_OP = 0x010C     # keyed one-shot hide (keyed handler FUN_0882CEC0, table 0x089C7B08[12] -> FUN_088372D0)
KEYED_SHOW_OP = 0x010B     # keyed one-shot show (table[11] -> FUN_088372B0)
RANGED_CAP = 16            # ranged events an actor runs at once (retail Bartz peaks at exactly 16)


def _vis_bone(e):
    raw = bytes.fromhex(e['raw'])
    return struct.unpack_from('<I', raw, 12)[0] if e['op'] in (0x011B, HIDE_OP) and len(raw) >= 16 else None


def ranged_concurrency(ev):
    """Most ranged events (header slot included) active at one tick."""
    h = ev['header']
    iv = [(h[4], h[5])] + [(e['start'], e['end']) for e in ev['ranged']]
    return max(sum(1 for s, e in iv if s <= t < e) for t in sorted({s for s, _ in iv}))


def normalize_visibility(ms, bones, drop=()):
    """Visibility events in the form the engine runs (in-game log 2026-10-20): an actor runs at most RANGED_CAP ranged
    events at a time, extra ones never run; a ranged 0x011C hide shows the bone again when it ends (also at every
    motion change). So per timeline: no ranged 0x011C for `bones` (the header slot is left as it is); a keyed one-shot
    hide (KEYED_HIDE_OP at frame 0) for each of `bones` the timeline does not show at tick 0; shows stay ranged 0x011B
    (their end hides the bone again). `drop` bones lose all their 0x011B/0x011C/keyed events (parts whose skin bones
    only scale up inside their own motions: never registered = drawn always, invisible at rest scale 0).
    Returns {'changed': [motions], 'concurrency': {motion: n}}; asserts nothing exceeds RANGED_CAP."""
    bones, drop = set(bones) - set(drop), set(drop)
    allb = bones | drop
    out, changed, cc = [], [], {}
    for b in ms.events:
        ev = decode_events(b)
        if not ev['ok']:
            out.append(b); continue
        h, pre = ev['header'], bytes.fromhex(ev.get('preamble', ''))
        shown0 = set()
        if h[6] == 0x011B and len(pre) >= 8 and h[4] <= 0 < h[5]:
            shown0.add(struct.unpack_from('<I', pre, 4)[0])
        keep = []
        for e in ev['ranged']:
            bn = _vis_bone(e)
            if bn is not None and ((e['op'] == HIDE_OP and bn in allb) or (e['op'] == 0x011B and bn in drop)):
                continue
            if bn is not None and e['op'] == 0x011B and e['start'] <= 0 < e['end']:
                shown0.add(bn)
            keep.append(e)
        kold = []
        for e in ev['keyed']:
            raw = bytes.fromhex(e['raw'])
            if (e['op'] & 0xFF) in (0x0B, 0x0C) and len(raw) >= 12 and struct.unpack_from('<I', raw, 8)[0] in allb:
                continue
            kold.append(e)
        hides = [{'frame': 0, 'op': KEYED_HIDE_OP, 'raw': struct.pack('<HHII', 0, KEYED_HIDE_OP, 0, bn).hex()}
                 for bn in sorted(bones - shown0)]
        new = dict(ev, ranged=keep, keyed=hides + kold)
        nb = encode_events(new)
        chk = decode_events(nb)
        assert chk['ok'] and len(chk['keyed']) <= 255
        cc[ev['motion']] = ranged_concurrency(chk)
        assert cc[ev['motion']] <= RANGED_CAP, (ev['motion'], cc[ev['motion']])
        if nb != b:
            changed.append(ev['motion'])
        out.append(nb)
    ms.events = out
    return {'changed': changed, 'concurrency': cc}


def hide_draw_bones(ms, bones, skip_motions=()):
    """Make extra draw bones start hidden in every motion (they are drawn unconditionally unless registered in the
    character's visibility table). Since 2026-10-20 this is normalize_visibility(): a keyed one-shot hide at frame 0
    in every timeline that does not show the bone at tick 0, and no ranged 0x011C (those took the 16 ranged-event
    slots and re-showed the bone when they ended; the earlier whole-motion / gap 0x011C version is gone).
    `skip_motions` is ignored. Returns (changed motions, [], [])."""
    r = normalize_visibility(ms, bones)
    return r['changed'], [], []
