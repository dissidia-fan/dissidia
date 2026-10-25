"""Body skins for character models (obj/p_<code>_*.gmo).

A character model holds, in its mesh model: the skeleton, the body (part 0, drawn by its owner bone, which carries
the body's skin weights: BlendBones / BlendOffsets), weapon and effect parts, materials and textures. Some extras
also carry warp-dash ghosts: parts 'warp_ghostN', each a copy of the body skinned to a 'g_' copy of the skeleton,
with tinted copies of the body textures.

A *skin* is exactly the body-dependent data, at the model's own slots:
  body part chunk, the owner bone's BlendBones/BlendOffsets, the image of every texture the body uses,
  and per ghost: the part chunk, its owner bone's BlendBones/BlendOffsets, its texture images.
apply_skin() writes those into a model; extract_skin() reads them back. Both touch nothing else, so
apply(extract(model)) is the model itself, and swapping skins back and forth is exact.
skin_from_body() builds a skin for a model from any other model on the same skeleton (bones 0..82 for Bartz-based
characters), e.g. the like's manikin (retail p_fiv100_zako.gmo): its body part, weights and textures go into the
model's body slots, and the ghosts are rebuilt from it (bone names 'g_' + name, textures tinted per stage).

A skin may also carry *weapons* (mod.json body.weapons): for each listed weapon part, the part chunk and its owner
bone's BlendBones/BlendOffsets, plus the images of the weapon's own textures (textures no other part uses). The rig
stores the default weapons (weapons_default(): the based-on character's own weapon parts, read from the disc, on its
own materials; the weapon-only textures blanked to grey; blade ghosts rebuilt from the donor blade). A skin without
weapons leaves the rig's weapons as they are.
"""
import json
import struct

import charmod_chunks as C

SET_MATERIAL, BLEND_SUBSET, SET_TEXTURE, BLEND_BONES, BLEND_OFFSETS, DRAW_PART, FILE_IMAGE = \
    0x8061, 0x8062, 0x8091, 0x8044, 0x8045, 0x804E, 0x8013


def _ref_index(v):
    return v & 0xFFF


def _set_ref_index(v, i):
    return (v & ~0xFFF) | i


def _mesh_model(g):
    """The model the game draws: the first with parts, as the Blender add-on and the Move Studio read it. Some retail
    models (gst100, one200) carry a second mesh model ('model-0-0000') after it."""
    ms = [c for c in g.root.walk() if c.type == C.MODEL]
    return next((m for m in ms if m.find(C.PART)), ms[-1])


def _u32(c):
    return struct.unpack_from('<I', c.payload)[0]


class Model:
    def __init__(self, data):
        self.g = C.GmoFile.from_bytes(data)
        self.mm = _mesh_model(self.g)
        self.bones = self.mm.find(C.BONE)
        self.parts = self.mm.find(C.PART)
        self.mats = self.mm.find(C.MATERIAL)
        self.texs = self.mm.find(C.TEXTURE)

    def to_bytes(self):
        return self.g.to_bytes()

    def bone_names(self):
        return [b.name for b in self.bones]

    def owner(self, part_i):
        for i, b in enumerate(self.bones):
            dp = b.first(DRAW_PART)
            if dp is not None and _ref_index(_u32(dp)) == part_i:
                return i
        raise KeyError('no bone draws part %d' % part_i)

    def blend_list(self, bone_i):
        c = self.bones[bone_i].first(BLEND_BONES)
        n = struct.unpack_from('<I', c.payload)[0]
        return [_ref_index(x) for x in struct.unpack_from('<%dI' % n, c.payload, 4)]

    def part_materials(self, part_i):
        out = []
        for mesh in self.parts[part_i].find(C.MESH):
            for c in mesh.find(SET_MATERIAL):
                i = _ref_index(_u32(c))
                if i not in out:
                    out.append(i)
        return sorted(out)

    def mat_textures(self, mat_i):
        out = []
        for c in self.mats[mat_i].walk():
            if c.type == SET_TEXTURE:
                out.append(_ref_index(_u32(c)))
        return out

    def ghost_parts(self):
        g = [(p.name, i) for i, p in enumerate(self.parts) if p.name.startswith('warp_ghost') and p.name[10:].isdigit()]
        return [i for _, i in sorted(g, key=lambda x: int(x[0][10:]))]

    def slots(self):
        """The model's body-dependent slots."""
        s = {'body': self._part_slots(0), 'ghosts': [self._part_slots(i) for i in self.ghost_parts()]}
        # body textures must belong to the body alone (else a skin would change other parts too)
        used = {}
        for pi in range(len(self.parts)):
            for m in self.part_materials(pi):
                for t in self.mat_textures(m):
                    used.setdefault(t, set()).add(pi)
        mine = {0} | set(self.ghost_parts())
        for grp in [s['body']] + s['ghosts']:
            for t in grp['textures']:
                assert used.get(t, set()) <= mine, 'texture %d is shared with parts %s' % (t, sorted(used[t] - mine))
        return s

    def _part_slots(self, pi):
        mats = self.part_materials(pi)
        tex = []
        for m in mats:
            for t in self.mat_textures(m):
                if t not in tex:
                    tex.append(t)
        return {'part': pi, 'owner': self.owner(pi), 'materials': mats, 'textures': tex}

    def texture_image(self, t):
        return self.texs[t].first(FILE_IMAGE).payload

    def set_texture_image(self, t, payload):
        self.texs[t].first(FILE_IMAGE).payload = payload


