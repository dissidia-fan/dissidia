"""Dissidia 012 projectile table ('magi' section of obj/p_<code>.objx) - read, edit, write.

Layout (all little-endian):
  header  : char[4] 'MAGI', u32 version (0x20101011), u32 n_records, u32 n_floats
  records : n_records x 204 bytes, sorted by id (the game binary-searches them: FUN_088289D0)
  pool    : n_floats x f32 constants shared by every record's parameters

Each record carries 46 parameters as (lo, hi, step) byte triples at +0x40 that index the
float pool (signed byte loads in the game -> keep the pool <= 128 entries). When an emitter is
created the game draws every parameter uniformly in [pool[lo], pool[hi]]; for every following
shot of the same emitter a parameter whose pool[step] != 0 is incremented by pool[step]
instead of being redrawn (FUN_08AD0694). Most records use lo == hi (a constant).

See MAGI_NOTES.md (psprecomp/work/magi_re) for the evidence behind each field name.

Typical use:
    from dfx import magi, objx
    arc = objx.Arc.from_bytes(open('p_fiv100.objx','rb').read())
    m = magi.Magi.from_bytes(arc.get('magi').data)
    r = m.clone_record(473, 3000)            # copy record id 473 as id 3000
    m.set_param(r, 'speed', 2.5)             # adds 2.5 to the pool if needed
    m.set_param(r, 'turn_rate', 3.0)
    r.flags |= magi.FLAG['PIERCE']
    arc.get('magi').data = m.to_bytes()
"""
from __future__ import annotations
import struct

MAGIC = b'MAGI'
VERSION = 0x20101011
REC_SIZE = 204
NPARAM = 46
PARAM_OFF = 0x40
POOL_MAX = 128          # trip bytes are loaded with lb (signed)
NONE = 0xFFFF

# ------------------------------------------------------------------ fixed fields
# (offset, struct fmt, name). Names with unk_ are unknown. Links hold record *ids* (-1 = none).
FIELDS = [
    (0x00, 'H', 'id'),
    (0x02, 'H', 'kind'),             # 0 = reflectable by guard, 1-3 = not reflectable (destroyed), 4 = attached class (0x140 bytes)
    (0x04, 'H', 'effect'),           # effect index in the owner's effe bank (bullet visual), 0xFFFF none
    (0x06, 'H', 'common_effect'),    # effect id in the shared/global bank for the 2nd visual, 0xFFFF = reuse 'effect'
    (0x08, 'H', 'max_alive'),        # max simultaneous bullets of this id per owner (0 = unlimited)
    (0x0a, 'H', 'atkp'),             # hit profile (atkp record id), 0xFFFF = no hitbox
    (0x0c, 'H', 'atkp_reflected'),   # atkp used after the projectile was reflected (0xFFFF = use atkp)
    (0x0e, 'h', 'link_emit'),        # child fired by the bullet: at birth / every interval (P30..P32) / at death (FLAG DEATH_EMIT)
    (0x10, 'h', 'link_terrain'),     # child fired on terrain impact (default)
    (0x12, 'h', 'link_terrain_1'),   # ... override for collision type 1
    (0x14, 'h', 'link_terrain_2'),   # ... override for collision type 2
    (0x16, 'h', 'link_terrain_4'),   # ... override for collision type 4
    (0x18, 'h', 'link_hit'),         # child fired when the bullet hits a character
    (0x1a, 'h', 'link_fall'),        # child fired when a ground-hugging bullet loses the floor
    (0x1c, 'h', 'link_clash'),       # child fired when this bullet is cancelled by a clashing projectile
    (0x1e, 'H', 'origin'),           # spawn origin mode 0..13 (0 owner, 1/2 owner bone, 5 emitter, 8/9 target ...)
    (0x20, 'H', 'origin_arg'),       # bone / sub-object index for 'origin'
    (0x22, 'H', 'direction'),        # launch direction mode 0..4 (0 keep, 1 owner facing, 2 owner+32, 3 emitter dir, 4 aim at target)
    (0x24, 'h', 'orient'),           # basis rotation before offsets (-1 none, 0 / 1 / 2)
    (0x26, 'H', 'target'),           # homing / aim target 0..5 (0 lock-on target, 1 owner, 4 partner, 5 stored target ...)
    (0x28, 'H', 'motion'),           # 0 straight, 1 homing, 2 parked (no self-motion), 3 attached to origin each frame
    (0x2a, 'H', 'unk_2a'),           # always 0 on the disc, no reader found (padding)
    (0x2c, 'f', 'hit_radius'),       # hit sphere radius (x size)
    (0x30, 'f', 'hit_length'),       # 0 = sphere, else capsule length along the direction (x size)
    (0x34, 'f', 'bounce_speed'),     # speed multiplier applied at each terrain bounce (FLAG BOUNCE)
    (0x38, 'I', 'flags'),
    (0x3c, '4s', 'ai_tags'),         # byte 0 read (signed) by the CPU AI threat code; list ends with 0xFF
]
LINKS = ['link_emit', 'link_terrain', 'link_terrain_1', 'link_terrain_2', 'link_terrain_4',
         'link_hit', 'link_fall', 'link_clash']

