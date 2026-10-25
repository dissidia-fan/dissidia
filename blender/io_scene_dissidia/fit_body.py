"""Fit a humanoid FBX body (Unity/Mixamo-style bone names) onto a Dissidia 012 armature and make it the body part.

Used by own_model.py (your own model as a character's body); configure() takes the fit's settings. Made from forge/noctis/noctis_body.py (Noctis's War of the Visions body).
    from io_scene_dissidia import fit_body; fit_body.configure(params); fit_body.run(fbx_path, tex_path, arm_name=...)

Each source bone gets its own affine map that carries its segment (joint -> next joint) onto the matching game
segment: rotated onto the game bone's direction, stretched along it to the game's length and scaled across it by a
girth factor. Vertices are moved by the weight-blended maps (like skinning), so the mesh lands in the game's bind
pose with the game's proportions, and joints line up with the bones the animations move.
"""
import math
import bpy
import numpy as np
from mathutils import Vector, Matrix

# source bone -> game bone (weights). Left/Right expand to l_/r_.
WEIGHT_MAP = {
    'Hips': 'hip', 'Spine': 'waist', 'Chest': 'chest', 'Neck': 'neck', 'Head': 'head',
    'Add_Skirt_00': 'hip', 'Root_Skirt': 'hip',
}
for side, s in (('Left', 'l_'), ('Right', 'r_')):
    WEIGHT_MAP.update({side + 'Shoulder': s + 'clavicle', side + 'Arm': s + 'uparm', side + 'ForeArm': s + 'foarm',
                       side + 'Hand': s + 'hand', side + 'UpLeg': s + 'femur', side + 'Leg': s + 'tibia',
                       side + 'Foot': s + 'foot'})

GIRTH = {'torso': 1.30, 'arm': 1.25, 'leg': 1.15, 'head': 1.43, 'foot': 1.25}


def _rot(a, b):
    a = Vector(a).normalized(); b = Vector(b).normalized()
    return a.rotation_difference(b).to_matrix().to_4x4()


def _seg_map(an, cn, ab, cb, girth):
    """4x4 taking segment an->cn onto ab->cb: stretch along, girth across, rotate, translate."""
    an, cn, ab, cb = map(Vector, (an, cn, ab, cb))
    dn = cn - an
    d = dn.normalized()
    along = (cb - ab).length / dn.length
    S = Matrix.Identity(3) * girth + (along - girth) * Matrix(np.outer(d, d).tolist())
    M = _rot(dn, cb - ab) @ S.to_4x4()
    return Matrix.Translation(ab) @ M @ Matrix.Translation(-an)


def _rigid_map(an, ab, R, scale):
    S = Matrix.Diagonal(Vector(scale)).to_4x4() if not isinstance(scale, (int, float)) else Matrix.Scale(scale, 4)
    return Matrix.Translation(Vector(ab)) @ R @ S @ Matrix.Translation(-Vector(an))


