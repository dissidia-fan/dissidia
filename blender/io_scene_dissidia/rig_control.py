"""A standard animator's control rig on top of a Dissidia skeleton.

The game bones can't change (the game reads their rest orientations), so the controls are extra bones in the
same armature that the game bones follow through constraints:

  CTL-master      ground circle: moves / turns everything
  CTL-torso       pelvis box: moves the body (game root)
  CTL-hips        pelvis circle: tilts the hips on their own
  FK-waist, FK-chest, FK-neck, FK-head          spine
  FK-clavicle.L/R, FK-uparm, FK-foarm, FK-hand  FK arm         IK-hand.L/R + IK-pole_arm.L/R  IK arm
  FK-femur, FK-tibia, FK-foot                   FK leg         IK-foot.L/R + IK-pole_leg.L/R  IK leg
  CTL-toe, CTL-fing*, CTL-mant*                 toes, fingers, cape (same axes as the game bones)

FK/IK per limb is a slider on CTL-master (ik_arm_L, ik_arm_R, ik_leg_L, ik_leg_R). Game bones follow
MCH-F-* / MCH-I-* bones that sit on the controls at the game bones' exact rest matrices, so the controls can
have clean animator axes without changing what the game reads.

Each action is either a game action (game bones keyed directly, rig off) or a rig action (act['dfx_rig']):
"Animate with controls" fits the controls to the action's game animation and switches the rig on for it;
export bakes rig actions back onto the game bones (bake_controls). Rig bones are never exported.
"""
import math
import numpy as np
import bpy
from mathutils import Matrix, Vector

RIG_PREFIXES = ('CTL-', 'FK-', 'IK-', 'MCH-')
FWD = Vector((0, 0, 1))          # armature space = game space: Y up, +Z forward, +X his left
SPINE = ['waist', 'chest', 'neck', 'head']
ARM = ['clavicle', 'uparm', 'foarm', 'hand']
LEG = ['femur', 'tibia', 'foot']
LOCAL = ['toe', 'fing1a', 'fing3b', 'mant1b', 'mant1a']        # controls with the game bone's own axes


def is_rig_bone(name):
    return name.startswith(RIG_PREFIXES)


def has_rig(arm):
    return 'CTL-master' in arm.data.bones


# ---------------------------------------------------------------- widgets
def _widget(name, verts, edges):
    ob = bpy.data.objects.get(name)
    me = bpy.data.meshes.get(name) or bpy.data.meshes.new(name)
    me.clear_geometry(); me.from_pydata(verts, edges, [])
    if ob is None:
        ob = bpy.data.objects.new(name, me)
        holder = bpy.data.collections.get('DFX bone shapes') or bpy.data.collections.new('DFX bone shapes')
        if holder.name not in bpy.context.scene.collection.children:
            bpy.context.scene.collection.children.link(holder)
        holder.objects.link(ob)
        for lc in bpy.context.view_layer.layer_collection.children:
            if lc.collection == holder:
                lc.exclude = True
    ob.data = me
    return ob


def _circle(name, r, y=0.0, plane='XZ', n=24):
    v = []
    for i in range(n):
        a = 2 * math.pi * i / n
        c, s = r * math.cos(a), r * math.sin(a)
        v.append((c, y, s) if plane == 'XZ' else (c, s, y))
    return _widget(name, v, [(i, (i + 1) % n) for i in range(n)])


def _box(name, x, y0, y1, z):
    v = [(sx * x, yy, sz * z) for yy in (y0, y1) for sx, sz in ((-1, -1), (1, -1), (1, 1), (-1, 1))]
    e = [(0, 1), (1, 2), (2, 3), (3, 0), (4, 5), (5, 6), (6, 7), (7, 4), (0, 4), (1, 5), (2, 6), (3, 7)]
    return _widget(name, v, e)


def _square(name, r):
    v = [(-r, -r, 0), (r, -r, 0), (r, r, 0), (-r, r, 0)]
    return _widget(name, v, [(0, 1), (1, 2), (2, 3), (3, 0)])


