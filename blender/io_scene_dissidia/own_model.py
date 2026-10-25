"""Give a character mod your own model as its body: import a humanoid (FBX, glTF/GLB/VRM, OBJ, .blend), fit it onto the
character's skeleton and save it as a skin in mods/<character>/skins/<name>/.

Rigged models (any common naming: Mixamo, Unity humanoid, Unreal, VRM, Rigify/Blender, Daz/CC...) are matched bone
by bone: each limb segment of the model is carried onto the game's segment (fit_body.fit_core), so the joints land
where the animations bend. A model without a skeleton is scaled to the character's height and weighted to the game
bones automatically (Blender's automatic weights): it has to stand in a pose close to the character's.
The texture is the model's own when it has one picture; otherwise its materials are baked into one atlas (the game
body has one material).

    from io_scene_dissidia import own_model
    rep = own_model.fit(context, arm, path, girth=1.0, max_tris=2500, tex_size=512, bake=True)
    rep = own_model.save_skin(arm, name)          # one .skin per model file of the character
"""
import os
import re
import tempfile

import bpy
import numpy as np
from mathutils import Matrix, Vector

from . import fit_body as FB

SIDES = ('Left', 'Right')
EXTS = ('.fbx', '.glb', '.gltf', '.vrm', '.obj', '.blend')


# ---------------------------------------------------------------- importing
def import_model(path):
    """-> (armature or None, [meshes]) of the file, imported into the scene"""
    ext = os.path.splitext(path)[1].lower()
    before = set(bpy.data.objects)
    if ext == '.fbx':
        bpy.ops.import_scene.fbx(filepath=path)
    elif ext in ('.glb', '.gltf', '.vrm'):
        p = path
        if ext == '.vrm':                                     # a VRM is a GLB with extra data
            p = os.path.join(tempfile.mkdtemp(prefix='dfx_vrm_'), os.path.basename(path)[:-4] + '.glb')
            with open(path, 'rb') as a, open(p, 'wb') as b:
                b.write(a.read())
        bpy.ops.import_scene.gltf(filepath=p)
    elif ext == '.obj':
        bpy.ops.wm.obj_import(filepath=path)
    elif ext == '.blend':                                     # a Blender file: append its objects (not cameras / lights)
        with bpy.data.libraries.load(path, link=False) as (src, dst):
            dst.objects = list(src.objects)
        col = bpy.data.collections.new(os.path.splitext(os.path.basename(path))[0])
        bpy.context.scene.collection.children.link(col)
        for o in dst.objects:
            if o is None:
                continue
            if o.type in ('CAMERA', 'LIGHT', 'LIGHT_PROBE', 'SPEAKER'):
                bpy.data.objects.remove(o)
                continue
            col.objects.link(o)
    else:
        raise RuntimeError('pick an FBX, glTF/GLB, VRM, OBJ or .blend file')
    new = [o for o in bpy.data.objects if o not in before]
    arms = [o for o in new if o.type == 'ARMATURE']
    meshes = [o for o in new if o.type == 'MESH' and len(o.data.polygons)]
    if not meshes:
        raise RuntimeError('the file has no mesh')
    src_arm = max(arms, key=lambda a: len(a.data.bones)) if arms else None
    if src_arm is not None:                                    # the skinned meshes only, not props lying about
        rigged = [o for o in meshes if o.find_armature() == src_arm or (o.parent == src_arm and o.parent_type == 'BONE')]
        meshes = rigged or meshes
    for o in new:                                              # keep what isn't part of the model out of the way
        if o not in meshes and o is not src_arm:
            o.hide_set(True)
    return src_arm, meshes, new


# ---------------------------------------------------------------- the model's skeleton
_PREFIX = re.compile(r'^(mixamorig\d*[:_]|j_bip_[clr]_|bip0?1[ _]?|def[-_]|orh[-_]|cc_base_|rig[:_]|armature[:_]|valvebiped\.bip01_)', re.I)


def _norm(name):
    n = _PREFIX.sub('', name.split(':')[-1] if ':' in name and not name.lower().startswith('mixamorig') else name)
    n = _PREFIX.sub('', n)
    return re.sub(r'[\s_.\-]+', '', n).lower()


def _side(name, x):
    n = name.lower()
    if re.search(r'(^|[^a-z])(left|l)([^a-z]|$)', re.sub(r'([a-z])([A-Z])', r'\1 \2', name).lower()) or 'left' in n or re.search(r'[._\- ]l$|^l[._\- ]|_l_', n):
        return 'Left'
    if re.search(r'(^|[^a-z])(right|r)([^a-z]|$)', re.sub(r'([a-z])([A-Z])', r'\1 \2', name).lower()) or 'right' in n or re.search(r'[._\- ]r$|^r[._\- ]|_r_', n):
        return 'Right'
    return None if abs(x) < 1e-6 else ('Left' if x > 0 else 'Right')


def _strip_side(n):
    return re.sub(r'^(left|right|l|r)|(left|right|l|r)$', '', n)


ROLE_WORDS = [                                                   # (role, words); first match wins, order matters
    ('Hand', ('hand', 'wrist')),
    ('ForeArm', ('forearm', 'lowerarm', 'elbow', 'arm2', 'armlower')),
    ('Arm', ('upperarm', 'arm', 'arm1', 'armupper', 'humerus')),
    ('Shoulder', ('shoulder', 'clavicle', 'collar', 'clav')),
    ('Foot', ('foot', 'ankle', 'leg3')),
    ('Leg', ('lowerleg', 'leg', 'calf', 'shin', 'knee', 'tibia', 'crus', 'leg2', 'leglower')),
    ('UpLeg', ('upleg', 'upperleg', 'thigh', 'femur', 'leg1', 'legupper')),
    ('Head', ('head',)),
    ('Neck', ('neck',)),
]
_SKIP = ('twist', 'roll', 'share', 'helper', 'end', 'nub', 'tip', 'finger', 'thumb', 'index', 'middle', 'ring', 'pinky',
         'little', 'toe', 'eye', 'jaw', 'hair', 'ik', 'pole', 'target', 'weapon', 'prop', 'skirt', 'bust', 'breast',
         'cloth', 'cape')


_HELPER = re.compile(r'(^|_)(add|sub|aux|corr|helper)[A-Z_]')   # corrective helpers beside the real limb (FF16: j_addKnee_l)


def _role(name):
    if _HELPER.search(name.split(':')[-1]):
        return None
    n = re.sub(r'0+(\d)$', r'\1', _strip_side(_norm(name)))    # numbered chains: leg_01/02/03 -> leg1/2/3, arm_01/02
    if re.search(r'neck\d$', n):
        return 'Neck'
    if any(w in n for w in _SKIP) and not n.endswith(('hand', 'foot', 'head')):
        return None
    for role, words in ROLE_WORDS:
        for w in words:
            if n == w or n.endswith(w) and not (role == 'Arm' and n.endswith('forearm')) and not (role == 'Leg' and n.endswith(('upleg', 'upperleg'))):
                if role == 'Arm' and ('fore' in n or 'lower' in n):
                    continue
                if role == 'Leg' and ('up' in n or 'thigh' in n):
                    continue
                return role
    return None


