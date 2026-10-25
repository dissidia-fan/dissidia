"""Character moveset (obj/p_<code>.objx) <-> a folder of editable files. Lossless: unpack then pack gives the same bytes.

Folder layout written by unpack():
  arc.json            entry order of the ARC container (which file holds each entry)
  moves.json          comb: commands -> steps (action ids). One entry per command id (an ability's 'cmd').
  actions.json        plya: action records (action id, script id; the rest as hex)
  motion_map.json     script id -> motion (timeline / animation) index
  timelines/NNN.json  per-motion event script: ranged events {start, end, op, args} and keyed events {frame, op, args}
                      (times in ticks, 2 per animation frame). Args are ints, or {"f": x} for floats.
  hits.json           atkp: hit profiles with named fields (power, rehit, reaction, knock-back ...)
  projectiles.json    magi: projectile records (field names: charmod_magi.py)
  sections/*.bin      everything else, unchanged (effects 'effe', sound bank 'snd_', 'brat', 'spec', ...)
Keys "_info", "what" and "used_by" are notes and are ignored when packing.
"""
import json
import math
import os
import struct
import sys

import charmod_magi as magi

ALIGN = {14: 64, 2: 16, 3: 16}


# ---------------------------------------------------------------- ARC container
def arc_read(d):
    assert d[:4] == b'ARC\x01', 'not an objx'
    n = struct.unpack_from('<I', d, 4)[0]
    ents = []
    for i in range(n):
        t, tag, o, s = struct.unpack_from('<I4sII', d, 16 + i * 16)
        ents.append((t, tag, d[o:o + s]))
    return d[8:16], ents


def arc_write(extra, ents):
    n = len(ents)
    table, body = bytearray(), bytearray()
    o = 16 + n * 16
    for t, tag, data in ents:
        pad = (-o) % ALIGN.get(t, 4)
        body += b'\0' * pad
        o += pad
        table += struct.pack('<I4sII', t, tag, o, len(data))
        body += data
        o += len(data)
    return b'ARC\x01' + struct.pack('<I', n) + extra + bytes(table) + bytes(body)


# ---------------------------------------------------------------- argument values
def f32_short(f):
    """Shortest decimal that packs to the same float32 (1.2999999523 -> 1.3)."""
    bits = struct.pack('<f', f)
    for p in range(1, 10):
        x = float('%.*g' % (p, f))
        if struct.pack('<f', x) == bits:
            return x
    return f


def _v_out(v):
    if v < 0x10000 or v >= 0xFFFF0000:
        return v
    f = struct.unpack('<f', struct.pack('<I', v))[0]
    if math.isfinite(f) and 1e-6 < abs(f) < 1e7:
        return {'f': f32_short(f)}
    return v


def _v_in(v):
    if isinstance(v, str):                       # "0x04A1" etc.
        return int(v, 0) & 0xFFFFFFFF
    if isinstance(v, dict):
        return struct.unpack('<I', struct.pack('<f', v['f']))[0]
    return int(v) & 0xFFFFFFFF


# Event op notes (from the mod logs; '?' = inferred). Informative only.
OPS = {
    0x0200: 'hit window {hit id (hits.json), hit group}', 0x0204: 'fire projectile {projectile id, ?}',
    0x011B: 'show draw bone while active {bone}', 0x011C: 'hide draw bone while active {bone}',
    0x010B: 'show draw bone once {bone}', 0x010C: 'hide draw bone once {bone}', 0x0101: 'combo chain window',
    0x0303: 'sound', 0x0306: 'screen shake', 0x0402: 'snap to face the target', 0x0925: 'weapon trail',
    0x0219: 'own effect', 0x0112: 'weapon volume (no damage)', 0x0C65: 'summon object', 0x075A: 'hold prop',
    0x0002: 'end of move', 0x0013: 'move control ?', 0x002D: 'plunge / fall ?', 0x0120: 'after blow-away ?',
}


