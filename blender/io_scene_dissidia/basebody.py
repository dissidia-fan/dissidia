"""Generic low-poly base body for a Dissidia 012 skeleton.

Builds a plain, featureless humanoid (no hair, clothes or face) fitted to the bind pose
of a Dissidia character's armature: one closed mesh lofted from rings, with clean
edge loops at every joint, skin weights set per ring, UV islands per body part and a
flat region-coloured texture to paint over. It can take over the character's body part
so it exports in the body's place.

Everything is built in armature space (GMO is Y-up, +Z forward, +X = character's left).
"""
import math

import bpy
import bmesh
import numpy as np
from mathutils import Vector

REGION_COLOURS = {            # flat placeholder colours, meant to be painted over
    'skin': (0.80, 0.66, 0.56),
    'top': (0.30, 0.32, 0.36),
    'legs': (0.45, 0.45, 0.48),
    'feet': (0.16, 0.15, 0.15),
}
REGIONS = list(REGION_COLOURS)

TORSO_SIDES = 16
LEG_SIDES = 10
ARM_SIDES = 8
HEAD_SIDES = 12


def _unit(v):
    v = np.asarray(v, float)
    n = np.linalg.norm(v)
    return v / n if n > 1e-12 else v


class _Builder:
    def __init__(self):
        self.bm = bmesh.new()
        self.wlayer = {}            # vert -> {bone: w}
        self.region = self.bm.faces.layers.int.new('dfx_region')
        self.seams = set()          # frozenset({v1, v2})

    def ring(self, centre, X, F, rx, rf, n, weights):
        """Ring of n verts; vertex k at angle 2πk/n, k=0 on +F, k=n/4 on +X."""
        vs = []
        for k in range(n):
            t = 2 * math.pi * k / n
            co = np.asarray(centre, float) + X * rx * math.sin(t) + F * rf * math.cos(t)
            v = self.bm.verts.new(Vector(co))
            self.wlayer[v] = dict(weights)
            vs.append(v)
        return vs

    def vert(self, co, weights):
        v = self.bm.verts.new(Vector(np.asarray(co, float)))
        self.wlayer[v] = dict(weights)
        return v

    def face(self, vs, region):
        f = self.bm.faces.new(vs)
        f[self.region] = REGIONS.index(region)
        return f

    def loft(self, rings, region, skip=()):
        """Quads between consecutive rings of equal size. skip = {(ring_i, k)} quads to leave open."""
        for i, (a, b) in enumerate(zip(rings, rings[1:])):
            n = len(a)
            for k in range(n):
                if (i, k) in skip:
                    continue
                k2 = (k + 1) % n
                self.face((a[k], a[k2], b[k2], b[k]), region)

    def cap(self, ring, tip, weights, region):
        v = self.vert(tip, weights)
        n = len(ring)
        for k in range(n):
            self.face((ring[k], ring[(k + 1) % n], v), region)
        return v

    def bridge(self, loop_a, loop_b, centre, axis, ref, region):
        """Join two closed loops of any sizes around a common axis, matching by angle."""
        axis = _unit(axis)
        ref = _unit(np.asarray(ref, float) - axis * np.dot(ref, axis))
        side = np.cross(axis, ref)

        def ang(v):
            d = np.array(v.co) - centre
            return math.atan2(np.dot(d, side), np.dot(d, ref)) % (2 * math.pi)

        A = sorted(loop_a, key=ang)
        Bl = sorted(loop_b, key=ang)
        aa = [ang(v) for v in A]
        bb = [ang(v) for v in Bl]
        # start both walks at the same place: rotate B so its first vertex is closest to A[0]
        j0 = int(np.argmin([min(abs(b - aa[0]), 2 * math.pi - abs(b - aa[0])) for b in bb]))
        Bl = Bl[j0:] + Bl[:j0]
        bb = bb[j0:] + bb[:j0]
        base = aa[0]
        ua = np.unwrap(np.array(aa)) - base
        ub = np.unwrap(np.array(bb)) - base
        if ub[0] > math.pi:
            ub -= 2 * math.pi
        na, nb = len(A), len(Bl)
        ua = list(ua) + [ua[0] + 2 * math.pi]
        ub = list(ub) + [ub[0] + 2 * math.pi]
        i = j = 0
        tris = []
        while i < na or j < nb:
            if j >= nb or (i < na and ua[i + 1] <= ub[j + 1]):
                tris.append((A[i % na], A[(i + 1) % na], Bl[j % nb]))
                i += 1
            else:
                tris.append((A[i % na], Bl[(j + 1) % nb], Bl[j % nb]))
                j += 1
        made = []
        for t in tris:
            if len(set(t)) == 3:
                try:
                    made.append(self.face(t, region))
                except ValueError:
                    pass
        bmesh.ops.join_triangles(self.bm, faces=made, angle_face_threshold=math.radians(40),
                                 angle_shape_threshold=math.radians(60))
        return made

    def seam_loop(self, loop):
        n = len(loop)
        for k in range(n):
            self.seams.add(frozenset((loop[k], loop[(k + 1) % n])))

    def seam_line(self, rings, k):
        for a, b in zip(rings, rings[1:]):
            self.seams.add(frozenset((a[k % len(a)], b[k % len(b)])))