def bone_map(src_arm):
    """{canonical name (fit_body's: Hips, Neck, Head, LeftShoulder, LeftArm, LeftForeArm, LeftHand, LeftUpLeg, LeftLeg,
    LeftFoot, ...): the model's bone} found by name and checked by the hierarchy; raises with what is missing."""
    B = src_arm.data.bones
    W = src_arm.matrix_world
    x = lambda b: (W @ b.head_local).x
    found = {}
    for b in B:
        r = _role(b.name)
        if not r:
            continue
        if r in ('Head', 'Neck'):
            key = r
        else:
            s = _side(b.name, x(b))
            if not s:
                continue
            key = s + r
        # the first in the hierarchy wins (Spine2's 'Neck' vs 'Neck1'); for hands/feet the one nearest the root
        if key not in found or len(b.parent_recursive) < len(found[key].parent_recursive):
            found[key] = b
    # walk up from hands and feet when names are unusual (bone_01 style chains)
    for s in SIDES:
        h, f = found.get(s + 'Hand'), found.get(s + 'Foot')
        if h is not None:
            chain = [p for p in h.parent_recursive if not any(w in _norm(p.name) for w in ('twist', 'roll', 'share'))]
            if s + 'ForeArm' not in found and len(chain) > 0:
                found[s + 'ForeArm'] = chain[0]
            if s + 'Arm' not in found and len(chain) > 1:
                found[s + 'Arm'] = chain[1]
        if f is not None:
            chain = [p for p in f.parent_recursive if not any(w in _norm(p.name) for w in ('twist', 'roll', 'share'))]
            if s + 'Leg' not in found and len(chain) > 0:
                found[s + 'Leg'] = chain[0]
            if s + 'UpLeg' not in found and len(chain) > 1:
                found[s + 'UpLeg'] = chain[1]
    if 'Neck' not in found and 'Head' in found and found['Head'].parent:
        found['Neck'] = found['Head'].parent
    if 'Head' not in found and 'Neck' in found and found['Neck'].children:
        found['Head'] = max(found['Neck'].children, key=lambda c: len(c.children_recursive))
    # hips: where the two legs meet
    if 'LeftUpLeg' in found and 'RightUpLeg' in found:
        la = [found['LeftUpLeg']] + list(found['LeftUpLeg'].parent_recursive)
        ra = set([found['RightUpLeg']] + list(found['RightUpLeg'].parent_recursive))
        common = next((p for p in la if p in ra), None)
        if common is not None:
            found['Hips'] = common
    # the spine between them: its lowest bone bends with the game's waist, its highest with the chest
    if 'Hips' in found and 'Neck' in found and found['Hips'] in found['Neck'].parent_recursive:
        chain = list(found['Neck'].parent_recursive)
        chain = chain[:chain.index(found['Hips'])][::-1]          # hips -> neck, exclusive
        if chain:
            found['Spine'] = chain[0]
        if len(chain) > 1:
            found['Chest'] = chain[-1]
    # shoulders: the arm's parent when it isn't the spine
    for s in SIDES:
        a = found.get(s + 'Arm')
        if a is not None and s + 'Shoulder' not in found and a.parent is not None:
            spine = found.get('Neck')
            col = set(spine.parent_recursive) if spine else set()
            if a.parent not in col and a.parent is not spine:
                found[s + 'Shoulder'] = a.parent
    need = ['Hips', 'Neck', 'Head'] + [s + r for s in SIDES for r in ('Arm', 'ForeArm', 'Hand', 'UpLeg', 'Leg', 'Foot')]
    missing = [n for n in need if n not in found]
    if missing:
        raise LookupError('bones not found: ' + ', '.join(missing))
    # sanity: left and right not swapped (left limbs on the model's left = the side its left hand is on)
    return {k: b.name for k, b in found.items()}


# ---------------------------------------------------------------- one mesh, canonical weights
# parts a game-sized body can't use: layers over the eye, the inside of the mouth, stubble and lashes (ripped models
# keep them as separate meshes; at a few thousand triangles they only show as shards)
DROP_PARTS = ('eyeshadow', 'eyewet', 'wet', 'tear', 'innermouth', 'mouthin', 'teeth', 'tooth', 'tongue', 'lash',
              'facehair', 'stubble', 'buzz')


def _part_words(o):
    names = [o.name] + [m.name for m in o.data.materials if m]
    return [re.sub(r'[\s_.\-]+', '', n).lower() for n in names]


def _prune(meshes):
    """-> (meshes kept, names dropped): the same part twice (rips often hold a mesh once per render pass) is kept
    once; overlay and inside parts (DROP_PARTS) go when anything else is left."""
    keep, seen, dropped = [], set(), []
    for o in meshes:
        me = o.data
        co = np.empty(len(me.vertices) * 3, np.float32)
        me.vertices.foreach_get('co', co)
        key = (len(me.vertices), len(me.polygons), round(float(co.sum()), 2), round(float(np.abs(co).sum()), 2))
        if key in seen:
            dropped.append(o.name)
            continue
        seen.add(key)
        keep.append(o)
    rest = [o for o in keep if not any(w in n for n in _part_words(o) for w in DROP_PARTS)]
    if rest and len(rest) < len(keep):
        dropped += [o.name for o in keep if o not in rest]
        keep = rest
    return keep, dropped


def _card_hair(meshes, model_dir):
    """the hair parts drawn as cards (strips the game showed through an alpha map, with no colour picture of their
    own): at a few thousand triangles they only show as shards"""
    out = []
    for o in meshes:
        if not any('hair' in n and 'facehair' not in n for n in _part_words(o)):
            continue
        mats = [m for m in o.data.materials if m]
        if all(_base_image(m) is None or _resolve(_base_image(m), model_dir, m.name) is False for m in mats):
            out.append(o)
    return out


def _clear_face(h, face):
    """take the strands hanging in front of the face (`face`: the face part) off `h`: fused, they would be a mask"""
    import bmesh
    fco = np.array([(face.matrix_world @ v.co)[:] for v in face.data.vertices], np.float32)
    hco = np.array([v.co[:] for v in h.data.vertices], np.float32)
    c, hc = fco.mean(0), hco.mean(0)
    d = c - hc; d[2] = 0
    if np.linalg.norm(d) < 1e-9:
        return
    d /= np.linalg.norm(d)
    lat = np.array([-d[1], d[0], 0.0], np.float32)
    half = float(np.abs((fco - c) @ lat).max()) * 0.8
    z0, z1 = float(fco[:, 2].min()), float(fco[:, 2].min() + (fco[:, 2].max() - fco[:, 2].min()) * 0.85)
    bm = bmesh.new(); bm.from_mesh(h.data)
    gone = []
    for f in bm.faces:
        p = np.array(f.calc_center_median()[:], np.float32)
        if (p - c) @ d > 0 and abs((p - c) @ lat) < half and z0 < p[2] < z1:
            gone.append(f)
    bmesh.ops.delete(bm, geom=gone, context='FACES')
    bm.to_mesh(h.data); bm.free()


def _solid_hair(parts, src_arm, colour, face=None):
    """card hair -> one solid mass (the cards thickened and fused into a volume, then smoothed), weighted like the
    cards it came from, in `colour`; the strands over the face are left out. -> the new mesh object"""
    h = _join(parts, src_arm)
    if face is not None:
        _clear_face(h, face)
    co = np.empty(len(h.data.vertices) * 3, np.float32)
    h.data.vertices.foreach_get('co', co)
    co = co.reshape(-1, 3)
    size = float((co.max(0) - co.min(0)).max()) or 1.0
    src = h.copy(); src.data = h.data.copy()
    bpy.context.scene.collection.objects.link(src)
    s = h.modifiers.new('dfx_sol', 'SOLIDIFY'); s.thickness = size * 0.03; s.offset = 0
    r = h.modifiers.new('dfx_rem', 'REMESH'); r.mode = 'VOXEL'; r.voxel_size = size * 0.02; r.adaptivity = 0
    m = h.modifiers.new('dfx_sm', 'SMOOTH'); m.iterations = 4; m.factor = 0.6
    with bpy.context.temp_override(object=h, active_object=h, selected_objects=[h], selected_editable_objects=[h]):
        for mod in list(h.modifiers):
            bpy.ops.object.modifier_apply(modifier=mod.name)
    h.vertex_groups.clear()
    for g in src.vertex_groups:
        h.vertex_groups.new(name=g.name)
    if src.vertex_groups:                                    # weights from the nearest card
        dt = h.modifiers.new('dfx_dt', 'DATA_TRANSFER')
        dt.object = src; dt.use_vert_data = True; dt.data_types_verts = {'VGROUP_WEIGHTS'}; dt.vert_mapping = 'NEAREST'
        dt.layers_vgroup_select_src = 'ALL'; dt.layers_vgroup_select_dst = 'NAME'
        with bpy.context.temp_override(object=h, active_object=h, selected_objects=[h], selected_editable_objects=[h]):
            bpy.ops.object.modifier_apply(modifier=dt.name)
    bpy.data.objects.remove(src)
    for mat in h.data.materials:                              # no picture: this colour
        if mat is None:
            continue
        mat.diffuse_color = (*colour, 1.0)
        bsdf = next((n for n in mat.node_tree.nodes if n.type == 'BSDF_PRINCIPLED'), None) if mat.use_nodes else None
        if bsdf is not None:
            for l in list(bsdf.inputs['Base Color'].links):
                mat.node_tree.links.remove(l)
            bsdf.inputs['Base Color'].default_value = (*colour, 1.0)
    for p in h.data.polygons:
        p.material_index = 0
    return h


def _join(meshes, src_arm):
    """One mesh object holding every part, transforms applied, bone-parented parts weighted to their bone."""
    for o in meshes:
        if o.parent_type == 'BONE' and o.parent_bone and src_arm is not None:
            g = o.vertex_groups.get(o.parent_bone) or o.vertex_groups.new(name=o.parent_bone)
            g.add(list(range(len(o.data.vertices))), 1.0, 'REPLACE')
        if o.data.shape_keys:
            o.shape_key_clear()
        mw = o.matrix_world.copy()
        o.parent = None
        o.matrix_world = mw
        if o.data.users > 1:
            o.data = o.data.copy()
        o.data.transform(o.matrix_world)
        o.matrix_world = Matrix.Identity(4)
        for m in [m for m in o.modifiers if m.type == 'ARMATURE']:
            o.modifiers.remove(m)
    if len(meshes) > 1:
        with bpy.context.temp_override(active_object=meshes[0], selected_editable_objects=meshes, selected_objects=meshes):
            bpy.ops.object.join()
    return meshes[0]


