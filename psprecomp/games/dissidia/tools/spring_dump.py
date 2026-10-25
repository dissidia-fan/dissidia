"""Read-only dump of a character's spring-bone (hair / cloth / cape physics) data.

The data is ARC entry type 6 of obj/p_<code>.objx (tag 'p_xx'; same entry in p_<code>_a.objx / _w.objx).
Layout and field meanings: docs/spring-bones.md.

  python3 spring_dump.py p_one200.objx [p_one200_1p.gmo]      # GMO only adds bone names / parents
  python3 spring_dump.py --raw p_one200.objx                  # also print each record as hex
  python3 spring_dump.py --json p_one200.objx [gmo]           # machine-readable
"""
import json
import math
import struct
import sys

SPRING_ARC_TYPE = 6


def arc_entry(objx, t):
    assert objx[:4] == b'ARC\x01', 'not an objx'
    n = struct.unpack_from('<I', objx, 4)[0]
    for i in range(n):
        typ, tag, o, s = struct.unpack_from('<I4sII', objx, 16 + i * 16)
        if typ == t:
            return objx[o:o + s]
    return None


def parse(b):
    ver, n_sph, o_sph, n_set, o_set, n_lnk, o_lnk, n_bone, o_bone = struct.unpack_from('<9I', b, 0)
    d = {'version': ver, 'spheres': [], 'param_sets': [], 'link_params': [], 'bones': []}
    for i in range(n_sph):
        grp, bone, x, y, z, r = struct.unpack_from('<hh4f', b, o_sph + i * 20)
        d['spheres'].append({'group': grp, 'bone': bone, 'offset': [x, y, z], 'radius': r})
    for i in range(n_set):
        f = struct.unpack_from('<7f', b, o_set + i * 28)
        d['param_sets'].append({'stiffness': f[0], 'max_pull': f[1], 'drag_xyz': list(f[2:5]),
                                'collide_push': f[5], 'unk_18': f[6]})
    for i in range(n_lnk):
        d['link_params'].append(list(struct.unpack_from('<2f', b, o_lnk + i * 8)))
    for i in range(n_bone):
        o = o_bone + i * 64
        bone, cg1, cg2, pset = struct.unpack_from('<4h', b, o)
        lim = struct.unpack_from('<4f', b, o + 8)
        flags = list(b[o + 24:o + 28])
        floor = struct.unpack_from('<f', b, o + 28)[0]
        links = []
        for k in range(8):
            lb, kind, lp = struct.unpack_from('<hBB', b, o + 32 + k * 4)
            if lb != -1:
                links.append({'bone': lb, 'at_tip': kind, 'link_param': lp, 'slot': k})
        d['bones'].append({'bone': bone, 'collide_groups': [cg1, cg2], 'param_set': pset,
                           'limit_a': lim[:2], 'limit_b': lim[2:], 'floor_flag': flags[0],
                           'flag_19': flags[1], 'flag_1a': flags[2], 'no_wind': flags[3],
                           'floor_offset': floor, 'links': links, 'raw': b[o:o + 64].hex()})
    return d


def gmo_bones(path):
    import charmod_chunks as C
    import charmod_gmo as G
    m = G.Model(open(path, 'rb').read())
    names, parents = [], []
    for bn in m.bones:
        names.append(bn.name)
        p = bn.first(0x8041)
        parents.append(struct.unpack_from('<I', p.payload)[0] & 0xFFF if p is not None else -1)
    return names, parents


def deg(a):
    return '%g' % round(math.degrees(a), 1)


def main(argv):
    raw = '--raw' in argv
    js = '--json' in argv
    args = [a for a in argv if not a.startswith('--')]
    b = arc_entry(open(args[0], 'rb').read(), SPRING_ARC_TYPE)
    if b is None:
        sys.exit('no ARC type 6 entry')
    d = parse(b)
    names, parents = gmo_bones(args[1]) if len(args) > 1 else ([], [])
    nm = lambda i: '%d%s' % (i, ('(%s)' % names[i]) if 0 <= i < len(names) else '')
    if js:
        if names:
            for r in d['bones']:
                r['name'] = names[r['bone']] if r['bone'] < len(names) else None
                r['parent'] = parents[r['bone']] if r['bone'] < len(parents) else None
        print(json.dumps(d, indent=1))
        return
    print('version %d: %d spheres, %d param sets, %d link params, %d spring bones' % (
        d['version'], len(d['spheres']), len(d['param_sets']), len(d['link_params']), len(d['bones'])))
    for i, s in enumerate(d['spheres']):
        print('  sphere %d: group %d bone %s offset %s r %.3f' % (i, s['group'], nm(s['bone']),
              [round(v, 3) for v in s['offset']], s['radius']))
    for i, p in enumerate(d['param_sets']):
        print('  set %d: stiffness %.3f max_pull %.3f drag %s push %.3f unk %.3f' % (
            i, p['stiffness'], p['max_pull'], [round(v, 3) for v in p['drag_xyz']], p['collide_push'], p['unk_18']))
    for i, l in enumerate(d['link_params']):
        print('  link param %d: %s' % (i, [round(v, 3) for v in l]))
    for r in d['bones']:
        par = (' parent %s' % nm(parents[r['bone']])) if parents else ''
        print('  bone %s%s: set %d groups %s limits [%s..%s] [%s..%s] flags floor=%d 19=%d 1a=%d nowind=%d floor_off %.3f%s' % (
            nm(r['bone']), par, r['param_set'], r['collide_groups'], deg(r['limit_a'][0]), deg(r['limit_a'][1]),
            deg(r['limit_b'][0]), deg(r['limit_b'][1]), r['floor_flag'], r['flag_19'], r['flag_1a'], r['no_wind'],
            r['floor_offset'], ''.join(' link->%s%s p%d' % (nm(l['bone']), '(tip)' if l['at_tip'] else '', l['link_param'])
                                        for l in r['links'])))
        if raw:
            print('     ' + r['raw'])


if __name__ == '__main__':
    main(sys.argv[1:])