def _diamond(name, d):
    v = [(d, 0, 0), (-d, 0, 0), (0, d, 0), (0, -d, 0), (0, 0, d), (0, 0, -d)]
    e = [(0, 2), (0, 3), (0, 4), (0, 5), (1, 2), (1, 3), (1, 4), (1, 5), (2, 4), (2, 5), (3, 4), (3, 5)]
    return _widget(name, v, e)


# ---------------------------------------------------------------- build
def _signed_angle(u, v, n):
    a = u.angle(v)
    return -a if u.cross(v).dot(n) < 0 else a


def _pole_angle(base, end, pole_loc):
    """IK pole angle that keeps the chain in its rest pose (base/end are edit bones)."""
    normal = (end.tail - base.head).cross(pole_loc - base.head)
    proj = normal.cross(base.tail - base.head)
    return _signed_angle(base.x_axis, proj, base.tail - base.head)


def _pin_end(pbs, d):
    """The IK hand/foot stays on the end of the limb (wrist/ankle) when the control is pulled out of reach,
    so the mesh never stretches; its rotation still comes from the control."""
    g_end = d['chain'][-1][0]
    pb = pbs[f'MCH-I-{g_end}']
    for c in [c for c in pb.constraints if c.name == 'DFX IK pin']:
        pb.constraints.remove(c)
    c = pb.constraints.new('COPY_LOCATION'); c.name = 'DFX IK pin'
    c.target = pb.id_data; c.subtarget = d['end']; c.head_tail = 1.0