def own_body_slots(gmo_bytes):
    """A model whose body (and ghosts) share a material or texture with other parts (weapons on the same atlas, as
    many retail models have) can't take a skin: the skin would repaint those parts too. This gives the body copies of
    the shared materials and textures of its own (appended, so every other index stays), with the same pixels.
    extract_skin / skin_from_body / apply_skin all go through it (assist models share their body texture too), so the
    copy is never stored. -> (bytes, n copies); the input bytes themselves when nothing is shared."""
    import copy
    m = Model(gmo_bytes)
    mine = {0} | set(m.ghost_parts())
    mat_users, tex_users = {}, {}
    for pi in range(len(m.parts)):
        for mi in m.part_materials(pi):
            mat_users.setdefault(mi, set()).add(pi)
            for t in m.mat_textures(mi):
                tex_users.setdefault(t, set()).add(pi)
    if all(tex_users.get(t, set()) <= mine for p in mine for mi in m.part_materials(p) for t in m.mat_textures(mi)):
        return gmo_bytes, 0
    new_mats, new_texs, mat_copy, tex_copy = [], [], {}, {}
    def mat_for(mi):
        if not (mat_users[mi] - mine):
            return mi
        if mi not in mat_copy:
            c = copy.deepcopy(m.mats[mi])
            c.name = (m.mats[mi].name or 'material')[:22] + '_body'
            mat_copy[mi] = len(m.mats) + len(new_mats)
            new_mats.append(c)
        return mat_copy[mi]
    def tex_for(t):
        if not (tex_users[t] - mine):
            return t
        if t not in tex_copy:
            c = copy.deepcopy(m.texs[t])
            c.name = (m.texs[t].name or 'file')[:22] + '_body'
            tex_copy[t] = len(m.texs) + len(new_texs)
            new_texs.append(c)
        return tex_copy[t]
    body_mats = set()
    for p in sorted(mine):
        for mesh in m.parts[p].find(C.MESH):
            for c in mesh.find(SET_MATERIAL):
                v = _u32(c)
                ni = mat_for(_ref_index(v))
                body_mats.add(ni)
                c.payload = struct.pack('<I', _set_ref_index(v, ni)) + c.payload[4:]
    for mi in sorted(body_mats):
        mc = m.mats[mi] if mi < len(m.mats) else new_mats[mi - len(m.mats)]
        for c in mc.walk():
            if c.type == SET_TEXTURE:
                v = _u32(c)
                c.payload = struct.pack('<I', _set_ref_index(v, tex_for(_ref_index(v)))) + c.payload[4:]
    def after_last(t, chunks):
        if not chunks:
            return
        kids = m.mm.children
        i = max(k for k, c in enumerate(kids) if c.type == t)
        kids[i + 1:i + 1] = chunks
    after_last(C.MATERIAL, new_mats)
    after_last(C.TEXTURE, new_texs)
    out = m.to_bytes()
    Model(out).slots()                                      # the body owns its textures now
    return out, len(new_mats) + len(new_texs)


# ---------------------------------------------------------------- skin container
# A skin is a list of (key, bytes): json index first, then the blobs.
def skin_to_bytes(skin):
    idx, blobs = {}, []
    o = 0
    for k, v in skin['blobs'].items():
        idx[k] = [o, len(v)]
        blobs.append(v)
        o += len(v)
    head = json.dumps({'version': 1, 'info': skin.get('info', ''), 'slots': skin['slots'], 'blobs': idx},
                      indent=1).encode()
    return b'DSKN' + struct.pack('<I', len(head)) + head + b''.join(blobs)