def build_maps(src_arm, arm):
    P = lambda n: src_arm.matrix_world @ src_arm.data.bones[n].head_local
    G = lambda n: arm.matrix_world @ arm.data.bones[n].head_local
    maps = {}
    # torso column: top of the legs -> neck (keeps the pelvis/leg seam and the neck seam continuous)
    an = Vector((0, P('Hips').y, P('LeftUpLeg').z)); ab = Vector((0, G('hip').y, G('l_femur').z))
    torso = _seg_map(an, P('Neck'), ab, G('neck'), GIRTH['torso'])
    for n in ('Hips', 'Spine', 'Chest', 'Neck', 'Root_Skirt', 'Add_Skirt_00', 'Root'):
        maps[n] = torso
    # head: rigid, pinned at the neck joint
    maps['Head'] = _rigid_map(P('Neck'), G('neck'), Matrix.Identity(4), GIRTH['head'])
    for side, s in (('Left', 'l_'), ('Right', 'r_')):
        maps[side + 'Shoulder'] = _seg_map(P(side + 'Shoulder'), P(side + 'Arm'), G(s + 'clavicle'), G(s + 'uparm'), GIRTH['arm'])
        maps[side + 'Arm'] = _seg_map(P(side + 'Arm'), P(side + 'ForeArm'), G(s + 'uparm'), G(s + 'foarm'), GIRTH['arm'])
        maps[side + 'ForeArm'] = _seg_map(P(side + 'ForeArm'), P(side + 'Hand'), G(s + 'foarm'), G(s + 'hand'), GIRTH['arm'])
        R = _rot(P(side + 'Hand') - P(side + 'ForeArm'), G(s + 'hand') - G(s + 'foarm'))
        maps[side + 'Hand'] = _rigid_map(P(side + 'Hand'), G(s + 'hand'), R, GIRTH['arm'])
        maps[side + 'UpLeg'] = _seg_map(P(side + 'UpLeg'), P(side + 'Leg'), G(s + 'femur'), G(s + 'tibia'), GIRTH['leg'])
        maps[side + 'Leg'] = _seg_map(P(side + 'Leg'), P(side + 'Foot'), G(s + 'tibia'), G(s + 'foot'), GIRTH['leg'])
        # foot: ankle onto ankle, sole kept on the ground
        k = GIRTH['foot']; kz = G(s + 'foot').z / P(side + 'Foot').z
        maps[side + 'Foot'] = _rigid_map(P(side + 'Foot'), G(s + 'foot'), Matrix.Identity(4), (k, k, kz))
    return maps


def _resized_image(path, size):
    src = bpy.data.images.load(path, check_existing=False)
    name = 'noctis_body_tex'
    old = bpy.data.images.get(name)
    if old is not None and old != src:
        bpy.data.images.remove(old)
    src.name = name
    if tuple(src.size) != (size, size):
        src.scale(size, size)
    src.pack()
    return src


# Where the eye parts sit in FF15ncts_00.png, as fractions of the atlas (x0, y0, x1, y1; y from the top)
IRIS_BOX = (355 / 1024, 8 / 1024, 418 / 1024, 72 / 1024)
IRIS_RIGHT = 406 / 1024
LASH_BOXES = ((62 / 1024, 74 / 1024, 130 / 1024, 112 / 1024), (186 / 1024, 74 / 1024, 256 / 1024, 112 / 1024))


