"""Live character lighting for the native build (recomp only).

The game's characters carry no vertex normals: the GE can only give them flat ambient, and all their shading is
painted into the textures. For a body that should catch the light (mod.json  body.lighting, see
docs/character-lighting.md), the build
  * adds smooth vertex normals to the body part (add_normals), computed from the mesh itself, so any skin works,
    whichever Blender add-on made it;
  * adds an FX shader draw for every body texture (fx_draws), matched on the texture's texture-pack id, that draws
    the body with a key light, a rim light and the game's own colour (fx_shaders/lit/character.frag).
A stock PSP has no FX shaders and draws the body as before (the normals are ignored without GE diffuse lights), but
the mod is marked recomp-only: the extra 4 bytes per vertex count against the PSP's memory.
"""
import os
import struct

import charmod_chunks as C

DEFAULTS = {                        # u_* uniforms of fx_shaders/lit/character.frag; mod.json body.lighting overrides
                                    # (the Move Studio's preview has a copy: studio/lighting.js LIT_DEFAULTS)
    'key_dir': [-0.35, 0.55, 0.76],   # towards the key light, camera space (x right, y up, z towards the viewer)
    'key': 1.0,                       # how much the light shapes the body (0 = the game's flat look)
    'wrap': 0.6,                      # how far the light wraps round the body: higher = more of it lit
    'threshold': 0.4,                 # where the shadow starts
    'edge': 0.04,                     # softness of the light/shadow edge (0.02 crisp ... 0.5 smooth)
    'bands': 2,                       # tones: 2 = lit + shadow (Duellum), 3 adds a mid tone, 0 = smooth gradient
    'light': '#fffaf2',               # tint of the lit tone
    'shadow': '#8b889c',              # tint of the shadow tone (multiplied in)
    'shadow_sat': 0.3,                # how much richer the shadow colours are
    'spec': 0.25,                     # hard highlight on bright parts (hair, metal, skin)
    'spec_size': 0.15,                # highlight size
    'rim': 0.75,                      # rim light strength
    'rim_width': 0.18,                # rim width
    'rim_dir': [0.6, 0.3, -0.75],     # which side the rim is on, camera space (right and up)
    'rim_colour': '#e2efff',
    'face': 0.7,                      # build time, not a uniform: how round the head's shading is (0 = as modelled)
}
NOT_UNIFORMS = {'face'}

# Named looks (mod.json body.lighting.preset): each changes some of the defaults; settings given beside the preset
# change it further. 'cel' is the defaults. The Move Studio's Lighting panel has a copy (studio/lighting.js LIT_PRESETS).
PRESETS = {
    'cel': {},                                                          # Duellum: two tones, crisp edge
    'anime': {'threshold': 0.46, 'edge': 0.015, 'shadow': '#7d7895', 'shadow_sat': 0.45, 'spec': 0.35,
              'rim': 0.9, 'rim_width': 0.14},                           # harder edge, deeper and richer shadows
    'three_tone': {'bands': 3, 'threshold': 0.3, 'edge': 0.035, 'shadow': '#86829a'},     # lit, mid, shadow
    'soft': {'bands': 0, 'key': 0.8, 'wrap': 0.45, 'shadow': '#727aa6', 'shadow_sat': 0.15, 'spec': 0.1,
             'rim': 0.55, 'rim_width': 0.3, 'face': 0.5},               # smooth gradient, no hard edge
    'dramatic': {'key_dir': [-0.8, 0.45, 0.4], 'threshold': 0.5, 'edge': 0.03, 'shadow': '#5e5878',
                 'shadow_sat': 0.45, 'spec': 0.3, 'rim': 1.2, 'rim_width': 0.22, 'rim_colour': '#ffe0bd'},
                                                                        # light from the side, strong warm rim
    'subtle': {'bands': 0, 'key': 0.45, 'wrap': 0.7, 'shadow': '#7a80a0', 'shadow_sat': 0.1, 'spec': 0.0,
               'rim': 0.35, 'rim_width': 0.25, 'face': 0.5},            # a little form, close to the retail look
}
SHADER = 'lit/character.frag'
SHADER_GAME_PATH = 'fx_shaders/' + SHADER                  # where the build installs it in the mods dir
SHADER_SRC = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'fx_shaders', SHADER)


def _vformat():
    import charmod_gmo as G
    return G._vertex_format()


def _arrays_by_ref(p):
    return {i: a for i, a in enumerate(p.find(C.ARRAYS))}