def skin_from_bytes(d):
    assert d[:4] == b'DSKN', 'not a skin file'
    n = struct.unpack_from('<I', d, 4)[0]
    head = json.loads(d[8:8 + n])
    base = 8 + n
    return {'info': head.get('info', ''), 'slots': head['slots'],
            'blobs': {k: d[base + o: base + o + s] for k, (o, s) in head['blobs'].items()}}


def _grp_blobs(m, grp, prefix, out):
    out[prefix + 'part'] = m.parts[grp['part']].to_bytes()
    b = m.bones[grp['owner']]
    out[prefix + 'blend_bones'] = b.first(BLEND_BONES).payload
    out[prefix + 'blend_offsets'] = b.first(BLEND_OFFSETS).payload
    for t in grp['textures']:
        out[prefix + 'tex%d' % t] = m.texture_image(t)


def extract_skin(gmo_bytes, info='', weapons=None):
    """weapons: mod.json body.weapons; when given, the skin also carries the model's weapons."""
    gmo_bytes = own_body_slots(gmo_bytes)[0]
    m = Model(gmo_bytes)
    s = m.slots()
    blobs = {}
    _grp_blobs(m, s['body'], 'body.', blobs)
    for k, g in enumerate(s['ghosts']):
        _grp_blobs(m, g, 'ghost%d.' % k, blobs)
    sk = {'info': info, 'slots': s, 'blobs': blobs}
    if weapons:
        add_weapons(sk, extract_weapons(gmo_bytes, weapons))
    return sk


def _parse_one(data):
    return C.parse_chunks(data, 0, len(data))[0]


def apply_skin(gmo_bytes, skin):
    gmo_bytes = own_body_slots(gmo_bytes)[0]                 # the slots skins are made for (same bytes when nothing is shared)
    m = Model(gmo_bytes)
    s = m.slots()
    ss = skin['slots']
    assert len(ss['ghosts']) == len(s['ghosts']), 'skin has %d ghosts, model %d' % (len(ss['ghosts']), len(s['ghosts']))
    for grp, sgrp, prefix in [(s['body'], ss['body'], 'body.')] + [
            (g, sg, 'ghost%d.' % k) for k, (g, sg) in enumerate(zip(s['ghosts'], ss['ghosts']))]:
        assert grp == sgrp, 'skin was made for other slots: %s vs %s' % (sgrp, grp)
        B = skin['blobs']
        m.mm.children[m.mm.children.index(m.parts[grp['part']])] = _parse_one(B[prefix + 'part'])
        b = m.bones[grp['owner']]
        b.first(BLEND_BONES).payload = B[prefix + 'blend_bones']
        b.first(BLEND_OFFSETS).payload = B[prefix + 'blend_offsets']
        for t in grp['textures']:
            m.set_texture_image(t, B[prefix + 'tex%d' % t])
    data = m.to_bytes()
    if ss.get('weapons'):
        data = apply_weapons(data, skin)
    return data


# ---------------------------------------------------------------- skin from another model's body
# ---------------------------------------------------------------- vertex ranges
# Positions / UVs are stored normalised against the model-wide ranges in the mesh model's 0x8015 commands
# (sel 0x180: pos offset xyz + scale xyz; sel 0x3: uv offset + scale). A part copied from another model (a Blender
# export may have picked other ranges) is re-encoded against the rig's ranges.
def model_ranges(m):
    r = {}
    for c in m.mm.find(0x8015):
        sel = struct.unpack_from('<I', c.payload)[0]
        if sel == 0x180:
            v = struct.unpack_from('<6f', c.payload, 4); r['pos'] = (v[:3], v[3:])
        elif sel == 0x3:
            v = struct.unpack_from('<4f', c.payload, 4); r['uv'] = (v[:2], v[2:])
    return r


def _vertex_format():
    import os, sys
    d = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', '..', '..', 'blender', 'io_scene_dissidia')
    if d not in sys.path:
        sys.path.insert(0, d)
    from dfx import gmo
    return gmo


POS_TOL, UV_TOL = 0.02, 0.02          # how far past the rig's range a vertex may be (normalised) before it's an error


