"""Import a Dissidia 012 character GMO into Blender.

Creates: an armature (bind pose), one mesh object per GMO Part (skinned, UVs,
vertex colours, materials with the decoded GIM textures) and one Action per
GMO Motion (named by motion ID, e.g. "_600A").

The original GMO is embedded in the .blend (Text datablock) as the export
template, together with content hashes so the exporter can keep anything
you did not touch byte-for-byte.
"""
import base64
import math
import numpy as np
import bpy
from mathutils import Matrix, Quaternion, Vector
from bpy_extras import anim_utils

from .dfx.gmo import Character, parse_bones, world_matrices
from .dfx import gim
from . import common


def _bind_worlds(ch):
    """World (bind) matrix per bone: from BlendOffsets where available, else rest chain."""
    rest = world_matrices(ch.bones)
    bind = {}
    for b in ch.bones:
        for bi, off in zip(b.blend_bones, b.blend_offsets):
            bind.setdefault(bi, np.linalg.inv(off.T))
    out = []
    for i, b in enumerate(ch.bones):
        if i in bind:
            out.append(bind[i])
        elif b.parent >= 0:
            # keep the rest local transform relative to the (possibly bind-adjusted) parent
            out.append(out[b.parent] @ b.local_matrix())
        else:
            out.append(rest[i])
    return out


def _normalised(m):
    m = np.array(m, dtype=np.float64)
    for k in range(3):
        n = np.linalg.norm(m[:3, k])
        if n > 1e-9:
            m[:3, k] /= n
    return m


def build_armature(ch, name, collection):
    arm = bpy.data.armatures.new(name)
    ob = bpy.data.objects.new(name, arm)
    collection.objects.link(ob)
    ob.rotation_euler = (math.radians(90), 0, 0)      # GMO is Y-up
    ob.show_in_front = True
    arm.display_type = 'STICK'

    worlds = [_normalised(m) for m in _bind_worlds(ch)]
    view_layer = bpy.context.view_layer
    view_layer.objects.active = ob
    bpy.ops.object.mode_set(mode='EDIT')
    ebs = []
    for i, b in enumerate(ch.bones):
        eb = arm.edit_bones.new(b.name)
        eb.length = 0.04
        eb.matrix = Matrix(worlds[i].tolist())
        ebs.append(eb)
    for i, b in enumerate(ch.bones):
        if b.parent >= 0:
            ebs[i].parent = ebs[b.parent]
    bpy.ops.object.mode_set(mode='OBJECT')
    for pb in ob.pose.bones:
        pb.rotation_mode = 'QUATERNION'
    from .exporter import distance_of
    from .dfx.chunks import BONE
    for bc in ch.mesh_model.find(BONE):          # spring-bone length (BlindData DISTANCE): kept as a bone property
        d = distance_of(bc)
        if d is not None and bc.name in arm.bones:
            arm.bones[bc.name]['dfx_distance'] = d
    return ob, worlds


def _rebind(positions, weights, bone_ids, owner, mesh_subset, worlds):
    """Map part-space vertices into the armature's bind space.

    GE skinning: v_world = sum_i w_i * BoneWorld_i @ O_i @ v  (O_i = this part's BlendOffset^T)
    Blender:     v_world = sum_i w_i * BoneWorld_i @ inv(EditWorld_i) @ v_b
    => v_b = sum_i w_i * EditWorld_i @ O_i @ v   (exact for single-bone vertices)
    """
    if owner is None or not owner.blend_offsets:
        return positions
    hom = np.c_[positions, np.ones(len(positions))]
    out = np.zeros_like(hom)
    if weights is None or not mesh_subset:
        return positions
    tot = np.zeros(len(positions))
    for j, s in enumerate(mesh_subset[:weights.shape[1]]):
        bid = owner.blend_bones[s]
        M = worlds[bid] @ owner.blend_offsets[s].T
        wj = weights[:, j]
        out += wj[:, None] * (hom @ M.T)
        tot += wj
    tot[tot == 0] = 1
    return (out[:, :3] / tot[:, None])