# Events with friendly names: op code -> (name, names of its first args). Other args stay in "args".
NAMED = {
    0x0200: ('hit_window', ['hit', 'group']), 0x0204: ('fire_projectile', ['projectile']),
    0x0219: ('effect', ['effect']), 0x0303: ('sound', ['sound']), 0x0306: ('screen_shake', []),
    0x011B: ('show_part', ['part']), 0x011C: ('hide_part', ['part']),
    0x010B: ('show_part_once', ['part']), 0x010C: ('hide_part_once', ['part']),
    0x0101: ('chain_window', []), 0x0402: ('face_target', []), 
    0x0112: ('weapon_volume', []), 0x0C65: ('summon_object', []), 0x075A: ('hold_prop', []), 0x0002: ('end_of_move', []),
    # traced in the game code 2026-10-23 (project notes: dissidia-012-event-ops.md)
    0x0005: ('can_act', []), 0x041E: ('can_act_now', []), 0x0028: ('dodge_cancel', []),
    0x0059: ('dodge_cancel_if_foe_free', []), 0x0022: ('attack_over_cpu', []), 0x0029: ('cpu_hint', []),
    0x0006: ('invincible', []), 0x006A: ('no_assist_cancel', []), 0x0049: ('cant_be_pushed', []),
    0x0032: ('follow_floor_slope', []), 0x000A: ('air_motion', []), 0x0015: ('ground_follow', []),
    0x002D: ('landing_allowed', []), 0x0023: ('keep_motion_when_falling', []),
    0x040B: ('turn_to_opponent', ['max_deg_per_tick']),
    0x0120: ('chase_window', ['hit']), 0x0467: ('chain_followup', ['flag', 'action', 'even_on_miss', 'mid_action']),
    0x042A: ('bone_effect', ['slot', 'record', 'record_bit', 'scale']), 0x0214: ('face_texture', ['face_part', 'pattern']),
    0x0227: ('pin_physics_bone', ['part', 'max_deg_per_tick']), 0x030C: ('motion_camera', ['preset', 'camera_bone']),
    0x0315: ('voice', ['voice', 'only_if_alt_off', 'only_if_alt_on']), 0x060F: ('footstep_sound', ['kind']),
    0x0335: ('summon_effect', ['effect', 'lifetime', 'part']), 0x0108: ('forced_hit', ['hit']),
    0x0422: ('ground_dust', ['part', 'offset', 'size', 'variant']),
    0x0D10: ('homing_dash', ['speed', 'track_ticks', 'height', 'angle', 'distance', 'arrive_radius', 'max_ticks',
                             'vspeed']),
}
# round 2 (all 30 retail fighters, 2026-10-23): see docs/moves-and-events.md. Ops whose ranged and keyed forms differ
# (0x0037, 0x0117) stay as hex.
NAMED.update({
    0x0216: ('pin_physics_group', ['group', 'max_deg_per_tick']), 0x0126: ('slow_foe_exburst', ['speed']),
    0x0013: ('no_body_collision', []), 0x0548: ('lunge', ['pitch', 'yaw', 'speed', 'only_without_target', 'after_homing']),
    0x0811: ('turn_or_swivel', ['deg_per_tick', 'away', 'swivel']),
    0x046D: ('turn_to_target', ['deg_per_tick', 'only_if_reeling', 'even_if_evading', 'skip_if_reeling']),
    0x0743: ('hit_window_fire', ['hit', 'group', 'projectile']), 0x003A: ('dodge_attack_cancel', []),
    0x0036: ('brave_attack_allowed', []), 0x0518: ('steer_during_move', ['turn_mult', 'speed_mult']),
    0x0047: ('invincible_with_ability', []), 0x0074: ('perfect_dodge_window', []),
    0x017C: ('wakeup_invincible', ['ticks']), 0x001D: ('chase_dash', []),
    0x044F: ('ground_slide', ['angle', 'speed', 'skip_if_target', 'after_homing']),
    0x0145: ('perfect_guard_window', ['off']), 0x0210: ('face_flight_direction', ['deg_per_tick', 'backwards']),
    0x004B: ('heavy_body', []), 0x072C: ('feathers', ['rate', 'chance']),
    0x0466: ('timed_effect', ['effect', 'lifetime', 'tick', 'part']), 0x0278: ('chain_window_if_hit', ['cpu_flag', 'hit']),
    0x0038: ('stop_momentum', []), 0x0062: ('root_motion_3d', []),
    0x0353: ('screen_shake_range', ['duration', 'x_amp', 'y_amp']),
    0x0539: ('pull_foe', ['speed', 'outer_radius', 'inner_radius', 'only_if_reeling', 'only_if_not_reeling']),
    0x0134: ('slow_motion_exburst', ['speed']), 0x0133: ('dim_stage', ['brightness']), 0x0030: ('floating_physics', []),
    0x025E: ('prop_hit_volume', ['slot', 'prop']), 0x045B: ('prop_hit', ['hit', 'prop', 'slot', 'modifier']),
    0x055F: ('prop_effect', ['slot', 'record']), 0x0360: ('prop_fade', ['prop', 'fade_in', 'fade_out']),
    0x0D64: ('aim_bones_at_target', []), 0x0D63: ('aim_bones_at_foe', []),
    0x047A: ('can_act_now_if', []), 0x003E: ('exburst_free_victim', []),
    0x0340: ('fade_self', ['fade_out', 'fade_back', 'opacity']), 0x002F: ('counts_as_evading', []),
    0x0524: ('button_shot', ['projectile', 'arg', 'cooldown', 'slot', 'button']),
    0x0069: ('force_assist_cancel', []), 0x027B: ('guard_counter_hit', ['hit', 'group']),
    0x0579: ('chain_followup_if_hit', []), 0x004A: ('no_jump', []), 0x013B: ('gravity', ['multiplier']),
    0x012B: ('white_flash', []), 0x022E: ('trail_length', ['part', 'extent']),
    0x0475: ('pulse_tint', ['r', 'g', 'b', 'period']), 0x0476: ('pulse_tint_partner', ['r', 'g', 'b', 'period']),
    0x086E: ('turn_to_target_or_aim', ['mode', 'away']), 0x0158: ('act_after_guard', []),
    0x016C: ('dash_cancel', ['only_after_hit']), 0x0072: ('can_guard', []), 0x004C: ('timed_input_latch', []),
    0x003D: ('no_shadow', []), 0x0173: ('assist_face_partner', []), 0x0652: ('steer_lean', []),
    0x0031: ('exburst_release', []), 0x0070: ('ground_contact_switch', []),
    # keyed
    0x0000: ('enter_state', []), 0x0311: ('sound_if_part_visible', ['draw_part', 'sound', 'positional']),
    0x0312: ('sound_if_form', ['form', 'sound']), 0x0123: ('take_off', ['only_without_target']),
    0x0925: ('ground_homing_dash', ['speed', 'track_ticks', 'angle', 'distance', 'arrive_radius', 'max_ticks']),
    0x0121: ('set_foe_speed', ['percent']), 0x0131: ('battle_message', ['value']), 0x021D: ('caption', ['image']),
    0x051B: ('screen_flash', ['fade_in', 'hold', 'fade_out', 'colour']), 0x002B: ('snap_yaw_foe_line', []),
    0x011E: ('switch_form', ['form']), 0x0220: ('fade_foe_out', ['ticks', 'floor_percent']),
    0x0633: ('shot_by_presses', ['projectile', 'arg', 'count', 'if_le', 'if_eq', 'if_ge']),
    0x011F: ('fade_foe_in', ['ticks']), 0x042D: ('projectile_variant', ['projectile', 'arg', 'alt_projectile', 'alt_arg']),
    0x0024: ('stop_turning', []), 0x0B36: ('timed_buff', []), 0x032E: ('rare_variant_roll', ['min', 'unlock_bit', 'one_in']),
    0x042F: ('effect_variant', ['effect', 'lifetime', 'alt_effect', 'alt_lifetime']), 0x0109: ('fade_self_in', ['ticks']),
})
BY_NAME = {v[0]: k for k, v in NAMED.items()}
BY_NAME['weapon_trail'] = 0x0925          # old name (it is a ground homing dash, not a trail)