def requantize_part(part_bytes, src_r, dst_r, what=''):
    """Re-encode a part's vertex arrays from src's ranges to dst's. Returns (bytes, worst excursion)."""
    import numpy as np
    if src_r == dst_r:
        return part_bytes, 0.0
    gmo = _vertex_format()
    p = _parse_one(part_bytes)
    worst = 0.0
    for c in p.find(C.ARRAYS):
        word, count, nmorph, flags = struct.unpack_from('<IIII', c.payload, 0)
        fmt = gmo.VertexFormat(word)
        off, st = fmt.layout()
        raw = bytearray(c.payload[16:16 + count * st * nmorph])
        buf = np.frombuffer(raw, np.uint8).reshape(count * nmorph, st)
        for key, kind, n, table, (lo, hi), rk in (
                ('p', fmt.pos, 3, {1: (np.int8, 128.0), 2: (np.int16, 32768.0), 3: (np.float32, 1.0)}, (-1.0, 32767 / 32768), 'pos'),
                ('t', fmt.tc, 2, gmo._NORM, (0.0, 65535 / 32768), 'uv')):
            if key not in off or not kind or rk not in src_r or rk not in dst_r:
                continue
            dt, sc = table[kind]
            sz = np.dtype(dt).itemsize
            cols = buf[:, off[key]:off[key] + n * sz].copy().view(dt).reshape(-1, n).astype(np.float64) / sc
            (so, ss), (do, ds) = src_r[rk], dst_r[rk]
            v = (cols * np.array(ss) + np.array(so) - np.array(do)) / np.array(ds)
            if kind == 3:
                lo, hi = -np.inf, np.inf
            ex = float(max(0.0, (lo - v).max(), (v - hi).max()))
            worst = max(worst, ex)
            if ex > (POS_TOL if rk == 'pos' else UV_TOL):
                raise ValueError('%s: %s outside the rig\'s range by %.3f (normalised); the rig needs a bigger range'
                                 % (what or p.name, 'positions' if rk == 'pos' else 'UVs', ex))
            v = np.clip(v, lo, hi)
            if np.issubdtype(dt, np.integer):
                info = np.iinfo(dt)
                q = np.clip(np.round(v * sc), info.min, info.max).astype(dt)
            else:
                q = v.astype(dt)
            buf[:, off[key]:off[key] + n * sz] = q.view(np.uint8).reshape(len(buf), n * sz)
        c.payload = c.payload[:16] + bytes(buf.tobytes()) + c.payload[16 + count * st * nmorph:]
    return p.to_bytes(), worst


def _remap_part(part_bytes, mat_map, name=None, index=None, ranges=None):
    import re
    if ranges is not None:
        part_bytes, _ = requantize_part(part_bytes, ranges[0], ranges[1], name or '')
    p = _parse_one(part_bytes)
    if name is not None:
        p.name = name
    if index is not None:                                 # mesh chunks are named '<n>mesh-<part index>-<k>'
        for mesh in p.find(C.MESH):
            if mesh.name:
                mesh.name = re.sub(r'mesh-\d+-', 'mesh-%d-' % index, mesh.name, count=1)
    for mesh in p.find(C.MESH):
        for c in mesh.find(SET_MATERIAL):
            v = _u32(c)
            c.payload = struct.pack('<I', _set_ref_index(v, mat_map[_ref_index(v)])) + c.payload[4:]
    return p.to_bytes()