def canonical_weights(mesh, src_arm, bm):
    """Rename/merge the mesh's vertex groups onto the canonical bones: every bone not in the map gives its weight to
    its nearest mapped ancestor (fingers to the hand, twist bones to their limb, hair to the head...), anything above
    the hips to the hips."""
    inv = {v: k for k, v in bm.items()}
    B = src_arm.data.bones
    def target(name):
        b = B.get(name)
        while b is not None:
            if b.name in inv:
                return inv[b.name]
            b = b.parent
        return 'Hips'
    old = {g.index: g.name for g in mesh.vertex_groups}
    tgt = {i: target(n) for i, n in old.items()}
    acc = [dict() for _ in mesh.data.vertices]
    for v in mesh.data.vertices:
        for g in v.groups:
            if g.weight > 0 and g.group in tgt:
                t = tgt[g.group]
                acc[v.index][t] = acc[v.index].get(t, 0.0) + g.weight
    mesh.vertex_groups.clear()
    vgs = {}
    for i, d in enumerate(acc):
        for t, w in d.items():
            if t not in vgs:
                vgs[t] = mesh.vertex_groups.new(name=t)
            vgs[t].add([i], w, 'REPLACE')
    # the armature's bones take the canonical names too (fit_body reads them by name); the others get out of the way
    names = {v: k for k, v in bm.items()}
    for i, b in enumerate(src_arm.data.bones):
        b.name = '__dfx_%d_%s' % (i, names.get(b.name, ''))
    for b in src_arm.data.bones:
        c = b.name.split('_', 4)[-1]
        if c:
            b.name = c
    return sum(1 for d in acc if d)


# ---------------------------------------------------------------- texture
def _base_image(mat):
    if not mat or not mat.use_nodes:
        return None
    bsdf = next((n for n in mat.node_tree.nodes if n.type == 'BSDF_PRINCIPLED'), None)
    if bsdf is not None and bsdf.inputs['Base Color'].is_linked:
        n = bsdf.inputs['Base Color'].links[0].from_node
        for _ in range(4):
            if n.type == 'TEX_IMAGE':
                return n.image
            ins = [i for i in n.inputs if i.is_linked]
            if not ins:
                break
            n = ins[0].links[0].from_node
    imgs = [n.image for n in mat.node_tree.nodes if n.type == 'TEX_IMAGE' and n.image]
    return imgs[0] if len(imgs) == 1 else None


IMG_EXT = ('.png', '.jpg', '.jpeg', '.tga', '.bmp', '.dds', '.tif', '.tiff', '.webp', '.exr')
NOT_COLOUR = ('norm', 'nrm', 'normal', 'roug', 'rough', 'occl', 'ao', 'meta', 'metal', 'mask', 'spec', 'height', 'disp', 'emis', 'opac', 'alph', 'shift', 'wet', 'refl', 'thic', 'tear', 'decal')   # 'alph' covers alpha too; thic: thickness


def _images_near(model_dir):
    """picture files in the model's folder (3 levels down) and its parent's (1 level down)"""
    out, seen = [], set()
    for root, depth in ((model_dir, 3), (os.path.dirname(model_dir), 1)):
        if not root or not os.path.isdir(root):
            continue
        for dp, ds, fs in os.walk(root):
            if dp.count(os.sep) - root.count(os.sep) >= depth:
                ds[:] = []
            for f in fs:
                q = os.path.join(dp, f)
                if f.lower().endswith(IMG_EXT) and q not in seen:
                    seen.add(q); out.append(q)
    return out


def _tokens(s):
    return [t for t in re.split(r'[^a-z0-9]+', s.lower()) if t]


def _resolve(img, model_dir, mat_name=''):
    """Make sure `img` has its pixels: when the file it names isn't where the model says, look near the model, first by
    its name, then (ripped models: Noesis names the material sm_3_m_body_b, the file is t_c2006b0001_body_b_base.tga)
    for a colour map whose name shares the material's words. -> the file on disk, None when it is only in memory
    (embedded in a GLB), or False when nothing fits (the material then gets its flat colour)."""
    p = bpy.path.abspath(img.filepath) if img.filepath else ''
    if p and os.path.isfile(p) and img.has_data:
        return p
    if img.packed_file is not None or (img.has_data and not img.filepath):
        return None
    files = _images_near(model_dir)
    base = os.path.basename(p or img.name).lower()
    stem = os.path.splitext(base)[0]
    def use(q):
        img.filepath = q
        img.reload()
        return q
    for q in files:
        f = os.path.basename(q).lower()
        if f == base or os.path.splitext(f)[0] == stem:
            return use(q)
    # by the words: drop the ripper's prefixes (sm_3_, m_), keep the part's words (body, b / hair, a / face)
    want = [t for t in _tokens(stem + ' ' + mat_name) if t not in ('sm', 'm', 'mat', 'material', 'diffuse', 'texture') and not t.isdigit()]
    best, score = None, 0
    for q in files:
        ft = _tokens(os.path.splitext(os.path.basename(q))[0])
        if any(t.startswith(NOT_COLOUR) for t in ft):
            continue
        hit = sum(1 for t in set(want) if t in ft)
        if not hit or not any(len(t) > 1 and t in ft for t in want):     # a one-letter match alone (a, b) isn't enough
            continue
        sc = hit * 10 + ('base' in ft or 'diff' in ft or 'albedo' in ft or 'col' in ft or 'color' in ft) * 3 - len(ft) * 0.1
        if sc > score:
            best, score = q, sc
    if best:
        return use(best)
    return False


def _save_png(img, path):
    """the picture's pixels as a PNG (a new image: a copy of one read from a file has no pixels of its own)"""
    w, h = img.size
    px = np.empty(w * h * 4, np.float32)
    img.pixels.foreach_get(px)
    img2 = bpy.data.images.new('dfx_png', w, h, alpha=True)
    img2.pixels.foreach_set(px)
    img2.filepath_raw = path
    img2.file_format = 'PNG'
    img2.save()
    bpy.data.images.remove(img2)
    return path


