"""Blender round trip of Tools > Retarget from FBX / glTF.

Exports a Bartz action to FBX with Blender's own exporter, re-imports it through the new tool (Bartz -> Bartz,
all names identical) and compares the resulting pose with the original, bone by bone, frame by frame.
Also runs auto_map on the sample FBX's bone names.

Run:  python3 tests/test_retarget_blender.py [action ...]      (uses the bpy module)
"""
import math
import os
import sys
import tempfile
import numpy as np
import bpy

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, '..'))
sys.path.insert(0, ROOT)
import io_scene_dissidia
from io_scene_dissidia import tools_bl, importer
from io_scene_dissidia.dfx import bonemap
from io_scene_dissidia.dfx.gmo import Character

BLEND = os.path.join(ROOT, 'bartz_noctis_moves.blend')
SAMPLE = os.path.join(ROOT, 'FF15ncts_00.fbx')


def pose_matrices(arm, act, frames, names, offset=0):
    ad = arm.animation_data_create()
    ad.action = act
    ad.action_slot = act.slots[0]
    out = np.empty((len(frames), len(names), 4, 4))
    for k, f in enumerate(frames):
        bpy.context.scene.frame_set(int(f) + offset)
        ev = arm.evaluated_get(bpy.context.evaluated_depsgraph_get())
        for j, n in enumerate(names):
            out[k, j] = np.array(ev.pose.bones[n].matrix)
    return out


def compare(A, B, names, hidden):
    """A, B: (nf, nb, 4, 4) armature-space pose.  -> per-bone max position error (m) and rotation error (deg)"""
    dp = np.linalg.norm(A[..., :3, 3] - B[..., :3, 3], axis=-1)
    Ra, Rb = A[..., :3, :3], B[..., :3, :3]
    na, nb = np.linalg.norm(Ra, axis=-2, keepdims=True), np.linalg.norm(Rb, axis=-2, keepdims=True)
    Ra, Rb = Ra / np.where(na == 0, 1, na), Rb / np.where(nb == 0, 1, nb)
    c = (np.trace(Ra @ np.swapaxes(Rb, -1, -2), axis1=-2, axis2=-1) - 1) / 2
    dr = np.degrees(np.arccos(np.clip(c, -1, 1)))
    keep = [i for i, n in enumerate(names) if n not in hidden]
    return dp[:, keep].max(0), dr[:, keep].max(0), [names[i] for i in keep]


def rest_prefixed_copy(arm, act, f0, f1):
    """Blender's FBX exporter writes the pose at the first exported frame as the bones' rest pose, so export a copy of
    the action that starts with one frame of the bind pose (frame 0) and has the real animation on frames 1..n+1."""
    from bpy_extras import anim_utils
    tmp = bpy.data.actions.new(act.name + '_rt_src')
    slot = tmp.slots.new(id_type='OBJECT', name=arm.name)
    cb = anim_utils.action_ensure_channelbag_for_slot(tmp, slot)
    src_cb = anim_utils.action_get_channelbag_for_slot(act, act.slots[0])
    frames = np.arange(f0, f1 + 1)
    for fc in src_cb.fcurves:
        rest = 1.0 if (fc.data_path.endswith('rotation_quaternion') and fc.array_index == 0) or fc.data_path.endswith('scale') else 0.0
        vals = np.r_[rest, [fc.evaluate(f) for f in frames]]
        nfc = cb.fcurves.new(fc.data_path, index=fc.array_index, group_name=fc.group.name if fc.group else '')
        nfc.keyframe_points.add(len(vals))
        co = np.empty(len(vals) * 2, np.float32)
        co[0::2] = np.arange(len(vals)); co[1::2] = vals
        nfc.keyframe_points.foreach_set('co', co)
        nfc.keyframe_points.foreach_set('interpolation', [1] * len(vals))
        nfc.update()
    return tmp