def skin_from_body(rig_bytes, body_bytes, info=''):
    """Skin for rig_bytes' slots made from body_bytes' body (part 0). Both models must share the body's bones.
    A rig whose body shares textures with other parts is taken as own_body_slots() gives it, as apply_skin() does."""
    rig_bytes = own_body_slots(rig_bytes)[0]
    rig, src = Model(rig_bytes), Model(body_bytes)
    s = rig.slots()
    sb = src._part_slots(0)
    rn, sn = rig.bone_names(), src.bone_names()
    sblend = src.blend_list(sb['owner'])
    for b in sblend:
        assert b < len(rn) and rn[b] == sn[b], 'body bone %d (%s) is not the same bone in the model' % (b, sn[b])
    smats, rmats = sb['materials'], s['body']['materials']
    assert len(smats) <= len(rmats), 'the body uses %d materials, the model has %d body materials' % (len(smats), len(rmats))
    mat_map = {sm: rmats[k] for k, sm in enumerate(smats)}
    # the texture each body material shows, in the model's slot of that material
    tex_img = {}
    for k, sm in enumerate(smats):
        st, rt = src.mat_textures(sm), rig.mat_textures(rmats[k])
        assert len(st) == len(rt), 'material texture layers differ'
        for a, b in zip(st, rt):
            tex_img[b] = src.texture_image(a)
    for t in s['body']['textures']:
        tex_img.setdefault(t, tex_img[next(iter(tex_img))])   # a model body slot the new body does not use
    rr = (model_ranges(src), model_ranges(rig))
    blobs = {'body.part': _remap_part(src.parts[0].to_bytes(), mat_map, ranges=rr),
             'body.blend_bones': src.bones[sb['owner']].first(BLEND_BONES).payload,
             'body.blend_offsets': src.bones[sb['owner']].first(BLEND_OFFSETS).payload}
    for t in s['body']['textures']:
        blobs['body.tex%d' % t] = tex_img[t]
    if s['ghosts']:
        import numpy as np
        import charmod_gim as gim
        gidx = {}
        for i, nm in enumerate(rn):
            if nm.startswith('g_'):
                gidx[nm[2:]] = i
        def parent(i):
            p = rig.bones[i].first(0x8041)
            if p is None:
                return -1
            r = _ref_index(_u32(p))
            return -1 if r == 0xFFF else r

        def ghost_of(b):
            while b >= 0:
                if rn[b][:29] in gidx:
                    return gidx[rn[b][:29]]
                b = parent(b)
            raise KeyError('no ghost bone for the body')
        n = len(sblend)
        hi = struct.unpack_from('<I', rig.bones[s['ghosts'][0]['owner']].first(BLEND_BONES).payload, 4)[0] & ~0xFFF
        gl = [hi | ghost_of(b) for b in sblend]
        rgba_cache = {}
        for k, g in enumerate(s['ghosts']):
            gm = g['materials']
            assert len(gm) >= len(smats), 'ghost %d has fewer materials than the body' % k
            gmap = {sm: gm[j] for j, sm in enumerate(smats)}
            pre = 'ghost%d.' % k
            blobs[pre + 'part'] = _remap_part(src.parts[0].to_bytes(), gmap, name=rig.parts[g['part']].name, ranges=rr)
            blobs[pre + 'blend_bones'] = struct.pack('<I%dI' % n, n, *gl)
            blobs[pre + 'blend_offsets'] = blobs['body.blend_offsets']
            for j, sm in enumerate(smats):
                st = src.mat_textures(sm)
                for a, t in zip(st, rig.mat_textures(gm[j])):
                    if a not in rgba_cache:
                        rgba_cache[a] = gim.decode(src.texture_image(a)[4:])
                    idx = ghost_indices(rgba_cache[a], k)
                    blobs[pre + 'tex%d' % t] = gim.wrap_for_gmo(gim.encode_indexed(idx, ghost_palette(k)))
            for t in g['textures']:
                if pre + 'tex%d' % t not in blobs:
                    blobs[pre + 'tex%d' % t] = rig.texture_image(t)
    return {'info': info, 'slots': s, 'blobs': blobs}


def skin_for_model(src_rig, skin, dst_rig, info=None):
    """The body of `skin` (made for src_rig) as a skin for another model on the same skeleton (an assist, another
    costume): put it on src_rig, then take that body for dst_rig."""
    return skin_from_body(dst_rig, apply_skin(src_rig, skin), info or skin.get('info', ''))


# ---------------------------------------------------------------- weapons
# spec (mod.json body.weapons):
#   default_from: the donor model on the disc (the based-on character's own weapons), e.g. obj/p_fiv100_1p.gmo
#   parts:   {rig part name: donor part name}   (a rig part whose owner bone differs from the donor's, like the
#             warp dash's thrown sword, gets the donor mesh on its own bone, held as on the donor's first bone)
#   ghosts:  {"of": donor part, "parts": [rig ghost parts]}   (stage copies, textures tinted like the warp ghosts)
#   textures: the weapon-only textures (no other part uses them)
def weapon_names(spec):
    return list(spec.get('parts', {})) + list(spec.get('ghosts', {}).get('parts', []))


def weapon_slots(m, spec):
    P = {p.name: i for i, p in enumerate(m.parts)}
    parts = [{'name': n, 'part': P[n], 'owner': m.owner(P[n])} for n in weapon_names(spec)]
    return {'parts': parts, 'textures': sorted(spec['textures'])}


def own_weapon_textures(m, spec):
    """Textures the listed weapon parts use and no other part does (for spec['textures'])."""
    names = set(weapon_names(spec))
    mine, other = set(), set()
    for pi, p in enumerate(m.parts):
        ts = {t for mt in m.part_materials(pi) for t in m.mat_textures(mt)}
        (mine if p.name in names else other).update(ts)
    return sorted(mine - other)