FLAG = {
    'PIERCE':          0x00000001,  # survives hitting a character (else destroyed on hit)
    'NO_TERRAIN':      0x00000002,  # no terrain collision (passes through walls / floor)
    'GROUND_HUG':      0x00000004,  # stays on the floor (y snapped, dir.y = 0); dies + link_fall if the floor drops > 1
    'FX_FOLLOW_DIR':   0x00000008,  # visual is oriented along the travel direction (FUN_08AD20BC/247C)
    'UNK_10':          0x00000010,  # sets hit-object flag 0x80 (guard-hit sound 0xD09 path)
    'DEATH_EMIT':      0x00000020,  # fire link_emit at death instead of at birth
    'FX_ALIGN_FLOOR':  0x00000040,  # visual aligned to the floor normal under it (raycast FUN_08AD45D8)
    'NO_EFFECT2':      0x00000080,  # don't spawn the secondary visual
    'NO_EFFECT':       0x00000100,  # don't spawn the primary visual
    'NO_VISUALS':      0x00000200,  # skip visual setup entirely
    'UNK_400':         0x00000400,  # passed to the hit object constructor
    'BOUNCE':          0x00000800,  # reflect off terrain (up to 5 iterations / frame), speed *= bounce_speed
    'SWEEP_ALT':       0x00001000,  # alternate terrain sweep (sphere sweep; also ground snap of spawn point)
    'HOMING_XZ':       0x00002000,  # homing only turns in the horizontal plane
    'HOMING_Y':        0x00004000,  # homing only changes the vertical component
    'CLASH_ATTACKER':  0x00008000,  # can cancel bullets flagged CLASH_VICTIM
    'CLASH_VICTIM':    0x00010000,  # can be cancelled by a CLASH_ATTACKER bullet (spawns link_clash)
    'INHERIT_SIZE':    0x00020000,  # size taken from the parent bullet instead of P25
    'CANCEL_ON_DAMAGE':0x00040000,  # emitter stops when the owner is in a damage state (action 500..798)
    'STOP_ON_FAIL':    0x00080000,  # emitter stops when a shot fails to spawn
    'CANCEL_ON_ACTION':0x00100000,  # emitter stops when the owner's action changes
    'LIMIT_NEWEST':    0x00200000,  # max_alive removal order variant
    'PULL_FIELD':      0x00400000,  # bullet drags opponents (P37..P42)
    'PULL_COND':       0x00800000,  # pull only while the victim passes FUN_0883809C
    'GIVE_EFFECT':     0x01000000,  # death child inherits the bullet's lingering effect
    'COLL_MASK_ALT':   0x02000000,  # alternate terrain collision mask
    'NO_IMPACT_FX':    0x04000000,  # no terrain-material (water, mat 0x1E/0x1F) impact effect
    'HOMING_STOP':     0x08000000,  # stop moving when homing reaches the stop distance
    'SPAWN_LOS':       0x10000000,  # check line of sight owner -> spawn point
    'CANCEL_COND1':    0x20000000,  # emitter stops on owner sub-state (+392 & 0x10 == 0)
    'CANCEL_COND2':    0x40000000,  # emitter stops when owner sub-object +796 <= 0
    'UNK_80000000':    0x80000000,  # sets hit object flag 2
}