def roundtrip(arm, action, opts, tmpdir, label, check=True):
    act = bpy.data.actions[action]
    sc = bpy.context.scene
    names = [b.name for b in Character.load(bytes(arm['dfx_template'])).bones]
    f0, f1 = int(act.frame_range[0]), int(act.frame_range[1])
    n = f1 - f0 + 1
    tmp_act = rest_prefixed_copy(arm, act, f0, f1)
    sc.frame_start, sc.frame_end = 0, n
    for o in bpy.context.scene.objects:
        o.select_set(False)
    arm.select_set(True)
    bpy.context.view_layer.objects.active = arm
    arm.animation_data_create().action = tmp_act
    arm.animation_data.action_slot = tmp_act.slots[0]
    sc.frame_set(0)
    fbx = os.path.join(tmpdir, f'{action}.fbx')
    bpy.ops.export_scene.fbx(filepath=fbx, use_selection=True, object_types={'ARMATURE'}, add_leaf_bones=False,
                             bake_anim=True, bake_anim_use_all_actions=False, bake_anim_use_nla_strips=False,
                             bake_anim_simplify_factor=0.0, bake_anim_force_startend_keying=True)
    arm.animation_data.action = act
    bpy.data.actions.remove(tmp_act)
    frames = np.arange(f0, f1 + 1)
    orig = pose_matrices(arm, act, frames, names)
    # bones the animation hides (scale 0) have no meaningful pose
    hidden = {n_ for j, n_ in enumerate(names) if np.linalg.norm(orig[:, j, :3, :3], axis=-2).min() < 1e-4}
    res = tools_bl.retarget_from_file(bpy.context, arm, fbx, name=f'rt_{action}', base_action=action, review=False,
                                      frame_start=1, frame_end=n, log=lambda *a: None, **opts)
    new = bpy.data.actions[res['actions'][0]]
    got = pose_matrices(arm, new, np.arange(len(frames)), names)
    dp, dr, kept = compare(orig, got, names, hidden)
    worst = np.argsort(-dp)[:3]
    print(f'  {label:22s} {action}: {len(frames)} frames, {len(kept)} bones ({len(hidden)} hidden skipped) | '
          f'position max {dp.max()*100:.3f} cm mean {dp.mean()*100:.3f} cm | rotation max {dr.max():.3f} deg mean {dr.mean():.3f} deg'
          f' | worst: ' + ', '.join(f'{kept[i]} {dp[i]*100:.2f} cm' for i in worst))
    if check:
        assert dp.max() < 0.01, f'position error {dp.max()}'
        assert dr.max() < 1.0, f'rotation error {dr.max()}'
    return res