def extract_weapons(gmo_bytes, spec):
    m = Model(gmo_bytes)
    ws = weapon_slots(m, spec)
    blobs = {}
    for w in ws['parts']:
        pre = 'wep.%s.' % w['name']
        blobs[pre + 'part'] = m.parts[w['part']].to_bytes()
        b = m.bones[w['owner']]
        blobs[pre + 'blend_bones'] = b.first(BLEND_BONES).payload
        blobs[pre + 'blend_offsets'] = b.first(BLEND_OFFSETS).payload
    for t in ws['textures']:
        blobs['wep.tex%d' % t] = m.texture_image(t)
    return {'slots': {'weapons': ws}, 'blobs': blobs}


def add_weapons(skin, wsk):
    skin['slots']['weapons'] = wsk['slots']['weapons']
    skin['blobs'].update(wsk['blobs'])
    return skin


def apply_weapons(gmo_bytes, skin):
    m = Model(gmo_bytes)
    ws = skin['slots']['weapons']
    B = skin['blobs']
    P = {p.name: i for i, p in enumerate(m.parts)}
    for w in ws['parts']:
        assert P.get(w['name']) == w['part'] and m.owner(w['part']) == w['owner'], \
            'skin weapons were made for other slots: %s' % w
        pre = 'wep.%s.' % w['name']
        m.mm.children[m.mm.children.index(m.parts[w['part']])] = _parse_one(B[pre + 'part'])
        b = m.bones[w['owner']]
        b.first(BLEND_BONES).payload = B[pre + 'blend_bones']
        b.first(BLEND_OFFSETS).payload = B[pre + 'blend_offsets']
    for t in ws['textures']:
        m.set_texture_image(t, B['wep.tex%d' % t])
    return m.to_bytes()


def _grey_like(img):
    """A flat grey image the size of a GMO texture image (keeps the texture slot, shows nothing of the old one)."""
    import numpy as np
    import charmod_gim as gim
    h, w = gim.decode(img[4:]).shape[:2]
    pal = np.zeros((256, 4), np.uint8); pal[:, :3] = 128; pal[:, 3] = 255
    return gim.wrap_for_gmo(gim.encode_indexed(np.zeros((h, w), np.uint8), pal))