def tweak_eyes(a):
    """Recolour the user's own eye texture toward the Dissidia cast's eyes. `a` is float RGB, row 0 = top, any size.
    Iris: the flat grey upper half becomes a deep blue that lightens downward, the blue gets richer,
    the white centre becomes a dark pupil with a small catchlight. Lash lines get darker and a touch heavier."""
    H, W = a.shape[:2]
    x0, y0, x1, y1 = (int(round(v * s)) for v, s in zip(IRIS_BOX, (W, H, W, H)))
    sub = a[y0:y1, x0:x1].copy()
    h, s, v = _rgb_to_hsv(np.clip(sub, 0, 1))
    blue = (h > 0.52) & (h < 0.70) & (s > 0.3) & (v > 0.3)
    if blue.sum() < 4:
        return a                                               # not this atlas; leave it alone
    bh, bs = float(np.median(h[blue])), float(np.median(s[blue]))
    grey = (s < 0.18) & (v > 0.35) & (v < 0.88)
    white = (v > 0.88) & (s < 0.2)
    yy, xx = np.mgrid[0:sub.shape[0], 0:sub.shape[1]]
    near = xx < (IRIS_RIGHT * W - x0)                          # the eyeball-white piece starts just right of the iris
    grey &= near; white &= near; blue &= near
    iris0 = blue | grey | white
    cy, cx = yy[iris0].mean(), xx[iris0].mean()
    r = np.sqrt(iris0.sum() / np.pi)
    inside = ((yy - cy) / (1.1 * r)) ** 2 + ((xx - cx) / (1.0 * r)) ** 2 < 1
    grey &= inside; white &= inside; blue &= inside
    iris = blue | grey | white
    t = np.clip((yy - (cy - r)) / (2 * r), 0, 1)                # 0 at the top of the iris, 1 at the bottom
    nh = np.full_like(h, bh)
    ns = np.clip(bs * 1.15 + 0.1 * (1 - t), 0, 1)
    nv = np.clip(0.32 + 0.66 * t ** 0.8, 0, 1)                 # deep at the top, bright at the bottom
    rgb_iris = _hsv_to_rgb(nh, ns, nv)
    sub[grey | blue] = rgb_iris[grey | blue]
    sub[white] = np.array([0.05, 0.06, 0.11], np.float32)       # pupil
    # dark limbal edge just inside the outline
    d = np.sqrt((yy - cy) ** 2 + (xx - cx) ** 2)
    edge = (grey | blue) & (d > 0.78 * r)
    sub[edge] *= 0.55
    # catchlight up and to the left of the pupil
    cl = ((yy - (cy - 0.42 * r)) / (0.20 * r)) ** 2 + ((xx - (cx - 0.38 * r)) / (0.17 * r)) ** 2 < 1
    sub[cl & iris] = 0.97
    cl2 = ((yy - (cy + 0.35 * r)) / (0.09 * r)) ** 2 + ((xx - (cx + 0.32 * r)) / (0.09 * r)) ** 2 < 1
    sub[cl2 & iris] = sub[cl2 & iris] * 0.4 + 0.6
    a = a.copy()
    a[y0:y1, x0:x1] = sub
    # lash lines: darken what is already dark, spread one pixel down
    for bx0, by0, bx1, by1 in LASH_BOXES:
        X0, Y0, X1, Y1 = int(bx0 * W), int(by0 * H), int(bx1 * W), int(by1 * H)
        L = a[Y0:Y1, X0:X1].copy()
        lum = L.mean(-1)
        dark = np.clip((0.42 - lum) / 0.25, 0, 1)
        L *= (1 - 0.55 * dark)[..., None]
        spread = np.zeros_like(dark); spread[1:] = dark[:-1]
        L *= (1 - 0.35 * np.clip(spread - dark, 0, 1))[..., None]
        a[Y0:Y1, X0:X1] = L
    return a


EYEBALL_BOX = (406 / 1024, 10 / 1024, 480 / 1024, 70 / 1024)   # the eyeball-white piece in the atlas
SQUALL_EYE_CELL = (48, 0, 80, 16)       # open-eye strip in Squall's 128x128 face sheet ('texture-0' in p_eht100_1p.gmo)
EYE_MAP = (1.5, 4.2, 30.5, 13.6)        # where the eyeball's front outline lands in the cell (pixels)


EYE_TEXTURE = 'texture-0'


def configure(p):
    """Settings from a recipe's "body" section: girth {torso, arm, leg, head, foot}; eyes {texture, cell, map,
    eyeball_box, iris_box, atlas} (boxes in pixels of an atlas of that size)."""
    global IRIS_BOX, IRIS_RIGHT, EYEBALL_BOX, SQUALL_EYE_CELL, EYE_MAP, EYE_TEXTURE
    GIRTH.update(p.get('girth', {}))
    e = p.get('eyes') or {}
    if e:
        A = float(e.get('atlas', 1024))
        if 'iris_box' in e:
            IRIS_BOX = tuple(v / A for v in e['iris_box'])
        if 'eyeball_box' in e:
            EYEBALL_BOX = tuple(v / A for v in e['eyeball_box'])
            IRIS_RIGHT = EYEBALL_BOX[0]
        SQUALL_EYE_CELL = tuple(e.get('cell', SQUALL_EYE_CELL))
        EYE_MAP = tuple(e.get('map', EYE_MAP))
        EYE_TEXTURE = e.get('texture', EYE_TEXTURE)


def squall_eye(gmo_path, pkg):
    """The eye strip (SQUALL_EYE_CELL) from a game model's face sheet as uint8 RGBA, row 0 = top."""
    import importlib
    gmo = importlib.import_module(pkg + '.dfx.gmo'); gim = importlib.import_module(pkg + '.dfx.gim')
    ch = gmo.Character.load(gmo_path)
    t = next(t for t in ch.textures if t.name == EYE_TEXTURE)
    sheet = gim.decode(t.gim)
    x0, y0, x1, y1 = SQUALL_EYE_CELL
    return np.ascontiguousarray(sheet[y0:y1, x0:x1])