def build_animated_sample(tmpdir):
    """Import the sample Noctis FBX (T-pose, Mixamo names), key three poses (T-pose, a squat with the arms
    hanging, arms forward with a leaning spine and turned head) at 24 fps frames 0/12/24, export to FBX.
    Returns (fbx path, expected world joint positions per key frame in the source's world space)."""
    from mathutils import Matrix, Vector
    bpy.ops.wm.read_factory_settings(use_empty=True)
    bpy.ops.import_scene.fbx(filepath=SAMPLE)
    src = next(o for o in bpy.data.objects if o.type == 'ARMATURE')
    sc = bpy.context.scene
    sc.render.fps = 24
    bpy.context.view_layer.objects.active = src
    pbs = src.pose.bones
    for pb in pbs:
        pb.rotation_mode = 'QUATERNION'

    def swing(name, axis, deg):
        pb = pbs[name]
        M = pb.matrix.copy()
        h = M.translation.copy()
        R = Matrix.Rotation(math.radians(deg), 4, axis)
        pb.matrix = Matrix.Translation(h) @ R @ Matrix.Translation(-h) @ M
        bpy.context.view_layer.update()

    def key(f):
        for pb in pbs:
            pb.keyframe_insert('location', frame=f)
            pb.keyframe_insert('rotation_quaternion', frame=f)

    def reset():
        for pb in pbs:
            pb.location = (0, 0, 0)
            pb.rotation_quaternion = (1, 0, 0, 0)
        bpy.context.view_layer.update()

    snap = {}

    def record(f):
        bpy.context.view_layer.update()
        snap[f] = {pb.name: np.array(src.matrix_world @ pb.matrix) for pb in pbs}

    reset(); key(0); record(0)
    # pose 1: arms hanging down 70 degrees, squat (thighs forward, shins back), head turned
    reset()
    swing('LeftArm', 'Y', 70); swing('RightArm', 'Y', -70); swing('LeftForeArm', 'Z', -35)
    swing('LeftUpLeg', 'X', 50); swing('RightUpLeg', 'X', 35); swing('LeftLeg', 'X', -70); swing('RightLeg', 'X', -50)
    pbs['Hips'].matrix = Matrix.Translation((0.05, -0.30, -0.18)) @ pbs['Hips'].matrix
    bpy.context.view_layer.update()
    key(12); record(12)
    # pose 2: arms forward, spine leaning, head turned, feet apart
    reset()
    swing('LeftArm', 'Z', -90); swing('RightArm', 'Z', 90); swing('RightForeArm', 'Z', 40)
    swing('Spine', 'X', 30); swing('Head', 'Z', 40); swing('RightUpLeg', 'Y', -25)
    pbs['Hips'].matrix = Matrix.Translation((-0.1, -0.5, 0.02)) @ pbs['Hips'].matrix
    bpy.context.view_layer.update()
    key(24); record(24)
    for fc in src.animation_data.action.fcurves if hasattr(src.animation_data.action, 'fcurves') else []:
        for kp in fc.keyframe_points:
            kp.interpolation = 'LINEAR'
    sc.frame_start, sc.frame_end = 0, 24
    for o in sc.objects:
        o.select_set(o == src)
    fbx = os.path.join(tmpdir, 'noctis_anim.fbx')
    bpy.ops.export_scene.fbx(filepath=fbx, use_selection=True, object_types={'ARMATURE'}, add_leaf_bones=False,
                             bake_anim=True, bake_anim_use_all_actions=False, bake_anim_use_nla_strips=False, bake_anim_simplify_factor=0.0,
                             bake_anim_force_startend_keying=True)
    return fbx, snap