def build_meshes(ch, arm_ob, collection, images, worlds):
    objs = []
    mats = []
    for mi, m in enumerate(ch.materials):
        mat = common.make_material(f'{arm_ob.name}_{m.name}', images.get(m.textures[0]) if m.textures else None)
        mat['dfx_material_index'] = mi
        mats.append(mat)

    for pi, part in enumerate(ch.parts):
        owner_i = ch.part_owner(pi)
        owner = ch.bones[owner_i] if owner_i >= 0 else None
        verts, uvs, cols, faces, face_mat = [], [], [], [], []
        vgroups = {}           # bone name -> list of (vertex, weight)
        base_for_array = {}
        for mi_local, mesh in enumerate(part.meshes):
            if owner is not None and owner.blend_bones and not mesh.blend_subset:
                mesh.blend_subset = list(range(len(owner.blend_bones)))   # implicit: whole palette
            bone_ids = [owner.blend_bones[s] for s in mesh.blend_subset] if owner and mesh.blend_subset else []
            for dc in mesh.draws:
                ai = dc.array_ref & 0xFFF
                arr = part.arrays[ai]
                if ai not in base_for_array:
                    base_for_array[ai] = len(verts)
                    p = arr.positions_norm() * ch.pos_scale + ch.pos_offset
                    t = arr.uvs_norm()
                    t = t * ch.uv_scale + ch.uv_offset if t is not None else np.zeros((arr.count, 2))
                    c = arr.colors()
                    c = c if c is not None else np.ones((arr.count, 4))
                    w = arr.weights()
                    p = _rebind(p, w, bone_ids, owner, mesh.blend_subset, worlds)
                    verts += p.tolist(); uvs += t.tolist(); cols += c.tolist()
                    base = base_for_array[ai]
                    if w is not None and bone_ids:
                        for j, bid in enumerate(bone_ids[:w.shape[1]]):
                            name = ch.bones[bid].name
                            lst = vgroups.setdefault(name, [])
                            for v in np.nonzero(w[:, j] > 0)[0]:
                                lst.append((base + int(v), float(w[v, j])))
                    elif owner is not None:
                        lst = vgroups.setdefault(owner.name, [])
                        lst += [(base + v, 1.0) for v in range(arr.count)]
                base = base_for_array[ai]
                for tri in dc.triangles():
                    faces.append(tuple(base + k for k in tri))
                    face_mat.append(mesh.material)

        me = bpy.data.meshes.new(part.name)
        me.from_pydata(verts, [], faces)
        me.validate(clean_customdata=False)
        uv_layer = me.uv_layers.new(name='UVMap')
        loops_v = np.zeros(len(me.loops), np.int64)
        me.loops.foreach_get('vertex_index', loops_v)
        uvs_np = np.array(uvs)
        luv = uvs_np[loops_v]
        luv[:, 1] = 1.0 - luv[:, 1]
        uv_layer.data.foreach_set('uv', luv.reshape(-1))
        col = me.color_attributes.new('Col', 'FLOAT_COLOR', 'POINT')
        col.data.foreach_set('color', np.array(cols, np.float32).reshape(-1))
        used = sorted(set(face_mat))
        slot_of = {}
        for gm in used:
            if 0 <= gm < len(mats):
                slot_of[gm] = len(me.materials)
                me.materials.append(mats[gm])
        me.polygons.foreach_set('material_index', [slot_of.get(m, 0) for m in face_mat[:len(me.polygons)]])
        me.update()

        ob = bpy.data.objects.new(part.name, me)
        collection.objects.link(ob)
        ob.parent = arm_ob
        ob['dfx_part_index'] = pi
        ob['dfx_draw_bone'] = ch.part_owner(pi)
        for bname, lst in vgroups.items():
            vg = ob.vertex_groups.new(name=bname)
            by_w = {}
            for v, wv in lst:
                by_w.setdefault(round(wv, 6), []).append(v)
            for wv, vs in by_w.items():
                vg.add(vs, wv, 'REPLACE')
        mod = ob.modifiers.new('Armature', 'ARMATURE')
        mod.object = arm_ob
        ob['dfx_hash'] = common.mesh_signature(ob)
        objs.append(ob)
    return objs