def use_eye_strip(body, strip_rgba, common):
    """Swap Noctis's eyes for a painted eye strip: the iris discs go, and each eyeball shows the strip,
    projected from the front (mirrored for the right eye) on a second material."""
    import bmesh
    me = body.data
    bm = bmesh.new(); bm.from_mesh(me)
    uvl = bm.loops.layers.uv.active
    def in_box(f, box):
        x0, y0, x1, y1 = box
        return all(x0 <= l[uvl].uv.x <= x1 and y0 <= 1 - l[uvl].uv.y <= y1 for l in f.loops)
    iris = [f for f in bm.faces if in_box(f, IRIS_BOX)]
    ball = [f for f in bm.faces if in_box(f, EYEBALL_BOX)]
    if not ball:
        bm.free(); return 'no eyeball faces found'
    bmesh.ops.delete(bm, geom=iris, context='FACES')
    ball = [f for f in bm.faces if in_box(f, EYEBALL_BOX)]
    # material slot 1 = the eye strip
    name = 'noctis_eye_tex'
    img = bpy.data.images.get(name)
    if img is not None:
        bpy.data.images.remove(img)
    h, w = strip_rgba.shape[:2]
    img = common.image_from_rgba(name, strip_rgba)
    old = bpy.data.materials.get('noctis_eye_mat')
    if old is not None:
        bpy.data.materials.remove(old)
    mat = common.make_material('noctis_eye_mat', img)
    while len(me.materials) > 1:
        me.materials.pop()
    me.materials.append(mat)
    # front projection per side, in armature space (x = character's left, y = up)
    ex0, ey0, ex1, ey1 = EYE_MAP
    for side in (1, -1):
        fs = [f for f in ball if f.calc_center_median().x * side > 0]
        if not fs:
            continue
        vs = {v for f in fs for v in f.verts}
        xs = [v.co.x for v in vs]; ys = [v.co.y for v in vs]
        xmin, xmax, ymin, ymax = min(xs), max(xs), min(ys), max(ys)
        for f in fs:
            f.material_index = 1
            for l in f.loops:
                tx = (l.vert.co.x - xmin) / (xmax - xmin)
                if side < 0:
                    tx = 1 - tx                                # mirror: outer corner stays outward
                ty = (ymax - l.vert.co.y) / (ymax - ymin)      # 0 = top
                px = ex0 + tx * (ex1 - ex0); py = ey0 + ty * (ey1 - ey0)
                l[uvl].uv = (px / w, 1 - py / h)
    bm.to_mesh(me); bm.free()
    return f'eyes: {len(iris)} iris faces removed, {len(ball)} eyeball faces show the strip'


def _load_rgb(path, size, eyes=True):
    """Source texture as float RGB (size, size, 3), row 0 = bottom (Blender order)."""
    try:
        from PIL import Image
        im = Image.open(path).convert('RGB')
        if eyes:
            a = tweak_eyes(np.asarray(im, np.float32) / 255.0)
            im = Image.fromarray((np.clip(a, 0, 1) * 255 + 0.5).astype(np.uint8))
        im = im.resize((size, size), Image.LANCZOS)
        return np.asarray(im, np.float32)[::-1] / 255.0
    except ImportError:
        img = bpy.data.images.load(path, check_existing=False)
        img.scale(size, size)
        a = np.empty(size * size * 4, np.float32); img.pixels.foreach_get(a)
        bpy.data.images.remove(img)
        return a.reshape(size, size, 4)[..., :3].copy()