def operator_test(arm, fbx, tmpdir):
    """The panel operator end to end: bone map JSON written and re-read, unsure maps stop, add-on unregisters."""
    import shutil
    path = os.path.join(tmpdir, 'op_test.fbx')
    shutil.copy(fbx, path)
    for o in bpy.context.scene.objects:
        o.select_set(False)
    arm.select_set(True)
    bpy.context.view_layer.objects.active = arm
    r = bpy.ops.dfx.retarget_fbx(filepath=path, clip_name='op_noctis', base_action='_000A')
    mp = path + '.bonemap.json'
    assert r == {'FINISHED'} and os.path.exists(mp) and 'op_noctis' in bpy.data.actions
    print(f'\noperator: created {mp} and action op_noctis ({int(bpy.data.actions["op_noctis"].frame_range[1])} frames); '
          f'active action {arm.animation_data.action.name}')
    # edit the JSON: unmap the left hand; the next run must respect it
    import json
    d = json.load(open(mp))
    d['slots']['l_hand'] = None
    json.dump(d, open(mp, 'w'))
    r = bpy.ops.dfx.retarget_fbx(filepath=path, clip_name='op_noctis', base_action='_000A')
    from bpy_extras import anim_utils
    a = bpy.data.actions['op_noctis']
    cb = anim_utils.action_get_channelbag_for_slot(a, a.slots[0])
    n_keys = {fc.data_path for fc in cb.fcurves if fc.data_path.startswith('pose.bones["l_hand"].rotation') and len(fc.keyframe_points) > 1}
    assert not n_keys, 'unmapped l_hand should be held, not animated'
    print('operator: edited bone map respected (l_hand unmapped -> held pose)')
    # the new action exports to GMO like any other: appended as a new motion
    from io_scene_dissidia import exporter
    gmo = os.path.join(tmpdir, 'out.gmo')
    rep = exporter.export_gmo(arm, gmo, False)
    ch2 = Character.load(open(gmo, 'rb').read())
    mm = [m for m in ch2.motions if m.name == 'op_noctis']
    assert len(mm) == 1 and len(mm[0].bindings) > 100
    print(f'export: {rep}; motion op_noctis is in the GMO ({len(ch2.motions)} motions, {mm[0].frame_start:g}-{mm[0].frame_end:g}, '
          f'{len(mm[0].bindings)} bindings, {len(mm[0].fcurves)} curves)')
    # an unrecognisable skeleton: the guessed map is written and the run stops before creating anything
    new = tools_bl._import_foreign(fbx)
    src = next(o for o in new['objects'] if o.type == 'ARMATURE')
    for i, b in enumerate(src.data.bones):
        b.name = f'node_{i:02d}'
    for o in bpy.context.scene.objects:
        o.select_set(o == src)
    odd = os.path.join(tmpdir, 'odd.fbx')
    bpy.ops.export_scene.fbx(filepath=odd, use_selection=True, object_types={'ARMATURE'}, add_leaf_bones=False,
                             bake_anim=True, bake_anim_use_all_actions=False, bake_anim_use_nla_strips=False)
    tools_bl._remove_imported(new)
    arm.select_set(True)
    bpy.context.view_layer.objects.active = arm
    n_act, n_obj = len(bpy.data.actions), len(bpy.data.objects)
    res = tools_bl.retarget_from_file(bpy.context, arm, odd, name='odd', review=True, log=lambda *a: None)
    assert res['stopped'] and not res['actions'] and os.path.exists(odd + '.bonemap.json')
    assert len(bpy.data.actions) == n_act and len(bpy.data.objects) == n_obj, 'stopped run left data behind'
    print(f'operator: unrecognisable skeleton stops the run (confidence {res["bonemap"].confidence:.0%}, '
          f'{len(res["bonemap"].missing_required)} required slots missing), map written, nothing left behind')
    io_scene_dissidia.unregister(); io_scene_dissidia.register()
    print('operator: unregister / register ok')