def add_normals(part_bytes, ranges, crease_deg=60.0, head=None):
    """The part with s8 vertex normals in every vertex array (arrays that already have normals are kept).
    ranges: model_ranges() of the model the part is drawn in (positions are stored normalised against them).
    head: (owner's BlendBones as bone indices, {head bone indices}, amount): the head's normals are bent towards a
    sphere round it by amount x its weight, the cel-shading trick that keeps faces and hair clean (one soft light /
    shadow side instead of blotches from the nose, brows and strands)."""
    import numpy as np
    gmo = _vformat()
    p = C.parse_chunks(part_bytes, 0, len(part_bytes))[0]
    arrays = _arrays_by_ref(p)
    off_, scl = (np.array(ranges['pos'][0]), np.array(ranges['pos'][1])) if 'pos' in ranges else (np.zeros(3), np.ones(3))
    info = {}
    for i, a in arrays.items():
        word, count, nmorph, flags = struct.unpack_from('<IIII', a.payload, 0)
        fmt = gmo.VertexFormat(word)
        if fmt.normal or nmorph != 1:
            continue
        va = gmo.VertexArray.from_chunk(a)
        info[i] = (fmt, va, va.positions_norm() * scl + off_)
    if not info:
        return part_bytes
    # every triangle of the part, as (array, vertex) corners
    tris = []
    for mesh in p.find(C.MESH):
        for dc in mesh.find(0x8066):
            d = gmo.DrawCall.from_payload(dc.payload)
            ai = d.array_ref & 0xFFF
            if ai in info:
                tris += [(ai, t) for t in d.triangles()]
    # weld corners by position across arrays (the exporter splits vertices at UV / colour seams and bone batches)
    keys, gid = {}, {}
    ext = max(float(np.ptp(np.concatenate([v[2] for v in info.values()]), 0).max()), 1e-6)
    q = 1e-4 * ext
    for ai, (_, _, P) in info.items():
        for vi, pos in enumerate(P):
            k = tuple(np.round(pos / q).astype(np.int64))
            gid[(ai, vi)] = keys.setdefault(k, len(keys))
    fn, fv = [], []
    for ai, (a, b, c) in tris:
        P = info[ai][2]
        n = np.cross(P[b] - P[a], P[c] - P[a])          # area weighted
        fn.append(n)
        fv.append(((ai, a), (ai, b), (ai, c)))
    fn = np.array(fn) if fn else np.zeros((0, 3))
    unit = fn / np.maximum(np.linalg.norm(fn, axis=1, keepdims=True), 1e-12)
    at = {}                                             # welded position -> faces
    for f, corners in enumerate(fv):
        for cv in corners:
            at.setdefault(gid[cv], []).append(f)
    own = {}                                            # array vertex -> faces that use it directly
    for f, corners in enumerate(fv):
        for cv in corners:
            own.setdefault(cv, []).append(f)
    vol = sum(float(np.dot(info[ai][2][a], np.cross(info[ai][2][b], info[ai][2][c]))) for ai, (a, b, c) in tris)
    if vol < 0:                                         # wound the other way (clockwise front faces): turn them out
        fn, unit = -fn, -unit
    cos_c = np.cos(np.radians(crease_deg))
    normals, weights = {}, {}
    if head:
        subset = {}                                     # array -> its mesh's BlendSubset (indices into BlendBones)
        for mesh in p.find(C.MESH):
            sub = mesh.first(0x8062)
            ids = list(struct.unpack_from('<%dI' % struct.unpack_from('<I', sub.payload)[0], sub.payload, 4)) if sub else []
            for dc in mesh.find(0x8066):
                subset.setdefault(gmo.DrawCall.from_payload(dc.payload).array_ref & 0xFFF, ids)
    for ai, (fmt, va, P) in info.items():
        N = np.zeros((len(P), 3))
        for vi in range(len(P)):
            mine = own.get((ai, vi))
            if not mine:
                N[vi] = (0, 0, 1)
                continue
            seed = unit[mine].sum(0)
            seed /= max(np.linalg.norm(seed), 1e-12)
            fs = at[gid[(ai, vi)]]
            keep = [f for f in fs if unit[f] @ seed >= cos_c]
            N[vi] = fn[keep].sum(0) if keep else seed
        normals[ai] = N / np.maximum(np.linalg.norm(N, axis=1, keepdims=True), 1e-12)
        if head:
            blend, hb, _ = head
            W, sub = va.weights(), subset.get(ai, [])
            w = np.zeros(len(P))
            if W is not None:
                for j in range(min(W.shape[1], len(sub))):
                    if sub[j] < len(blend) and blend[sub[j]] in hb:
                        w += W[:, j]
            weights[ai] = np.clip(w, 0, 1)
    if head and head[2] > 0:
        allP = np.concatenate([info[ai][2] for ai in weights]); allW = np.concatenate([weights[ai] for ai in weights])
        if (allW > 0.5).sum() > 8:
            sel = allP[allW > 0.5]
            centre = sel.mean(0)                        # the head's middle (hair spikes pull it a little: fine)
            for ai, w in weights.items():
                S_ = info[ai][2] - centre
                S_ /= np.maximum(np.linalg.norm(S_, axis=1, keepdims=True), 1e-12)
                k = (head[2] * w)[:, None]
                Nb = normals[ai] * (1 - k) + S_ * k
                normals[ai] = Nb / np.maximum(np.linalg.norm(Nb, axis=1, keepdims=True), 1e-12)
    out = {}
    for ai, (fmt, va, P) in info.items():
        # the stored value goes through the same scale as the stored position (the normalised range): divide by
        # scale^2 so that after it the direction is the surface normal's (inverse transpose of a scale)
        N = normals[ai] / (scl * scl)
        N /= np.maximum(np.linalg.norm(N, axis=1, keepdims=True), 1e-12)
        out[ai] = _with_normals(gmo, fmt, va, N)
    for ai, payload in out.items():
        a = arrays[ai]
        a.payload = payload
        hs = 8 + len(a.header)                        # leaf header: total chunk size, offset of the first vertex
        a.header = struct.pack('<II', hs + len(payload), hs + 16) + a.header[8:]
    return p.to_bytes()