def build_rig(context, arm):
    """Add the control rig to a Dissidia armature (the body bones it knows about). Safe to run again: an
    existing rig is removed first."""
    if has_rig(arm):
        remove_rig(context, arm)
    context.view_layer.objects.active = arm
    bpy.ops.object.mode_set(mode='EDIT')
    eb = arm.data.edit_bones
    J = {b.name: b.head.copy() for b in eb}
    have = lambda n: n in eb
    made = {}

    def mk(name, head, tail, parent=None, roll_to=FWD, copy_of=None, connect=False):
        b = eb.new(name)
        if copy_of is not None:
            b.head, b.tail, b.roll = eb[copy_of].head.copy(), eb[copy_of].tail.copy(), eb[copy_of].roll
        else:
            b.head, b.tail = Vector(head), Vector(tail)
            b.align_roll(Vector(roll_to))
        b.parent = eb[parent] if parent else None
        b.use_connect = connect
        b.use_deform = False
        made[name] = b
        return b

    pelvis = J['root']
    mk('CTL-master', (pelvis.x, 0, pelvis.z), (pelvis.x, 0, pelvis.z - 0.6), roll_to=(0, 1, 0))
    mk('CTL-torso', pelvis, pelvis + Vector((0, 0, -0.25)), 'CTL-master', roll_to=(0, 1, 0))
    mk('CTL-hips', pelvis, pelvis + Vector((0, -0.2, 0)), 'CTL-torso')
    follow = {'root': 'CTL-torso', 'hip': 'CTL-hips'}
    # spine
    tails = {'waist': 'chest', 'chest': 'neck', 'neck': 'head'}
    parent = 'CTL-torso'
    for n in SPINE:
        if not have(n):
            continue
        t = J[tails[n]] if n in tails and have(tails[n]) else J[n] + Vector((0, 0.2, 0))
        mk('FK-' + n, J[n], t, parent, connect=False)
        follow[n] = 'FK-' + n; parent = 'FK-' + n
    chest = 'FK-chest' if 'FK-chest' in made else parent
    ik = {}
    for s, S in (('l', 'L'), ('r', 'R')):
        # arm
        a = {k: f'{s}_{k}' for k in ARM}
        if all(have(v) for v in a.values()):
            sh, el, wr = J[a['uparm']], J[a['foarm']], J[a['hand']]
            hand_t = wr + (wr - el).normalized() * 0.09
            mk(f'FK-clavicle.{S}', J[a['clavicle']], sh, chest)
            mk(f'FK-uparm.{S}', sh, el, f'FK-clavicle.{S}')
            mk(f'FK-foarm.{S}', el, wr, f'FK-uparm.{S}', connect=True)
            mk(f'FK-hand.{S}', wr, hand_t, f'FK-foarm.{S}', connect=True)
            for k in ARM:
                follow[a[k]] = f'FK-{k}.{S}'
            mk(f'IK-hand.{S}', wr, hand_t, 'CTL-master')
            pole = el + Vector((0, 0, -0.45))
            mk(f'IK-pole_arm.{S}', pole, pole + Vector((0, 0, -0.08)), 'CTL-master')
            mk(f'MCH-IK-uparm.{S}', sh, el, f'FK-clavicle.{S}')
            mk(f'MCH-IK-foarm.{S}', el, wr, f'MCH-IK-uparm.{S}', connect=True)
            ik[f'arm_{S}'] = dict(chain=[(a['uparm'], f'MCH-IK-uparm.{S}'), (a['foarm'], f'MCH-IK-foarm.{S}'),
                                         (a['hand'], f'IK-hand.{S}')],
                                  end=f'MCH-IK-foarm.{S}', base=f'MCH-IK-uparm.{S}', target=f'IK-hand.{S}',
                                  pole=f'IK-pole_arm.{S}', pole_loc=pole)
        # leg
        l = {k: f'{s}_{k}' for k in LEG}
        if all(have(v) for v in l.values()):
            hp, kn, an = J[l['femur']], J[l['tibia']], J[l['foot']]
            toe = J[f'{s}_toe'] if have(f'{s}_toe') else an + Vector((0, -0.08, 0.12))
            mk(f'FK-femur.{S}', hp, kn, 'CTL-hips')
            mk(f'FK-tibia.{S}', kn, an, f'FK-femur.{S}', connect=True)
            mk(f'FK-foot.{S}', an, toe, f'FK-tibia.{S}', roll_to=(0, 1, 0), connect=True)
            for k in LEG:
                follow[l[k]] = f'FK-{k}.{S}'
            mk(f'IK-foot.{S}', an, toe, 'CTL-master', roll_to=(0, 1, 0))
            pole = kn + Vector((0, 0, 0.5))
            mk(f'IK-pole_leg.{S}', pole, pole + Vector((0, 0, 0.08)), 'CTL-master')
            mk(f'MCH-IK-femur.{S}', hp, kn, 'CTL-hips')
            mk(f'MCH-IK-tibia.{S}', kn, an, f'MCH-IK-femur.{S}', connect=True)
            ik[f'leg_{S}'] = dict(chain=[(l['femur'], f'MCH-IK-femur.{S}'), (l['tibia'], f'MCH-IK-tibia.{S}'),
                                         (l['foot'], f'IK-foot.{S}')],
                                  end=f'MCH-IK-tibia.{S}', base=f'MCH-IK-femur.{S}', target=f'IK-foot.{S}',
                                  pole=f'IK-pole_leg.{S}', pole_loc=pole)
    # local controls: same rest and parent as the game bone
    local = {}
    for n in [b.name for b in eb]:
        if is_rig_bone(n) or n[2:] not in LOCAL or n[:2] not in ('l_', 'r_'):
            continue
        c = mk(f'CTL-{n[2:]}.{n[0].upper()}', None, None, eb[n].parent.name if eb[n].parent else None, copy_of=n)
        local[n] = c.name
    # follow bones at the game bones' rest, on the controls
    for g, c in follow.items():
        mk(f'MCH-F-{g}', None, None, c, copy_of=g)
    for key, d in ik.items():
        for g, c in d['chain']:
            mk(f'MCH-I-{g}', None, None, c, copy_of=g)
    pole_angles = {key: _pole_angle(eb[d['base']], eb[d['end']], d['pole_loc']) for key, d in ik.items()}
    bpy.ops.object.mode_set(mode='POSE')
    pbs = arm.pose.bones
    for n in made:
        pbs[n].rotation_mode = 'QUATERNION'
        # like any standard rig: only the master, torso, IK hands/feet and poles move; everything else rotates
        movable = n in ('CTL-master', 'CTL-torso') or n.startswith(('IK-hand', 'IK-foot', 'IK-pole'))
        if not movable and not n.startswith('MCH-'):
            pbs[n].lock_location = (True, True, True)
            pbs[n].lock_scale = (True, True, True)
    # switches
    m = pbs['CTL-master']
    for key in ik:
        m[f'ik_{key}'] = 1.0 if key.startswith('leg') else 0.0
        m.id_properties_ui(f'ik_{key}').update(min=0.0, max=1.0, soft_min=0.0, soft_max=1.0,
                                               description=f'0 = FK, 1 = IK ({key.replace("_", " ")})')
    if 'dfx_rig_on' not in arm:
        arm['dfx_rig_on'] = 0.0

    def drive(con, expr, ikkey=None):
        fc = con.driver_add('influence'); d = fc.driver; d.type = 'SCRIPTED'
        v = d.variables.new(); v.name = 'on'; v.type = 'SINGLE_PROP'
        v.targets[0].id_type = 'OBJECT'; v.targets[0].id = arm; v.targets[0].data_path = '["dfx_rig_on"]'
        if ikkey:
            w = d.variables.new(); w.name = 'ik'; w.type = 'SINGLE_PROP'
            w.targets[0].id_type = 'OBJECT'; w.targets[0].id = arm
            w.targets[0].data_path = f'pose.bones["CTL-master"]["ik_{ikkey}"]'
        d.expression = expr
    for g in follow:
        pb = pbs[g]
        _clear_rig_constraints(pb)
        c = pb.constraints.new('COPY_TRANSFORMS'); c.name = 'DFX rig'
        c.target = arm; c.subtarget = f'MCH-F-{g}'
        drive(c, 'on')
    for key, d in ik.items():
        k = pbs[d['end']].constraints.new('IK'); k.name = 'DFX IK'
        k.target = arm; k.subtarget = d['target']; k.pole_target = arm; k.pole_subtarget = d['pole']
        k.pole_angle = pole_angles[key]; k.chain_count = 2
        for g, c in d['chain']:
            ct = pbs[g].constraints.new('COPY_TRANSFORMS'); ct.name = 'DFX rig IK'
            ct.target = arm; ct.subtarget = f'MCH-I-{g}'
            drive(ct, 'on * ik', key)
        _pin_end(pbs, d)
    for g, c in local.items():
        pb = pbs[g]
        _clear_rig_constraints(pb)
        ct = pb.constraints.new('COPY_TRANSFORMS'); ct.name = 'DFX rig'
        ct.target = arm; ct.subtarget = c; ct.owner_space = 'LOCAL'; ct.target_space = 'LOCAL'
        drive(ct, 'on')
    _calibrate_poles(context, arm, ik)
    _style(arm, ik, local)
    arm['dfx_rig_map'] = repr(dict(follow=follow, ik={k: [(g, c) for g, c in d['chain']] for k, d in ik.items()},
                                   local=local))
    bpy.ops.object.mode_set(mode='OBJECT')
    set_rig_on(arm, False)
    return made


