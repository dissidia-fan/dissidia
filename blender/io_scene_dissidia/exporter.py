"""Export a Dissidia armature (+ child meshes, images, actions) back to GMO.

The embedded original GMO is the template. Anything whose content hash is
unchanged since import is kept byte-for-byte; edited parts, textures and
motions are re-encoded. `force=True` re-encodes everything (encoder test).
"""
import struct
import numpy as np
import bpy
from bpy_extras import anim_utils

from .dfx.chunks import Chunk, GmoFile, BONE, PART, MESH, ARRAYS, MATERIAL, TEXTURE, MOTION
from .dfx.gmo import (Character, VertexArray, DrawCall, Motion, FCurveData, Binding,
                      make_ref, parse_bones)
from .dfx import gim
from . import common

MAX_SUBSET = 8          # PSP GE skinning matrix limit per draw


class ExportError(Exception):
    pass


# ---------------------------------------------------------------- helpers
def _edit_worlds(arm_ob, names):
    bones = arm_ob.data.bones
    out = []
    for n in names:
        if n not in bones:
            raise ExportError(f'Bone "{n}" is missing. Adding/removing bones is not supported yet.')
        out.append(np.array(bones[n].matrix_local, np.float64))
    return out


def _set_cmd(bone_chunk, ctype, payload):
    for c in bone_chunk.children:
        if c.type == ctype:
            c.payload = payload
            return
    # insert after ParentBone if present
    idx = next((i + 1 for i, c in enumerate(bone_chunk.children) if c.type == 0x8041), 0)
    bone_chunk.children.insert(idx, Chunk(ctype, payload=payload))


def _mesh_data(ob, bone_index):
    """Triangulated corner data: positions, uvs, colours, per-vertex weights dict."""
    me = ob.data
    me.calc_loop_triangles()
    nv = len(me.vertices)
    co = np.empty(nv * 3, np.float32); me.vertices.foreach_get('co', co); co = co.reshape(-1, 3).astype(np.float64)
    tris = np.empty(len(me.loop_triangles) * 3, np.int32); me.loop_triangles.foreach_get('loops', tris); tris = tris.reshape(-1, 3)
    tmat = np.empty(len(me.loop_triangles), np.int32); me.loop_triangles.foreach_get('material_index', tmat)
    lv = np.empty(len(me.loops), np.int32); me.loops.foreach_get('vertex_index', lv)
    if me.uv_layers:
        uv = np.empty(len(me.loops) * 2, np.float32); me.uv_layers[0].data.foreach_get('uv', uv)
        uv = uv.reshape(-1, 2).astype(np.float64); uv[:, 1] = 1.0 - uv[:, 1]
    else:
        uv = np.zeros((len(me.loops), 2))
    col = None
    ca = me.color_attributes.get('Col') or (me.color_attributes[0] if len(me.color_attributes) else None)
    if ca is not None:
        n = len(ca.data)
        c = np.empty(n * 4, np.float32); ca.data.foreach_get('color', c); c = c.reshape(-1, 4)
        col = c[lv] if ca.domain == 'POINT' else c
    else:
        col = np.ones((len(me.loops), 4))
    gname = {g.index: g.name for g in ob.vertex_groups}
    vw = [dict() for _ in range(nv)]
    for v in me.vertices:
        for g in v.groups:
            bn = gname.get(g.group)
            if bn in bone_index and g.weight > 1e-4:
                vw[v.index][bone_index[bn]] = g.weight
    return co, tris, tmat, lv, uv, col, vw


def _limit_weights(w, limit=4):
    items = sorted(w.items(), key=lambda kv: -kv[1])[:limit]
    s = sum(v for _, v in items) or 1.0
    return {k: v / s for k, v in items}


def _batch_triangles(tri_bones, limit=MAX_SUBSET):
    """Greedy grouping of triangles so each batch references <= limit bones."""
    batches = []
    for ti, bs in enumerate(tri_bones):
        for b in batches:
            u = b['bones'] | bs
            if len(u) <= limit:
                b['bones'] = u
                b['tris'].append(ti)
                break
        else:
            batches.append({'bones': set(bs), 'tris': [ti]})
    return batches