# ------------------------------------------------------------------ the 46 parameters
# index -> name. 'E' = emitter (read once per emitter / shot), 'S' = spawn transform,
# 'B' = copied into the bullet at birth (FUN_08AD46F0). Angles in degrees, times in game ticks.
PARAMS = [
    'shots',            # 0  E  number of bullets the emitter fires (trunc)
    'first_delay',      # 1  E  ticks before the first shot
    'interval',         # 2  E  ticks between shots
    'fwd_dist',         # 3  S  spawn distance along the (yawed) forward axis
    'fwd_yaw',          # 4  S  yaw of that forward offset (deg)
    'off_x',            # 5  S  local spawn offset x
    'off_y',            # 6  S  local spawn offset y
    'off_z',            # 7  S  local spawn offset z
    'launch_pitch',     # 8  S  launch direction rotation 1 (deg)
    'launch_yaw',       # 9  S  launch direction rotation 2 (deg)
    'aim_dist',         # 10 B  aim point offset distance from the target
    'aim_yaw',          # 11 B  aim point offset angle (deg)
    'aim_height',       # 12 B  aim point height offset
    'move_time',        # 13 B  bullet moves only while age <= this (0 = always)
    'speed',            # 14 B  initial speed (units / tick)
    'speed_min',        # 15 B  speed clamp min
    'speed_max',        # 16 B  speed clamp max
    'accel',            # 17 B  speed change per tick
    'accel_start',      # 18 B  age at which accel starts
    'turn_rate',        # 19 B  homing turn rate (deg / tick)
    'homing_start',     # 20 B  age at which homing starts
    'homing_time',      # 21 B  homing duration (0 = until stop distance)
    'homing_stop',      # 22 B  homing ends when closer to the target than this
    'gravity',          # 23 B  downward acceleration
    'gravity_start',    # 24 B  age at which gravity starts
    'size',             # 25 B  scale (multiplies hit_radius / hit_length)
    'size_min',         # 26 B  size clamp min
    'size_max',         # 27 B  size clamp max
    'size_rate',        # 28 B  size change per tick
    'size_start',       # 29 B  age at which size_rate applies
    'emit_count',       # 30 B  number of link_emit children fired in flight (0 = one at birth)
    'emit_delay',       # 31 B  age of the first in-flight child
    'emit_interval',    # 32 B  interval between in-flight children (the bullet reads the raw triple)
    'tgt_dist_min',     # 33 S  target-relative spawn: horizontal distance clamp min
    'tgt_dist_max',     # 34 S  ... max
    'tgt_height_min',   # 35 S  ... vertical clamp min
    'tgt_height_max',   # 36 S  ... max
    'pull_start',       # 37 B  pull field start age (with pull_time 0 = always)
    'pull_time',        # 38 B  pull field duration
    'pull_strength',    # 39 B  pull displacement at full strength
    'pull_full_dist',   # 40 B  distance at which the pull reaches full strength
    'pull_min_dist',    # 41 B  pull acts from this distance ...
    'pull_max_dist',    # 42 B  ... up to this distance
    'lifetime',         # 43 B  bullet dies at this age
    'hit_time',         # 44 B  hitbox stays active this long
    'hit_delay',        # 45 B  hitbox appears at this age
]
PARAM_INDEX = {n: i for i, n in enumerate(PARAMS)}
# value a parameter has when it 'does nothing' (used by dumps to hide defaults)
PARAM_DEFAULT = {n: 0.0 for n in PARAMS}
PARAM_DEFAULT.update(shots=1.0, size=1.0, size_min=1.0, size_max=1.0,
                     tgt_dist_max=9999.0, tgt_height_max=9999.0, hit_time=9999.0)
assert len(PARAMS) == NPARAM

_FMT = '<' + ''.join(f for _, f, _ in FIELDS)
_FOFF = {n: (o, f) for o, f, n in FIELDS}


def _f32(v):
    return struct.unpack('<f', struct.pack('<f', v))[0]