def weapons_default(rig_bytes, donor_bytes, spec, info=''):
    """Weapon skin for the rig made from the donor's (the based-on character's) own weapons."""
    import charmod_gim as gim
    rig, src = Model(rig_bytes), Model(donor_bytes)
    rn, sn = rig.bone_names(), src.bone_names()
    SP = {p.name: i for i, p in enumerate(src.parts)}
    RP = {p.name: i for i, p in enumerate(rig.parts)}
    ws = weapon_slots(rig, spec)
    blobs = {}
    rr = (model_ranges(src), model_ranges(rig))

    def same_bones(pl):
        n = struct.unpack_from('<I', pl)[0]
        for v in struct.unpack_from('<%dI' % n, pl, 4):
            b = _ref_index(v)
            assert b < len(rn) and rn[b] == sn[b], 'donor bone %d (%s) is not the same bone in the rig' % (b, sn[b])

    def donor_mesh(rig_name, donor_name, mat_map=None):
        si = SP[donor_name]
        smats = src.part_materials(si)
        if mat_map is None:                       # the donor's own materials, at the same index in the rig
            for mt in smats:
                assert mt < len(rig.mats) and rig.mats[mt].to_bytes() == src.mats[mt].to_bytes(), \
                    'donor material %d (%s) is not in the rig' % (mt, src.mats[mt].name)
            mat_map = {mt: mt for mt in smats}
        part = _remap_part(src.parts[si].to_bytes(), mat_map, name=rig_name, ranges=rr)
        so, ro = src.owner(si), rig.owner(RP[rig_name])
        sbb = src.bones[so].first(BLEND_BONES).payload
        sbo = src.bones[so].first(BLEND_OFFSETS).payload
        if rn[ro] == sn[so]:
            same_bones(sbb)
            return part, sbb, sbo
        # another bone (the thrown sword): every vertex on the rig part's own bone, held as on the donor's first bone
        n = struct.unpack_from('<I', sbb)[0]
        rbb = rig.bones[ro].first(BLEND_BONES).payload
        first = struct.unpack_from('<I', rbb, 4)[0]
        off0 = sbo[4:4 + 64]
        return part, struct.pack('<I%dI' % n, n, *([first] * n)), struct.pack('<I', n) + off0 * n

    for rname, dname in spec.get('parts', {}).items():
        part, bb, bo = donor_mesh(rname, dname)
        pre = 'wep.%s.' % rname
        blobs[pre + 'part'], blobs[pre + 'blend_bones'], blobs[pre + 'blend_offsets'] = part, bb, bo
    ghosts = spec.get('ghosts', {})
    gtex = set()
    if ghosts.get('parts'):
        of = ghosts['of']
        smats = src.part_materials(SP[of])
        rgba = {}
        for k, gname in enumerate(ghosts['parts']):
            gm = rig.part_materials(RP[gname])
            assert len(gm) >= len(smats), '%s has fewer materials than the donor %s' % (gname, of)
            mat_map = {mt: gm[j] for j, mt in enumerate(smats)}
            part = _remap_part(src.parts[SP[of]].to_bytes(), mat_map, name=gname, ranges=rr)
            so = src.owner(SP[of])
            sbb = src.bones[so].first(BLEND_BONES).payload
            same_bones(sbb)
            pre = 'wep.%s.' % gname
            blobs[pre + 'part'] = part
            blobs[pre + 'blend_bones'] = sbb
            blobs[pre + 'blend_offsets'] = src.bones[so].first(BLEND_OFFSETS).payload
            for j, mt in enumerate(smats):
                for a, t in zip(src.mat_textures(mt), rig.mat_textures(gm[j])):
                    if a not in rgba:
                        rgba[a] = gim.decode(src.texture_image(a)[4:])
                    size = gim.decode(rig.texture_image(t)[4:]).shape[1]
                    idx = ghost_indices(rgba[a], k % 4, size=size)
                    blobs['wep.tex%d' % t] = gim.wrap_for_gmo(gim.encode_indexed(idx, ghost_palette(k % 4)))
                    gtex.add(t)
    for t in ws['textures']:
        if t not in gtex:
            blobs['wep.tex%d' % t] = _grey_like(rig.texture_image(t))
    return {'info': info, 'slots': {'weapons': ws}, 'blobs': blobs}


def weapons_from_model(rig_bytes, src_bytes, spec, info=''):
    """Weapon skin for the rig from another model's weapons (e.g. the Blender export after a skin recipe fitted new
    weapons into the slots). Parts are matched by name (spec parts: rig part -> part in src). Each goes on the rig
    material(s) in spec['skin_materials'] with src's texture images in those materials' textures; bones as in
    weapons_default. Blade ghosts are rebuilt from src's ghosts['of'] part. Unfilled weapon textures go grey."""
    import charmod_gim as gim
    rig, src = Model(rig_bytes), Model(src_bytes)
    rn, sn = rig.bone_names(), src.bone_names()
    SP = {p.name: i for i, p in enumerate(src.parts)}
    RP = {p.name: i for i, p in enumerate(rig.parts)}
    ws = weapon_slots(rig, spec)
    owned = set(ws['textures'])
    blobs = {}
    rr = (model_ranges(src), model_ranges(rig))

    def bones_for(rig_name, si):
        so, ro = src.owner(si), rig.owner(RP[rig_name])
        sbb = src.bones[so].first(BLEND_BONES).payload
        sbo = src.bones[so].first(BLEND_OFFSETS).payload
        n = struct.unpack_from('<I', sbb)[0]
        if rn[ro] == sn[so]:
            for v in struct.unpack_from('<%dI' % n, sbb, 4):
                b = _ref_index(v)
                assert b < len(rn) and rn[b] == sn[b], 'bone %d (%s) is not the same bone in the rig' % (b, sn[b])
            return sbb, sbo
        rbb = rig.bones[ro].first(BLEND_BONES).payload
        first = struct.unpack_from('<I', rbb, 4)[0]
        return struct.pack('<I%dI' % n, n, *([first] * n)), struct.pack('<I', n) + sbo[4:4 + 64] * n

    def put(rig_name, src_name, targets):
        si = SP[src_name]
        smats = src.part_materials(si)
        assert len(smats) <= len(targets), '%s has %d materials, the slot %d' % (src_name, len(smats), len(targets))
        mat_map = {mt: targets[j] for j, mt in enumerate(smats)}
        pre = 'wep.%s.' % rig_name
        blobs[pre + 'part'] = _remap_part(src.parts[si].to_bytes(), mat_map, name=rig_name, index=RP[rig_name], ranges=rr)
        blobs[pre + 'blend_bones'], blobs[pre + 'blend_offsets'] = bones_for(rig_name, si)
        for j, mt in enumerate(smats):
            for a, t in zip(src.mat_textures(mt), rig.mat_textures(targets[j])):
                assert t in owned, 'texture %d of material %d is not a weapon-only texture' % (t, targets[j])
                blobs['wep.tex%d' % t] = src.texture_image(a)

    for rname, sname in spec.get('parts', {}).items():
        put(rname, sname, spec['skin_materials'][rname])
    ghosts = spec.get('ghosts', {})
    if ghosts.get('parts'):
        of = ghosts['of']
        si = SP[of]
        smats = src.part_materials(si)
        rgba = {}
        for k, gname in enumerate(ghosts['parts']):
            gm = rig.part_materials(RP[gname])
            assert len(gm) >= len(smats), '%s has fewer materials than %s' % (gname, of)
            pre = 'wep.%s.' % gname
            blobs[pre + 'part'] = _remap_part(src.parts[si].to_bytes(), {mt: gm[j] for j, mt in enumerate(smats)}, name=gname,
                                              ranges=rr)
            sbb, sbo = bones_for(of, si)
            blobs[pre + 'blend_bones'], blobs[pre + 'blend_offsets'] = sbb, sbo
            for j, mt in enumerate(smats):
                for a, t in zip(src.mat_textures(mt), rig.mat_textures(gm[j])):
                    if a not in rgba:
                        rgba[a] = gim.decode(src.texture_image(a)[4:])
                    size = gim.decode(rig.texture_image(t)[4:]).shape[1]
                    blobs['wep.tex%d' % t] = gim.wrap_for_gmo(gim.encode_indexed(ghost_indices(rgba[a], k % 4, size=size),
                                                                               ghost_palette(k % 4)))
    for t in ws['textures']:
        if 'wep.tex%d' % t not in blobs:
            blobs['wep.tex%d' % t] = _grey_like(rig.texture_image(t))
    return {'info': info, 'slots': {'weapons': ws}, 'blobs': blobs}