def _rgb_to_hsv(c):
    mx, mn = c.max(-1), c.min(-1)
    d = mx - mn
    h = np.zeros_like(mx)
    r, g, b = c[..., 0], c[..., 1], c[..., 2]
    m = d > 1e-6
    rr = m & (mx == r); gg = m & (mx == g) & ~rr; bb = m & ~rr & ~gg
    h[rr] = ((g - b)[rr] / d[rr]) % 6
    h[gg] = (b - r)[gg] / d[gg] + 2
    h[bb] = (r - g)[bb] / d[bb] + 4
    return h / 6, np.where(mx > 1e-6, d / np.maximum(mx, 1e-6), 0), mx


def _hsv_to_rgb(h, s, v):
    i = np.floor(h * 6).astype(int) % 6
    f = h * 6 - np.floor(h * 6)
    p, q, t = v * (1 - s), v * (1 - s * f), v * (1 - s * (1 - f))
    tbl = [(v, t, p), (q, v, p), (p, v, t), (p, q, v), (t, p, v), (v, p, q)]
    out = np.zeros(h.shape + (3,), np.float32)
    for k, (a, b, c) in enumerate(tbl):
        m = i == k
        out[m] = np.stack([a[m], b[m], c[m]], -1)
    return out


def grade(rgb):
    """Dissidia look: neutral (less blue) blacks, warmer skin, a bit more contrast and colour."""
    h, s, v = _rgb_to_hsv(np.clip(rgb, 0, 1))
    dark = np.clip((0.45 - v) / 0.35, 0, 1)                       # 1 for deep shadows/black cloth
    s = s * (1 - 0.35 * dark)                                     # take the blue cast off the blacks
    skin = (((h > 0.015) & (h < 0.13)) & (s > 0.10) & (s < 0.65) & (v > 0.45)).astype(np.float32)
    h = h + skin * (0.065 - h) * 0.35                             # skin hue toward warm peach
    s = np.clip(s * (1 + 0.30 * skin) * 1.08, 0, 1)
    v = np.clip((v - 0.015) / 0.985, 0, 1)
    v = np.clip(0.5 + (v - 0.5) * 1.12, 0, 1)
    return _hsv_to_rgb(h % 1, s, v)


def bake_lighting(body, size=512, samples=48):
    """Soft painted-in lighting like the game's textures: sky occlusion plus a key light from the front and above.
    Returns float (size, size) light factor ~0..1+, 1 = fully lit. Mirrored UVs get the same (symmetric) light."""
    sc = bpy.context.scene
    saved = dict(engine=sc.render.engine, world=sc.world)
    hidden = {}
    for o in sc.objects:
        hidden[o.name] = o.hide_render
        o.hide_render = o is not body
    world = bpy.data.worlds.new('dfx_bake_world')
    world.use_nodes = True
    bg = next(n for n in world.node_tree.nodes if n.type == 'BACKGROUND')
    bg.inputs['Color'].default_value = (1, 1, 1, 1); bg.inputs['Strength'].default_value = 1.0
    sc.world = world
    sun = bpy.data.objects.new('dfx_bake_sun', bpy.data.lights.new('dfx_bake_sun', 'SUN'))
    sun.data.energy = 2.2; sun.data.angle = math.radians(25)
    sun.rotation_euler = (math.radians(50), 0, 0)               # from the front (-Y) and above
    sc.collection.objects.link(sun)
    sc.render.engine = 'CYCLES'
    sc.cycles.samples = samples
    sc.cycles.device = 'CPU'
    target = bpy.data.images.new('dfx_bake_light', size, size, float_buffer=True, alpha=False)
    mat = body.data.materials[0]
    node = mat.node_tree.nodes.new('ShaderNodeTexImage'); node.image = target
    for n in mat.node_tree.nodes:
        n.select = False
    node.select = True; mat.node_tree.nodes.active = node
    try:
        with bpy.context.temp_override(object=body, active_object=body, selected_objects=[body], selected_editable_objects=[body]):
            bpy.ops.object.bake(type='DIFFUSE', pass_filter={'DIRECT', 'INDIRECT'}, margin=6, use_clear=True)
        px = np.empty(size * size * 4, np.float32); target.pixels.foreach_get(px)
        L = px.reshape(size, size, 4)[..., 0].copy()
    finally:
        mat.node_tree.nodes.remove(node)
        bpy.data.images.remove(target)
        bpy.data.objects.remove(sun); bpy.data.worlds.remove(world)
        sc.render.engine = saved['engine']; sc.world = saved['world']
        for o in sc.objects:
            if o.name in hidden:
                o.hide_render = hidden[o.name]
    covered = L > 1e-4
    ref = np.percentile(L[covered], 97) if covered.any() else 1.0
    L = np.where(covered, L / ref, 1.0)
    return L