def one_texture(mesh, size=512, model_dir=''):
    """-> path of one PNG for the whole mesh on its first UV map: the model's own picture when it uses one (with UVs
    inside it), else every material baked into a new atlas (a new UV layout)."""
    me = mesh.data
    tmp = tempfile.mkdtemp(prefix='dfx_own_')
    mats = [m for m in me.materials]
    imgs = {m.name: _base_image(m) for m in mats if m}
    slot = lambda i: me.materials[i] if i < len(me.materials) else None
    used = {imgs.get(slot(p.material_index).name) if slot(p.material_index) else None for p in me.polygons} if mats else {None}
    if me.uv_layers:
        uv = np.empty(len(me.loops) * 2, np.float32)
        me.uv_layers[0].data.foreach_get('uv', uv)
        inside = uv.size == 0 or (uv.min() > -0.01 and uv.max() < 1.01)
    else:
        inside = False
    missing = []
    for mname, img in list(imgs.items()):
        if img is not None and _resolve(img, model_dir, mname) is False:
            missing.append(mname); imgs[mname] = None                     # its flat colour, not an error
    if missing:
        used = {imgs.get(slot(p.material_index).name) if slot(p.material_index) else None for p in me.polygons} if mats else {None}
    if len(used) == 1 and None not in used and inside:
        img = next(iter(used))
        while len(me.uv_layers) > 1:
            me.uv_layers.remove(me.uv_layers[-1])
        f = bpy.path.abspath(img.filepath) if img.filepath else ''
        path = f if f.lower().endswith(('.png', '.jpg', '.jpeg')) and os.path.isfile(f) else _save_png(img, os.path.join(tmp, 'body.png'))
        return path, 'its own texture (%s)' % img.name
    # several pictures: one atlas, a grid cell per material (its picture scaled into the cell, its UVs with it)
    slots = list(me.materials) or [None]
    # one cell per picture: materials showing the same picture (or the same flat colour) share it
    def look(m):
        img = imgs.get(m.name) if m else None
        if img is not None:                                      # by file: rips give each material its own copy
            return ('img', bpy.path.abspath(img.filepath) if img.filepath else img.name)
        bsdf = next((n for n in m.node_tree.nodes if n.type == 'BSDF_PRINCIPLED'), None) if m and m.use_nodes else None
        return ('rgb', tuple(round(v, 3) for v in (bsdf.inputs['Base Color'].default_value if bsdf else (m.diffuse_color if m else (0.8, 0.8, 0.8, 1)))))
    used_slots = {p.material_index for p in me.polygons}
    cell_of, firsts = {}, []                                     # material index -> cell; cell -> its first material
    for i, m in enumerate(slots):
        if i not in used_slots and len(slots) > 1:
            continue
        k = look(m)
        if k not in cell_of:
            cell_of[k] = len(firsts); firsts.append(m)
        cell_of[i] = cell_of[k]
    g = 1
    while g * g < len(firsts):
        g += 1
    cell = size // g
    atlas = np.zeros((size, size, 4), np.float32)              # Blender order: row 0 = bottom
    for k, m in enumerate(firsts):
        col, row = k % g, g - 1 - k // g                         # first picture top left
        img = imgs.get(m.name) if m else None
        if img is not None and not img.has_data:
            try:
                img.pixels[0]                                      # embedded (GLB) pictures load on first use
            except Exception:
                pass
        if img is not None and img.has_data:
            c = img.copy()
            c.scale(cell, cell)
            px = np.empty(cell * cell * 4, np.float32)
            c.pixels.foreach_get(px)
            bpy.data.images.remove(c)
            block = px.reshape(cell, cell, 4)
        else:
            bsdf = next((n for n in m.node_tree.nodes if n.type == 'BSDF_PRINCIPLED'), None) if m and m.use_nodes else None
            rgba = list(bsdf.inputs['Base Color'].default_value) if bsdf else list(m.diffuse_color if m else (0.8, 0.8, 0.8, 1))
            block = np.tile(np.array(rgba, np.float32), (cell, cell, 1))
        atlas[row * cell:(row + 1) * cell, col * cell:(col + 1) * cell] = block
    if not me.uv_layers:
        me.uv_layers.new(name='UVMap')
    uvl = me.uv_layers[0]
    uv = np.empty(len(me.loops) * 2, np.float32)
    uvl.data.foreach_get('uv', uv)
    uv = uv.reshape(-1, 2)
    inset = 1.0 / max(cell, 1)                                   # keep off the cell's edges (filtering)
    for poly in me.polygons:
        k = cell_of.get(min(poly.material_index, len(slots) - 1), 0)
        col, row = k % g, g - 1 - k // g
        ls = list(poly.loop_indices)
        q = uv[ls]
        q = q - np.floor(q.min(0))                               # tiled UVs: the face's own tile
        q = np.clip(q, inset, 1 - inset)
        uv[ls] = (np.array([col, row]) + q) / g
    uvl.data.foreach_set('uv', uv.reshape(-1))
    for l in [l for l in me.uv_layers if l != uvl]:
        me.uv_layers.remove(l)
    uvl.name = 'UVMap'
    out = bpy.data.images.new('dfx_own_atlas', size, size, alpha=True)
    out.pixels.foreach_set(atlas.reshape(-1))
    path = os.path.join(tmp, 'body.png')
    out.filepath_raw = path; out.file_format = 'PNG'; out.save()
    bpy.data.images.remove(out)
    return path, '%d picture%s in one %dx%d texture%s' % (len(firsts), '' if len(firsts) == 1 else 's', size, size, ('; no picture found for %s, so flat colour' % ', '.join(missing[:6]) + (' …' if len(missing) > 6 else '')) if missing else '')


# ---------------------------------------------------------------- fitting
def _coords(o):
    """world positions of a mesh's vertices (bound_box lags behind data.transform until the scene updates)"""
    co = np.empty(len(o.data.vertices) * 3, np.float32)
    o.data.vertices.foreach_get('co', co)
    co = co.reshape(-1, 3)
    M = np.array(o.matrix_world)
    return co @ M[:3, :3].T + M[:3, 3]


def _game_height(arm):
    W = arm.matrix_world
    head = W @ arm.data.bones['head'].head_local
    foot = min((W @ arm.data.bones[b].head_local).z for b in ('l_foot', 'r_foot'))
    return head.z, foot


def fit(context, arm, path, girth=1.0, max_tris=2500, tex_size=512, bake=True, progress=None, hair_colour=(0.35, 0.25, 0.18)):
    """Fit the model in `path` onto the game armature `arm` as its body. -> report dict"""
    say = progress or (lambda *_: None)
    if context.object and context.object.mode != 'OBJECT':
        bpy.ops.object.mode_set(mode='OBJECT')
    pos_prev = arm.data.pose_position
    arm.data.pose_position = 'REST'
    context.view_layer.update()
    rep = {'file': os.path.basename(path)}
    created = []
    try:
        say('importing %s' % os.path.basename(path))
        src_arm, meshes, created = import_model(path)
        bm = None
        if src_arm is not None:
            try:
                bm = bone_map(src_arm)
            except LookupError as ex:
                rep['skeleton'] = 'not used (%s): weighted automatically instead' % ex
                bm = None
        meshes, dropped = _prune(meshes)
        if dropped:
            rep['left_out'] = '%d part%s (repeats, eye layers, inside the mouth, stubble)' % (len(dropped), '' if len(dropped) == 1 else 's')
        cards = _card_hair(meshes, os.path.dirname(path))
        if cards:
            say('making the hair solid')
            face = next((o for o in meshes if o not in cards and any(n.endswith('face') or 'mface' in n for n in _part_words(o))), None)
            solid = _solid_hair(cards, src_arm, tuple(hair_colour), face)
            meshes = [o for o in meshes if o not in cards] + [solid]
            created.append(solid)
            rep['hair'] = 'its strands made one solid mass, in the hair colour'
        say('joining %d mesh%s' % (len(meshes), '' if len(meshes) == 1 else 'es'))
        mesh = _join(meshes, src_arm)
        rep['source_tris'] = sum(len(p.vertices) - 2 for p in mesh.data.polygons)
        say('texture')
        tex_path, rep['texture'] = one_texture(mesh, max(256, tex_size), os.path.dirname(path))
        if bm:
            rep['skeleton'] = 'matched %d bones (%s)' % (len(bm), ', '.join('%s=%s' % kv for kv in sorted(bm.items()) if kv[0] in ('Hips', 'Head', 'LeftHand', 'LeftFoot')))
            # face the same way as the game skeleton, and its size (girth factors assume the game's size)
            G = lambda n: arm.matrix_world @ arm.data.bones[n].head_local
            P = lambda n: src_arm.matrix_world @ src_arm.data.bones[bm[n]].head_local
            lat_s = P('LeftUpLeg') - P('RightUpLeg'); lat_g = G('l_femur') - G('r_femur')
            lat_s.z = lat_g.z = 0
            import math
            ang = math.atan2(lat_g.y, lat_g.x) - math.atan2(lat_s.y, lat_s.x)      # a turn about the vertical only
            R = Matrix.Rotation(ang, 4, 'Z') if lat_s.length > 1e-6 else Matrix.Identity(4)
            hs = (P('Head') - (P('LeftFoot') + P('RightFoot')) / 2).length
            hg = (G('head') - (G('l_foot') + G('r_foot')) / 2).length
            k = hg / hs if hs > 1e-9 else 1.0
            T = Matrix.Scale(k, 4) @ R
            src_arm.matrix_world = T @ src_arm.matrix_world
            mesh.data.transform(T)
            context.view_layer.update()
            say('weights onto the game bones')
            canonical_weights(mesh, src_arm, bm)
            context.view_layer.update()
            src_arm.data.pose_position = 'REST'
            maps = _maps(src_arm, arm, girth)
            for n in ('Spine', 'Chest'):                       # the column moves as one piece, like fit_body's torso
                if n in bm:
                    maps[n] = maps['Hips']
            weight_map = dict(FB.WEIGHT_MAP)
            say('fitting the body')
            r = FB.fit_core(src_arm, mesh, arm, maps, weight_map, tex_path, max_tris=max_tris, tex_size=tex_size,
                            bake=bake, tweak_eye=False)
        else:
            say('weighting automatically')
            r = _auto_fit(context, arm, mesh, src_arm, tex_path, max_tris, tex_size, bake)
            rep.setdefault('skeleton', 'none in the file: weighted automatically')
        rep.update({k: r[k] for k in ('tris_in', 'tris', 'verts') if k in r})
        rep['body'] = r.get('body')
    finally:
        for o in created:                                   # what the file brought that isn't the new body
            try:
                if o.name != rep.get('body'):
                    bpy.data.objects.remove(o)
            except ReferenceError:                          # already gone (fit_core removes the source)
                pass
        arm.data.pose_position = pos_prev
        context.view_layer.update()
    return rep