def actions_from_motions(motions, anim_bones, parents, names, worlds, arm_ob, index_base=0, replace=False):
    """Create one Blender Action per GMO Motion. `worlds` are the armature's edit (bind) world
    matrices in GMO space, indexed like `names`/`parents`. With replace=True an existing
    action of the same name (for this armature) is overwritten and keeps its motion index."""
    edit_local = [np.linalg.inv(worlds[p]) @ worlds[i] if p >= 0 else worlds[i] for i, p in enumerate(parents)]
    inv_edit_local = [np.linalg.inv(m) for m in edit_local]
    actions = []
    for k, mot in enumerate(motions):
        # GMO FrameLoop (0x80B1) is {loop start, end}: the game plays a motion from frame 0 and at the end jumps back
        # to the loop start (EBOOT FUN_089B1994; FrameRepeat 0x80B4 defaults to 1 = loop). The frames before the loop
        # start belong to the motion (the dash's start-up, the jump's take-off), so the action covers 0..end and keeps
        # the loop start for the exporter ('dfx_loop_start'). Sampling from the loop start dropped them.
        loop0 = float(mot.frame_start)
        f0, f1 = (0 if loop0 > 0 else int(math.floor(loop0))), int(math.ceil(mot.frame_end))
        frames = np.arange(f0, f1 + 1, dtype=np.float64)
        chans = {}
        for bnd in mot.bindings:
            fc = mot.fcurves[bnd.fcurve]
            chans.setdefault(bnd.bone, {})[bnd.command] = fc.evaluate(frames)
        idx = index_base + k
        old = bpy.data.actions.get(mot.name)
        if replace and old is not None and old.get('dfx_armature') == arm_ob.name:
            idx = old.get('dfx_motion_index', idx)
            old.name = mot.name + '.old'
            old.use_fake_user = False
            bpy.data.actions.remove(old)
        act = bpy.data.actions.new(mot.name)
        act.use_fake_user = True
        act.use_frame_range = True
        act.frame_start, act.frame_end = (float(f0) if loop0 > 0 else mot.frame_start), mot.frame_end
        if loop0 > 0:
            act['dfx_loop_start'] = loop0
        act['dfx_motion_index'] = idx
        act['dfx_frame_rate'] = mot.frame_rate
        act['dfx_armature'] = arm_ob.name
        slot = act.slots.new(id_type='OBJECT', name=arm_ob.name)
        cb = anim_utils.action_ensure_channelbag_for_slot(act, slot)
        for bi, ch_ in chans.items():
            if bi >= len(names) or bi >= len(anim_bones):
                continue
            ab = anim_bones[bi]
            T = ch_.get(0x48, np.tile(ab.translate, (len(frames), 1)))
            R = ch_.get(0x4B, np.tile(ab.rotate, (len(frames), 1)))
            S = ch_.get(0x4D, np.tile(ab.scale, (len(frames), 1)))
            loc, rot, scl = common.local_trs_to_basis(T, R, S, inv_edit_local[bi])
            common.write_bone_fcurves(cb, names[bi], frames, loc, rot, scl)
        actions.append(act)
    return actions


def build_actions(ch, arm_ob, worlds, fps_scene=True):
    anim_bones = parse_bones(ch.anim_model) if ch.anim_model else ch.bones
    actions = actions_from_motions(ch.motions, anim_bones, [b.parent for b in ch.bones],
                                   [b.name for b in ch.bones], worlds, arm_ob)
    for act in actions:
        act['dfx_hash'] = common.action_signature(act)
    if actions:
        arm_ob.animation_data_create()
        arm_ob.animation_data.action = actions[0]
        arm_ob.animation_data.action_slot = actions[0].slots[0]
    return actions


def import_gmo(filepath, import_anims=True):
    with open(filepath, 'rb') as f:
        data = f.read()
    ch = Character.load(data)
    name = bpy.path.display_name_from_filepath(filepath)
    coll = bpy.data.collections.new(name)
    bpy.context.scene.collection.children.link(coll)
    bpy.context.scene.render.fps = 30

    images = {}
    for ti, t in enumerate(ch.textures):
        if not t.gim:            # filename-only reference (image supplied by another file)
            continue
        rgba = gim.decode(t.gim)
        img = common.image_from_rgba(f'{name}_{t.filename or t.name}', rgba)
        img['dfx_texture_index'] = ti
        img['dfx_hash'] = common.image_signature(img)
        images[ti] = img

    import time
    t0 = time.time()
    arm_ob, worlds = build_armature(ch, name, coll)
    t1 = time.time()
    build_meshes(ch, arm_ob, coll, images, worlds)
    t2 = time.time()
    if import_anims:
        build_actions(ch, arm_ob, worlds)
    print(f'[dfx] armature {t1-t0:.1f}s meshes {t2-t1:.1f}s actions {time.time()-t2:.1f}s')

    arm_ob['dfx_template'] = data          # original GMO bytes = export template
    arm_ob['dfx_source'] = filepath
    # a moveset next to the model (p_xxx_1p.gmo -> p_xxx.objx) is loaded automatically
    import os, re
    base = re.sub(r'_(\d+p|zako)(_a)?$', '', name)
    cand = os.path.join(os.path.dirname(filepath), base + '.objx')
    if os.path.exists(cand):
        try:
            from . import moveset_bl
            moveset_bl.attach_moveset(arm_ob, cand)
        except Exception as e:
            print('[dfx] could not load moveset', cand, e)
    try:
        from . import rig_display
        rig_display.tidy_rig(arm_ob)
    except Exception as e:
        print('[dfx] tidy bones skipped:', e)
    return arm_ob
