"""Shared helpers for the Dissidia Blender add-on."""
import hashlib
import numpy as np
import bpy


def make_material(name, image):
    mat = bpy.data.materials.new(name)
    try:
        mat.use_nodes = True
    except Exception:
        pass
    nt = mat.node_tree
    bsdf = next((n for n in nt.nodes if n.type == 'BSDF_PRINCIPLED'), None)
    if bsdf is None:
        bsdf = nt.nodes.new('ShaderNodeBsdfPrincipled')
    attr = nt.nodes.new('ShaderNodeVertexColor')
    attr.layer_name = 'Col'
    attr.location = (-600, -200)
    mix = nt.nodes.new('ShaderNodeMix')
    mix.data_type = 'RGBA'
    mix.blend_type = 'MULTIPLY'
    mix.inputs['Factor'].default_value = 1.0
    mix.location = (-300, 100)
    if image is not None:
        tex = nt.nodes.new('ShaderNodeTexImage')
        tex.image = image
        tex.interpolation = 'Linear'
        tex.location = (-600, 200)
        nt.links.new(tex.outputs['Color'], mix.inputs['A'])
        nt.links.new(tex.outputs['Alpha'], bsdf.inputs['Alpha'])
    nt.links.new(attr.outputs['Color'], mix.inputs['B'])
    nt.links.new(mix.outputs['Result'], bsdf.inputs['Base Color'])
    try:
        mat.surface_render_method = 'DITHERED'
    except Exception:
        pass
    return mat


def image_from_rgba(name, rgba):
    h, w = rgba.shape[:2]
    img = bpy.data.images.new(name, w, h, alpha=True)
    px = (np.flipud(rgba).astype(np.float32) / 255.0).reshape(-1)
    img.pixels.foreach_set(px)
    img.pack()
    img.use_fake_user = True      # keep texture-animation sheets that no material uses
    return img


def image_rgba(img):
    w, h = img.size
    px = np.empty(w * h * 4, np.float32)
    img.pixels.foreach_get(px)
    return np.flipud((px.reshape(h, w, 4) * 255 + 0.5).clip(0, 255).astype(np.uint8))


def image_signature(img):
    return hashlib.sha1(image_rgba(img).tobytes()).hexdigest()


def mesh_signature(ob):
    me = ob.data
    n = len(me.vertices)
    co = np.empty(n * 3, np.float32); me.vertices.foreach_get('co', co)
    lv = np.empty(len(me.loops), np.int32); me.loops.foreach_get('vertex_index', lv)
    mi = np.empty(len(me.polygons), np.int32); me.polygons.foreach_get('material_index', mi)
    h = hashlib.sha1()
    h.update(np.round(co, 5).tobytes()); h.update(lv.tobytes()); h.update(mi.tobytes())
    if me.uv_layers:
        uv = np.empty(len(me.loops) * 2, np.float32); me.uv_layers[0].data.foreach_get('uv', uv)
        h.update(np.round(uv, 5).tobytes())
    for vg in ob.vertex_groups:
        h.update(vg.name.encode())
    for v in me.vertices:
        for g in v.groups:
            h.update(np.array([v.index, g.group, round(g.weight, 4)], np.float32).tobytes())
    return h.hexdigest()


def action_signature(act):
    h = hashlib.sha1()
    for slot in act.slots:
        from bpy_extras import anim_utils
        cb = anim_utils.action_get_channelbag_for_slot(act, slot)
        if cb is None:
            continue
        for fc in sorted(cb.fcurves, key=lambda f: (f.data_path, f.array_index)):
            h.update(fc.data_path.encode()); h.update(bytes([fc.array_index]))
            co = np.empty(len(fc.keyframe_points) * 2, np.float32)
            fc.keyframe_points.foreach_get('co', co)
            h.update(np.round(co, 5).tobytes())
    h.update(np.array([act.frame_start, act.frame_end], np.float32).tobytes())
    return h.hexdigest()


# ---------------------------------------------------------------- transforms
def quat_to_mat(q):
    x, y, z, w = q[..., 0], q[..., 1], q[..., 2], q[..., 3]
    m = np.empty(q.shape[:-1] + (3, 3))
    m[..., 0, 0] = 1 - 2 * (y * y + z * z); m[..., 0, 1] = 2 * (x * y - z * w); m[..., 0, 2] = 2 * (x * z + y * w)
    m[..., 1, 0] = 2 * (x * y + z * w); m[..., 1, 1] = 1 - 2 * (x * x + z * z); m[..., 1, 2] = 2 * (y * z - x * w)
    m[..., 2, 0] = 2 * (x * z - y * w); m[..., 2, 1] = 2 * (y * z + x * w); m[..., 2, 2] = 1 - 2 * (x * x + y * y)
    return m