def _maps(src_arm, arm, girth):
    """fit_body.build_maps for any skeleton: the shoulder is optional, girth one factor for all."""
    keep = dict(FB.GIRTH)
    for k in FB.GIRTH:
        FB.GIRTH[k] = girth
    try:
        P = lambda n: src_arm.matrix_world @ src_arm.data.bones[n].head_local
        G = lambda n: arm.matrix_world @ arm.data.bones[n].head_local
        maps = {}
        an = Vector((0, P('Hips').y, P('LeftUpLeg').z)); ab = Vector((0, G('hip').y, G('l_femur').z))
        maps['Hips'] = FB._seg_map(an, P('Neck'), ab, G('neck'), girth)
        maps['Neck'] = maps['Hips']
        maps['Head'] = FB._rigid_map(P('Neck'), G('neck'), Matrix.Identity(4), girth)
        for side, s in (('Left', 'l_'), ('Right', 'r_')):
            if side + 'Shoulder' in src_arm.data.bones:
                maps[side + 'Shoulder'] = FB._seg_map(P(side + 'Shoulder'), P(side + 'Arm'), G(s + 'clavicle'), G(s + 'uparm'), girth)
            maps[side + 'Arm'] = FB._seg_map(P(side + 'Arm'), P(side + 'ForeArm'), G(s + 'uparm'), G(s + 'foarm'), girth)
            maps[side + 'ForeArm'] = FB._seg_map(P(side + 'ForeArm'), P(side + 'Hand'), G(s + 'foarm'), G(s + 'hand'), girth)
            R = FB._rot(P(side + 'Hand') - P(side + 'ForeArm'), G(s + 'hand') - G(s + 'foarm'))
            maps[side + 'Hand'] = FB._rigid_map(P(side + 'Hand'), G(s + 'hand'), R, girth)
            maps[side + 'UpLeg'] = FB._seg_map(P(side + 'UpLeg'), P(side + 'Leg'), G(s + 'femur'), G(s + 'tibia'), girth)
            maps[side + 'Leg'] = FB._seg_map(P(side + 'Leg'), P(side + 'Foot'), G(s + 'tibia'), G(s + 'foot'), girth)
            kz = G(s + 'foot').z / P(side + 'Foot').z if abs(P(side + 'Foot').z) > 1e-6 else 1.0
            maps[side + 'Foot'] = FB._rigid_map(P(side + 'Foot'), G(s + 'foot'), Matrix.Identity(4), (girth, girth, kz))
        return maps
    finally:
        FB.GIRTH.update(keep)


BODY_BONES = ('hip', 'waist', 'chest', 'neck', 'head', 'l_clavicle', 'l_uparm', 'l_foarm', 'l_hand', 'r_clavicle',
              'r_uparm', 'r_foarm', 'r_hand', 'l_femur', 'l_tibia', 'l_foot', 'r_femur', 'r_tibia', 'r_foot')


def _auto_fit(context, arm, mesh, src_arm, tex_path, max_tris, tex_size, bake):
    """No usable skeleton: stand the mesh on the game skeleton's feet at its height, weight it automatically to the
    body bones, then hand it to fit_core with identity maps."""
    top, foot = _game_height(arm)
    co = _coords(mesh)
    zmin = float(co[:, 2].min()); h = float(co[:, 2].max()) - zmin
    gh = (top - foot) * 1.12                                      # head bone to the top of the head
    k = gh / h if h > 1e-9 else 1.0
    cx, cy = (float(co[:, 0].min() + co[:, 0].max()) / 2, float(co[:, 1].min() + co[:, 1].max()) / 2)
    gx = (arm.matrix_world @ arm.data.bones['hip'].head_local)
    T = Matrix.Translation((gx.x, gx.y, foot)) @ Matrix.Scale(k, 4) @ Matrix.Translation((-cx, -cy, -zmin))
    mesh.data.transform(T)
    mesh.vertex_groups.clear()
    # automatic weights against a temporary copy of the game armature that only deforms with the body bones
    tmp_arm = arm.copy(); tmp_arm.data = arm.data.copy()
    context.scene.collection.objects.link(tmp_arm)
    tmp_arm.animation_data_clear()
    for b in tmp_arm.data.bones:
        b.use_deform = b.name in BODY_BONES
    tmp_arm.data.pose_position = 'REST'
    for o in context.view_layer.objects:
        o.select_set(False)
    mesh.select_set(True); tmp_arm.select_set(True); context.view_layer.objects.active = tmp_arm
    bpy.ops.object.parent_set(type='ARMATURE_AUTO')
    mesh.parent = None
    for m in [m for m in mesh.modifiers if m.type == 'ARMATURE']:
        mesh.modifiers.remove(m)
    bpy.data.objects.remove(tmp_arm)
    # identity maps on game-named groups: fit_core wants {group: 4x4} and {group: game bone}
    groups = [g.name for g in mesh.vertex_groups]
    if not groups:
        raise RuntimeError('automatic weights failed: the mesh may not be closed or may not stand in a pose like the character\'s')
    I = Matrix.Identity(4)
    maps = {g: I for g in groups}
    maps['Hips'] = I
    holder = bpy.data.objects.new('dfx_own_tmp_arm', bpy.data.armatures.new('dfx_own_tmp_arm'))
    context.scene.collection.objects.link(holder)
    return FB.fit_core(holder, mesh, arm, maps, {g: g for g in groups}, tex_path, max_tris=max_tris, tex_size=tex_size,
                       bake=bake, tweak_eye=False)


# ---------------------------------------------------------------- saving as a skin
def character_models(md, name, fold=True):
    """[(game path, rig: a file relative to the folder, or {'like': a game path read from the ISO})] the body goes on:
    every model of the character, the fighter (1P) and its costume slots (2P, 3P, zako, which then show the same
    model), the assists (_a) and _w, so none keeps the based-on character's body."""
    import json
    m = json.load(open(os.path.join(md, name, 'mod.json')))
    code = (m.get('character') or {}).get('code', '')
    out, F = [], m.get('files') or {}
    for gp, ref in F.items():
        mm = re.match(r'^obj/p_%s_(1p|2p|3p|zako)(_a|_w)?\.gmo$' % re.escape(code), gp)
        if not mm:
            continue
        one = 'obj/p_%s_1p%s.gmo' % (code, mm.group(2) or '')
        if fold and mm.group(1) != '1p' and one in F:        # the costumes take the first one's model (use_skin folds them)
            ref = F[one]
        if isinstance(ref, str) or isinstance(ref, dict) and 'like' in ref:
            out.append((gp, ref))
    return out


def _rig_bytes(md, name, ref, iso):
    if isinstance(ref, str):
        return open(os.path.join(md, name, ref), 'rb').read()
    if not iso or not os.path.isfile(iso):
        raise RuntimeError('read from the game ISO, which isn\'t set')
    from .dfx.iso import IsoPackage
    pk = IsoPackage(iso)
    try:
        return pk.read(ref['like'])
    finally:
        pk.close()


def _skin_in_use(md, name, stem):
    """the skin mod.json uses for this model (a named one with a file for it), or None"""
    import json
    from . import charmod_moves as CM
    _, G = CM.tools(md)
    cur = ((json.load(open(os.path.join(md, name, 'mod.json'))).get('body') or {}).get('skin') or '')
    if cur in ('', 'manikin', 'as_is'):
        return None
    for f in (stem + '.skin', 'body.skin'):
        p = os.path.join(md, name, 'skins', cur, f)
        if os.path.isfile(p):
            return G.skin_from_bytes(open(p, 'rb').read())
    return None


def save_skin(arm, skin, progress=None):
    """Export the open character and make skins/<skin>/<stem>.skin for every model of the character (see
    character_models). A rig whose body shares a texture with other parts is taken with copies of its own
    (charmod_gmo.own_body_slots, which apply_skin does too), so nothing is rewritten.
    -> {'skin', 'models': [game paths it applies to], 'skipped': {path: why}}"""
    say = progress or (lambda *_: None)
    from . import charmod_moves as CM, charmods, exporter
    md, name = arm['dfx_mod_dir'], arm['dfx_mod_name']
    CO, G = CM.tools(md)
    iso = charmods.iso_path(bpy.context)
    tmp = tempfile.mkdtemp(prefix='dfx_own_skin_')
    ex_path = os.path.join(tmp, 'body.gmo')
    say('exporting')
    exporter.export_gmo(arm, ex_path, motions=False)
    ex = open(ex_path, 'rb').read()
    from . import cape
    cape.sync_folder(arm, ex)                                # new bones (a cape) into the rigs first, + its physics
    out_dir = os.path.join(md, name, 'skins', skin)
    os.makedirs(out_dir, exist_ok=True)
    done, skipped, made = [], {}, {}
    for gp, ref in character_models(md, name) or [('obj/' + os.path.basename(arm['dfx_mod_rig']), arm['dfx_mod_rig'])]:
        stem = os.path.splitext(os.path.basename(gp))[0]
        key = ref if isinstance(ref, str) else 'iso:' + ref['like']
        try:
            if key not in made:
                say('skin for %s' % os.path.basename(gp))
                rig = _rig_bytes(md, name, ref, iso)
                sk = G.skin_from_body(rig, ex, 'own model: %s' % arm.get('dfx_own_model', 'body'))
                old = _skin_in_use(md, name, stem)
                if old is not None and old['slots'].get('weapons'):          # weapons put in before stay
                    try:
                        G.apply_weapons(G.apply_skin(rig, sk), old)
                        G.add_weapons(sk, {'slots': {'weapons': old['slots']['weapons']},
                                           'blobs': {k: v for k, v in old['blobs'].items() if k.startswith('wep.')}})
                    except Exception:
                        pass
                data = G.skin_to_bytes(sk)
                G.apply_skin(rig, G.skin_from_bytes(data))           # it must apply to the rig
                made[key] = data
            open(os.path.join(out_dir, stem + '.skin'), 'wb').write(made[key])
            done.append(gp)
        except Exception as ex_:
            skipped[gp] = str(ex_)
    if not done:
        raise RuntimeError('no skin could be made: ' + '; '.join('%s: %s' % kv for kv in skipped.items()))
    return {'skin': skin, 'models': done, 'skipped': skipped}