def _calibrate_poles(context, arm, ik):
    """Pick each IK pole angle so the chain sits exactly at its rest pose when the controls are at rest."""
    ad = arm.animation_data
    keep = ad.action if ad else None
    if ad:
        ad.action = None
    saved = {pb.name: pb.matrix_basis.copy() for pb in arm.pose.bones}
    for pb in arm.pose.bones:
        pb.matrix_basis.identity()
    pbs = arm.pose.bones
    rest = lambda n: np.array(arm.data.bones[n].matrix_local)

    def err(key, ang):
        pbs[ik[key]['end']].constraints['DFX IK'].pole_angle = ang
        context.view_layer.update()
        return sum(np.abs(np.array(pbs[n].matrix) - rest(n)).max() for n in (ik[key]['base'], ik[key]['end']))
    for key in ik:
        best = min((err(key, a), a) for a in np.radians(np.arange(-180, 180, 5)))
        lo, hi = best[1] - np.radians(5), best[1] + np.radians(5)
        for _ in range(30):                                  # golden-section refine
            m1, m2 = lo + (hi - lo) * 0.382, lo + (hi - lo) * 0.618
            if err(key, m1) < err(key, m2):
                hi = m2
            else:
                lo = m1
        err(key, (lo + hi) / 2)
    for pb in arm.pose.bones:
        pb.matrix_basis = saved[pb.name]
    if ad:
        ad.action = keep