class Ctx:
    """Name tables of one moveset folder: hits.json names, bones.json (the model's bone names)."""

    def __init__(self, d=None):
        self.hit_name, self.hit_id, self.bone_name, self.bone_id = {}, {}, {}, {}
        if d is None:
            return
        hp, bp = os.path.join(d, 'hits.json'), os.path.join(d, 'bones.json')
        if os.path.exists(hp):
            names = {}
            for h in rj(hp)['hits']:
                names.setdefault(h['name'], []).append(int(h['id'], 16))
            for n, ids in names.items():
                if len(ids) == 1 and n and not n.startswith('0x'):
                    self.hit_name[ids[0]], self.hit_id[n] = n, ids[0]
        if os.path.exists(bp):
            bones = rj(bp)['bones']
            for i, n in enumerate(bones):
                if bones.count(n) == 1 and n and not n.startswith('0x'):
                    self.bone_name[i], self.bone_id[n] = n, i


_CTX = Ctx()


def op_code(op):
    if op.startswith('0x'):
        return int(op, 16)
    if op in BY_NAME:
        return BY_NAME[op]
    raise ValueError('unknown event op %r: use one of %s, or a code like "0x0200"' % (op, ', '.join(sorted(BY_NAME))))


def _arg_in(name, v):
    if isinstance(v, str) and not v.startswith('0x'):
        table = {'hit': (_CTX.hit_id, 'hits.json'), 'part': (_CTX.bone_id, 'bones.json')}.get(name)
        if table is None:
            raise ValueError('%s: %r is not a number' % (name, v))
        if v not in table[0]:
            raise ValueError('%s %r is not in %s' % (name, v, table[1]))
        return table[0][v]
    return _v_in(v)