class Record:
    """One 204-byte record. Fixed fields are properties; params are [lo, hi, step] pool indices."""

    def __init__(self, raw: bytes):
        assert len(raw) == REC_SIZE
        self.raw = bytearray(raw)

    def __getattr__(self, name):
        fo = _FOFF.get(name)
        if fo is None:
            raise AttributeError(name)
        o, f = fo
        v = struct.unpack_from('<' + f, self.raw, o)[0]
        return v

    def __setattr__(self, name, value):
        if name == 'raw':
            object.__setattr__(self, name, value)
            return
        fo = _FOFF.get(name)
        if fo is None:
            raise AttributeError(name)
        o, f = fo
        struct.pack_into('<' + f, self.raw, o, value)

    # ---- params
    def trip(self, p):
        i = p if isinstance(p, int) else PARAM_INDEX[p]
        o = PARAM_OFF + 3 * i
        return [self.raw[o], self.raw[o + 1], self.raw[o + 2]]

    def set_trip(self, p, lo, hi, step):
        i = p if isinstance(p, int) else PARAM_INDEX[p]
        o = PARAM_OFF + 3 * i
        for k, v in enumerate((lo, hi, step)):
            if not 0 <= v < POOL_MAX:
                raise ValueError('pool index %d out of range' % v)
            self.raw[o + k] = v

    @property
    def tail(self):
        return bytes(self.raw[0xCA:0xCC])

    def fields(self):
        return {n: getattr(self, n) for _, _, n in FIELDS}

    def flag_names(self):
        f = self.flags
        return [k for k, v in FLAG.items() if f & v]

    def links(self):
        return {n: getattr(self, n) for n in LINKS if getattr(self, n) != -1}

    def __repr__(self):
        return '<magi.Record id=%d atkp=%d>' % (self.id, self.atkp)