# ---------------------------------------------------------------- parts
def encode_part(ob, part_chunk, owner_chunk, part_index, ch, bone_names, worlds, pos_off, pos_scl, uv_off, uv_scl, mat_index_of_slot):
    bone_index = {n: i for i, n in enumerate(bone_names)}
    co, tris, tmat, lv, uv, col, vw = _mesh_data(ob, bone_index)
    vw = [_limit_weights(w) if w else w for w in vw]
    # owner fallback for unweighted vertices
    owner_name = owner_chunk.name
    for i, w in enumerate(vw):
        if not w:
            vw[i] = {bone_index[owner_name]: 1.0} if owner_name in bone_index else {}

    palette = sorted({b for w in vw for b in w})
    if not palette:
        raise ExportError(f'{ob.name}: no vertex weights')
    pal_pos = {b: i for i, b in enumerate(palette)}

    meshes, arrays = [], []
    for gm in sorted(set(mat_index_of_slot[s] for s in set(tmat.tolist()))):
        slots = [s for s, m in mat_index_of_slot.items() if m == gm]
        sel = np.nonzero(np.isin(tmat, slots))[0]
        tri_bones = [set().union(*(vw[lv[l]].keys() for l in tris[t])) for t in sel]
        for batch in _batch_triangles(tri_bones):
            subset = sorted(batch['bones'])
            sub_pos = {b: i for i, b in enumerate(subset)}
            vmap, P, T, C, W, idx = {}, [], [], [], [], []
            for k in batch['tris']:
                for l in tris[sel[k]]:
                    v = lv[l]
                    key = (v, round(uv[l, 0], 5), round(uv[l, 1], 5), tuple(np.round(col[l], 3)))
                    if key not in vmap:
                        vmap[key] = len(P)
                        P.append(co[v]); T.append(uv[l]); C.append(col[l])
                        w = np.zeros(len(subset))
                        for b, wt in vw[v].items():
                            w[sub_pos[b]] = wt
                        W.append(w)
                    idx.append(vmap[key])
            P = (np.array(P) - pos_off) / pos_scl
            T = (np.array(T) - uv_off) / uv_scl
            if np.abs(P).max() > 1.02 or T.min() < -0.02 or T.max() > 2.02:
                raise ExportError(f'{ob.name}: geometry/UVs outside encodable range')
            P = np.clip(P, -1.0, 32767 / 32768); T = np.clip(T, 0.0, 65535 / 32768)
            ai = len(arrays)
            arr = VertexArray.encode(f'arrays-{ai}-0000', P, T, np.array(C), np.array(W), len(subset))
            arrays.append(arr)
            mc = Chunk(MESH)
            mc.name = f'16mesh-{part_index}-{len(meshes):04d}'
            mc.children.append(Chunk(0x8061, payload=struct.pack('<I', make_ref(MATERIAL, gm, scope=2))))
            mc.children.append(Chunk(0x8062, payload=struct.pack(f'<I{len(subset)}I', len(subset), *[pal_pos[b] for b in subset])))
            dc = DrawCall(make_ref(ARRAYS, ai), 3, len(idx), 1, idx)
            mc.children.append(Chunk(0x8066, payload=dc.to_payload()))
            meshes.append(mc)

    part_chunk.children = meshes + [a.to_chunk() for a in arrays]
    # owner bone palette + inverse bind matrices
    _set_cmd(owner_chunk, 0x8044, struct.pack(f'<I{len(palette)}I', len(palette), *[make_ref(BONE, b) for b in palette]))
    offs = b''.join(np.linalg.inv(worlds[b]).T.astype('<f4').tobytes() for b in palette)
    _set_cmd(owner_chunk, 0x8045, struct.pack('<I', len(palette)) + offs)
    return len(meshes), sum(a.count for a in arrays)


# ---------------------------------------------------------------- motions
def _reduce(frames, vals, tol, slerp=False):
    """Keep the minimal set of keys so linear (or slerp) interpolation stays within tol."""
    n = len(frames)
    if n <= 2 or np.allclose(vals, vals[0], atol=tol):
        return [0] if np.allclose(vals, vals[0], atol=tol) else list(range(n))
    keep = [0]
    i = 0
    while i < n - 1:
        j = i + 2
        while j < n:
            t = (frames[i + 1:j] - frames[i]) / (frames[j] - frames[i])
            interp = vals[i] + (vals[j] - vals[i]) * t[:, None]
            if slerp:
                interp /= np.linalg.norm(interp, axis=1, keepdims=True)
            if np.abs(interp - vals[i + 1:j]).max() > tol:
                break
            j += 1
        i = j - 1
        keep.append(i)
    return keep