def ev_args(d):
    """-> (op code, [u32 args]) for an event in either form."""
    op = op_code(d['op'])
    names = NAMED.get(op, ('', []))[1][:op >> 8]
    vals = []
    for n in names:
        if n not in d:
            break
        vals.append(_arg_in(n, d[n]))
    vals += [_v_in(a) for a in d.get('args', [])]
    if len(vals) != op >> 8:
        raise ValueError('event %s needs %d values, has %d: %s' % (d['op'], op >> 8, len(vals), json.dumps(d)))
    return op, vals


def friendly(e):
    """Event with a hex op -> named op and named args (where known)."""
    op = int(e['op'], 16)
    if op not in NAMED:
        w = OPS.get(op)
        if w:
            e['what'] = w
        return e
    name, names = NAMED[op]
    names = names[:op >> 8]
    out = {k: e[k] for k in ('start', 'end', 'frame') if k in e}
    out['op'] = name
    args = list(e['args'])
    for i, n in enumerate(names):
        v = args[i]
        if isinstance(v, int) and n == 'hit':
            v = _CTX.hit_name.get(v, '0x%04X' % v if v < 0x10000 else v)
        elif isinstance(v, int) and n == 'part':
            v = _CTX.bone_name.get(v, v)
        out[n] = v
    if args[len(names):]:
        out['args'] = args[len(names):]
    for k in ('x', 'pad'):
        if k in e:
            out[k] = e[k]
    return out


def _ev(d, keyed):
    op, vals = ev_args(d)
    args = b''.join(struct.pack('<I', a) for a in vals)
    if keyed:
        return struct.pack('<HHI', d['frame'], op, d.get('x', 0)) + args
    return struct.pack('<HHHHI', d['start'], d['end'], op, d.get('pad', 0), d.get('x', 0)) + args


# ---------------------------------------------------------------- timelines (type 8)
def _ev_decode_empty(b):
    """A timeline with no ranged events: 8-byte header {motion, counts, keyed offset, size}, then keyed events."""
    if len(b) < 8:
        return None
    motion, cnt, off_k, size = struct.unpack_from('<4H', b, 0)
    if cnt & 0xFF or size != len(b) or off_k != 8:
        return None
    keyed, p = [], 8
    for _ in range(cnt >> 8):
        if p + 8 > len(b):
            return None
        fr, op, x = struct.unpack_from('<HHI', b, p)
        n = op >> 8
        if p + 8 + 4 * n > len(b):
            return None
        e = {'frame': fr, 'op': '0x%04X' % op, 'args': [_v_out(a) for a in struct.unpack_from(f'<{n}I', b, p + 8)]}
        if x:
            e['x'] = x
        keyed.append(e)
        p += 8 + 4 * n
    if p != len(b):
        return None
    t = {'motion': motion, 'ranged': [], 'keyed': keyed}
    return t if ev_encode(t) == b else None


def ev_decode(b):
    if len(b) >= 8 and not (struct.unpack_from('<H', b, 2)[0] & 0xFF):
        return _ev_decode_empty(b)
    if len(b) < 20:
        return None
    h = list(struct.unpack_from('<8H', b, 0))
    n_r, n_k, off_k, size = (h[1] & 0xFF) - 1, h[1] >> 8, h[2], h[3]
    if size != len(b) or n_r < 0 or off_k > len(b):
        return None
    keyed, p = [], off_k
    for _ in range(n_k):
        if p + 8 > len(b):
            return None
        fr, op, x = struct.unpack_from('<HHI', b, p)
        n = op >> 8
        if p + 8 + 4 * n > len(b):
            return None
        e = {'frame': fr, 'op': '0x%04X' % op, 'args': [_v_out(a) for a in struct.unpack_from(f'<{n}I', b, p + 8)]}
        if x:
            e['x'] = x
        keyed.append(e)
        p += 8 + 4 * n
    if p != len(b):
        return None
    # slot 0 is the header's own event: h[4] start, h[5] end, h[6] op, h[7] pad; u32 x + args follow the header
    n0 = h[6] >> 8
    if 16 + 4 + 4 * n0 > off_k:
        return None
    x0 = struct.unpack_from('<I', b, 16)[0]
    s0 = {'start': h[4], 'end': h[5], 'op': '0x%04X' % h[6],
          'args': [_v_out(a) for a in struct.unpack_from(f'<{n0}I', b, 20)]}
    if h[7]:
        s0['pad'] = h[7]
    if x0:
        s0['x'] = x0
    p = 20 + 4 * n0
    ranged = [s0]
    for _ in range(n_r):
        if p + 12 > off_k:
            return None
        s, e_, op, pad, x = struct.unpack_from('<HHHHI', b, p)
        n = op >> 8
        if p + 12 + 4 * n > off_k:
            return None
        ev = {'start': s, 'end': e_, 'op': '0x%04X' % op,
              'args': [_v_out(a) for a in struct.unpack_from(f'<{n}I', b, p + 12)]}
        if pad:
            ev['pad'] = pad
        if x:
            ev['x'] = x
        ranged.append(ev)
        p += 12 + 4 * n
    if p != off_k:
        return None
    t = {'motion': h[0], 'ranged': ranged, 'keyed': keyed}
    if ev_encode(t) != b:
        return None
    return t


