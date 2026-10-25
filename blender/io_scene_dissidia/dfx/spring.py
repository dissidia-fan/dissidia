"""Spring-bone (cape / hair / cloth physics) data: ARC entry type 6 of obj/p_<code>.objx.

Layout and field meanings: psprecomp/games/dissidia/docs/spring-bones.md. Pure Python (no bpy), so the repo's
tools can use it too. parse() / build() round-trip retail sections byte for byte; cape_section() puts a cape's
records (Cecil-style chains, see cape.py) into an existing section without touching the records already there.
"""
import struct

SPRING_ARC_TYPE = 6
HEADER = 0x24
SPHERE, PSET, LINK, RECORD = 20, 0x1C, 8, 0x40
_PI_F32 = struct.unpack('<f', bytes.fromhex('d80f4940'))[0]     # +-pi: "no limit" in retail

# parameter sets: stiffness, pull clamp, drag x / y / z, collision push, unused
PRESETS = {
    # Cecil (for100): one set for the whole cape, no stiffness (pure pendulum), drag 0.1; links [0.1, 0.0]
    'cecil': {'sets': lambda n: [[0.0, 0.0, 0.1, 0.1, 0.1, 0.0, 0.0]], 'level_set': lambda k, n: 0,
              'link': [0.1, 0.0], 'hem_floor': False},
    # Exdeath (fiv200): stiffness graded from the top (0.1) to the hem (0), one set per level; links [0.1, 0.1];
    # floor flag on the hem bones
    'graded': {'sets': lambda n: [[round(0.1 * (n - 1 - k) / (n - 1), 4) if n > 1 else 0.05, 0.0, 0.1, 0.1, 0.1, 0.0, 0.0]
                                  for k in range(n)],
               'level_set': lambda k, n: k, 'link': [0.1, 0.1], 'hem_floor': True},
}
_F = lambda h: struct.unpack('<f', bytes.fromhex(h))[0]
TOP_LIMITS = (_F('900a06bf'), _F('900a0640'), -_PI_F32, _PI_F32)     # Cecil's top bones: -30 / 120 deg, exact bits
FREE_LIMITS = (-_PI_F32, _PI_F32, -_PI_F32, _PI_F32)


def arc_entry(objx, t=SPRING_ARC_TYPE):
    n = struct.unpack_from('<I', objx, 4)[0]
    for i in range(n):
        typ, tag, o, s = struct.unpack_from('<I4sII', objx, 16 + i * 16)
        if typ == t:
            return objx[o:o + s]
    return None


def empty():
    return {'version': 2, 'spheres': [], 'sets': [], 'links': [], 'records': [], 'tail': b''}


def parse(b):
    """-> {'version', 'spheres': [[group, bone, x, y, z, r]], 'sets': [[7 floats]], 'links': [[a, b]],
    'records': [bytearray(64)], 'tail': bytes after the tables (retail: none)}"""
    if not b:
        return empty()
    ver, n_s, o_s, n_p, o_p, n_l, o_l, n_r, o_r = struct.unpack_from('<9I', b, 0)
    d = {'version': ver,
         'spheres': [list(struct.unpack_from('<hh4f', b, o_s + i * SPHERE)) for i in range(n_s)],
         'sets': [list(struct.unpack_from('<7f', b, o_p + i * PSET)) for i in range(n_p)],
         'links': [list(struct.unpack_from('<2f', b, o_l + i * LINK)) for i in range(n_l)],
         'records': [bytearray(b[o_r + i * RECORD:o_r + (i + 1) * RECORD]) for i in range(n_r)]}
    end = max(HEADER, o_s + n_s * SPHERE, o_p + n_p * PSET, o_l + n_l * LINK, o_r + n_r * RECORD)
    d['tail'] = bytes(b[end:])
    d['_layout'] = (o_s, o_p, o_l, o_r)
    return d


def build(d):
    """Header + tables back to back (spheres, parameter sets, link params, records), as retail packs them."""
    body = bytearray()
    o_s = HEADER
    for s in d['spheres']:
        body += struct.pack('<hh4f', *s)
    o_p = HEADER + len(body)
    for p in d['sets']:
        body += struct.pack('<7f', *p)
    o_l = HEADER + len(body)
    for l in d['links']:
        body += struct.pack('<2f', *l)
    o_r = HEADER + len(body)
    for r in d['records']:
        body += bytes(r)
    head = struct.pack('<9I', d.get('version', 2), len(d['spheres']), o_s, len(d['sets']), o_p,
                       len(d['links']), o_l, len(d['records']), o_r)
    return head + bytes(body) + d.get('tail', b'')


# ---------------------------------------------------------------- records
def rec_bone(r):
    return struct.unpack_from('<h', r, 0)[0]


def rec_groups(r):
    return struct.unpack_from('<hh', r, 2)