def _clear_rig_constraints(pb):
    for c in list(pb.constraints):
        if c.name.startswith('DFX'):
            pb.constraints.remove(c)


def remove_rig(context, arm):
    context.view_layer.objects.active = arm
    for pb in arm.pose.bones:
        _clear_rig_constraints(pb)
    bpy.ops.object.mode_set(mode='EDIT')
    eb = arm.data.edit_bones
    for b in [b for b in eb if is_rig_bone(b.name)]:
        eb.remove(b)
    bpy.ops.object.mode_set(mode='OBJECT')
    if arm.animation_data:
        for d in list(arm.animation_data.drivers):
            if 'DFX' in d.data_path:
                arm.animation_data.drivers.remove(d)


def _style(arm, ik, local):
    ad = arm.data
    sets = {'Torso': (True, 'THEME09'), 'Arms FK': (True, None), 'Arms IK': (True, 'THEME03'),
            'Legs FK': (True, None), 'Legs IK': (True, 'THEME03'), 'Fingers & toes': (True, None),
            'Cape': (True, 'THEME07'), 'Rig mechanism': (False, None)}
    colls = {n: ad.collections.get(n) or ad.collections.new(n) for n in sets}
    for n, (vis, _) in sets.items():
        colls[n].is_visible = vis
    W = dict(master=_circle('WGT-master', 0.6, plane='XY'), torso=_square('WGT-torso', 0.22),
             hips=_circle('WGT-hips', 0.2), fk=_circle('WGT-fk', 0.45, y=0.5), fk_small=_circle('WGT-fk_small', 0.6, y=0.5),
             head=_circle('WGT-head', 0.9, y=0.6), hand_ik=_box('WGT-hand_ik', 0.05, -0.03, 0.09, 0.05),
             foot_ik=_box('WGT-foot_ik', 0.06, -0.06, 0.18, 0.05), pole=_diamond('WGT-pole', 0.04),
             local=_circle('WGT-local', 0.035))
    for b in ad.bones:
        n = b.name
        if not is_rig_bone(n):
            continue
        pb = arm.pose.bones[n]
        side = n.rsplit('.', 1)[-1] if '.' in n else ''
        col = {'L': 'THEME04', 'R': 'THEME01'}.get(side, 'THEME09')
        coll, shape, size = 'Rig mechanism', None, True
        if n.startswith('MCH-'):
            pass
        elif n == 'CTL-master':
            coll, shape, size = 'Torso', W['master'], False
        elif n == 'CTL-torso':
            coll, shape, size = 'Torso', W['torso'], False
        elif n == 'CTL-hips':
            coll, shape, size = 'Torso', W['hips'], False
        elif n in ('FK-waist', 'FK-chest', 'FK-neck'):
            coll, shape = 'Torso', W['fk_small']
        elif n == 'FK-head':
            coll, shape = 'Torso', W['head']
        elif n.startswith(('FK-clavicle', 'FK-uparm', 'FK-foarm', 'FK-hand')):
            coll, shape = 'Arms FK', W['fk']
        elif n.startswith(('FK-femur', 'FK-tibia', 'FK-foot')):
            coll, shape = 'Legs FK', W['fk']
        elif n.startswith('IK-hand'):
            coll, shape, size, col = 'Arms IK', W['hand_ik'], False, 'THEME03'
        elif n.startswith('IK-foot'):
            coll, shape, size, col = 'Legs IK', W['foot_ik'], False, 'THEME03'
        elif n.startswith('IK-pole_arm'):
            coll, shape, size, col = 'Arms IK', W['pole'], False, 'THEME03'
        elif n.startswith('IK-pole_leg'):
            coll, shape, size, col = 'Legs IK', W['pole'], False, 'THEME03'
        elif n.startswith(('CTL-toe', 'CTL-fing')):
            coll, shape, size = 'Fingers & toes', W['local'], False
        elif n.startswith('CTL-mant'):
            coll, shape, size, col = 'Cape', W['local'], False, 'THEME07'
        for c in list(b.collections):
            c.unassign(b)
        colls[coll].assign(b)
        b.color.palette = col if coll != 'Rig mechanism' else 'DEFAULT'
        if shape is not None:
            pb.custom_shape = shape
            pb.use_custom_shape_bone_size = size
            if hasattr(pb, 'custom_shape_wire_width'):
                pb.custom_shape_wire_width = 2.0
    ad.display_type = 'OCTAHEDRAL'
    ad.show_bone_custom_shapes = True
    arm.show_in_front = True
    for scr in bpy.data.screens:                     # no dotted parent lines across the rig
        for area in scr.areas:
            if area.type == 'VIEW_3D':
                for sp in area.spaces:
                    if sp.type == 'VIEW_3D':
                        sp.overlay.show_relationship_lines = False