def stylize(body, tex_path, size=512, bake=True, eyes=True):
    """Build the body texture: graded source texture times baked soft lighting. Returns the Blender image."""
    rgb = grade(_load_rgb(tex_path, size, eyes))
    if bake:
        L = bake_lighting(body, size)
        shade = np.clip(0.42 + 0.62 * L, 0.35, 1.12)             # shadows keep 40% of the colour; tops get a lift
        rgb = np.clip(rgb * shade[..., None], 0, 1)
    name = 'noctis_body_tex'
    img = bpy.data.images.get(name)
    if img is not None:
        bpy.data.images.remove(img)
    img = bpy.data.images.new(name, size, size, alpha=True)
    rgba = np.concatenate([rgb, np.ones(rgb.shape[:2] + (1,), np.float32)], -1)
    img.pixels.foreach_set(rgba.reshape(-1).astype(np.float32))
    img.pack()
    return img


def smooth_joint(body, arm, parent, child, above=0.05, below=0.04, radius=0.11):
    """Blend the weights across a joint (e.g. tibia -> foot) with a smooth ramp along the limb, so the low-poly
    mesh bends round the joint instead of pinching. Only vertices weighted to just these two bones, near the joint."""
    B = arm.data.bones; inv = body.matrix_world.inverted()
    jp = inv @ (arm.matrix_world @ B[child].head_local)
    a = jp - inv @ (arm.matrix_world @ B[parent].head_local); a.normalize()
    gp, gc = body.vertex_groups[parent], body.vertex_groups[child]
    n = 0
    for v in body.data.vertices:
        gs = {body.vertex_groups[g.group].name for g in v.groups if g.weight > 0}
        if not gs or gs - {parent, child} or (v.co - jp).length > radius:
            continue
        x = min(1.0, max(0.0, ((v.co - jp).dot(a) + above) / (above + below)))
        wc = x * x * (3 - 2 * x)
        gp.add([v.index], 1.0 - wc, 'REPLACE'); gc.add([v.index], wc, 'REPLACE')
        if wc <= 1e-4: gc.remove([v.index])
        if wc >= 1 - 1e-4: gp.remove([v.index])
        n += 1
    return n


def twist_share(body, arm, s, share=0.5):
    """Forearm skin takes part of the hand's twist, rising towards the wrist, so twisting the hand turns the forearm
    gradually (the game skeleton has no twist bones)."""
    B = arm.data.bones; inv = body.matrix_world.inverted()
    el = inv @ (arm.matrix_world @ B[f'{s}_foarm'].head_local); wr = inv @ (arm.matrix_world @ B[f'{s}_hand'].head_local)
    a = wr - el; L = a.length; a.normalize()
    gf, gh = body.vertex_groups[f'{s}_foarm'], body.vertex_groups[f'{s}_hand']; n = 0
    for v in body.data.vertices:
        ws = {body.vertex_groups[g.group].name: g.weight for g in v.groups if g.weight > 0}
        if set(ws) - {f'{s}_foarm', f'{s}_hand'} or f'{s}_foarm' not in ws:
            continue
        t = (v.co - el).dot(a) / L
        if t < 0.15 or t > 1.0 or (v.co - el - a * (t * L)).length > 0.09:
            continue
        u = (t - 0.15) / 0.85; wh = max(ws.get(f'{s}_hand', 0.0), share * u * u * (3 - 2 * u))
        gf.add([v.index], 1 - wh, 'REPLACE'); gh.add([v.index], wh, 'REPLACE'); n += 1
    return n