def _with_normals(gmo, fmt, va, N):
    """A copy of the array in a format with s8 normals: every other field byte for byte."""
    import numpy as np
    base = fmt.word & 0xFFFFFF
    nf = gmo.VertexFormat(base | (1 << 5))
    nf = gmo.VertexFormat((base | (1 << 5)) | (nf.layout()[1] << 24))
    so, sst = fmt.layout()
    do, dst = nf.layout()
    src = np.frombuffer(va.raw, np.uint8).reshape(va.count, fmt.stride)
    dst_buf = np.zeros((va.count, dst), np.uint8)
    sizes = {'w': fmt.nweights * gmo.VertexFormat._SZ[fmt.weight], 't': 2 * gmo.VertexFormat._SZ[fmt.tc],
             'c': gmo.VertexFormat._CSZ[fmt.color], 'p': 3 * gmo.VertexFormat._SZ[fmt.pos]}
    for k, n in sizes.items():
        if k in so and n:
            dst_buf[:, do[k]:do[k] + n] = src[:, so[k]:so[k] + n]
    dst_buf[:, do['n']:do['n'] + 3] = np.clip(np.round(N * 127), -127, 127).astype(np.int8).view(np.uint8)
    body = struct.pack('<IIII', nf.word, va.count, va.nmorph, va.flags) + dst_buf.tobytes()
    return body + b'\0' * ((-len(body)) % 4)


HEAD_BONES = ('head', 'hair')         # the bones whose vertices get the rounded head normals (mod.json lighting.face)


def head_info(m, amount):
    """add_normals' head argument for a charmod_gmo.Model: (owner's BlendBones as bone indices, head bones, amount)"""
    import charmod_gmo as G
    owner = m.bones[m.slots()['body']['owner']]
    bb = owner.first(G.BLEND_BONES)
    if not bb or amount <= 0:
        return None
    n = struct.unpack_from('<I', bb.payload)[0]
    blend = [G._ref_index(v) for v in struct.unpack_from('<%dI' % n, bb.payload, 4)]
    names = m.bone_names()
    hb = {i for i, nm in enumerate(names) if nm.lower() in HEAD_BONES}
    return (blend, hb, float(amount)) if hb else None


def light_model(model_bytes, s=None):
    """The model with normals on its body part (its warp ghosts are left as they are). s: settings() (for 'face')."""
    import charmod_gmo as G
    m = G.Model(model_bytes)
    pi = m.slots()['body']['part']
    face = (s or DEFAULTS).get('face', DEFAULTS['face'])
    new = add_normals(m.parts[pi].to_bytes(), G.model_ranges(m), head=head_info(m, face))
    m.mm.children[m.mm.children.index(m.parts[pi])] = C.parse_chunks(new, 0, len(new))[0]
    return m.to_bytes()


def body_texture_ids(model_bytes):
    """texture-pack ids of the body's textures (what the FX shader matches), in body texture order"""
    import charmod_gmo as G
    import texpack_id as TP
    m = G.Model(model_bytes)
    return [TP.gim_id(m.texture_image(t)[4:])[0] for t in m.slots()['body']['textures']]


def settings(body):
    """mod.json body.lighting -> settings dict, or None when off. true = the defaults; an object: "preset" (PRESETS)
    over the defaults, then its other settings over that."""
    v = (body or {}).get('lighting')
    if not v:
        return None
    s = dict(DEFAULTS)
    if isinstance(v, dict):
        p = v.get('preset')
        if p and p not in PRESETS:
            print('  body.lighting: no preset %r (presets: %s); using the defaults' % (p, ', '.join(PRESETS)))
        s.update(PRESETS.get(p) or {})
        s.update({k: x for k, x in v.items() if k in DEFAULTS})
    return s


def fx_draws(name, ids, s):
    """manifest draws for one character's lit body"""
    import math
    def unit(v):
        n = math.sqrt(sum(x * x for x in v)) or 1.0
        return [round(x / n, 4) for x in v]
    u = {'u_' + k: (unit(v) if k.endswith('_dir') else v) for k, v in s.items() if k not in NOT_UNIFORMS}
    return [{'name': 'lit/%s/%d' % (name, k), 'hash': '%016x' % tid, 'frag': SHADER, 'uniforms': u}
            for k, tid in enumerate(dict.fromkeys(ids))]