def encode_action(act, arm_ob, bone_names, edit_local, template_motion):
    slot = act.slots[0] if act.slots else None
    cb = anim_utils.action_get_channelbag_for_slot(act, slot) if slot else None
    if cb is None:
        raise ExportError(f'Action {act.name} has no animation data')
    f0, f1 = act.frame_range
    frames = np.arange(int(round(f0)), int(round(f1)) + 1, dtype=np.float64)
    curves = {}
    for fc in cb.fcurves:
        if not fc.data_path.startswith('pose.bones["'):
            continue
        bn = fc.data_path.split('"')[1]
        prop = fc.data_path.rsplit('.', 1)[1]
        curves.setdefault(bn, {})[(prop, fc.array_index)] = fc
    bindings, fcurves = [], []

    def add(bi, cmd, interp, vals, keep):
        k = np.c_[frames[keep], vals[keep]]
        fcurves.append(FCurveData(f'fcurve-{len(fcurves)}', 1 if interp == 'lin' else 4, vals.shape[1], k))
        if len(keep) == 1:
            fcurves[-1].interp = 0
        bindings.append(Binding(bi, cmd, 0, len(fcurves) - 1))

    for bi, bn in enumerate(bone_names):
        if bn not in curves:
            continue
        cv = curves[bn]

        def sample(prop, n, default):
            out = np.tile(np.array(default, np.float64), (len(frames), 1))
            for k in range(n):
                fc = cv.get((prop, k))
                if fc is not None:
                    out[:, k] = [fc.evaluate(f) for f in frames]
            return out
        loc = sample('location', 3, (0, 0, 0))
        rot = sample('rotation_quaternion', 4, (1, 0, 0, 0))
        rot /= np.linalg.norm(rot, axis=1, keepdims=True)
        scl = sample('scale', 3, (1, 1, 1))
        T, R, S = common.basis_to_local_trs(loc, rot, scl, edit_local[bi])
        add(bi, 0x48, 'lin', T, _reduce(frames, T, 1e-4))
        add(bi, 0x4B, 'slerp', R, _reduce(frames, R, 2e-4, slerp=True))
        add(bi, 0x4D, 'lin', S, _reduce(frames, S, 1e-4))

    rate = act.get('dfx_frame_rate', 30.0)
    extra = template_motion.extra if template_motion else []
    header = template_motion.header if template_motion else b''
    # FrameLoop = {loop start, end}; keys always start at the action's first frame (see importer.actions_from_motions)
    loop0 = act.get('dfx_loop_start')
    fs = float(loop0) if loop0 is not None and float(f0) < float(loop0) <= float(f1) else float(f0)
    m = Motion(act.name, fs, float(f1), float(rate), bindings, fcurves, extra, header)
    return m.to_chunk()


# ---------------------------------------------------------------- new bones / parts / materials
def _ordered_new_bones(arm_ob, existing):
    """Armature bones not in the template, parents first."""
    out, placed = [], set(existing)
    from .rig_control import is_rig_bone           # control-rig bones are Blender-only
    pending = [b for b in arm_ob.data.bones if b.name not in placed and not is_rig_bone(b.name)]
    while pending:
        progressed = False
        for b in list(pending):
            if b.parent is None or b.parent.name in placed:
                out.append(b); placed.add(b.name); pending.remove(b); progressed = True
        if not progressed:
            raise ExportError('could not order new bones')
    return out


def _bone_chunk(name, parent_idx, local, extra_cmds=(), rest_scale0=False):
    c = Chunk(BONE)
    c.name = name[:31]
    if parent_idx >= 0:
        c.children.append(Chunk(0x8041, payload=struct.pack('<I', make_ref(BONE, parent_idx))))
    T, R, S = common.decompose(local[None])
    c.children.append(Chunk(0x8048, payload=struct.pack('<3f', *T[0])))
    if np.abs(R[0] - [0, 0, 0, 1]).max() > 1e-6:
        c.children.append(Chunk(0x804B, payload=struct.pack('<4f', *R[0])))
    if rest_scale0:
        c.children.append(Chunk(0x804D, payload=struct.pack('<3f', 0.0, 0.0, 0.0)))
    c.children += list(extra_cmds)
    return c