def mat_to_quat(m):
    """Rotation matrices (...,3,3) -> quaternions (...,4) xyzw."""
    m = np.asarray(m, np.float64)
    shp = m.shape[:-2]
    a = m.reshape(-1, 3, 3)
    n = len(a)
    q = np.empty((n, 4))
    m00, m11, m22 = a[:, 0, 0], a[:, 1, 1], a[:, 2, 2]
    tr = m00 + m11 + m22
    c0 = tr > 0
    c1 = ~c0 & (m00 > m11) & (m00 > m22)
    c2 = ~c0 & ~c1 & (m11 > m22)
    c3 = ~c0 & ~c1 & ~c2
    s = np.sqrt(np.maximum(tr + 1.0, 1e-12)) * 2
    q[c0] = np.stack([(a[:, 2, 1] - a[:, 1, 2]) / s, (a[:, 0, 2] - a[:, 2, 0]) / s, (a[:, 1, 0] - a[:, 0, 1]) / s, 0.25 * s], 1)[c0]
    s = np.sqrt(np.maximum(1.0 + m00 - m11 - m22, 1e-12)) * 2
    q[c1] = np.stack([0.25 * s, (a[:, 0, 1] + a[:, 1, 0]) / s, (a[:, 0, 2] + a[:, 2, 0]) / s, (a[:, 2, 1] - a[:, 1, 2]) / s], 1)[c1]
    s = np.sqrt(np.maximum(1.0 + m11 - m00 - m22, 1e-12)) * 2
    q[c2] = np.stack([(a[:, 0, 1] + a[:, 1, 0]) / s, 0.25 * s, (a[:, 1, 2] + a[:, 2, 1]) / s, (a[:, 0, 2] - a[:, 2, 0]) / s], 1)[c2]
    s = np.sqrt(np.maximum(1.0 + m22 - m00 - m11, 1e-12)) * 2
    q[c3] = np.stack([(a[:, 0, 2] + a[:, 2, 0]) / s, (a[:, 1, 2] + a[:, 2, 1]) / s, 0.25 * s, (a[:, 1, 0] - a[:, 0, 1]) / s], 1)[c3]
    q /= np.linalg.norm(q, axis=-1, keepdims=True)
    return q.reshape(shp + (4,))


def compose(T, R, S):
    n = len(T)
    M = np.zeros((n, 4, 4)); M[:, 3, 3] = 1
    M[:, :3, :3] = quat_to_mat(np.asarray(R, np.float64)) * np.asarray(S)[:, None, :]
    M[:, :3, 3] = T
    return M


def decompose(M):
    T = M[:, :3, 3].copy()
    S = np.linalg.norm(M[:, :3, :3], axis=1)
    Rm = M[:, :3, :3] / np.where(S[:, None, :] == 0, 1, S[:, None, :])
    R = mat_to_quat(Rm)
    return T, R, S


def continuous(q):
    q = q.copy()
    for i in range(1, len(q)):
        if (q[i] * q[i - 1]).sum() < 0:
            q[i] = -q[i]
    return q


def _zero_scale(S):
    """Rows scaled to nothing (hidden bones): convert them as if unscaled, then zero the scale again,
    so their rotation survives the round trip (a zero-scale matrix can't be decomposed)."""
    S = np.asarray(S, np.float64)
    z = np.all(np.abs(S) < 1e-9, axis=1)
    return z, np.where(z[:, None], 1.0, S)


def local_trs_to_basis(T, R, S, inv_edit_local):
    z, S1 = _zero_scale(S)
    M = inv_edit_local[None] @ compose(T, R, S1)
    loc, rot, scl = decompose(M)
    scl[z] = 0.0
    return loc, continuous(rot), scl


def basis_to_local_trs(loc, rot_wxyz, scl, edit_local):
    rot = np.asarray(rot_wxyz)[:, [1, 2, 3, 0]]
    z, S1 = _zero_scale(scl)
    M = edit_local[None] @ compose(loc, rot, S1)
    T, R, S = decompose(M)
    S[z] = 0.0
    return T, continuous(R), S


def write_bone_fcurves(cb, bone, frames, loc, rot_xyzw, scl):
    base = f'pose.bones["{bone}"]'
    rot_wxyz = rot_xyzw[:, [3, 0, 1, 2]]
    for path, data in (('location', loc), ('rotation_quaternion', rot_wxyz), ('scale', scl)):
        for k in range(data.shape[1]):
            vals = data[:, k]
            if np.allclose(vals, vals[0], atol=1e-6):
                fr, vv = frames[:1], vals[:1]
            else:
                fr, vv = frames, vals
            fc = cb.fcurves.new(f'{base}.{path}', index=k, group_name=bone)
            fc.keyframe_points.add(len(fr))
            co = np.empty(len(fr) * 2, np.float32)
            co[0::2] = fr; co[1::2] = vv
            fc.keyframe_points.foreach_set('co', co)
            fc.keyframe_points.foreach_set('interpolation', [1] * len(fr))   # LINEAR
            fc.update()