def bake_costume(arm, assist_rel, progress=None):
    """A costume is a model of its own (models/p_<code>_<costume>.gmo and its assist): the open character, with its new
    body, is written into its rig file, and the same body is put into the assist file. No skin: the costume's files
    are not in body.models, so the build uses them as they are. -> {'files': [written, relative]}"""
    say = progress or (lambda *_: None)
    from . import charmod_moves as CM, exporter
    md, name, rig_rel = arm['dfx_mod_dir'], arm['dfx_mod_name'], arm['dfx_mod_rig']
    CO, G = CM.tools(md)
    tmp = os.path.join(tempfile.mkdtemp(prefix='dfx_costume_'), 'rig.gmo')
    say('writing the costume model')
    exporter.export_gmo(arm, tmp, motions=False)             # the rig's own motions, untouched
    ex = open(tmp, 'rb').read()
    G.Model(ex).slots()
    open(os.path.join(md, name, rig_rel), 'wb').write(ex)
    out = [rig_rel]
    if assist_rel:
        ap = os.path.join(md, name, assist_rel)
        if os.path.isfile(ap):
            say('the same body on its assist')
            a = open(ap, 'rb').read()
            open(ap, 'wb').write(G.apply_skin(a, G.skin_from_body(a, ex, 'costume body')))
            out.append(assist_rel)
    return {'files': out}


def use_skin(md, name, skin, models):
    """mod.json body: skin = `skin`, models = the game paths it goes on (the build swaps the body in those)."""
    import json
    from collections import OrderedDict
    p = os.path.join(md, name, 'mod.json')
    m = json.load(open(p), object_pairs_hook=OrderedDict)
    b = m.setdefault('body', OrderedDict())
    b['skin'] = skin
    b['models'] = list(OrderedDict.fromkeys(list(b.get('models') or []) + list(models)))
    # one look now: every costume plays the first one's model (its assist the first assist); the costume models left
    # over from the based-on character are removed (the Move Studio does the same)
    code = (m.get('character') or {}).get('code', '')
    F = m.get('files') or {}
    before = {v for v in F.values() if isinstance(v, str)}
    for gp in list(F):
        mm = re.match(r'^obj/p_%s_(2p|3p|zako)(_a|_w)?\.gmo$' % re.escape(code), gp)
        one = mm and 'obj/p_%s_1p%s.gmo' % (code, mm.group(2) or '')
        if mm and one in F:
            F[gp] = json.loads(json.dumps(F[one]))
            if one in b['models'] and gp not in b['models']:
                b['models'].append(gp)
    after = {v for v in F.values() if isinstance(v, str)}
    with open(p, 'w') as f:
        json.dump(m, f, indent=1, ensure_ascii=False)
        f.write('\n')
    for rel in sorted(before - after):
        q = os.path.join(md, name, rel)
        if rel.startswith('models/') and rel.endswith('.gmo') and os.path.isfile(q):
            try:
                os.remove(q)
            except OSError:
                pass


# ---------------------------------------------------------------- your own weapon
WEAPON_PART = re.compile(r'^wep\d+$')


def _mod(md, name):
    import json
    return json.load(open(os.path.join(md, name, 'mod.json')))


def weapon_groups(md, name):
    """mod.json body.weapon_groups: [{name, parts, hint, length, flip, grip_at_origin, max_tex}] - the weapons the
    character should have, each one model for its parts (a held blade and its thrown copy); none: every wepN alone"""
    return list(((_mod(md, name).get('body') or {}).get('weapon_groups')) or [])


def weapon_spec(G, rig, mspec=None):
    """the weapons a skin can carry for this model: mod.json body.weapons when the mod has it (its parts, blade ghosts
    and weapon-only materials), else every wepN part on its own materials, their pictures the ones no part but weapons
    uses (the body gets copies of shared ones). -> (spec, the model as skins see it)"""
    import copy
    m = G.Model(G.own_body_slots(rig)[0])
    P = {p.name: i for i, p in enumerate(m.parts)}
    if mspec:
        spec = copy.deepcopy(mspec)
        spec['parts'] = {k: v for k, v in spec.get('parts', {}).items() if k in P}
        spec['skin_materials'] = {k: v for k, v in spec.get('skin_materials', {}).items() if k in spec['parts']}
        if spec.get('ghosts') and not all(g in P for g in spec['ghosts'].get('parts', [])):
            spec.pop('ghosts')
        return spec, m
    names = [p.name for p in m.parts if WEAPON_PART.match(p.name or '')]
    spec = {'parts': {n: n for n in names}, 'skin_materials': {n: list(m.part_materials(P[n])) for n in names}}
    spec['textures'] = G.own_weapon_textures(m, spec)
    return spec, m


def weapon_twins(G, rig, slot, md=None, name=None):
    """-> (the slots that take this model: its group in mod.json body.weapon_groups, else those drawn with the same
    material (mod.json body.weapons) or picture as `slot`; the parts other than weapons that share its picture)"""
    if md:
        g = next((g for g in weapon_groups(md, name) if slot in g.get('parts', [])), None)
        if g:
            return list(g['parts']), []
    mspec = ((_mod(md, name).get('body') or {}).get('weapons')) if md else None
    spec, m = weapon_spec(G, rig, mspec)
    P = {p.name: i for i, p in enumerate(m.parts)}
    if slot not in P:
        raise RuntimeError('this character has no weapon %s' % slot)
    if mspec:
        mine = set(spec['skin_materials'].get(slot, []))
        return [slot] + [n for n, ms in spec['skin_materials'].items() if n != slot and set(ms) & mine and n.startswith('wep')], []
    tex = lambda i: {t for mt in m.part_materials(i) for t in m.mat_textures(mt)}
    mine = tex(P[slot])
    twins = [n for n in spec['parts'] if n != slot and tex(P[n]) & mine]
    other = [p.name for i, p in enumerate(m.parts) if p.name not in spec['parts'] and tex(i) & mine]
    return [slot] + twins, other


def one_picture(ob, size=256, model_dir=''):
    """the weapon's materials -> one material with one picture (a PSP weapon slot has one texture)"""
    from . import common
    path, _ = one_texture(ob, size, model_dir)
    img = bpy.data.images.load(path, check_existing=False)
    if max(img.size) > size:
        k = size / max(img.size)
        img.scale(max(8, int(img.size[0] * k)), max(8, int(img.size[1] * k)))
    img.pack()
    mat = common.make_material(ob.name + '_own', img)
    ob.data.materials.clear()
    ob.data.materials.append(mat)
    for p in ob.data.polygons:
        p.material_index = 0


def _scale_about_hand(arm, ob, k):
    from .tools_bl import hand_bone_of
    hand = hand_bone_of(ob)
    piv = ob.matrix_world.inverted() @ (arm.matrix_world @ arm.data.bones[hand].head_local)
    co = np.empty(len(ob.data.vertices) * 3, np.float32)
    ob.data.vertices.foreach_get('co', co)
    co = co.reshape(-1, 3)
    p = np.array(piv[:], np.float32)
    ob.data.vertices.foreach_set('co', (p + (co - p) * k).reshape(-1))
    ob.data.update()