class Magi:
    def __init__(self, records=None, pool_bits=None, version=VERSION):
        self.version = version
        self.records = records or []
        self.pool_bits = pool_bits or []     # f32 as raw u32 so round trip is bit exact

    # ---- io
    @classmethod
    def from_bytes(cls, d: bytes):
        magic, ver, n, nf = struct.unpack_from('<4sIII', d, 0)
        if magic != MAGIC:
            raise ValueError('not a MAGI table')
        end = 16 + n * REC_SIZE
        if end + nf * 4 != len(d):
            raise ValueError('size mismatch: %d records %d floats vs %d bytes' % (n, nf, len(d)))
        recs = [Record(d[16 + i * REC_SIZE:16 + (i + 1) * REC_SIZE]) for i in range(n)]
        pool = list(struct.unpack_from('<%dI' % nf, d, end))
        return cls(recs, pool, ver)

    def to_bytes(self) -> bytes:
        out = bytearray(struct.pack('<4sIII', MAGIC, self.version, len(self.records), len(self.pool_bits)))
        for r in self.records:
            out += r.raw
        out += struct.pack('<%dI' % len(self.pool_bits), *self.pool_bits)
        return bytes(out)

    @classmethod
    def from_objx(cls, arc):
        e = arc.get('magi')
        return cls.from_bytes(e.data) if e else None

    def into_objx(self, arc):
        arc.get('magi').data = self.to_bytes()

    # ---- pool
    @property
    def pool(self):
        return [struct.unpack('<f', struct.pack('<I', b))[0] for b in self.pool_bits]

    def pool_value(self, i):
        return struct.unpack('<f', struct.pack('<I', self.pool_bits[i]))[0]

    def pool_index(self, value, add=True):
        """Index of an exactly equal f32 in the pool; appended when missing (add=True)."""
        bits = struct.unpack('<I', struct.pack('<f', value))[0]
        if bits in self.pool_bits:
            return self.pool_bits.index(bits)
        if not add:
            return None
        if len(self.pool_bits) >= POOL_MAX:
            raise ValueError('float pool full (%d entries; trip bytes are signed)' % POOL_MAX)
        self.pool_bits.append(bits)
        return len(self.pool_bits) - 1

    # ---- records
    def ids(self):
        return [r.id for r in self.records]

    def find(self, rid):
        for r in self.records:
            if r.id == rid:
                return r
        return None

    def index_of(self, rid):
        for i, r in enumerate(self.records):
            if r.id == rid:
                return i
        return -1

    def link_indices(self, rec):
        """link field -> record index (or None when the id is not in this table)."""
        out = {}
        for n in LINKS:
            v = getattr(rec, n)
            if v != -1:
                i = self.index_of(v)
                out[n] = i if i >= 0 else None
        return out

    def param(self, rec, p):
        """(lo, hi, step) as float values."""
        lo, hi, st = rec.trip(p)
        return self.pool_value(lo), self.pool_value(hi), self.pool_value(st)

    def params(self, rec):
        return {n: self.param(rec, i) for i, n in enumerate(PARAMS)}

    def set_param(self, rec, p, value=None, lo=None, hi=None, step=None):
        """Set a parameter. value sets lo=hi=value (step unchanged unless given).
        Missing constants are appended to the float pool."""
        cur = rec.trip(p)
        if value is not None:
            lo = hi = value
        new = list(cur)
        if lo is not None:
            new[0] = self.pool_index(_f32(lo))
        if hi is not None:
            new[1] = self.pool_index(_f32(hi))
        if step is not None:
            new[2] = self.pool_index(_f32(step))
        rec.set_trip(p, *new)
        return new

    def insert(self, rec):
        """Insert keeping the table sorted by id (the game binary-searches it)."""
        if self.find(rec.id) is not None:
            raise ValueError('id %d already present' % rec.id)
        i = 0
        while i < len(self.records) and self.records[i].id < rec.id:
            i += 1
        self.records.insert(i, rec)
        return rec

    def clone_record(self, src_id, new_id, clear_links=False):
        src = self.find(src_id)
        if src is None:
            raise KeyError(src_id)
        r = Record(bytes(src.raw))
        r.id = new_id
        if clear_links:
            for n in LINKS:
                setattr(r, n, -1)
        return self.insert(r)

    def remove(self, rid):
        self.records.pop(self.index_of(rid))

    def compact_pool(self):
        """Drop unused pool entries and renumber the triples (pool[0]/[1] kept)."""
        used = {0, 1}
        for r in self.records:
            used.update(r.raw[PARAM_OFF:PARAM_OFF + 3 * NPARAM])
        order = sorted(used)
        remap = {o: n for n, o in enumerate(order)}
        for r in self.records:
            for k in range(PARAM_OFF, PARAM_OFF + 3 * NPARAM):
                r.raw[k] = remap[r.raw[k]]
        self.pool_bits = [self.pool_bits[o] for o in order]

    # ---- json-friendly view
    def to_dict(self):
        out = {'version': self.version, 'pool': self.pool, 'records': []}
        for r in self.records:
            d = r.fields()
            d['ai_tags'] = r.ai_tags.hex()
            d['flags'] = '0x%08x' % r.flags
            d['flag_names'] = r.flag_names()
            d['tail'] = r.tail.hex()
            d['params'] = {n: r.trip(i) for i, n in enumerate(PARAMS)}
            d['values'] = {n: list(self.param(r, i)) for i, n in enumerate(PARAMS)}
            out['records'].append(d)
        return out

    @classmethod
    def from_dict(cls, j):
        m = cls([], [struct.unpack('<I', struct.pack('<f', v))[0] for v in j['pool']], j['version'])
        for d in j['records']:
            r = Record(bytes(REC_SIZE))
            for _, f, n in FIELDS:
                v = d[n]
                if n == 'ai_tags':
                    v = bytes.fromhex(v)
                elif n == 'flags' and isinstance(v, str):
                    v = int(v, 16)
                setattr(r, n, v)
            for n, t in d['params'].items():
                r.set_trip(n, *t)
            r.raw[0xCA:0xCC] = bytes.fromhex(d.get('tail', '0000'))
            m.records.append(r)
        return m


def parse(d: bytes) -> Magi:
    return Magi.from_bytes(d)


def clone_record(m: Magi, src_id, new_id, clear_links=False):
    return m.clone_record(src_id, new_id, clear_links)


def set_param(m: Magi, rec, name, value=None, **kw):
    return m.set_param(rec, name, value, **kw)