def ev_encode(t):
    rs = t['ranged']
    if not rs:                                           # no ranged events: 8-byte header, keyed events from +8
        kb = b''.join(_ev(e, True) for e in t['keyed'])
        assert len(t['keyed']) <= 255
        return struct.pack('<4H', t['motion'], len(t['keyed']) << 8, 8, 8 + len(kb)) + kb
    s0 = rs[0]
    op0, a0 = ev_args(s0)
    pre = struct.pack('<I', s0.get('x', 0)) + b''.join(struct.pack('<I', a) for a in a0)
    rb = b''.join(_ev(e, False) for e in rs[1:])
    kb = b''.join(_ev(e, True) for e in t['keyed'])
    assert len(rs) <= 255 and len(t['keyed']) <= 255
    off_k = 16 + len(pre) + len(rb)
    h = [t['motion'], (len(t['keyed']) << 8) | len(rs), off_k, off_k + len(kb),
         s0['start'], s0['end'], op0, s0.get('pad', 0)]
    return struct.pack('<8H', *h) + pre + rb + kb


# ---------------------------------------------------------------- hits (atkp, 68-byte records)
HIT_FIELDS = [  # (offset, struct format, name)
    (0x00, 'H', 'id'), (0x02, 'H', 'unk_02'), (0x04, 'B', 'class'), (0x05, 'B', 'reaction'), (0x06, 'B', 'unk_06'),
    (0x07, 'b', 'unk_07'), (0x08, 'I', 'flags'), (0x0C, 'B', 'hit_effect_bit'), (0x0D, 'B', 'hitstop'),
    (0x0E, 'B', 'unk_0e'), (0x0F, 'B', 'unk_0f'), (0x10, 'h', 'power'), (0x12, 'H', 'rehit'),
    (0x14, '4s', 'shake'), (0x18, 'f', 'kb_yaw'), (0x1C, 'f', 'kb_forward'), (0x20, 'f', 'kb_up'),
    (0x24, 'f', 'stun'), (0x28, 'f', 'follow'), (0x2C, 'h', 'kb_bone'), (0x2E, 'H', 'unk_2e'), (0x30, 'I', 'unk_30')]
HIT_INFO = {
    'power': 'brave damage per hit (x the attack/defence factor). Retail single hits 3-8, finishers 10-16. Keep >= 1.',
    'rehit': '0 = one hit per hit window; N = the window can hit the same foe again every N ticks',
    'reaction': '0x0D stagger (held at kb_forward / kb_up from the attacker), 0x0B knock, 0x0C blow, 0x04 blow away, '
                '0x06 launch, 0x01 flinch / hold',
    'kb_forward': 'stagger / knock / blow: target distance in front of the attacker (m; negative = behind)',
    'kb_up': 'stagger / knock / blow: target height (m)',
    'stun': 'ticks to reach the target = hit-stun (60 ticks = 1 s)',
    'follow': 'ticks the target keeps following the attacker',
    'flags': 'bit 0x02 = HP attack (damage = your brave; power ignored), bit 0x01 = random power 1..power',
    'hit_effect_bit': 'contact effect: effect records whose mask has this bit play on hit',
    'hitstop': 'probably hit-stop (not confirmed)',
}


def hit_decode(r):
    d = {}
    for o, f, n in HIT_FIELDS:
        v = struct.unpack_from('<' + f, r, o)[0]
        if f == '4s':
            v = v.hex()
        elif f == 'f' and math.isfinite(v):
            v = f32_short(v)
        elif f == 'I':
            v = '0x%08X' % v
        elif n == 'id':
            v = '0x%04X' % v
        d[n] = v
    name = r[52:68].split(b'\0')[0].decode('latin1')
    d = {'id': d.pop('id'), 'name': name, **d}
    if r[52:68] != name.encode('latin1').ljust(16, b'\0'):
        d['name_raw'] = r[52:68].hex()
    if hit_encode(d) != r:  # float that does not survive the round trip etc.
        d['raw'] = r.hex()
    return d