# ---------------------------------------------------------------- ghost tint (same as forge dfx/warp.py)
GHOST_TEX = 128
GHOST_CUTS = {0: None, 1: None, 2: 0.42, 3: 0.68}


def _value_noise(n, cells, seed):
    import numpy as np
    rng = np.random.default_rng(seed)
    g = rng.random((cells + 1, cells + 1))
    x = np.linspace(0, cells, n, endpoint=False)
    i = x.astype(int)
    t = x - i
    t = t * t * (3 - 2 * t)
    a = g[i][:, i]; b = g[i][:, i + 1]; c = g[i + 1][:, i]; d = g[i + 1][:, i + 1]
    ty, tx = t[:, None], t[None, :]
    return (a * (1 - tx) + b * tx) * (1 - ty) + (c * (1 - tx) + d * tx) * ty


def ghost_palette(stage):
    import numpy as np
    t = np.linspace(0, 1, 254)[:, None]
    if stage == 0:
        rgb = np.c_[0.05 + 0.25 * t, 0.08 + 0.30 * t, 0.30 + 0.55 * t]
    else:
        rgb = np.c_[0.55 + 0.45 * t, 0.62 + 0.38 * t, 0.95 + 0.05 * t]
    pal = np.zeros((256, 4)); pal[:254, :3] = rgb; pal[:254, 3] = 1; pal[254] = 1
    return np.round(pal * 255).astype(np.uint8)


def ghost_indices(rgba, stage, size=GHOST_TEX):
    import numpy as np
    h, w = rgba.shape[:2]
    ys = (np.arange(size) * h // size); xs = (np.arange(size) * w // size)
    src = rgba[ys][:, xs].astype(float) / 255
    lum = np.clip(0.3 * src[..., 0] + 0.59 * src[..., 1] + 0.11 * src[..., 2], 0, 1)
    idx = np.round(lum * 253).astype(np.uint8)
    idx[src[..., 3] < 0.5] = 255
    cut = GHOST_CUTS[stage]
    if cut is not None:
        noise = 0.6 * _value_noise(size, 6, 3) + 0.4 * _value_noise(size, 14, 5)
        idx[(noise < cut + 0.04) & (idx != 255)] = 254
        idx[noise < cut] = 255
    return idx