GAME_COLLS = ('Body', 'Fingers', 'Cape & hair')
RIG_COLLS = ('Torso', 'Arms FK', 'Arms IK', 'Legs FK', 'Legs IK', 'Fingers & toes', 'Cape')


def set_rig_on(arm, on):
    """Controls drive the game bones (on) or the game bones play their own keys (off). Shows the matching bones."""
    arm['dfx_rig_on'] = 1.0 if on else 0.0
    for c in arm.data.collections_all:
        if c.name in RIG_COLLS:                 # arms start in FK and legs in IK: show those controls
            c.is_visible = bool(on) and c.name not in ('Arms IK', 'Legs FK')
        elif c.name in GAME_COLLS:
            c.is_visible = not on
    arm.update_tag()


def sync_action(arm):
    """Called when the active action changes: rig on for rig actions only."""
    if not has_rig(arm):
        return
    act = arm.animation_data.action if arm.animation_data else None
    want = bool(act is not None and act.get('dfx_rig'))
    if bool(arm.get('dfx_rig_on', 0)) != want:
        set_rig_on(arm, want)


# ---------------------------------------------------------------- game animation -> controls, and back
def _rig_map(arm):
    import ast
    return ast.literal_eval(arm['dfx_rig_map'])


def _M(m):
    return np.array(m, float)


def _safe(M, prev):
    """A pose matrix with any zero scale replaced by 1 (keeps the previous rotation if it collapsed)."""
    R = M[:3, :3]; s = np.linalg.norm(R, axis=0)
    if s.min() > 1e-5:
        return M, False
    out = M.copy()
    out[:3, :3] = prev[:3, :3] if prev is not None else np.eye(3)
    return out, True


def _channelbag(act):
    from bpy_extras import anim_utils
    slot = act.slots[0] if act.slots else act.slots.new(id_type='OBJECT', name='Armature')
    return anim_utils.action_ensure_channelbag_for_slot(act, slot)


def _write(cb, bone, frames, mats, interp='BEZIER', tol=(3e-4, 5e-4, 3e-4)):
    """Key a bone's basis matrices (4x4, per frame) as loc / quaternion / scale, with redundant keys removed."""
    from .exporter import _reduce
    for fc in [f for f in cb.fcurves if f.data_path.startswith(f'pose.bones["{bone}"]')]:
        cb.fcurves.remove(fc)
    loc, rot, scl = [], [], []
    for M in mats:
        l, r, s = Matrix(M.tolist()).decompose()
        loc.append(l[:]); rot.append((r.w, r.x, r.y, r.z)); scl.append(s[:])
    loc, rot, scl = np.array(loc), np.array(rot), np.array(scl)
    for i in range(1, len(rot)):
        if np.dot(rot[i], rot[i - 1]) < 0:
            rot[i] = -rot[i]
    frames = np.asarray(frames, float)
    base = f'pose.bones["{bone}"]'
    for path, data, t in (('location', loc, tol[0]), ('rotation_quaternion', rot, tol[1]), ('scale', scl, tol[2])):
        keep = _reduce(frames, data, t)
        for k in range(data.shape[1]):
            fc = cb.fcurves.new(f'{base}.{path}', index=k, group_name=bone)
            fc.keyframe_points.add(len(keep))
            co = np.empty(len(keep) * 2, np.float32); co[0::2] = frames[keep]; co[1::2] = data[keep, k]
            fc.keyframe_points.foreach_set('co', co)
            for kp in fc.keyframe_points:
                kp.interpolation = interp
                kp.handle_left_type = kp.handle_right_type = 'AUTO_CLAMPED'
            fc.update()