def rec_set(r):
    return struct.unpack_from('<h', r, 6)[0]


def rec_links(r):
    return [struct.unpack_from('<hBB', r, 0x20 + k * 4) for k in range(4)]


def make_record(bone, groups, pset, limits, links=(), floor=False, frame_gravity=True):
    r = bytearray(RECORD)
    struct.pack_into('<hhhh', r, 0, bone, groups[0], groups[1], pset)
    struct.pack_into('<4f', r, 8, *limits)
    r[0x18], r[0x19], r[0x1A], r[0x1B] = int(bool(floor)), int(bool(frame_gravity)), 0, 0
    struct.pack_into('<f', r, 0x1C, 0.0)
    links = list(links)[:4]
    for k in range(8):
        lb, tip, lp = links[k] if k < len(links) else (-1, 0, 0)
        struct.pack_into('<hBB', r, 0x20 + k * 4, lb, tip, lp)
    return r


def _remove(d, bones):
    """Drop the records of `bones`, and the parameter sets, link params and sphere groups only they used."""
    keep = [r for r in d['records'] if rec_bone(r) not in bones]
    gone = [r for r in d['records'] if rec_bone(r) in bones]
    if not gone:
        return d
    used_sets = {rec_set(r) for r in keep}
    used_links = {lp for r in keep for lb, _t, lp in rec_links(r) if lb != -1}
    used_groups = {g for r in keep for g in rec_groups(r) if g}
    drop_sets = {rec_set(r) for r in gone} - used_sets
    drop_links = {lp for r in gone for lb, _t, lp in rec_links(r) if lb != -1} - used_links
    drop_groups = {g for r in gone for g in rec_groups(r) if g} - used_groups
    set_map, links_map = {}, {}
    sets, lks = [], []
    for i, s in enumerate(d['sets']):
        if i not in drop_sets:
            set_map[i] = len(sets)
            sets.append(s)
    for i, l in enumerate(d['links']):
        if i not in drop_links:
            links_map[i] = len(lks)
            lks.append(l)
    for r in keep:
        struct.pack_into('<h', r, 6, set_map.get(rec_set(r), rec_set(r)))
        for k, (lb, tip, lp) in enumerate(rec_links(r)):
            if lb != -1:
                struct.pack_into('<hBB', r, 0x20 + k * 4, lb, tip, links_map.get(lp, lp))
    out = dict(d)
    out.update(records=keep, sets=sets, links=lks,
               spheres=[s for s in d['spheres'] if s[0] not in drop_groups])
    return out


def cape_section(existing, cfg, index_of):
    """The section `existing` (bytes; b'' = none) with the cape described by `cfg` (cape.py's dfx_cape) in it.
    index_of: bone name -> GMO bone index (from the exported model). Records of the cape's bones already there
    (an earlier save) are replaced, with the sets / links / spheres only they used; everything else stays."""
    d = parse(existing)
    if existing and d['version'] != 2:
        raise ValueError('spring section version %d (expected 2)' % d['version'])
    chains = cfg['chains']
    bones = {index_of[n] for c in chains for n in c['bones']}
    d = _remove(d, bones)
    preset = PRESETS[cfg.get('preset', 'cecil')]
    n = max(len(c['bones']) for c in chains)
    used = [g for s in d['spheres'] for g in (s[0],)] + [g for r in d['records'] for g in rec_groups(r)]
    group = max([0] + used) + 1
    set0 = len(d['sets'])
    d['sets'] = d['sets'] + [list(map(float, s)) for s in preset['sets'](n)]
    link0 = len(d['links'])
    d['links'] = d['links'] + [list(map(float, cfg.get('link', preset['link'])))]
    for s in cfg.get('spheres', []):
        d['spheres'].append([group, index_of[s['bone']]] + [float(v) for v in s['offset']] + [float(s['radius'])])
    groups = (group, 0) if cfg.get('spheres') else (0, 0)
    by_side = {c['side']: c for c in chains}
    hub = 'b' if 'b' in by_side else ('l' if 'l' in by_side and 'r' in by_side else None)
    order = sorted(chains, key=lambda c: (c['side'] != hub, c['side']))      # the linked-to chain first
    recs = []
    for c in order:
        m = len(c['bones'])
        for k, name in enumerate(c['bones']):
            links = []
            if hub and c['side'] != hub:
                hb = by_side[hub]['bones']
                if k < len(hb):
                    links.append((index_of[hb[k]], 1, link0))
            recs.append(make_record(index_of[name], groups, set0 + preset['level_set'](k, m),
                                    TOP_LIMITS if k == 0 else FREE_LIMITS, links,
                                    floor=preset['hem_floor'] and k == m - 1))
    d['records'] = d['records'] + recs
    if set0 + len(preset['sets'](n)) > 126:
        raise ValueError('too many parameter sets (the game keeps the count in a signed byte)')
    return build(d)