def hit_encode(d):
    r = bytearray(68)
    for o, f, n in HIT_FIELDS:
        v = d[n]
        if f == '4s':
            v = bytes.fromhex(v)
        elif isinstance(v, str):
            v = int(v, 16)
        struct.pack_into('<' + f, r, o, v)
    r[52:68] = bytes.fromhex(d['name_raw']) if 'name_raw' in d else d['name'].encode('latin1')[:15].ljust(16, b'\0')
    return bytes(r)


# ---------------------------------------------------------------- json helpers
def wj(path, obj, compact=True):
    """compact: every item of a top-level list on one line (events, hits, commands ...)."""
    with open(path, 'w') as f:
        if not compact or not isinstance(obj, dict):
            json.dump(obj, f, indent=1, ensure_ascii=False)
        else:
            parts = []
            for k, v in obj.items():
                key = json.dumps(k, ensure_ascii=False)
                if isinstance(v, list) and v and all(isinstance(x, dict) for x in v):
                    items = ',\n'.join('  ' + json.dumps(x, ensure_ascii=False) for x in v)
                    parts.append(' %s: [\n%s\n ]' % (key, items))
                else:
                    parts.append(' %s: %s' % (key, json.dumps(v, ensure_ascii=False)))
            f.write('{\n' + ',\n'.join(parts) + '\n}')
        f.write('\n')


def rj(path):
    with open(path) as f:
        return json.load(f)


# ---------------------------------------------------------------- builders
def comb_build(j):
    cmds = j['commands']
    n = len(cmds)
    o = 4 + 4 * n
    offs, body = [], bytearray()
    for c in cmds:
        offs.append(o + len(body))
        body += struct.pack('<4H', int(c['id'], 16), len(c['steps']), *c['prereq'])
        for s in c['steps']:
            body += struct.pack('<4H', s['action'], s['b'], s['c'], s['flags'])
    return struct.pack('<HH', j['version'], n) + struct.pack(f'<{n}I', *offs) + bytes(body)


def plya_build(j):
    out = bytearray(struct.pack('<II', j['header'], len(j['actions'])))
    for a in j['actions']:
        r = bytearray.fromhex(a['raw'])
        assert len(r) == 116
        struct.pack_into('<H', r, 0, a['action'])
        struct.pack_into('<H', r, 0x1C, a['script'])
        out += r
    return bytes(out) + bytes.fromhex(j['tail'])


def mmap_build(j):
    return struct.pack('<II', j['version'], len(j['rows'])) + b''.join(
        struct.pack('<HHI', r['script'], r['motion'], r['phase']) for r in j['rows']) + bytes.fromhex(j['tail'])


def atkp_build(j):
    recs = [bytes.fromhex(h['raw']) if 'raw' in h else hit_encode(h) for h in j['hits']]
    return struct.pack('<II', j['header'], len(recs)) + b''.join(recs) + bytes.fromhex(j['tail'])


def timelines_build(d, j):
    blobs = []
    for fn in j['files']:
        p = os.path.join(d, fn)
        if fn.endswith('.bin'):
            with open(p, 'rb') as f:
                blobs.append(f.read())
        else:
            blobs.append(ev_encode(rj(p)))
    n = len(blobs)
    o = 8 + 4 * n
    offs = []
    for b in blobs:
        offs.append(o)
        o += len(b)
    return struct.pack('<II', j['version'], n) + struct.pack(f'<{n}I', *offs) + b''.join(blobs)


def magi_build(j):
    j = {k: v for k, v in j.items() if not k.startswith('_')}
    return magi.Magi.from_dict(j).to_bytes()