def _frames(act):
    f0, f1 = act.frame_range
    return list(range(int(round(f0)), int(round(f1)) + 1))


def match_controls(context, arm, act):
    """Fit the controls to the action's game-bone animation and switch the action to the rig."""
    from . import tools_bl
    rm = _rig_map(arm)
    sc = context.scene
    tools_bl.set_action(arm, act)
    set_rig_on(arm, False)
    frames = _frames(act)
    game = set(rm['follow']) | {g for ch in rm['ik'].values() for g, _ in ch} | set(rm['local'])
    rec = {g: [] for g in game}
    basis = {g: [] for g in rm['local']}
    hidden = []
    prev = {}
    for f in frames:
        sc.frame_set(f)
        for g in game:
            M, z = _safe(_M(arm.pose.bones[g].matrix), prev.get(g))
            prev[g] = M; rec[g].append(M)
            if g == 'root' and z:
                hidden.append(f)
        for g in rm['local']:
            basis[g].append(_M(arm.pose.bones[g].matrix_basis))
    rest = {b.name: _M(b.matrix_local) for b in arm.data.bones}
    inv = np.linalg.inv
    # control world matrices
    world = {}
    world['CTL-master'] = [rest['CTL-master']] * len(frames)
    for g, c in rm['follow'].items():                       # control = game @ game_rest^-1 @ control_rest
        world[c] = [G @ inv(rest[g]) @ rest[c] for G in rec[g]]
    for key, chain in rm['ik'].items():
        S = key[-1]
        limb = 'hand' if key.startswith('arm') else 'foot'
        world[f'IK-{limb}.{S}'] = list(world[f'FK-{limb}.{S}'])
        (g0, _), (g1, _), (g2, _) = chain
        pole = f'IK-pole_{key.split("_")[0]}.{S}'
        dflt = rest[pole][:3, 3] - rest[g1][:3, 3]
        ws = []
        for k in range(len(frames)):
            a, b, c = rec[g0][k][:3, 3], rec[g1][k][:3, 3], rec[g2][k][:3, 3]
            ac = c - a; t = np.dot(b - a, ac) / max(1e-9, np.dot(ac, ac))
            off = b - (a + t * ac)
            d = off / np.linalg.norm(off) * np.linalg.norm(dflt) if np.linalg.norm(off) > 0.01 else dflt
            P = rest[pole].copy(); P[:3, 3] = b + d
            ws.append(P)
        world[pole] = ws
    # world -> basis, parents first (all parents are controls or the master)
    bones = arm.data.bones
    order = [b.name for b in bones if b.name in world]
    depth = lambda n: 0 if bones[n].parent is None else 1 + depth(bones[n].parent.name)
    order.sort(key=depth)
    cb = _channelbag(act)
    for n in order:
        b = bones[n]
        mats = []
        for k in range(len(frames)):
            if b.parent is None:
                mats.append(inv(rest[n]) @ world[n][k])
            else:
                p = b.parent.name
                Pw = world[p][k] if p in world else rest[p]
                mats.append(inv(inv(rest[p]) @ rest[n]) @ inv(Pw) @ world[n][k])
        _write(cb, n, frames, mats)
    for g, c in rm['local'].items():
        _write(cb, c, frames, basis[g])
    if hidden:                                              # warp-style vanish: hide the body again
        for fc in cb.fcurves:
            if fc.data_path == 'pose.bones["CTL-torso"].scale':
                for f in hidden:
                    fc.keyframe_points.insert(f, 0.0).interpolation = 'CONSTANT'
                for f in frames:
                    if f - 1 in hidden and f not in hidden:
                        fc.keyframe_points.insert(f, 1.0)
                for kp in fc.keyframe_points:                 # an instant vanish / reappear
                    kp.interpolation = 'CONSTANT'
                fc.update()
    act['dfx_rig'] = True
    set_rig_on(arm, True)
    sc.frame_set(frames[0])
    return len(order) + len(rm['local'])