def run(fbx_path, tex_path, arm_name=None, max_tris=2500, tex_size=512, bake=True, eyes_from=None, source_height=None):
    """eyes_from: a game GMO whose face sheet supplies the eyes (Squall's p_eht100_1p.gmo); None keeps Noctis's own.
    source_height: the source model's real height in metres. FBX importers differ in unit handling (Blender 5 brings
    the War of the Visions rips in 100x smaller than 4.x), and the girth factors are tuned for the source at its real
    size, so the imported model is first scaled to this height."""
    import sys, importlib
    pkg = __package__ or next((k for k in sys.modules if k.split('.')[-1] == 'io_scene_dissidia'), 'io_scene_dissidia')
    basebody, common, tools_bl = (importlib.import_module(f'{pkg}.{m}') for m in ('basebody', 'common', 'tools_bl'))
    arm = bpy.data.objects[arm_name] if arm_name else next(o for o in bpy.context.scene.objects if o.type == 'ARMATURE' and o.get('dfx_template') is not None)
    if bpy.context.object and bpy.context.object.mode != 'OBJECT':
        bpy.ops.object.mode_set(mode='OBJECT')
    # rest pose for the duration
    pos_prev = arm.data.pose_position
    arm.data.pose_position = 'REST'
    before = set(bpy.data.objects)
    bpy.ops.import_scene.fbx(filepath=fbx_path)
    new = [o for o in bpy.data.objects if o not in before]
    src_arm = next(o for o in new if o.type == 'ARMATURE')
    src = next(o for o in new if o.type == 'MESH')
    src.shape_key_clear()
    if source_height:
        zs = [(src.matrix_world @ v.co).z for v in src.data.vertices]
        h = max(zs) - min(zs)
        if h > 1e-9 and abs(h / source_height - 1) > 0.02:
            src_arm.matrix_world = Matrix.Scale(source_height / h, 4) @ src_arm.matrix_world
            bpy.context.view_layer.update()
    maps = build_maps(src_arm, arm)
    res = fit_core(src_arm, src, arm, maps, WEIGHT_MAP, tex_path, max_tris=max_tris, tex_size=tex_size, bake=bake,
                   eyes_from=eyes_from, tweak_eye=True)
    arm.data.pose_position = pos_prev
    return res