# ---------------------------------------------------------------- bone length (spring-bone physics)
# BlindData 0x80F1 "DISTANCE" + f32 on a mesh-model bone: the length the game's spring physics gives the bone (its
# tip = (length, 0, 0) in the bone's frame; docs/spring-bones.md). Blender keeps it as the bone property
# 'dfx_distance' (the importer sets it from the file; Add cape bones sets it on the new bones).
DISTANCE = 0x80F1
DISTANCE_TAG = b'DISTANCE\0\0\0\0'


def distance_of(bone_chunk):
    for c in bone_chunk.children:
        if c.type == DISTANCE and c.payload[:12] == DISTANCE_TAG and len(c.payload) >= 16:
            return struct.unpack_from('<f', c.payload, 12)[0]
    return None


def _distance_cmds(bl):
    v = bl.get('dfx_distance') if bl is not None else None
    return [Chunk(DISTANCE, payload=DISTANCE_TAG + struct.pack('<f', float(v)))] if v is not None and float(v) > 0 else []


def _sync_distance(bone_chunk, bl):
    """An existing bone: write its 'dfx_distance' if it differs from the file's (bit for bit); else leave it."""
    want = _distance_cmds(bl)
    if not want:
        return False
    for c in bone_chunk.children:
        if c.type == DISTANCE and c.payload[:12] == DISTANCE_TAG:
            if c.payload == want[0].payload:
                return False
            c.payload = want[0].payload
            return True
    bone_chunk.children.append(want[0])
    return True


def _insert_after_last(model, btype, chunks):
    idx = max((i for i, c in enumerate(model.children) if c.type == btype), default=len(model.children) - 1)
    model.children[idx + 1:idx + 1] = chunks


def _material_image(mat):
    if mat is None or not mat.node_tree:
        return None
    for n in mat.node_tree.nodes:
        if n.type == 'TEX_IMAGE' and n.image is not None:
            return n.image
    return None


class _Resources:
    """Allocates new GMO materials/textures for Blender materials that the template lacks."""

    def __init__(self, ch, arm_name):
        import copy
        self.copy = copy
        self.ch, self.arm_name = ch, arm_name
        self.mats = ch.mesh_model.find(MATERIAL)
        self.texs = ch.mesh_model.find(TEXTURE)
        self.mat_template = next((m for m in self.mats if any(l.find(0x8091) for l in m.find(0x0009))), self.mats[0] if self.mats else None)
        self.tex_template = next((t for t in self.texs if t.first(0x8013) is not None), None)
        self.new_mats, self.new_texs = [], []
        self.by_material, self.by_image = {}, {}

    def texture_index(self, img):
        if img.get('dfx_texture_index') is not None and img.name.startswith(self.arm_name):
            return int(img['dfx_texture_index'])
        if img.name in self.by_image:
            return self.by_image[img.name]
        if self.tex_template is None:
            raise ExportError('template has no texture to clone')
        t = self.copy.deepcopy(self.tex_template)
        t.name = f'file{len(self.texs) + len(self.new_texs)}'
        fn = (img.name.rsplit('.', 1)[0] + '.tga').encode('latin1', 'replace') + b'\0'
        fn += b'\0' * ((-len(fn)) % 4)
        for c in t.children:
            if c.type == 0x8012:
                c.payload = fn
            elif c.type == 0x8013:
                c.payload = gim.wrap_for_gmo(gim.encode(common.image_rgba(img)))
        idx = len(self.texs) + len(self.new_texs)
        self.new_texs.append(t); self.by_image[img.name] = idx
        return idx

    def material_index(self, mat):
        if mat is not None and mat.get('dfx_material_index') is not None and mat.name.startswith(self.arm_name):
            return int(mat['dfx_material_index'])
        key = mat.name if mat else '<none>'
        if key in self.by_material:
            return self.by_material[key]
        if self.mat_template is None:
            raise ExportError('template has no material to clone')
        m = self.copy.deepcopy(self.mat_template)
        m.name = key[:27]
        img = _material_image(mat)
        for layer in m.find(0x0009):
            for c in list(layer.children):
                if c.type == 0x8091:
                    if img is None:
                        layer.children.remove(c)
                    else:
                        c.payload = struct.pack('<I', make_ref(TEXTURE, self.texture_index(img), scope=2))
        idx = len(self.mats) + len(self.new_mats)
        self.new_mats.append(m); self.by_material[key] = idx
        return idx

    def commit(self):
        if self.new_mats:
            _insert_after_last(self.ch.mesh_model, MATERIAL, self.new_mats)
        if self.new_texs:
            _insert_after_last(self.ch.mesh_model, TEXTURE, self.new_texs)