def cross_test(tmpdir):
    fbx, snap = build_animated_sample(tmpdir)
    bpy.ops.wm.open_mainfile(filepath=BLEND)
    arm = next(o for o in bpy.data.objects if o.type == 'ARMATURE' and 'dfx_template' in o)
    names = [b.name for b in Character.load(bytes(arm['dfx_template'])).bones]
    X = np.array(arm.matrix_world.inverted())
    bartz = bpy.data.actions['_000A']
    print('\ncross-skeleton: Noctis sample (T-pose, Mixamo names, Z up, 24 fps) -> Bartz, base pose from _000A')
    chain = [('LeftArm', 'LeftForeArm', 'l_uparm', 'l_foarm'), ('LeftForeArm', 'LeftHand', 'l_foarm', 'l_hand'),
             ('RightArm', 'RightForeArm', 'r_uparm', 'r_foarm'), ('RightForeArm', 'RightHand', 'r_foarm', 'r_hand'),
             ('LeftUpLeg', 'LeftLeg', 'l_femur', 'l_tibia'), ('LeftLeg', 'LeftFoot', 'l_tibia', 'l_foot'),
             ('RightUpLeg', 'RightLeg', 'r_femur', 'r_tibia'), ('RightLeg', 'RightFoot', 'r_tibia', 'r_foot'),
             ('Chest', 'Neck', 'chest', 'neck'), ('Neck', 'Head', 'neck', 'head'), ('Spine', 'Chest', 'waist', 'chest')]
    out = {}
    for label, opts in (('rest alignment on', {}), ('rest alignment off', dict(rest_align=False))):
        res = tools_bl.retarget_from_file(bpy.context, arm, fbx, name='noctis_test', base_action='_000A', review=False,
                                          log=lambda *a: None, start_at_origin=False, **opts)
        act = bpy.data.actions[res['actions'][0]]
        nframes = int(act.frame_range[1]) + 1
        got = pose_matrices(arm, act, [0, 15, 30], names)
        errs = []
        for k, f in enumerate((0, 12, 24)):
            for sa, sb, da, db in chain:
                u = X[:3, :3] @ (snap[f][sb][:3, 3] - snap[f][sa][:3, 3])
                v = got[k, names.index(db), :3, 3] - got[k, names.index(da), :3, 3]
                c = np.dot(u, v) / (np.linalg.norm(u) * np.linalg.norm(v))
                errs.append((np.degrees(np.arccos(np.clip(c, -1, 1))), f, sa))
        e = np.array([x[0] for x in errs])
        worst = max(errs)
        print(f'  {label:20s}: {nframes} frames; limb-direction error over {len(e)} limbs x 3 poses: max {e.max():.2f} deg '
              f'mean {e.mean():.2f} deg (worst {worst[2]} at src frame {worst[1]})')
        # hips travel: Bartz root vs source hips, scaled by the hip-height ratio
        if not opts:
            hs = np.array([X[:3, :3] @ (snap[f]['Hips'][:3, 3] - snap[0]['Hips'][:3, 3]) for f in (0, 12, 24)])
            hd = np.array([got[k, names.index('hip'), :3, 3] - got[0, names.index('hip'), :3, 3] for k in range(3)])
            ratio = np.linalg.norm(hd[2]) / np.linalg.norm(hs[2])
            print(f'    hips travel at the last pose: source {np.round(hs[2], 3).tolist()} m, Bartz {np.round(hd[2], 3).tolist()} m '
                  f'(ratio {ratio:.3f}); rest hip heights: source 0.665 m, Bartz {arm.data.bones["hip"].head_local[1]:.3f} m')
            assert e.max() < 1.0, 'limb directions drifted'
        out[label] = e.max()
    assert out['rest alignment off'] > 20, 'without alignment the T-pose / A-pose mismatch should show'
    operator_test(arm, fbx, tmpdir)


def main():
    bpy.ops.wm.open_mainfile(filepath=BLEND)
    io_scene_dissidia.register()
    arm = next(o for o in bpy.data.objects if o.type == 'ARMATURE' and 'dfx_template' in o)
    actions = sys.argv[1:] or ['_632A', '_000A']
    tmp = tempfile.mkdtemp()
    print('round trip Bartz -> FBX -> Bartz')
    n_obj = len(bpy.data.objects)
    for a in actions:
        for label, opts, check in (('default', dict(start_at_origin=False), True),
                                   ('foot fix off', dict(start_at_origin=False, foot_fix=False), True),
                                   ('no rest alignment', dict(start_at_origin=False, rest_align=False), True),
                                   ('start at origin', dict(start_at_origin=True), False)):
            roundtrip(arm, a, opts, tmp, label, check)
    assert len(bpy.data.objects) == n_obj, 'imported objects were not removed'
    new = [a for a in bpy.data.actions if a.get('dfx_retarget')]
    assert all('dfx_motion_index' not in a and a['dfx_armature'] == arm.name for a in new)

    cross_test(tmp)

    print('\nauto_map on the sample FBX')
    bpy.ops.wm.read_factory_settings(use_empty=True)
    new = tools_bl._import_foreign(SAMPLE)
    src = next(o for o in new['objects'] if o.type == 'ARMATURE')
    sn, sp = tools_bl.foreign_bone_names(src)
    bpy.ops.wm.open_mainfile(filepath=BLEND)
    arm = next(o for o in bpy.data.objects if o.type == 'ARMATURE' and 'dfx_template' in o)
    dn = [b.name for b in Character.load(bytes(arm['dfx_template'])).bones]
    bm = bonemap.auto_map(sn, dn, sp)
    print(bm.describe())
    print('unmapped slots:', bm.unmapped, '| required missing:', bm.missing_required)
    print('source bones not in the map:', [n for n in sn if n not in {a for a, _ in bm.slots.values()}])


if __name__ == '__main__':
    main()