def save_weapons(arm, skin, slots, progress=None):
    """Export the open character and put its weapons into skins/<skin>/<stem>.skin for every model of the character
    that has them: into the file that is there (its body stays), else onto the body the game uses now (as stored,
    or the based-on character's manikin). -> {'skin', 'models', 'skipped'}"""
    import json
    say = progress or (lambda *_: None)
    from . import charmod_moves as CM, charmods, exporter
    md, name = arm['dfx_mod_dir'], arm['dfx_mod_name']
    _, G = CM.tools(md)
    iso = charmods.iso_path(bpy.context)
    m = json.load(open(os.path.join(md, name, 'mod.json')))
    body = m.get('body') or {}
    tmp = tempfile.mkdtemp(prefix='dfx_own_wep_')
    ex_path = os.path.join(tmp, 'weapons.gmo')
    say('exporting')
    exporter.export_gmo(arm, ex_path, motions=False)
    ex = open(ex_path, 'rb').read()
    from . import cape
    cape.sync_folder(arm, ex)                                # new bones (a cape) into the rigs first, + its physics
    out_dir = os.path.join(md, name, 'skins', skin)
    os.makedirs(out_dir, exist_ok=True)
    done, skipped, made = [], {}, {}
    for gp, ref in character_models(md, name) or [('obj/' + os.path.basename(arm['dfx_mod_rig']), arm['dfx_mod_rig'])]:
        stem = os.path.splitext(os.path.basename(gp))[0]
        key = ref if isinstance(ref, str) else 'iso:' + ref['like']
        try:
            if key not in made:
                say('weapons for %s' % os.path.basename(gp))
                rig = _rig_bytes(md, name, ref, iso)
                mspec = body.get('weapons')
                spec, rm = weapon_spec(G, rig, mspec)
                have = {p.name for p in G.Model(ex).parts}
                spec['parts'] = {k: v for k, v in spec['parts'].items() if v in have}
                if not any(spec['parts'].get(s_) in slots for s_ in spec['parts']):
                    raise RuntimeError('no %s in this model' % ', '.join(slots))
                spec['skin_materials'] = {k: spec['skin_materials'][k] for k in spec['parts']}
                P = {pp.name: i for i, pp in enumerate(rm.parts)}
                if not mspec:
                    spec['textures'] = G.own_weapon_textures(rm, spec)
                    # a weapon drawn with a picture another kind of part uses too (an effect) stays as the model has it
                    owned = set(spec['textures'])
                    for k in list(spec['parts']):
                        if not {t for mt in rm.part_materials(P[k]) for t in rm.mat_textures(mt)} <= owned:
                            if k in slots:
                                raise RuntimeError('%s shares its picture with a part that isn\'t a weapon' % k)
                            del spec['parts'][k]; del spec['skin_materials'][k]
                    spec['textures'] = G.own_weapon_textures(rm, spec)
                wsk = G.weapons_from_model(G.own_body_slots(rig)[0], ex, spec, 'own weapons')
                p = os.path.join(out_dir, stem + '.skin')
                if os.path.isfile(p):
                    sk = G.skin_from_bytes(open(p, 'rb').read())
                else:                                         # no skin yet: the body the game shows now
                    cur = body.get('skin') or 'manikin'
                    old = _skin_in_use(md, name, stem)
                    if old is not None:
                        sk = old
                    elif cur == 'as_is' or not iso:
                        sk = G.extract_skin(rig, 'the body as stored')
                    else:
                        src = body.get('manikin_from', 'obj/p_%s_zako.gmo' % (m.get('character') or {}).get('based_on', ''))
                        sk = G.skin_from_body(rig, _rig_bytes(md, name, {'like': src}, iso), 'manikin: ' + src)
                # the weapons not replaced now stay as the skin has them (the open scene may show others)
                if sk['slots'].get('weapons') == wsk['slots']['weapons']:
                    changed = {k for k, v in spec['parts'].items() if k in slots or v in slots}
                    gh = spec.get('ghosts') or {}
                    if gh.get('of') in slots:
                        changed |= set(gh.get('parts', []))
                    mats_of = lambda k: spec['skin_materials'].get(k) or (rm.part_materials(P[k]) if k in P else [])
                    ctex = {t for k in changed for mt in mats_of(k) for t in rm.mat_textures(mt)}
                    for k, v in list(sk['blobs'].items()):
                        mm = re.match(r'^wep\.(.+)\.(part|blend_bones|blend_offsets)$', k)
                        if mm and mm.group(1) not in changed:
                            wsk['blobs'][k] = v
                        mt_ = re.match(r'^wep\.tex(\d+)$', k)
                        if mt_ and int(mt_.group(1)) not in ctex:
                            wsk['blobs'][k] = v
                for k in [k for k in sk['blobs'] if k.startswith('wep.')]:
                    del sk['blobs'][k]
                G.add_weapons(sk, wsk)
                data = G.skin_to_bytes(sk)
                G.apply_skin(rig, G.skin_from_bytes(data))       # it must apply to the rig
                made[key] = data
            open(os.path.join(out_dir, stem + '.skin'), 'wb').write(made[key])
            done.append(gp)
        except Exception as ex_:
            skipped[gp] = str(ex_)
    if not done:
        raise RuntimeError('no skin could be made: ' + '; '.join('%s: %s' % kv for kv in skipped.items()))
    return {'skin': skin, 'models': done, 'skipped': skipped}


# ---------------------------------------------------------------- the operator (Character Selector, or the Studio)
from bpy.props import BoolProperty, EnumProperty, FloatProperty, FloatVectorProperty, IntProperty, StringProperty   # noqa: E402
from bpy_extras.io_utils import ImportHelper   # noqa: E402


def _job(jid, **kw):
    if not jid:
        return
    from . import studio_link
    studio_link.JOBS.setdefault(jid, {}).update(kw)


class DFX_OT_own_model(bpy.types.Operator, ImportHelper):
    """Replace this character's body with your own model (FBX, glTF/GLB, VRM, OBJ or .blend): it is fitted onto the
    character's skeleton (its own skeleton matched bone by bone, or weighted automatically), and saved as a skin in
    the character's folder"""
    bl_idname = 'dfx.own_model'
    bl_label = 'Use this model'
    bl_options = {'UNDO'}
    filter_glob: StringProperty(default='*.fbx;*.glb;*.gltf;*.vrm;*.obj;*.blend', options={'HIDDEN'})
    skin: StringProperty(name='Skin name', description='Folder in skins/ (empty: the file\'s name)', default='')
    girth: FloatProperty(name='Bulk', default=1.0, min=0.6, max=1.8,
                         description='Thickness of the limbs, torso and head on the game skeleton (1 = the model\'s own)')
    max_tris: IntProperty(name='Triangles', default=2500, min=500, max=8000,
                          description='At most this many triangles (the PSP\'s bodies have 1,500 to 3,000)')
    hair_colour: FloatVectorProperty(name='Hair colour', subtype='COLOR_GAMMA', size=3, min=0, max=1, default=(0.35, 0.25, 0.18),
                                     description='Hair made of strands with no colour picture (ripped game models) is made solid, in this colour')
    tex_size: EnumProperty(name='Texture', items=[('256', '256 x 256', ''), ('512', '512 x 512', '')], default='512')
    bake: BoolProperty(name='Painted lighting', default=True,
                       description='Bake soft lighting into the texture, as the game\'s characters have (slower)')
    use_in_game: BoolProperty(name='Use it in the game', default=True,
                              description='mod.json: the build puts this body on the character')
    job: StringProperty(options={'HIDDEN'})
    costume: StringProperty(options={'HIDDEN'}, description='2p / 3p / zako: the model goes into that costume\'s own files')
    assist: StringProperty(options={'HIDDEN'}, description='the costume\'s assist model file (relative to the character folder)')

    @classmethod
    def poll(cls, context):
        from .panel import active_arm
        a = active_arm(context)
        return a is not None and 'dfx_mod_name' in a

    def invoke(self, context, event):
        _job(self.job, state='picking', message='Pick the model in Blender')
        return super().invoke(context, event)

    def cancel(self, context):
        _job(self.job, state='cancelled', message='No model picked')

    def draw(self, context):
        L = self.layout
        L.label(text='Fitted onto this character\'s skeleton.')
        for p in ('skin', 'girth', 'max_tris', 'tex_size', 'bake', 'hair_colour'):
            L.prop(self, p)
        if not self.job:
            L.prop(self, 'use_in_game')

    def execute(self, context):
        from .panel import active_arm
        arm = active_arm(context)
        path = bpy.path.abspath(self.filepath)
        if not os.path.isfile(path) or os.path.splitext(path)[1].lower() not in EXTS:
            _job(self.job, state='error', message='Pick an FBX, glTF/GLB, VRM, OBJ or .blend file')
            self.report({'ERROR'}, 'Pick an FBX, glTF/GLB, VRM, OBJ or .blend file')
            return {'CANCELLED'}
        skin = re.sub(r'[^A-Za-z0-9_.-]+', '-', self.skin.strip() or os.path.splitext(os.path.basename(path))[0]).strip('-') or 'own-model'
        say = lambda m: (_job(self.job, state='working', message=m), print('Dissidia own model:', m))
        wm = context.window_manager
        wm.progress_begin(0, 3)
        try:
            rep = fit(context, arm, path, girth=self.girth, max_tris=self.max_tris, tex_size=int(self.tex_size),
                      bake=self.bake, progress=say, hair_colour=tuple(self.hair_colour))
            wm.progress_update(2)
            arm['dfx_own_model'] = os.path.basename(path)
            if self.costume:                                  # a costume: its own model files carry the body
                res = bake_costume(arm, self.assist, progress=say)
                _job(self.job, state='done', message='%s: %s, %s tris' % (self.costume.upper(), rep.get('skeleton', ''), rep.get('tris', '?')),
                     costume=self.costume, files=res['files'], report=rep)
                self.report({'INFO'}, 'Costume %s: %s' % (self.costume, ', '.join(res['files'])))
                return {'FINISHED'}
            res = save_skin(arm, skin, progress=say)
            if self.use_in_game and not self.job:
                use_skin(arm['dfx_mod_dir'], arm['dfx_mod_name'], skin, res['models'])
        except Exception as ex:
            import traceback
            traceback.print_exc()
            _job(self.job, state='error', message=str(ex))
            self.report({'ERROR'}, 'Own model: %s' % ex)
            return {'CANCELLED'}
        finally:
            wm.progress_end()
        rep.update(res)
        note = '%s: %s, %s tris, %s' % (skin, rep.get('skeleton', ''), rep.get('tris', '?'), rep.get('texture', ''))
        if res['skipped']:
            note += '; not for %s' % ', '.join(os.path.basename(k) for k in res['skipped'])
        _job(self.job, state='done', message=note, skin=skin, models=res['models'], report=rep)
        self.report({'INFO'}, 'Made skin ' + note)
        return {'FINISHED'}