# ---------------------------------------------------------------- unpack / pack
def unpack(objx_bytes, out_dir, notes=None, bones=None):
    """notes: optional {'commands': {cmd id int: text}} written as _info next to each command (ignored on pack).
    bones: optional bone names of the model this moveset drives (shown instead of bone numbers in timelines)."""
    notes = notes or {}
    if bones:
        os.makedirs(out_dir, exist_ok=True)
        wj(os.path.join(out_dir, 'bones.json'), {'_info': 'bone names of the model (index = bone number); timelines name '
                                                          'parts by these. Comes from the model; not packed.',
                                                 'bones': list(bones)}, compact=False)
    extra, ents = arc_read(objx_bytes)
    os.makedirs(os.path.join(out_dir, 'sections'), exist_ok=True)
    arc = {'_info': 'ARC entry order; each entry is packed from the named file.',
           'header_extra': extra.hex(), 'entries': []}
    done = set()
    for i, (t, tag, data) in enumerate(ents):
        stag = tag.decode('latin1').rstrip('\0')
        key = {7: 'motion_map', 8: 'timelines'}.get(t, stag if t == 1 and stag in ('comb', 'plya', 'atkp', 'magi') else None)
        ent = {'type': t, 'tag': tag.hex()}
        if key and key not in done:
            done.add(key)
            fn = _unpack_section(key, data, out_dir, notes)
            if fn:
                ent['file'] = fn
        if 'file' not in ent:
            safe = ''.join(c if c.isalnum() else '_' for c in stag) or 'x'
            fn = 'sections/%02d_%s_%d.bin' % (i, safe, t)
            with open(os.path.join(out_dir, fn), 'wb') as f:
                f.write(data)
            ent['file'] = fn
        arc['entries'].append(ent)
    wj(os.path.join(out_dir, 'arc.json'), arc)
    for i, (t, tag, data) in enumerate(ents):
        if tag.rstrip(b'\0') == b'effe':
            write_effects_note(out_dir, data)
            break
    make_friendly(out_dir)
    assert pack(out_dir) == objx_bytes, 'objx round trip differs'
    return arc


def write_effects_note(out_dir, effe):
    """effects.json: the character's effect records (event `effect` arg = record id) with the effect names, bones and
    motions they are placed on. A note for the Move Studio and for people; not packed. Needs the Blender add-on's
    FEP reader (blender/io_scene_dissidia/dfx/fep.py) next to the tools; skipped without it."""
    try:
        fp = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', '..', '..', 'blender',
                                           'io_scene_dissidia', 'dfx'))
        if fp not in sys.path:
            sys.path.insert(0, fp)
        import fep
        m = fep.Mpk.from_bytes(effe)
        names = fep.Fep.from_bytes(m.get(0x40)).names()
        recs = fep.Fepa.from_bytes(m.get(0x4D))
    except Exception:
        return
    by = {}
    for i in range(len(recs.records)):
        inf = recs.info(i)
        e = by.setdefault(inf['id'], {'id': inf['id'], 'name': '', 'bones': [], 'motions': []})
        if inf['effect'] < len(names) and not e['name']:
            e['name'] = names[inf['effect']][1]
        if inf['bone'] and inf['bone'] not in e['bones']:
            e['bones'].append(inf['bone'])
        if inf['motion'] and inf['motion'] not in e['motions']:
            e['motions'].append(inf['motion'])
    wj(os.path.join(out_dir, 'effects.json'),
       {'_info': 'Effect records of this character (the `effect` event takes the id). 9999 = weapon-trail records, '
                 '8888 = other weapon / hand effects (used by bone_effect). A note: not packed.',
        'effects': [by[k] for k in sorted(by)]})


def make_friendly(d):
    """Rewrite the timelines with event names, hit names and part (bone) names."""
    global _CTX
    ip = os.path.join(d, 'timelines', 'index.json')
    if not os.path.exists(ip):
        return
    _CTX = Ctx(d)
    proj = {}
    pp = os.path.join(d, 'projectiles.json')
    if os.path.exists(pp):
        for r in rj(pp)['records']:
            if r.get('atkp') in _CTX.hit_name:
                proj[r['id']] = _CTX.hit_name[r['atkp']]
    for fn in rj(ip)['files']:
        if not fn.endswith('.json'):
            continue
        p = os.path.join(d, fn)
        t = rj(p)
        t['ranged'] = [friendly(e) for e in t['ranged']]
        t['keyed'] = [friendly(e) for e in t['keyed']]
        for e in t['ranged'] + t['keyed']:
            if e.get('op') == 'fire_projectile' and e.get('projectile') in proj:
                e['_note'] = 'hits with ' + proj[e['projectile']]
        wj(p, t)