def bake_controls(context, arm, act):
    """Write the controls' result onto the game bones' keys (what the exporter reads)."""
    from . import tools_bl
    rm = _rig_map(arm)
    sc = context.scene
    tools_bl.set_action(arm, act)
    set_rig_on(arm, True)
    frames = _frames(act)
    game = list(rm['follow']) + [g for ch in rm['ik'].values() for g, _ in ch] + list(rm['local'])
    game = list(dict.fromkeys(game))
    rest = {b.name: _M(b.matrix_local) for b in arm.data.bones}
    inv = np.linalg.inv
    out = {g: [] for g in game}
    last = {}
    for f in frames:
        sc.frame_set(f)
        for g in game:
            pb = arm.pose.bones[g]
            M = _M(pb.matrix)
            if pb.parent is None:
                B = inv(rest[g]) @ M
            else:
                P = _M(pb.parent.matrix)
                if np.linalg.norm(P[:3, :3], axis=0).min() < 1e-3:   # parent hidden (scale 0): hold
                    B = last.get(g, np.eye(4))
                else:
                    B = inv(inv(rest[pb.parent.name]) @ rest[g]) @ inv(P) @ M
            last[g] = B; out[g].append(B)
    cb = _channelbag(act)
    for g in game:
        mats = out[g]
        if g == 'root':                                     # keep a vanish (scale 0) exact
            fixed = []
            for B in mats:
                s = np.linalg.norm(B[:3, :3], axis=0)
                if s.min() < 1e-5:
                    Z = np.eye(4); Z[:3, 3] = B[:3, 3]; Z[:3, :3] *= 0.0
                    fixed.append(Z)
                else:
                    fixed.append(B)
            mats = fixed
        _write_game(cb, g, frames, mats)
    return len(game)


def _write_game(cb, bone, frames, mats):
    """Game bones: linear keys on every frame (the exporter reduces them)."""
    loc, rot, scl = [], [], []
    prev = None
    for M in mats:
        s = np.linalg.norm(M[:3, :3], axis=0)
        if s.min() < 1e-6:
            q = prev if prev is not None else (1, 0, 0, 0)
            loc.append(M[:3, 3]); rot.append(q); scl.append((0, 0, 0)); continue
        l, r, sc = Matrix(M.tolist()).decompose()
        q = (r.w, r.x, r.y, r.z)
        if prev is not None and np.dot(q, prev) < 0:
            q = tuple(-x for x in q)
        prev = q
        loc.append(l[:]); rot.append(q); scl.append(sc[:])
    from . import common
    for fc in [f for f in cb.fcurves if f.data_path.startswith(f'pose.bones["{bone}"]')]:
        cb.fcurves.remove(fc)
    rot = np.array(rot)
    common.write_bone_fcurves(cb, bone, np.array(frames, float), np.array(loc), rot[:, [1, 2, 3, 0]], np.array(scl))


def rig_actions(arm):
    from . import moveset_bl
    return [a for a in moveset_bl.actions_for(arm) if a.get('dfx_rig')]


def bake_all(context, arm):
    """Bake every rig action (before export / costume copy). Returns their names."""
    if not has_rig(arm):
        return []
    from . import tools_bl
    keep = arm.animation_data.action if arm.animation_data else None
    f = context.scene.frame_current
    done = []
    for act in rig_actions(arm):
        bake_controls(context, arm, act); done.append(act.name)
    if keep is not None:
        tools_bl.set_action(arm, keep)
        sync_action(arm)
    context.scene.frame_set(f)
    return done