def _mesh_bounds(obs):
    lo, hi, ulo, uhi = np.full(3, np.inf), np.full(3, -np.inf), np.full(2, np.inf), np.full(2, -np.inf)
    for ob in obs:
        me = ob.data
        co = np.empty(len(me.vertices) * 3, np.float32); me.vertices.foreach_get('co', co); co = co.reshape(-1, 3)
        if len(co):
            lo = np.minimum(lo, co.min(0)); hi = np.maximum(hi, co.max(0))
        if me.uv_layers and len(me.loops):
            uv = np.empty(len(me.loops) * 2, np.float32); me.uv_layers[0].data.foreach_get('uv', uv); uv = uv.reshape(-1, 2)
            uv[:, 1] = 1.0 - uv[:, 1]
            ulo = np.minimum(ulo, uv.min(0)); uhi = np.maximum(uhi, uv.max(0))
    return lo, hi, ulo, uhi


def _set_8015(model, sel, floats):
    for c in model.find(0x8015):
        if struct.unpack_from('<I', c.payload)[0] == sel:
            c.payload = struct.pack('<I', sel) + struct.pack(f'<{len(floats)}f', *floats)
            return


# ---------------------------------------------------------------- main
def export_gmo(arm_ob, filepath, force=False, motions=True):
    from . import rig_control
    if rig_control.has_rig(arm_ob):                 # control-rig actions: bake onto the game bones first
        rig_control.bake_all(bpy.context, arm_ob)
    template = bytes(arm_ob['dfx_template'])
    ch = Character.load(template)
    names = [b.name for b in ch.bones]
    missing = [n for n in names if n not in arm_ob.data.bones]
    if missing:
        raise ExportError(f'Bones removed from the armature: {missing[:6]}. Removing bones is not supported '
                          '(movesets address bones by index); hide or unweight them instead.')
    new_bones = _ordered_new_bones(arm_ob, names)
    all_names = names + [b.name for b in new_bones]
    worlds = _edit_worlds(arm_ob, all_names)
    idx_of = {n: i for i, n in enumerate(all_names)}
    stats = {'parts_kept': 0, 'parts_encoded': 0, 'tex_kept': 0, 'tex_encoded': 0,
             'motions_kept': 0, 'motions_encoded': 0, 'bones_added': 0, 'parts_added': 0}

    # --- skeleton (mesh model): did the bind pose of existing bones change?
    from .importer import _bind_worlds, _normalised
    orig = [_normalised(m) for m in _bind_worlds(ch)]
    bones_changed = any(np.abs(a - b).max() > 1e-4 for a, b in zip(orig, worlds[:len(names)]))
    bone_chunks = ch.mesh_model.find(BONE)
    if bones_changed or force:
        for i, (bc, b) in enumerate(zip(bone_chunks, ch.bones)):
            local = np.linalg.inv(worlds[b.parent]) @ worlds[i] if b.parent >= 0 else worlds[i]
            T, R, S = common.decompose(local[None])
            _set_cmd(bc, 0x8048, struct.pack('<3f', *T[0]))
            if b.has_rotate or np.abs(R[0] - [0, 0, 0, 1]).max() > 1e-6:
                _set_cmd(bc, 0x804B, struct.pack('<4f', *R[0]))
        # the animation skeleton carries the same rest pose; bones a motion doesn't key fall back to it
        if ch.anim_model is not None:
            for i, (bc, b) in enumerate(zip(ch.anim_model.find(BONE), ch.bones)):
                local = np.linalg.inv(worlds[b.parent]) @ worlds[i] if b.parent >= 0 else worlds[i]
                T, R, S = common.decompose(local[None])
                _set_cmd(bc, 0x8048, struct.pack('<3f', *T[0]))

    def local_of(i):
        b = arm_ob.data.bones[all_names[i]]
        return np.linalg.inv(worlds[idx_of[b.parent.name]]) @ worlds[i] if b.parent else worlds[i]

    parent_idx = lambda i: idx_of[arm_ob.data.bones[all_names[i]].parent.name] if arm_ob.data.bones[all_names[i]].parent else -1
    for bc, b in zip(bone_chunks, ch.bones):           # bone lengths edited in Blender (normally none)
        if names.count(b.name) == 1:
            _sync_distance(bc, arm_ob.data.bones.get(b.name))
    new_mesh_bones, new_anim_bones = [], []
    for b in new_bones:
        i = idx_of[b.name]
        new_mesh_bones.append(_bone_chunk(b.name, parent_idx(i), local_of(i), _distance_cmds(b)))
        new_anim_bones.append(_bone_chunk(b.name, parent_idx(i), local_of(i), rest_scale0=bool(b.get('dfx_rest_scale0'))))
    stats['bones_added'] = len(new_bones)

    # --- parts: existing and new
    meshes = [o for o in arm_ob.children if o.type == 'MESH']
    mesh_obs = {int(o['dfx_part_index']): o for o in meshes if 'dfx_part_index' in o}
    new_part_obs = [o for o in meshes if 'dfx_part_index' not in o]
    part_chunks = ch.mesh_model.find(PART)

    # position / uv encodable ranges
    pos_off, pos_scl, uv_off, uv_scl = ch.pos_offset.copy(), ch.pos_scale.copy(), ch.uv_offset.copy(), ch.uv_scale.copy()
    to_encode = [o for o in meshes if 'dfx_part_index' not in o or force or bones_changed
                 or o.get('dfx_hash') != common.mesh_signature(o)]
    lo, hi, ulo, uhi = _mesh_bounds(to_encode) if to_encode else (np.full(3, np.nan),) * 2 + (np.full(2, np.nan),) * 2
    force_parts = False
    TOL = 0.01           # small excursions are clamped by the encoder
    pos_out = np.isfinite(lo).all() and (np.any((lo - pos_off) / pos_scl < -1.0 - TOL) or np.any((hi - pos_off) / pos_scl > 1.0 + TOL))
    uv_out = np.isfinite(ulo).all() and (np.any((ulo - uv_off) / uv_scl < -TOL) or np.any((uhi - uv_off) / uv_scl > 2.0 + TOL))
    if pos_out or uv_out:
        force_parts = True          # the global range changes, so every part is re-encoded against it
        lo, hi, ulo, uhi = _mesh_bounds(meshes)
        if pos_out:
            pos_off = (lo + hi) / 2
            pos_scl = np.full(3, float(np.max(hi - lo) / 2 * 1.01) or 1.0)
            _set_8015(ch.mesh_model, 0x180, list(pos_off) + list(pos_scl))
        if uv_out:
            uv_off = ulo.copy()
            uv_scl = np.maximum((uhi - ulo) / 2 * 1.0001, 1e-6)
            _set_8015(ch.mesh_model, 0x3, list(uv_off) + list(uv_scl))

    res = _Resources(ch, arm_ob.name)

    def slot_map_for(ob):
        return {s: res.material_index(mat) for s, mat in enumerate(ob.data.materials)} or {0: res.material_index(None)}

    for pi, pc in enumerate(part_chunks):
        ob = mesh_obs.get(pi)
        if ob is None:
            continue
        changed = force or force_parts or bones_changed or ob.get('dfx_hash') != common.mesh_signature(ob)
        if not changed:
            stats['parts_kept'] += 1
            continue
        owner_i = ch.part_owner(pi)
        encode_part(ob, pc, bone_chunks[owner_i], pi, ch, all_names, worlds, pos_off, pos_scl, uv_off, uv_scl, slot_map_for(ob))
        stats['parts_encoded'] += 1

    # brand-new parts: a draw bone at the origin (game convention) + a Part block
    root_i = idx_of.get('root', 0)
    new_parts, new_locals = [], []
    drawbones = {o.name: ch.part_owner(i) for i, o in mesh_obs.items()}
    for k, ob in enumerate(new_part_obs):
        pi = len(part_chunks) + k
        owner_name = ob.name[:31]
        if owner_name in idx_of:
            owner_name = (ob.name[:27] + f'_p{pi}')
        oi = len(all_names)
        all_names.append(owner_name); idx_of[owner_name] = oi
        worlds.append(np.eye(4))
        # effect parts (dfx_fx) get a top-level draw bone so hiding the body (root scale 0) can't hide them
        par = -1 if ob.get('dfx_fx') else root_i
        local = np.eye(4) if par < 0 else np.linalg.inv(worlds[root_i])
        new_locals.append(local)
        draw = Chunk(0x804E, payload=struct.pack('<I', make_ref(PART, pi)))
        mb = _bone_chunk(owner_name, par, local, [draw])
        new_mesh_bones.append(mb)
        new_anim_bones.append(_bone_chunk(owner_name, par, local))
        pc = Chunk(PART)
        pc.name = ob.name[:27]
        encode_part(ob, pc, mb, pi, ch, all_names, worlds, pos_off, pos_scl, uv_off, uv_scl, slot_map_for(ob))
        new_parts.append(pc)
        stats['parts_added'] += 1
        drawbones[ob.name] = oi

    if new_mesh_bones:
        _insert_after_last(ch.mesh_model, BONE, new_mesh_bones)
        if ch.anim_model is not None and ch.anim_model is not ch.mesh_model:
            _insert_after_last(ch.anim_model, BONE, new_anim_bones)
    if new_parts:
        _insert_after_last(ch.mesh_model, PART, new_parts)
    res.commit()

    # --- textures
    tex_chunks = ch.mesh_model.find(TEXTURE)
    for img in bpy.data.images:
        if 'dfx_texture_index' not in img or not img.name.startswith(arm_ob.name):
            continue
        ti = int(img['dfx_texture_index'])
        if not force and img.get('dfx_hash') == common.image_signature(img):
            stats['tex_kept'] += 1
            continue
        rgba = common.image_rgba(img)
        for c in tex_chunks[ti].children:
            if c.type == 0x8013:
                c.payload = gim.wrap_for_gmo(gim.encode(rgba))
        stats['tex_encoded'] += 1

    # --- motions (motions=False: keep the template's as they are, e.g. a skin recipe that only needs the parts)
    if motions and ch.anim_model is not None:
        anim_names = [b.name for b in parse_bones(ch.anim_model)]
        if len(anim_names) != len(all_names):    # names may differ (some tools rename draw bones in one model only;
            raise ExportError('animation skeleton differs from mesh skeleton')   # the game goes by bone number)
        first_new = len(all_names) - len(new_part_obs)
        edit_local = [local_of(i) if i < first_new else new_locals[i - first_new] for i in range(len(all_names))]
        orig_chunks = ch.anim_model.find(MOTION)
        acts = [a for a in bpy.data.actions if a.get('dfx_armature') == arm_ob.name]
        known = sorted([a for a in acts if 'dfx_motion_index' in a], key=lambda a: a['dfx_motion_index'])
        new = sorted([a for a in acts if 'dfx_motion_index' not in a], key=lambda a: a.name)
        out_chunks = []
        for a in known + new:
            mi = a.get('dfx_motion_index')
            if mi is not None and not force and not bones_changed and a.get('dfx_hash') == common.action_signature(a):
                out_chunks.append(orig_chunks[mi])
                stats['motions_kept'] += 1
            else:
                tm = ch.motions[mi] if mi is not None else None
                out_chunks.append(encode_action(a, arm_ob, all_names, edit_local, tm))
                stats['motions_encoded'] += 1
        others = [c for c in ch.anim_model.children if c.type != MOTION]
        first = next(i for i, c in enumerate(ch.anim_model.children) if c.type == MOTION)
        ch.anim_model.children = others[:first] + out_chunks + others[first:]

    import json
    arm_ob['dfx_drawbones'] = json.dumps(drawbones)
    # draw bones added past the retail list: the game draws them unconditionally until an event
    # registers them in the visibility table, so build_objx() gives every motion a hide event for them
    arm_ob['dfx_new_drawbones'] = json.dumps([drawbones[o.name] for o in new_part_obs])
    data = ch.gmo.to_bytes()
    GmoFile.from_bytes(data)       # sanity re-parse
    with open(filepath, 'wb') as f:
        f.write(data)
    extra = ''
    if stats['bones_added'] or stats['parts_added']:
        extra = f", bones added {stats['bones_added']}, parts added {stats['parts_added']}"
    return (f"Wrote {len(data):,} bytes. Parts kept/encoded {stats['parts_kept']}/{stats['parts_encoded']}, "
            f"textures {stats['tex_kept']}/{stats['tex_encoded']}, motions {stats['motions_kept']}/{stats['motions_encoded']}{extra}")