def _frame(t, hint):
    """Ring axes for a ring whose direction of travel is t: returns X, F with X×F = t."""
    t = _unit(t)
    F = _unit(hint - t * np.dot(hint, t))
    X = np.cross(F, t)
    return X, F


def build(arm):
    """Create the base body mesh object for `arm` (not yet parented). Returns (object, info)."""
    bones = arm.data.bones

    def H(n, side=None):
        if side:
            b = bones.get(f'{side}_{n}')
            if b is None and side == 'r' and bones.get(f'l_{n}') is not None:
                return np.array(bones[f'l_{n}'].head_local) * np.array([-1, 1, 1])
            return np.array(b.head_local) if b is not None else None
        b = bones.get(n)
        return np.array(b.head_local) if b is not None else None

    for need in ('hip', 'chest', 'neck', 'head'):
        if H(need) is None:
            raise RuntimeError(f'Armature has no "{need}" bone; is this a Dissidia 012 character?')
    for need in ('femur', 'tibia', 'foot', 'toe', 'uparm', 'foarm', 'hand'):
        if H(need, 'l') is None:
            raise RuntimeError(f'Armature has no "l_{need}" bone; is this a Dissidia 012 character?')

    hip, chest, neck, head = H('hip'), H('chest'), H('neck'), H('head')
    # sizes are tuned on Bartz (head bone at 1.591); other characters scale with their head height
    s = max(0.5, min(2.0, head[1] / 1.591))
    Y = lambda y: np.array([0, y * s, 0])
    up = np.array([0, 1.0, 0]); fwd = np.array([0, 0, 1.0]); xax = np.array([1.0, 0, 0])

    B = _Builder()

    # ---------------------------------------------------------------- torso
    T = TORSO_SIDES
    torso_spec = [      # centre, rx, rz, weights
        (hip + Y(-0.095) + [0, 0, 0.005], 0.135, 0.100, {'hip': 1.0}),
        (hip + Y(-0.02), 0.150, 0.105, {'hip': 0.85, 'waist': 0.15}),
        (hip + Y(0.06), 0.140, 0.098, {'hip': 0.4, 'waist': 0.6}),
        ((hip + chest) / 2 + Y(0.05), 0.130, 0.092, {'waist': 0.6, 'chest': 0.4}),
        (chest + Y(0.07) + [0, 0, 0.005], 0.142, 0.100, {'chest': 0.85, 'waist': 0.15}),
        (chest + Y(0.14) + [0, 0, 0.008], 0.152, 0.105, {'chest': 1.0}),
        (chest + Y(0.185), 0.162, 0.100, {'chest': 1.0}),
        (chest + Y(0.232) + [0, 0, -0.008], 0.150, 0.088, {'chest': 1.0}),
    ]
    torso = [B.ring(c, xax, fwd, rx * s, rz * s, T, wt) for c, rx, rz, wt in torso_spec]
    # arm openings: rings 5-7, sides k = 3..5 (left, +X) and 11..13 (right)
    A0, A1 = 5, 7
    skip = {(i, k) for i in range(A0, A1) for k in (3, 4, 11, 12)}
    B.loft(torso, 'top', skip)
    for i in range(A0, A1 + 1):                 # shoulder verts share the clavicle
        for k in (3, 4, 5, 11, 12, 13):
            side = 'l' if k < 8 else 'r'
            B.wlayer[torso[i][k]] = ({'chest': 0.5, f'{side}_clavicle': 0.5} if i > A0
                                     else {'chest': 0.75, f'{side}_clavicle': 0.25})
    B.seam_line(torso, T // 2)                  # back centre

    # crotch: front centre (k=0) and back centre (k=8) of ring 0 plus a crotch vertex split the legs
    r0 = torso[0]
    crotch = B.vert(hip + Y(-0.12) + [0, 0, -0.005], {'hip': 1.0})
    leg_loops = {'l': r0[0:9] + [crotch], 'r': r0[8:16] + [r0[0], crotch]}

    # ---------------------------------------------------------------- legs + feet
    for side, sg in (('l', 1.0), ('r', -1.0)):
        femur, tibia, foot, toe = H('femur', side), H('tibia', side), H('foot', side), H('toe', side)
        S = side + '_'
        X = xax * sg
        lx = femur[0] * 1.05
        kx = tibia[0] * 1.02
        ky, ay = tibia[1], foot[1]

        def LR(y, rx, rz, wt, x, dz=0.0):
            return B.ring([x, y, femur[2] + dz * s], X, fwd, rx * s, rz * s, LEG_SIDES, wt)

        legs = [
            LR(femur[1] - 0.06 * s, 0.088, 0.090, {S + 'femur': 0.8, 'hip': 0.2}, lx),
            LR(femur[1] * 0.55 + ky * 0.45, 0.075, 0.078, {S + 'femur': 1.0}, lx * 0.6 + kx * 0.4),
            LR(ky + 0.07 * s, 0.062, 0.064, {S + 'femur': 0.9, S + 'tibia': 0.1}, kx, 0.005),
            LR(ky, 0.058, 0.060, {S + 'femur': 0.5, S + 'tibia': 0.5}, kx, 0.01),
            LR(ky - 0.07 * s, 0.056, 0.058, {S + 'tibia': 0.9, S + 'femur': 0.1}, kx),
            LR(ky * 0.6 + ay * 0.4, 0.056, 0.062, {S + 'tibia': 1.0}, kx, -0.012),
            LR(ay + 0.11 * s, 0.044, 0.046, {S + 'tibia': 1.0}, foot[0], -0.02),
        ]
        B.loft(legs, 'legs')
        B.bridge(leg_loops[side], legs[0], np.array([lx * 0.8, float(legs[0][0].co[1]), femur[2]]),
                 up, fwd, 'legs')
        B.seam_loop(legs[0])
        B.seam_line(legs, LEG_SIDES // 2)
        # foot: rings turn from horizontal (shin) to vertical (instep/toes); k=0 stays on top/front
        fx = foot[0]
        tilt = _unit([0, 0.75, 0.66])
        feet = [
            B.ring([fx, ay + 0.015 * s, foot[2] + 0.005 * s], X, fwd, 0.046 * s, 0.050 * s, LEG_SIDES,
                   {S + 'tibia': 0.4, S + 'foot': 0.6}),
            B.ring([fx, 0.052 * s, foot[2] + 0.035 * s], X, tilt, 0.050 * s, 0.066 * s, LEG_SIDES,
                   {S + 'foot': 1.0}),
            B.ring([toe[0], 0.042 * s, toe[2] - 0.005 * s], X, up, 0.050 * s, 0.040 * s, LEG_SIDES,
                   {S + 'foot': 0.5, S + 'toe': 0.5}),
            B.ring([toe[0], 0.033 * s, toe[2] + 0.06 * s], X, up, 0.044 * s, 0.031 * s, LEG_SIDES,
                   {S + 'toe': 1.0}),
        ]
        for r in feet[1:]:                      # flatten the soles onto the ground
            for v in r:
                if v.co[1] < 0.006 * s:
                    v.co[1] = 0.0
        B.loft([legs[-1]] + feet, 'feet')
        B.cap(feet[-1], [toe[0], 0.025 * s, toe[2] + 0.095 * s], {S + 'toe': 1.0}, 'feet')
        B.seam_loop(legs[-1])
        B.seam_line([legs[-1]] + feet, LEG_SIDES // 2)

    # ---------------------------------------------------------------- arms + hands
    for side, sg in (('l', 1.0), ('r', -1.0)):
        S = side + '_'
        sh, el, wr = H('uparm', side), H('foarm', side), H('hand', side)
        fing = H('fing3b', side)
        if fing is None:
            fing = wr + (wr - el) * 0.35
        thumb = H('fing1a', side)
        cols = (3, 4, 5) if side == 'l' else (11, 12, 13)
        opening = ([torso[A0][k] for k in cols] + [torso[A0 + 1][cols[2]]]
                   + [torso[A1][k] for k in reversed(cols)] + [torso[A0 + 1][cols[0]]])
        t_up = _unit(el - sh)
        t_lo = _unit(wr - el)
        t_hd = _unit(fing - wr)

        def AR(c, t, rx, rf, wt, n=ARM_SIDES):
            Xa, Fa = _frame(t, fwd)
            return B.ring(c, Xa, Fa, rx * s, rf * s, n, wt)

        arms = [
            AR(sh + t_up * 0.065 * s, t_up, 0.050, 0.054, {S + 'uparm': 0.75, S + 'clavicle': 0.25}),
            AR(sh * 0.5 + el * 0.5, t_up, 0.046, 0.050, {S + 'uparm': 1.0}),
            AR(el - t_up * 0.05 * s, t_up, 0.041, 0.043, {S + 'uparm': 0.9, S + 'foarm': 0.1}),
            AR(el, (t_up + t_lo) / 2, 0.039, 0.041, {S + 'uparm': 0.5, S + 'foarm': 0.5}),
            AR(el + t_lo * 0.05 * s, t_lo, 0.040, 0.043, {S + 'foarm': 0.9, S + 'uparm': 0.1}),
            AR(el * 0.45 + wr * 0.55, t_lo, 0.036, 0.040, {S + 'foarm': 1.0}),
            AR(wr - t_lo * 0.015 * s, t_lo, 0.028, 0.032, {S + 'foarm': 0.5, S + 'hand': 0.5}),
        ]
        B.loft(arms, 'top')
        hand = [            # palm: thin across the hand, wide front-to-back
            AR(wr * 0.55 + fing * 0.45, t_hd, 0.022, 0.044, {S + 'hand': 1.0}),
            AR(fing, t_hd, 0.018, 0.043, {S + 'hand': 0.4, S + 'fing3b': 0.6}),
            AR(fing + t_hd * 0.045 * s, t_hd, 0.014, 0.038, {S + 'fing3b': 1.0}),
        ]
        B.loft([arms[-1]] + hand, 'skin')
        B.cap(hand[-1], fing + t_hd * 0.075 * s, {S + 'fing3b': 1.0}, 'skin')
        centre = np.mean([np.array(v.co) for v in opening], 0)
        B.bridge(opening, arms[0], centre, t_up, fwd, 'top')
        B.seam_loop(arms[0])
        B.seam_loop(arms[-1])
        B.seam_line(arms, ARM_SIDES // 2)
        B.seam_line([arms[-1]] + hand, ARM_SIDES // 2)
        if thumb is not None:          # a small closed thumb sitting on the palm
            t_th = _unit(thumb - wr + np.array([0, -0.01, 0.015]) * s)
            th = [AR(wr + (thumb - wr) * 0.6, t_th, 0.016, 0.016, {S + 'hand': 0.5, S + 'fing1a': 0.5}, 6),
                  AR(thumb + t_th * 0.02 * s, t_th, 0.013, 0.013, {S + 'fing1a': 1.0}, 6)]
            B.loft(th, 'skin')
            B.cap(th[-1], thumb + t_th * 0.045 * s, {S + 'fing1a': 1.0}, 'skin')
            B.cap(list(reversed(th[0])), wr + (thumb - wr) * 0.3, {S + 'hand': 1.0}, 'skin')
            B.seam_line(th, 3)
            B.seam_loop(th[0])

    # ---------------------------------------------------------------- neck + head
    N = HEAD_SIDES
    neck_rings = [
        B.ring(neck + [0, -0.005 * s, 0.0], xax, fwd, 0.056 * s, 0.056 * s, N, {'chest': 0.3, 'neck': 0.7}),
        B.ring(neck + Y(0.06) + [0, 0, 0.005 * s], xax, fwd, 0.050 * s, 0.052 * s, N, {'neck': 0.6, 'head': 0.4}),
    ]
    hz = head[2] + 0.012 * s
    head_spec = [     # y offset from head bone, rx, rz, z offset
        (-0.025, 0.058, 0.072, 0.022),
        (0.020, 0.080, 0.096, 0.008),
        (0.075, 0.092, 0.106, 0.0),
        (0.130, 0.086, 0.100, -0.004),
        (0.170, 0.060, 0.072, -0.008),
    ]
    head_rings = [B.ring([head[0], head[1] + dy * s, hz + dz * s], xax, fwd, rx * s, rz * s, N, {'head': 1.0})
                  for dy, rx, rz, dz in head_spec]
    B.loft(neck_rings, 'skin')
    B.loft([neck_rings[-1]] + head_rings, 'skin')
    B.cap(head_rings[-1], [head[0], head[1] + 0.188 * s, hz - 0.01 * s], {'head': 1.0}, 'skin')
    B.bridge(torso[-1], neck_rings[0], np.array([0, torso_spec[-1][0][1], neck[2]]), up, fwd, 'top')
    B.seam_loop(neck_rings[0])
    B.seam_line(neck_rings + head_rings, N // 2)

    # ---------------------------------------------------------------- finish
    bm = B.bm
    bmesh.ops.recalc_face_normals(bm, faces=bm.faces)
    for e in bm.edges:
        if frozenset(e.verts) in B.seams:
            e.seam = True
    bm.verts.index_update()
    vweights = [B.wlayer.get(v, {}) for v in bm.verts]
    me = bpy.data.meshes.new(f'{arm.name}_base_body')
    bm.to_mesh(me)
    bm.free()
    ob = bpy.data.objects.new(me.name, me)
    for c in arm.users_collection:
        c.objects.link(ob)
    groups = {}
    for vi, wt in enumerate(vweights):
        for bn, wv in wt.items():
            if bn not in bones:
                continue
            g = groups.get(bn) or ob.vertex_groups.new(name=bn)
            groups[bn] = g
            g.add([vi], float(wv), 'REPLACE')
    for p in me.polygons:
        p.use_smooth = True
    me.calc_loop_triangles()
    return ob, {'tris': len(me.loop_triangles), 'verts': len(me.vertices), 'scale': s}


# ---------------------------------------------------------------- UVs + texture
def unwrap(ob):
    view_layer = bpy.context.view_layer
    prev = view_layer.objects.active
    prev_sel = [o for o in view_layer.objects if o.select_get()]
    for o in prev_sel:
        o.select_set(False)
    ob.select_set(True)
    view_layer.objects.active = ob
    if not ob.data.uv_layers:
        ob.data.uv_layers.new(name='UVMap')
    bpy.ops.object.mode_set(mode='EDIT')
    bpy.ops.mesh.select_all(action='SELECT')
    bpy.ops.uv.unwrap(method='ANGLE_BASED', margin=0.02)
    try:
        bpy.ops.uv.pack_islands(margin=0.015, rotate=True)
    except Exception:
        pass
    bpy.ops.object.mode_set(mode='OBJECT')
    ob.select_set(False)
    for o in prev_sel:
        o.select_set(True)
    view_layer.objects.active = prev


def make_texture(ob, size=128, name='base_body_tex'):
    """Fill each face's UV area with its region colour (with a few pixels of bleed)."""
    me = ob.data
    reg = me.attributes.get('dfx_region')
    freg = np.zeros(len(me.polygons), np.int32)
    if reg is not None:
        reg.data.foreach_get('value', freg)
    img = bpy.data.images.get(name) or bpy.data.images.new(name, size, size, alpha=True)
    if tuple(img.size) != (size, size):
        img.scale(size, size)
    buf = np.zeros((size, size, 3), np.float32)
    filled = np.zeros((size, size), bool)
    uv = np.empty(len(me.loops) * 2); me.uv_layers[0].data.foreach_get('uv', uv); uv = uv.reshape(-1, 2)
    me.calc_loop_triangles()
    yy, xx = np.mgrid[0:size, 0:size]
    pix = np.stack([(xx + 0.5) / size, (yy + 0.5) / size], -1)
    for lt in me.loop_triangles:
        colr = REGION_COLOURS[REGIONS[freg[lt.polygon_index]]]
        a, b, c = (uv[l] for l in lt.loops)
        lo = np.clip(np.floor(np.minimum(np.minimum(a, b), c) * size).astype(int) - 1, 0, size - 1)
        hi = np.clip(np.ceil(np.maximum(np.maximum(a, b), c) * size).astype(int) + 1, 0, size)
        sub = pix[lo[1]:hi[1], lo[0]:hi[0]]
        v0, v1, v2 = b - a, c - a, sub - a
        den = v0[0] * v1[1] - v1[0] * v0[1]
        if abs(den) < 1e-14:
            continue
        u = (v2[..., 0] * v1[1] - v1[0] * v2[..., 1]) / den
        v = (v0[0] * v2[..., 1] - v2[..., 0] * v0[1]) / den
        inside = (u >= -1e-3) & (v >= -1e-3) & (u + v <= 1 + 1e-3)
        blk = buf[lo[1]:hi[1], lo[0]:hi[0]]
        blk[inside] = colr
        filled[lo[1]:hi[1], lo[0]:hi[0]] |= inside
    for _ in range(4):                       # bleed colours outward so filtering never picks up the gaps
        for dy, dx in ((0, 1), (0, -1), (1, 0), (-1, 0)):
            src = np.roll(filled, (dy, dx), (0, 1))
            take = ~filled & src
            buf[take] = np.roll(buf, (dy, dx), (0, 1))[take]
            filled |= take
    buf[~filled] = REGION_COLOURS['top']
    rgba = np.concatenate([buf, np.ones((size, size, 1), np.float32)], -1)
    img.pixels.foreach_set(rgba.reshape(-1))
    img.pack()
    return img


def make_material_for(ob, img, name='base_body_mat'):
    from . import common
    mat = bpy.data.materials.get(name) or common.make_material(name, img)
    ob.data.materials.clear()
    ob.data.materials.append(mat)
    col = ob.data.color_attributes.get('Col') or ob.data.color_attributes.new('Col', 'FLOAT_COLOR', 'POINT')
    col.data.foreach_set('color', np.ones(len(col.data) * 4, np.float32))
    return mat


# ---------------------------------------------------------------- entry points
def body_object(arm):
    """The armature's current body part (part 0)."""
    for o in arm.children:
        if o.type == 'MESH' and o.get('dfx_part_index') == 0:
            return o
    return None


def create(arm, replace=False, tex_size=128):
    """Build the base body on `arm`. With replace=True it takes over the body part (part 0)."""
    ob, info = build(arm)
    ob.parent = arm
    ob.matrix_parent_inverse.identity()
    ob.matrix_basis.identity()
    mod = ob.modifiers.new('Armature', 'ARMATURE')
    mod.object = arm
    unwrap(ob)
    img = make_texture(ob, tex_size, name=f'base_body_tex_{arm.name}')
    make_material_for(ob, img, name=f'base_body_mat_{arm.name}')
    ob['dfx_base_body'] = True
    msg = f"Base body: {info['tris']} triangles, {len(ob.vertex_groups)} bones"
    if replace:
        msg += '; ' + replace_body(arm, ob)
    return ob, msg


def replace_body(arm, new_ob):
    """Make new_ob export as the body part. The old body is kept, unparented and hidden, as a reference."""
    old = body_object(arm)
    if old is None or old is new_ob:
        return 'no existing body part found'
    pi, draw = old['dfx_part_index'], old.get('dfx_draw_bone')
    if old.get('dfx_base_body'):           # replacing an earlier base body: just swap it out
        name = old.name
        bpy.data.objects.remove(old)
        new_ob.name = name
        new_ob.data.name = name
        new_ob['dfx_part_index'] = pi
        if draw is not None:
            new_ob['dfx_draw_bone'] = draw
        return 'it replaced the previous base body'
    mw = old.matrix_world.copy()
    old.parent = None
    old.matrix_world = mw
    del old['dfx_part_index']
    old['dfx_reference_only'] = True
    old.name = old.name + '_original'
    ref = bpy.data.collections.get('Original body (not exported)')
    if ref is None:
        ref = bpy.data.collections.new('Original body (not exported)')
        bpy.context.scene.collection.children.link(ref)
    for c in list(old.users_collection):
        c.objects.unlink(old)
    ref.objects.link(old)
    try:
        old.hide_set(True)
    except Exception:
        pass
    old.hide_render = True
    new_ob['dfx_part_index'] = pi
    if draw is not None:
        new_ob['dfx_draw_bone'] = draw
    return f'it is now the body part (the old body is hidden in "{ref.name}")'