def fit_core(src_arm, src, arm, maps, weight_map, tex_path, max_tris=2500, tex_size=512, bake=True, eyes_from=None,
             tweak_eye=True):
    """Carry mesh `src` (weighted to `src_arm`'s bones) onto the game armature `arm` with the per-bone `maps`
    ({source bone: 4x4}), weights renamed by `weight_map` ({source bone: game bone}); make it the body (texture from
    tex_path, graded and lit), remove the source objects. The game armature must be in its rest pose."""
    import sys, importlib
    pkg = __package__ or next((k for k in sys.modules if k.split('.')[-1] == 'io_scene_dissidia'), 'io_scene_dissidia')
    basebody, common, tools_bl = (importlib.import_module(f'{pkg}.{m}') for m in ('basebody', 'common', 'tools_bl'))
    me = src.data
    n = len(me.vertices)
    W = src.matrix_world
    groups = {g.index: g.name for g in src.vertex_groups}
    inv = arm.matrix_world.inverted()
    co_new = []
    gw = []                                   # per vertex {game bone: weight}
    for v in me.vertices:
        p = W @ v.co
        acc, tot, d = Vector((0, 0, 0)), 0.0, {}
        for g in v.groups:
            nm = groups[g.group]
            if g.weight <= 0 or nm not in maps:
                continue
            acc += g.weight * (maps[nm] @ p); tot += g.weight
            gb = weight_map.get(nm)
            if gb:
                d[gb] = d.get(gb, 0.0) + g.weight
        q = acc / tot if tot > 0 else maps['Hips'] @ p
        co_new.append(inv @ q)
        gw.append(d or {'hip': 1.0})

    # new body object in armature space
    body = bpy.data.objects.new('noctis_body', me)
    me.name = 'noctis_body'
    for c in arm.users_collection:
        c.objects.link(body)
    body.parent = arm
    body.matrix_parent_inverse.identity()
    body.matrix_basis.identity()
    for v, c in zip(me.vertices, co_new):
        v.co = c
    try:
        me.normals_split_custom_set(None)
    except Exception:
        pass
    for poly in me.polygons:
        poly.use_smooth = True
    body.vertex_groups.clear()
    vgs = {}
    for i, d in enumerate(gw):
        s = sum(d.values())
        for b, w in d.items():
            if b not in vgs:
                vgs[b] = body.vertex_groups.new(name=b)
            vgs[b].add([i], w / s, 'REPLACE')
    body.modifiers.new('Armature', 'ARMATURE').object = arm
    bpy.data.objects.remove(src)
    bpy.data.objects.remove(src_arm)

    # fewer triangles for the PSP
    me.calc_loop_triangles()
    tris = len(me.loop_triangles)
    if max_tris and tris > max_tris:
        dec = body.modifiers.new('Decimate', 'DECIMATE')
        dec.ratio = max_tris / tris
        body.modifiers.move(len(body.modifiers) - 1, 0)
        with bpy.context.temp_override(object=body, active_object=body, selected_objects=[body]):
            bpy.ops.object.modifier_apply(modifier=dec.name)
    me.calc_loop_triangles()

    # texture + a material the exporter understands
    me.materials.clear()
    old_mat = bpy.data.materials.get('noctis_body_mat')
    if old_mat:
        bpy.data.materials.remove(old_mat)
    me.materials.append(common.make_material('noctis_body_mat', None))
    for a in list(me.color_attributes):
        me.color_attributes.remove(a)
    col = me.color_attributes.new('Col', 'FLOAT_COLOR', 'POINT')
    col.data.foreach_set('color', np.ones(len(col.data) * 4, np.float32))
    if me.uv_layers:
        me.uv_layers[0].name = 'UVMap'
    img = stylize(body, tex_path, tex_size, bake=bake, eyes=tweak_eye)
    mat = me.materials[0]
    tex = next((n for n in mat.node_tree.nodes if n.type == 'TEX_IMAGE'), None)
    if tex is None:
        tex = mat.node_tree.nodes.new('ShaderNodeTexImage')
        mix = next(n for n in mat.node_tree.nodes if n.type == 'MIX')
        mat.node_tree.links.new(tex.outputs['Color'], mix.inputs['A'])
    tex.image = img
    tex.interpolation = 'Linear'
    eye_msg = None
    if eyes_from:
        eye_msg = use_eye_strip(body, squall_eye(eyes_from, pkg), common)

    have = lambda *g: all(n in body.vertex_groups for n in g)
    for s_ in ('l', 'r'):                    # ankles bend instead of pinching
        if have(f'{s_}_tibia', f'{s_}_foot'):
            smooth_joint(body, arm, f'{s_}_tibia', f'{s_}_foot')
        if have(f'{s_}_uparm', f'{s_}_foarm'):
            smooth_joint(body, arm, f'{s_}_uparm', f'{s_}_foarm', 0.04, 0.04, 0.09)   # elbows
        if have(f'{s_}_foarm', f'{s_}_hand'):
            twist_share(body, arm, s_)                                              # forearm twist from the hand
    body['dfx_base_body'] = True            # lets a later run swap it out cleanly
    old_body = basebody.body_object(arm)
    msg = basebody.replace_body(arm, body)

    # warp ghost copies follow the new body
    ghost_set = {b.name[2:] for b in arm.data.bones if b.name.startswith('g_')}
    ghosts = 0
    if any(o.name.startswith('warp_ghost') for o in bpy.data.objects) and ghost_set:
        missing = set(vgs) - ghost_set
        tools_bl._ghost_objects(arm, body, ghost_set)
        ghosts = len([o for o in bpy.data.objects if o.name.startswith('warp_ghost')])
    return dict(tris_in=tris, tris=len(me.loop_triangles), verts=len(me.vertices), groups=sorted(vgs),
                replaced=msg, ghosts=ghosts, body=body.name, eyes=eye_msg)