def _unpack_section(key, data, out_dir, notes):
    if key == 'comb':
        ver, n = struct.unpack_from('<HH', data, 0)
        cmds = []
        for o in struct.unpack_from(f'<{n}I', data, 4):
            cid, ns, pa, pb = struct.unpack_from('<4H', data, o)
            steps = [dict(zip(('action', 'b', 'c', 'flags'), struct.unpack_from('<4H', data, o + 8 + k * 8)))
                     for k in range(ns)]
            c = {'id': '0x%02X' % cid, 'prereq': [pa, pb], 'steps': steps}
            if cid in notes.get('commands', {}):
                c['_info'] = notes['commands'][cid]
            cmds.append(c)
        obj = {'_info': 'Commands: an ability record points at one by its cmd id. Steps = presses; action ids are in '
                        'actions.json. Step flags: 0x100 = needs a press, 0 = plays by itself once the previous hit connected.',
               'version': ver, 'commands': cmds}
        if comb_build(obj) != data:
            return None
        wj(os.path.join(out_dir, 'moves.json'), obj)
        return 'moves.json'
    if key == 'plya':
        hdr, n = struct.unpack_from('<II', data, 0)
        recs = []
        for k in range(n):
            r = data[8 + k * 116: 8 + (k + 1) * 116]
            recs.append({'action': struct.unpack_from('<H', r, 0)[0], 'script': struct.unpack_from('<H', r, 0x1C)[0],
                         'raw': r.hex()})
        obj = {'_info': 'Action records (116 bytes). "action" = id used by moves.json steps, "script" -> motion_map.json; '
                        'both override the matching bytes of raw.',
               'header': hdr, 'actions': recs, 'tail': data[8 + n * 116:].hex()}
        if plya_build(obj) != data:
            return None
        wj(os.path.join(out_dir, 'actions.json'), obj)
        return 'actions.json'
    if key == 'motion_map':
        ver, n = struct.unpack_from('<II', data, 0)
        rows = [dict(zip(('script', 'motion', 'phase'), struct.unpack_from('<HHI', data, 8 + k * 8))) for k in range(n)]
        obj = {'_info': 'script id -> motion index (= timelines/NNN.json and the animation in the model)',
               'version': ver, 'rows': rows, 'tail': data[8 + n * 8:].hex()}
        if mmap_build(obj) != data:
            return None
        wj(os.path.join(out_dir, 'motion_map.json'), obj)
        return 'motion_map.json'
    if key == 'timelines':
        os.makedirs(os.path.join(out_dir, 'timelines'), exist_ok=True)
        ver, n = struct.unpack_from('<II', data, 0)
        offs = list(struct.unpack_from(f'<{n}I', data, 8)) + [len(data)]
        files = []
        for k in range(n):
            b = data[offs[k]:offs[k + 1]]
            t = ev_decode(b)
            if t is None:
                fn = 'timelines/%03d_raw.bin' % k
                with open(os.path.join(out_dir, fn), 'wb') as f:
                    f.write(b)
            else:
                fn = 'timelines/%03d.json' % t['motion']
                if fn in files:
                    fn = 'timelines/%03d_%d.json' % (t['motion'], k)
                wj(os.path.join(out_dir, fn), t)
            files.append(fn)
        wj(os.path.join(out_dir, 'timelines', 'index.json'),
           {'_info': 'Timeline order in the file. Events: ticks (2 per animation frame); "what" is a note.',
            'version': ver, 'files': files})
        if timelines_build(out_dir, rj(os.path.join(out_dir, 'timelines', 'index.json'))) != data:
            return None
        return 'timelines/index.json'
    if key == 'atkp':
        hdr, n = struct.unpack_from('<II', data, 0)
        obj = {'_info': HIT_INFO, 'header': hdr,
               'hits': [hit_decode(data[8 + k * 68: 8 + (k + 1) * 68]) for k in range(n)],
               'tail': data[8 + n * 68:].hex()}
        if atkp_build(obj) != data:
            return None
        wj(os.path.join(out_dir, 'hits.json'), obj)
        return 'hits.json'
    if key == 'magi':
        try:
            m = magi.Magi.from_bytes(data)
            d = json.loads(json.dumps(m.to_dict()))
            if magi_build(d) != data:
                return None
        except Exception:
            return None
        d = {'_info': 'Projectile records. "params" are indices into "pool" (the values packed); "values" is a note '
                      'showing what they resolve to. See charmod_magi.py for field meanings.', **d}
        wj(os.path.join(out_dir, 'projectiles.json'), d, compact=False)
        return 'projectiles.json'
    return None


def pack(d):
    global _CTX
    _CTX = Ctx(d)
    arc = rj(os.path.join(d, 'arc.json'))
    builders = {'moves.json': comb_build, 'actions.json': plya_build, 'motion_map.json': mmap_build,
                'hits.json': atkp_build, 'projectiles.json': magi_build}
    ents = []
    for e in arc['entries']:
        fn = e['file']
        p = os.path.join(d, fn)
        if fn in builders:
            data = builders[fn](rj(p))
        elif fn == 'timelines/index.json':
            data = timelines_build(d, rj(p))
        else:
            with open(p, 'rb') as f:
                data = f.read()
        ents.append((e['type'], bytes.fromhex(e['tag']), data))
    return arc_write(bytes.fromhex(arc['header_extra']), ents)