class DFX_OT_own_weapon(bpy.types.Operator, ImportHelper):
    """Replace one of this character's weapons with your own model (FBX, glTF/GLB or OBJ): it is lined up with the old
    weapon, hilt on the grip, given one picture, and saved into the skin the game uses (or a new one)"""
    bl_idname = 'dfx.own_weapon'
    bl_label = 'Use this weapon'
    bl_options = {'UNDO'}
    filter_glob: StringProperty(default='*.fbx;*.glb;*.gltf;*.obj', options={'HIDDEN'})
    slot: StringProperty(name='Weapon', description='The weapon part it replaces (wep0, wep1...)')
    skin: StringProperty(name='Skin', description='Folder in skins/ it goes into (empty: the one the game uses, else own-weapons)')
    flip: BoolProperty(name='Flip direction', default=False, description='Use if it ends up held by the blade')
    length: FloatProperty(name='Length', default=0.0, min=0.0, max=5.0, unit='LENGTH',
                          description='Total length, pommel to tip. 0 = the old weapon\'s length')
    keep_scale: BoolProperty(name='Keep model scale', default=False, description='Otherwise it is scaled to the old weapon\'s length')
    grip_at_origin: BoolProperty(name='Grip at the model\'s origin', default=False,
                                 description='Put the hand on the model\'s own origin (game rips are made that way) instead of the handle found from the shape')
    max_tris: IntProperty(name='Triangles', default=600, min=50, max=3000, description='At most this many triangles')
    tex_size: EnumProperty(name='Texture', items=[('128', '128 x 128', ''), ('256', '256 x 256', '')], default='256')
    use_in_game: BoolProperty(name='Use it in the game', default=True, description='mod.json: the build puts this skin on the character')
    job: StringProperty(options={'HIDDEN'})

    @classmethod
    def poll(cls, context):
        from .panel import active_arm
        a = active_arm(context)
        return a is not None and 'dfx_mod_name' in a

    def invoke(self, context, event):
        from .panel import active_arm
        arm = active_arm(context)
        g = next((g for g in weapon_groups(arm['dfx_mod_dir'], arm['dfx_mod_name']) if self.slot in g.get('parts', [])), None) if arm else None
        if g:                                                   # the character's own sizes for this weapon
            self.length = float(g.get('length', self.length))
            self.flip = bool(g.get('flip', False))
            self.grip_at_origin = bool(g.get('grip_at_origin', False))
            if str(g.get('max_tex', '')) in ('128', '256'):
                self.tex_size = str(g['max_tex'])
        _job(self.job, state='picking', message='Pick the weapon model in Blender')
        return super().invoke(context, event)

    def cancel(self, context):
        _job(self.job, state='cancelled', message='No model picked')

    def draw(self, context):
        L = self.layout
        from .panel import active_arm
        arm = active_arm(context)
        g = next((g for g in weapon_groups(arm['dfx_mod_dir'], arm['dfx_mod_name']) if self.slot in g.get('parts', [])), None) if arm else None
        L.label(text='Replaces %s.' % ((g or {}).get('name') or self.slot or 'a weapon'))
        if g and g.get('hint'):
            L.label(text=g['hint'])
        for p in ('flip', 'length', 'keep_scale', 'grip_at_origin', 'max_tris', 'tex_size'):
            L.prop(self, p)
        if not self.job:
            L.prop(self, 'slot')
            L.prop(self, 'skin')
            L.prop(self, 'use_in_game')

    def execute(self, context):
        import json
        from .panel import active_arm
        from . import charmod_moves as CM, tools_bl
        arm = active_arm(context)
        path = bpy.path.abspath(self.filepath)
        fail = lambda msg: (_job(self.job, state='error', message=msg), self.report({'ERROR'}, 'Own weapon: %s' % msg), {'CANCELLED'})[2]
        if not os.path.isfile(path) or os.path.splitext(path)[1].lower() not in ('.fbx', '.glb', '.gltf', '.obj'):
            return fail('Pick an FBX, glTF/GLB or OBJ file')
        md, name = arm['dfx_mod_dir'], arm['dfx_mod_name']
        say = lambda m: (_job(self.job, state='working', message=m), print('Dissidia own weapon:', m))
        try:
            _, G = CM.tools(md)
            rig = open(os.path.join(md, name, arm['dfx_mod_rig']), 'rb').read()
            slots, other = weapon_twins(G, rig, self.slot, md, name)
            if other:
                return fail('%s is drawn with the same picture as %s, which isn\'t a weapon, so it can\'t take its own model'
                            % (self.slot, ', '.join(other[:3])))
            for sl in slots:
                ob = next((o for o in arm.children if o.type == 'MESH' and o.name.split('.')[0] == sl), None)
                if ob is None:
                    return fail('the character in Blender has no %s: import it again from the mods folder' % sl)
                say('fitting %s' % sl)
                tools_bl.replace_weapon_from_file(context, arm, ob, path, self.flip, self.keep_scale, max_tris=self.max_tris,
                                                  length=self.length, grip_at_origin=self.grip_at_origin)
                one_picture(ob, int(self.tex_size), os.path.dirname(path))
            cur = ((json.load(open(os.path.join(md, name, 'mod.json'))).get('body') or {}).get('skin') or '')
            skin = re.sub(r'[^A-Za-z0-9_.-]+', '-', self.skin.strip()).strip('-') or (
                cur if cur not in ('', 'manikin', 'as_is') else 'own-weapons')
            # the game stores positions inside the model's own range: a weapon that reaches past it is made a little
            # smaller about the hand until it fits
            obs = [next(o for o in arm.children if o.type == 'MESH' and o.name.split('.')[0] == sl) for sl in slots]
            shrunk = 1.0
            for _ in range(8):
                try:
                    res = save_weapons(arm, skin, slots, progress=say)
                    # the fighter's models decide; an assist model whose range is laid out otherwise keeps its old weapon
                    why = ' '.join(v for k, v in res['skipped'].items() if not re.search(r'_(a|w)\.gmo$', k))
                except RuntimeError as ex:
                    res, why = None, str(ex)
                over = [float(x) for x in re.findall(r"outside the rig's range by ([0-9.]+)", why)]
                if not over:                                       # every model (assists too) takes it
                    if res is None:
                        raise RuntimeError(why)
                    break
                k = 0.97 / (1.0 + max(over))
                shrunk *= k
                say('a little smaller to fit the model (%d%%)' % round(shrunk * 100))
                for ob in obs:
                    _scale_about_hand(arm, ob, k)
            else:
                raise RuntimeError('the weapon doesn\'t fit the model\'s range even at %d%% of its size' % round(shrunk * 100))
            if self.use_in_game and not self.job:
                use_skin(md, name, skin, res['models'])
        except Exception as ex:
            import traceback
            traceback.print_exc()
            return fail(str(ex))
        note = '%s in %s%s' % (', '.join(slots), skin, ' (the same picture: they change together)' if len(slots) > 1 else '')
        if shrunk < 0.995:
            note += '; made %d%% of its size to fit the model' % round(shrunk * 100)
        if res['skipped']:
            note += '; %s keep%s the old weapon' % (', '.join(os.path.basename(k) for k in res['skipped']), 's' if len(res['skipped']) == 1 else '')
        _job(self.job, state='done', message=note, skin=skin, models=res['models'], slots=slots)
        self.report({'INFO'}, 'Own weapon: ' + note)
        return {'FINISHED'}


classes = (DFX_OT_own_model, DFX_OT_own_weapon)


def register():
    for c in classes:
        bpy.utils.register_class(c)


def unregister():
    for c in reversed(classes):
        bpy.utils.unregister_class(c)
