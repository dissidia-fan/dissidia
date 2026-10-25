"""Character tools that work on native Blender data (so results stay hand-editable)."""
import json
import os
import math
import struct
import numpy as np
import bpy
from mathutils import Matrix, Quaternion, Vector
from bpy_extras import anim_utils

from . import common, moveset_bl
from .dfx.gmo import Character, parse_bones

# The dash is R+Triangle: script 240 -> motion _500A (34 frames, 20..34 loop while it lasts), on the ground and in
# the air. Holding X after a jump is a second, shorter dash (script 103, motion _023A, cut after ~19 frames).
# R+X (scripts 400 / 410 / 440 / 460, motions _100A _120A _140A _150A) is the dodge and stays retail.
DASH_SCRIPTS = (240,)
HOLDX_SCRIPTS = (103,)
DODGE_SCRIPTS = (400, 410, 440, 460)
DASH_KINDS = {'DASH': DASH_SCRIPTS, 'HOLDX': HOLDX_SCRIPTS, 'DODGE': DODGE_SCRIPTS}


def channelbag(act):
    slot = act.slots[0] if act.slots else act.slots.new(id_type='OBJECT', name='Armature')
    return anim_utils.action_ensure_channelbag_for_slot(act, slot)


def set_action(arm, act):
    arm.animation_data_create()
    arm.animation_data.action = act
    if act.slots:
        arm.animation_data.action_slot = act.slots[0]


def remove_bone_curves(cb, bone):
    for fc in [f for f in cb.fcurves if f.data_path.startswith(f'pose.bones["{bone}"]')]:
        cb.fcurves.remove(fc)


def dash_actions(arm, kind='DASH'):
    mm = json.loads(arm.get('dfx_motion_map', '[]'))
    scripts = DASH_KINDS.get(kind, DASH_SCRIPTS)
    idx = sorted({mi for sid, mi, ph in mm if sid in scripts})
    return [a for a in moveset_bl.actions_for(arm) if a.get('dfx_motion_index') in idx]


def hand_bone_of(ob):
    best, w = None, -1
    for vg in ob.vertex_groups:
        tot = 0.0
        for v in ob.data.vertices:
            for g in v.groups:
                if g.group == vg.index:
                    tot += g.weight
        if tot > w:
            best, w = vg.name, tot
    return best


# ---------------------------------------------------------------- warp dash
def _fx_object(arm, like_ob, name, P, T, idx, rgba, bone, colors=None, collection_name='Warp FX'):
    """An effect mesh (rebuilt each time) rigid on `bone`, with a generated texture. Tagged dfx_fx so the
    exporter gives it a top-level draw bone."""
    old = bpy.data.objects.get(name)
    if old is not None:
        me_old = old.data
        bpy.data.objects.remove(old, do_unlink=True)
        if me_old.users == 0:
            bpy.data.meshes.remove(me_old)
    me = bpy.data.meshes.new(name)
    me.from_pydata([tuple(p) for p in P], [], [tuple(idx[i:i + 3]) for i in range(0, len(idx), 3)])
    uvl = me.uv_layers.new(name='UVMap')
    col = me.color_attributes.new('Col', 'BYTE_COLOR', 'CORNER')
    for poly in me.polygons:
        for li in poly.loop_indices:
            vi = me.loops[li].vertex_index
            u, v = T[vi]
            uvl.data[li].uv = (u, 1.0 - v)
            c = colors[vi] if colors is not None else (1, 1, 1, 1)
            col.data[li].color = tuple(c)
    img_name = name + '.tga'
    img = bpy.data.images.get(img_name)
    if img is not None:
        bpy.data.images.remove(img)
    img = common.image_from_rgba(img_name, rgba)
    me.materials.append(common.make_material(name, img))
    ob = bpy.data.objects.new(name, me)
    coll = bpy.data.collections.get(collection_name)
    if coll is None:
        coll = bpy.data.collections.new(collection_name)
        (like_ob.users_collection[0] if like_ob.users_collection else bpy.context.scene.collection).children.link(coll)
    coll.objects.link(ob)
    ob.parent = arm
    ob.modifiers.new('Armature', 'ARMATURE').object = arm
    ob.vertex_groups.new(name=bone).add(list(range(len(me.vertices))), 1.0, 'REPLACE')
    ob['dfx_fx'] = True
    return ob


def _ghost_objects(arm, body_ob, ghost_set):
    """One copy of the body mesh per ghost stage, skinned to the g_ skeleton, with tinted textures."""
    from .dfx import warp
    out = []
    for st in range(warp.GHOST_STAGES):
        name = f'warp_ghost{st}'
        old = bpy.data.objects.get(name)
        if old is not None:
            me_old = old.data; bpy.data.objects.remove(old, do_unlink=True)
            if me_old.users == 0:
                bpy.data.meshes.remove(me_old)
        ob = bpy.data.objects.new(name, body_ob.data.copy())
        coll = bpy.data.collections.get('Warp FX') or bpy.data.collections.new('Warp FX')
        if coll.name not in [c.name for c in bpy.context.scene.collection.children_recursive]:
            (body_ob.users_collection[0] if body_ob.users_collection else bpy.context.scene.collection).children.link(coll)
        coll.objects.link(ob)
        ob.parent = arm
        ob.modifiers.new('Armature', 'ARMATURE').object = arm
        if not len(ob.vertex_groups):              # older Blender: groups live on the object
            for vg in body_ob.vertex_groups:
                ob.vertex_groups.new(name=vg.name)
            for v in body_ob.data.vertices:
                for g in v.groups:
                    ob.vertex_groups[g.group].add([v.index], g.weight, 'REPLACE')
        for vg in ob.vertex_groups:                  # skin the copy to the ghost skeleton
            if vg.name in ghost_set:
                vg.name = 'g_' + vg.name
        for i, slot in enumerate(ob.data.materials):
            img = None
            if slot is not None and slot.node_tree:
                img = next((n.image for n in slot.node_tree.nodes if n.type == 'TEX_IMAGE' and n.image), None)
            rgba = common.image_rgba(img) if img is not None else np.full((8, 8, 4), 255, np.uint8)
            ghost_rgba = warp.ghost_palette(st)[warp.ghost_indices(rgba, st)]
            iname = f'{name}_{i}.tga'
            if bpy.data.images.get(iname):
                bpy.data.images.remove(bpy.data.images[iname])
            ob.data.materials[i] = common.make_material(f'{name}_{i}', common.image_from_rgba(iname, ghost_rgba))
        for k in [k for k in ob.keys() if k.startswith('dfx_')]:
            del ob[k]
        ob['dfx_fx'] = True
        out.append(ob)
    return out


def _copy_to_ghost(cb, ghost_names, root):
    for fc in list(cb.fcurves):
        if fc.data_path.startswith('pose.bones["g_'):
            cb.fcurves.remove(fc)
    for fc in list(cb.fcurves):
        if not fc.data_path.startswith('pose.bones["'):
            continue
        bone = fc.data_path.split('"')[1]
        if bone not in ghost_names:
            continue
        new = cb.fcurves.new(fc.data_path.replace(f'["{bone}"]', f'["g_{bone}"]', 1), index=fc.array_index, group_name='g_' + bone)
        n = len(fc.keyframe_points)
        co = np.empty(n * 2, np.float32); fc.keyframe_points.foreach_get('co', co)
        if bone == root and fc.data_path.endswith('scale'):     # the body's root is hidden (0); the ghost's never is
            vals = co[1::2]; last = next((v for v in vals if abs(v) > 1e-6), 1.0)
            for i in range(len(vals)):
                if abs(vals[i]) > 1e-6:
                    last = vals[i]
                else:
                    vals[i] = last
            co[1::2] = vals
        it = np.empty(n, np.int32); fc.keyframe_points.foreach_get('interpolation', it)
        new.keyframe_points.add(n); new.keyframe_points.foreach_set('co', co); new.keyframe_points.foreach_set('interpolation', it)
        new.update()


def _remove_fx(prefixes):
    for o in [o for o in bpy.data.objects if o.get('dfx_fx') and o.name.startswith(prefixes)]:
        me = o.data
        bpy.data.objects.remove(o, do_unlink=True)
        if me.users == 0:
            bpy.data.meshes.remove(me)


def restore_retail(arm, actions, ch=None):
    """Put `actions` back to the game's original animation and drop their warp events (earlier versions of
    the warp were authored on the dodge motions). Returns the names restored."""
    from .dfx.gmo import Character as _Ch
    from .importer import actions_from_motions, _normalised
    ch = ch or _Ch.load(bytes(arm['dfx_template']))
    abones = parse_bones(ch.anim_model)
    names = [b.name for b in abones]
    worlds = [_normalised(np.array(arm.data.bones[n].matrix_local)) for n in names]
    parents = [b.parent for b in abones]
    ev = moveset_bl.get_events(arm)
    done = []
    for act in actions:
        mi = act.get('dfx_motion_index')
        d = ev.get(moveset_bl.action_key(act))
        tagged = bool(d) and any(e.get('tag') == 'warp' for e in d.get('ranged', []) + d.get('keyed', []))
        if not tagged or mi is None or mi >= len(ch.motions):
            continue
        name = act.name
        d['ranged'] = [e for e in d['ranged'] if e.get('tag') != 'warp']
        d['keyed'] = [e for e in d['keyed'] if e.get('tag') != 'warp']
        mot = ch.motions[mi]
        new = actions_from_motions([mot], abones, parents, names, worlds, arm, index_base=mi, replace=True)[0]
        new.name = name
        done.append(name)
    moveset_bl.set_events(arm, ev)
    moveset_bl._marker_action.pop(arm.name, None)
    return done


def apply_warp_dash(context, arm, weapon_ob, actions, params=None, effect=0x3C, sword_name='warp_sword',
                    ghost=True, trail=True, bolts=False, streak=False, shards=False, kind='DASH'):
    """Noctis-style warp dash (see dfx/warp.py): crouch, underarm throw with the weapon's hand, fade out
    through a blue ghost while the sword flies, lightning round the hovering sword, then reappear holding
    it in a tumble landing. Effects are ordinary objects in a 'Warp FX' collection.
    The body is rebuilt from the game's original animations, so re-applying with new timing is safe.
    Returns (thrown mesh object, names of the rewritten actions)."""
    from .dfx import warp
    from .dfx.motion_encode import build_motion
    from .importer import actions_from_motions, _normalised
    p = {**warp.DEFAULTS, **({'DASH': warp.RUN_DEFAULTS, 'HOLDX': warp.DASH_DEFAULTS}.get(kind) or {}), **(params or {})}
    hand = hand_bone_of(weapon_ob)
    if hand is None:
        raise RuntimeError(f'{weapon_ob.name} has no vertex groups')
    ch = Character.load(bytes(arm['dfx_template']))
    abones = parse_bones(ch.anim_model)
    names = [b.name for b in abones]
    if hand not in names:
        raise RuntimeError(f'{hand} is not a game bone')
    hand_i = names.index(hand)
    wep_i = next((i for i, b in enumerate(ch.bones) if b.draw_part >= 0 and b.blend_bones and b.blend_bones[0] == hand_i), None)
    if wep_i is None:
        raise RuntimeError(f'no weapon part is held by {hand}')
    blade, blen = _blade_of(arm, weapon_ob, hand)       # measured on the weapon in the slot now (it may be replaced)
    G = np.asarray(ch.pos_offset, float)
    # author every dash first: the ghost is a snapshot of the throw pose
    authored = {}
    for act in actions:
        mi = act.get('dfx_motion_index')
        if mi is not None and mi < len(ch.motions):
            authored[act.name] = (act, warp.author(ch, abones, ch.motions, mi, hand_i, blade, p))
    if not authored:
        raise RuntimeError('none of these actions come from the game file')
    tm0 = next(iter(authored.values()))[1][4]
    # 1. bones: the thrown copy at the hand's bind pose; effects bound at G (inside the vertex range)
    fx_bones = {}
    for on, nm in ((trail, 'warp_trail'), (bolts, 'warp_bolts'), (streak, 'warp_streak'), (shards, 'warp_shards')):
        if on:
            fx_bones[nm] = Matrix.Translation(Vector(G))
    context.view_layer.objects.active = arm
    bpy.ops.object.mode_set(mode='EDIT')
    eb = arm.data.edit_bones
    sb = eb.get(sword_name) or eb.new(sword_name)
    sb.parent = None
    sb.head, sb.tail, sb.roll = eb[hand].head.copy(), eb[hand].tail.copy(), eb[hand].roll   # an exact copy of the hand
    for nm, M in fx_bones.items():
        b = eb.get(nm) or eb.new(nm)
        b.parent = None; b.matrix = M; b.length = 0.3
    ghost_names = []
    body_ob = next((o for o in arm.children if o.type == 'MESH' and o.get('dfx_part_index') == 0), None)
    if ghost and body_ob is not None:
        # ghost skeleton: copies of every bone the body is skinned to, and their parents
        need = set()
        for vg in body_ob.vertex_groups:
            b = eb.get(vg.name)
            while b is not None and b.name not in need:
                need.add(b.name); b = b.parent
        for nm in [b.name for b in eb if b.name in need]:          # edit_bones are parent-first
            gb = eb.get('g_' + nm) or eb.new('g_' + nm)
            src = eb[nm]
            gb.head, gb.tail, gb.roll = src.head.copy(), src.tail.copy(), src.roll
            gb.parent = eb.get('g_' + src.parent.name) if src.parent is not None else None
            ghost_names.append(nm)
    bpy.ops.object.mode_set(mode='OBJECT')
    for nm in [sword_name] + list(fx_bones) + ['g_' + n for n in ghost_names]:
        arm.pose.bones[nm].rotation_mode = 'QUATERNION'
    # the game poses a bone no motion animates with its rest values. These top-level effect bones are only
    # animated by the dash itself, so in every other motion (the engine can cut the dash at any frame) they
    # would be drawn for a frame at their bind pose before the hide events run: rest scale 0 instead.
    for nm in [sword_name] + list(fx_bones) + ['g_' + n for n in ghost_names]:
        if arm.data.bones[nm].parent is None:
            arm.data.bones[nm]['dfx_rest_scale0'] = 1
    # 2. the thrown copy of the weapon, rigid on that bone
    ob = bpy.data.objects.get(sword_name + '_mesh')
    if ob is None:
        ob = bpy.data.objects.new(sword_name + '_mesh', weapon_ob.data.copy())
        ob['dfx_copy_of'] = weapon_ob.name
        for c in weapon_ob.users_collection:
            c.objects.link(ob)
        ob.parent = arm
        ob.modifiers.new('Armature', 'ARMATURE').object = arm
    else:                                           # always the current weapon's mesh
        old_me = ob.data; ob.data = weapon_ob.data.copy(); ob['dfx_copy_of'] = weapon_ob.name
        if old_me.users == 0:
            bpy.data.meshes.remove(old_me)
    ob.vertex_groups.clear()
    ob.vertex_groups.new(name=sword_name).add(list(range(len(ob.data.vertices))), 1.0, 'REPLACE')
    # 3. effect objects
    _remove_fx(('warp_ghost', 'warp_trail', 'warp_bolts', 'warp_streak', 'warp_shards'))
    ghost_obs, fx_obs = [], {}
    if ghost and body_ob is not None:
        ghost_obs = _ghost_objects(arm, body_ob, set(ghost_names))
    trail_obs, trail_fix = [], None
    if trail:                                       # stage parts, stored half size around G (see warp.add_trail)
        ta, tb = tm0['trail']
        M = (np.asarray(ta) + np.asarray(tb)) / 2; k_ = warp.TRAIL_SHRINK
        trail_fix = (M - G, 1 / k_)
        for st, ((lP, lT, lI), (dP, dT, dI)) in enumerate(warp.trail_stages(ta, tb, tm0['release'], tm0['vanish'], p['trail_len'])):
            ds = []
            for nm, (P, T, I), rgba in ((f'warp_trail{st}', (lP, lT, lI), warp.trail_line_rgba()), (f'warp_trail{st}d', (dP, dT, dI), warp.dot_rgba())):
                if len(I):
                    ds.append(_fx_object(arm, weapon_ob, nm, G + (P - M) * k_, T, I, rgba, 'warp_trail'))
            trail_obs.append(ds)
    for on, nm, geo, rgba in ((bolts, 'warp_bolts', warp.bolt_geometry, warp.bolt_rgba),
                              (streak, 'warp_streak', warp.streak_geometry, warp.streak_rgba),
                              (shards, 'warp_shards', warp.shard_geometry, warp.crystal_rgba)):
        if on:
            P, T, I = geo()
            fx_obs[nm] = _fx_object(arm, weapon_ob, nm, P + G, T, I, rgba(), nm)
    inv_edit = np.linalg.inv(np.array(arm.data.bones[sword_name].matrix_local))
    worlds = [_normalised(np.array(arm.data.bones[n].matrix_local)) for n in names]
    parents = [b.parent for b in abones]
    # 4. animation, rebuilt from the original game motions. The dodge goes back to retail when the dash is
    # authored on the player's dash (it carried the warp in earlier versions).
    for other in DASH_KINDS:                         # the other dashes go back to the game's animation
        if other != kind:
            restore_retail(arm, dash_actions(arm, other), ch)
    ev = moveset_bl.get_events(arm)
    tmpl_show = tmpl_fx = None
    for d in ev.values():
        for e in d.get('ranged', []):
            if e['op'] == moveset_bl.SHOW and tmpl_show is None:
                tmpl_show = e
        for e in d.get('keyed', []):
            if e['op'] == moveset_bl.OWN_FX and tmpl_fx is None:
                tmpl_fx = e
    done = []
    for name, (act, (frames, tr, sword, sw_scale, tm)) in authored.items():
        mi = act['dfx_motion_index']
        m = ch.motions[mi]
        mot = build_motion(m.name, frames, tr, m.frame_rate, extra=m.extra, header=m.header,
                           frame_start=m.frame_start, frame_end=m.frame_end)
        mot.name = name
        new = actions_from_motions([mot], abones, parents, names, worlds, arm, index_base=mi, replace=True)[0]
        new['dfx_rig'] = False                      # tool output plays on the game bones
        cb = channelbag(new)
        loc, rot, scl = [], [], []
        for k in range(len(frames)):
            basis = Matrix((inv_edit @ sword[k]).tolist())
            l, r, _ = basis.decompose()
            loc.append(l[:]); rot.append((r.x, r.y, r.z, r.w)); scl.append((sw_scale[k],) * 3)
        remove_bone_curves(cb, sword_name)
        common.write_bone_fcurves(cb, sword_name, frames, np.array(loc), common.continuous(np.array(rot)), np.array(scl))
        R, V, FS, CE = tm['reappear'], tm['vanish'], tm['fade_start'], tm['catch_end']
        tracks = {}
        if bolts:
            tracks['warp_bolts'] = warp.bolt_track(frames, sword, blen, blade, R - 5, R)
        if streak:
            tracks['warp_streak'] = tm['streak']
        if shards:
            tracks['warp_shards'] = warp.shard_track(frames, sword, blen, blade, [(CE, 'out')])
        if trail_fix is not None:                        # fixed: puts the half-size trail back in place
            nfr = len(frames)
            remove_bone_curves(cb, 'warp_trail')
            tS = np.full((nfr, 3), trail_fix[1])
            if p.get('loop') or p.get('play_end'):      # nothing of it is left to be seen when the dash is cut
                tS[frames >= (int(round(m.frame_start)) if p.get('loop') else R)] = 0.0
            common.write_bone_fcurves(cb, 'warp_trail', frames, np.tile(trail_fix[0], (nfr, 1)),
                                      np.tile([0, 0, 0, 1.0], (nfr, 1)), tS)
        for nm, (sT, sQ, sS) in tracks.items():          # rest is a pure translation G, so basis = (T - G, Q, S)
            remove_bone_curves(cb, nm)
            common.write_bone_fcurves(cb, nm, frames, sT - G, common.continuous(sQ), sS)
        # ghost skeleton plays the body's animation (its root is never hidden)
        root = 'root' if 'root' in arm.pose.bones else arm.pose.bones[0].name
        if ghost_names:
            _copy_to_ghost(cb, ghost_names, root)
            if p.get('loop') or p.get('play_end'):
                # the game draws every effect part for a frame or two at the start of the next motion (before its
                # hide events run), blended from this motion's last pose: keep the ghost collapsed once it is done
                ls = int(round(m.frame_start)) if p.get('loop') else V + 3
                for fc in cb.fcurves:
                    if fc.data_path == f'pose.bones["g_{root}"].scale':
                        for kp in fc.keyframe_points:
                            if kp.co[0] >= ls:
                                kp.co[1] = 0.0
                        fc.update()
        # instant vanish / reappear
        for fc in cb.fcurves:
            if fc.data_path in (f'pose.bones["{root}"].scale', f'pose.bones["{sword_name}"].scale',
                                'pose.bones["warp_bolts"].scale'):
                for kp in fc.keyframe_points:
                    kp.interpolation = 'CONSTANT'
        # events (replace earlier warp events)
        d = ev.get(moveset_bl.action_key(new))
        if d and d.get('ok') and tmpl_show and tmpl_fx:
            d['ranged'] = [e for e in d['ranged'] if e.get('tag') != 'warp']
            d['keyed'] = [e for e in d['keyed'] if e.get('tag') != 'warp']

            PE = tm['play_end']                      # nothing past the last frame the engine plays

            def show(obj, s0, s1):
                s1 = min(s1, PE)
                if s0 >= s1:
                    return
                e = json.loads(json.dumps(tmpl_show)); e.update(tag='warp', show_object=obj.name)
                moveset_bl.set_event_frames(e, start=s0, end=s1)
                d['ranged'].append(e)
            show(ob, 0, CE)
            if ghost_obs:
                for d_, s0, s1 in warp.ghost_show_ranges(FS, ghost_obs):
                    show(d_, s0, s1)
            if trail_obs:
                for d_, s0, s1 in warp.trail_show_ranges(tm['release'], trail_obs):
                    show(d_, s0, s1)
            if bolts:
                show(fx_obs['warp_bolts'], R - 5, R)
            if streak:
                show(fx_obs['warp_streak'], V + 1, R)
            if shards:
                show(fx_obs['warp_shards'], CE, CE + 5)
            for fr in ((V,) if p.get('loop') else (V, R)):
                if fr >= PE:
                    continue
                k = json.loads(json.dumps(tmpl_fx)); k['tag'] = 'warp'
                raw = bytearray.fromhex(k['raw']); struct.pack_into('<II', raw, 8, effect, 30); k['raw'] = bytes(raw).hex()
                moveset_bl.set_event_frames(k, frame=fr)
                d['keyed'].append(k)
        done.append(name)
    moveset_bl.set_events(arm, ev)
    moveset_bl._marker_action.pop(arm.name, None)
    from . import rig_display
    rig_display.tidy_rig(arm)                       # new ghost / effect bones into their (hidden) collections
    return ob, done


# ---------------------------------------------------------------- key-pose moves (dfx/keyanim.py)
def key_moves():
    # The key-pose files are per character (dfx/moves_<name>.py) and are not part of the public tree;
    # whichever are present are offered.
    moves = {}
    try:
        from .dfx import moves_noctis
        moves['sword_combo'] = ('Sword ground combo (0xaa)', moves_noctis.SWORD_COMBO)
    except ImportError:
        pass
    try:
        from .dfx import moves_yshtola
        moves['yshtola_blizzard'] = ("Y'shtola Blizzard chain (0x2a5/0x2a6)", moves_yshtola.BLIZZARD_CHAIN)
    except ImportError:
        pass
    return moves


def apply_key_move(context, arm, weapon_ob, move='sword_combo', effect=0x3E):
    """Rebuild the move's animations from key poses (dfx/moves_noctis.py), held in `weapon_ob`'s hand,
    and make that weapon the one shown during them. Re-applying is safe: the body is rebuilt each time.
    Returns the names of the rewritten actions."""
    from .dfx import keyanim
    from .dfx.motion_encode import build_motion
    from .importer import actions_from_motions, _normalised
    moves = key_moves()
    if move not in moves:
        raise RuntimeError(f'key-pose move {move!r} is not installed (dfx/moves_<character>.py)')
    spec = moves[move][1]
    hand = hand_bone_of(weapon_ob)
    if hand is None:
        raise RuntimeError(f'{weapon_ob.name} has no vertex groups')
    ch = Character.load(bytes(arm['dfx_template']))
    abones = parse_bones(ch.anim_model)
    names = [b.name for b in abones]
    if hand not in names:
        raise RuntimeError(f'{hand} is not a game bone')
    blade, _ = _blade_of(arm, weapon_ob, hand)       # measured on the weapon in the slot now
    rig = keyanim.Rig(abones, ch.motions, names.index(hand), blade)
    worlds = [_normalised(np.array(arm.data.bones[n].matrix_local)) for n in names]
    parents = [b.parent for b in abones]
    by_index = {a.get('dfx_motion_index'): a for a in moveset_bl.actions_for(arm)}
    ev = moveset_bl.get_events(arm)
    tmpl_show = next((e for d in ev.values() for e in d.get('ranged', []) if e['op'] == moveset_bl.SHOW), None)
    tmpl_fx = next((e for d in ev.values() for e in d.get('keyed', []) if e['op'] == moveset_bl.OWN_FX), None)
    done = []
    for mname, st in spec.items():
        mi = next((i for i, m in enumerate(ch.motions) if m.name == mname), None)
        act = by_index.get(mi)
        if mi is None or act is None:
            continue
        m = ch.motions[mi]
        frames, tr, _ = rig.author(st['keys'], m)
        mot = build_motion(m.name, frames, tr, m.frame_rate, extra=m.extra, header=m.header,
                           frame_start=0.0, frame_end=m.frame_end)
        mot.name = act.name
        new = actions_from_motions([mot], abones, parents, names, worlds, arm, index_base=mi, replace=True)[0]
        new['dfx_rig'] = False                      # tool output plays on the game bones
        d = ev.get(moveset_bl.action_key(new))
        if d and d.get('ok') and tmpl_show and 'show' in st:   # moves without 'show' keep their own weapon events
            # only this weapon is shown during the move; the dismiss sparkle where it goes away
            d['ranged'] = [e for e in d['ranged'] if e['op'] != moveset_bl.SHOW]
            d['keyed'] = [e for e in d['keyed'] if e.get('tag') != 'keymove']
            e = json.loads(json.dumps(tmpl_show)); e.update(tag='keymove', show_object=weapon_ob.name)
            moveset_bl.set_event_frames(e, start=st['show'][0], end=st['show'][1])
            d['ranged'].append(e)
            for key in ('summon', 'dismiss'):
                if key in st and tmpl_fx:
                    k = json.loads(json.dumps(tmpl_fx)); k['tag'] = 'keymove'
                    raw = bytearray.fromhex(k['raw']); struct.pack_into('<II', raw, 8, effect, 30); k['raw'] = bytes(raw).hex()
                    moveset_bl.set_event_frames(k, frame=st[key])
                    d['keyed'].append(k)
        done.append(new.name)
    if not done:
        raise RuntimeError('none of the move\'s animations are on this armature')
    moveset_bl.set_events(arm, ev)
    moveset_bl._marker_action.pop(arm.name, None)
    return done


# ---------------------------------------------------------------- reference footage
def add_reference_clip(context, path, start_frame=0, name='Reference clip', width=2.4):
    """Show a video, or an image sequence (pick its first frame), next to the character and behind it in a
    'Reference camera' view, synced to the timeline: scene frame `start_frame` shows the clip's first frame.
    Returns (image, plane object, camera object)."""
    import os
    sc = context.scene
    img = bpy.data.images.load(path, check_existing=True)
    ext = os.path.splitext(path)[1].lower()
    if ext in ('.mov', '.mp4', '.avi', '.mkv', '.webm', '.m4v'):
        img.source = 'MOVIE'
    else:
        img.source = 'SEQUENCE'
    folder = os.path.dirname(path)
    if img.source == 'SEQUENCE':
        n = len([f for f in os.listdir(folder) if os.path.splitext(f)[1].lower() == ext])
    else:
        n = img.frame_duration or 1000

    def sync(iu):
        iu.frame_duration = n
        iu.frame_start = start_frame          # scene frame that shows the first picture
        iu.frame_offset = 0
        iu.use_auto_refresh = True
        iu.use_cyclic = False
    w, h = (img.size[0], img.size[1]) if img.size[0] else (16, 9)
    # a plane beside him (to his left, facing back towards the reference camera)
    ob = bpy.data.objects.get(name)
    if ob is None:
        me = bpy.data.meshes.new(name)
        hw, hh = width / 2, width * h / w / 2
        me.from_pydata([(-hw, -hh, 0), (hw, -hh, 0), (hw, hh, 0), (-hw, hh, 0)], [], [(0, 1, 2, 3)])
        me.uv_layers.new(name='UVMap')
        for li, uv in zip(range(4), ((0, 0), (1, 0), (1, 1), (0, 1))):
            me.uv_layers[0].data[li].uv = uv
        ob = bpy.data.objects.new(name, me)
        sc.collection.objects.link(ob)
        ob.location = (2.6, -0.4, width * h / w / 2 + 0.05)
        ob.rotation_euler = (math.radians(90), 0, math.radians(180))
    mat = bpy.data.materials.get(name) or bpy.data.materials.new(name)
    mat.use_nodes = True
    nt = mat.node_tree; nt.nodes.clear()
    tex = nt.nodes.new('ShaderNodeTexImage'); tex.image = img; tex.interpolation = 'Linear'
    sync(tex.image_user)
    em = nt.nodes.new('ShaderNodeEmission'); out = nt.nodes.new('ShaderNodeOutputMaterial')
    nt.links.new(tex.outputs['Color'], em.inputs['Color']); nt.links.new(em.outputs['Emission'], out.inputs['Surface'])
    ob.data.materials.clear(); ob.data.materials.append(mat)
    # a camera behind his right shoulder with the clip as its background (View > Cameras > Active Camera)
    cam = bpy.data.objects.get('Reference camera')
    if cam is None:
        cam = bpy.data.objects.new('Reference camera', bpy.data.cameras.new('Reference camera'))
        sc.collection.objects.link(cam)
        cam.location = Vector((-1.5, 3.7, 1.7))
        cam.rotation_euler = (Vector((0.0, -0.4, 0.95)) - cam.location).to_track_quat('-Z', 'Y').to_euler()
        cam.data.lens = 35
    cam.data.background_images.clear()
    bg = cam.data.background_images.new()
    bg.image = img; bg.alpha = 0.6; bg.display_depth = 'BACK'; bg.frame_method = 'FIT'
    sync(bg.image_user)
    cam.data.show_background_images = True
    # solid view shows textures, so the plane plays in the normal viewport too
    for scr in bpy.data.screens:
        for area in scr.areas:
            if area.type == 'VIEW_3D':
                for sp in area.spaces:
                    if sp.type == 'VIEW_3D':
                        sp.shading.color_type = 'TEXTURE'
    return img, ob, cam


# ---------------------------------------------------------------- retarget
def retarget_into(arm, src_path, only=None):
    from .dfx.retarget import retarget
    from .importer import actions_from_motions, _normalised
    src = Character.load(open(src_path, 'rb').read())
    dst = Character.load(bytes(arm['dfx_template']))
    motions = retarget(src, dst, log=lambda *a: None)
    if only:
        motions = [m for m in motions if m.name in only]
    names = [b.name for b in dst.bones]
    worlds = [_normalised(np.array(arm.data.bones[n].matrix_local)) for n in names]
    existed = {m.name for m in motions if bpy.data.actions.get(m.name) and bpy.data.actions[m.name].get('dfx_armature') == arm.name}
    acts = actions_from_motions(motions, parse_bones(dst.anim_model), [b.parent for b in dst.bones], names, worlds, arm,
                                index_base=0, replace=True)
    for a in acts:
        if a.name not in existed and 'dfx_motion_index' in a:
            del a['dfx_motion_index']            # brand-new animation slot
    return [a.name for a in acts]


# ---------------------------------------------------------------- copy animations to another costume
def copy_to_costume(arm, target_path, out_path, action_names=None):
    """Export this armature's animations into another costume's GMO (bones matched by name)."""
    import tempfile, os
    from . import exporter
    from .dfx.motion_encode import sample_motion, build_motion
    from .dfx.retarget import with_motions
    tmp = os.path.join(tempfile.gettempdir(), 'dfx_costume_src.gmo')
    exporter.export_gmo(arm, tmp)
    src = Character.load(open(tmp, 'rb').read())
    tgt = Character.load(open(target_path, 'rb').read())
    sb, tb = parse_bones(src.anim_model), parse_bones(tgt.anim_model)
    tidx = {b.name: i for i, b in enumerate(tb)}
    missing = sorted({b.name for b in sb} - set(tidx))
    out = list(tgt.motions)
    by_name = {m.name: i for i, m in enumerate(out)}
    copied = []
    for m in src.motions:
        if action_names and m.name not in action_names:
            continue
        # from frame 0: FrameLoop's start is the loop start, the frames before it play too (importer.actions_from_motions)
        fr = np.arange(min(0, int(np.floor(m.frame_start))), int(np.ceil(m.frame_end)) + 1, dtype=float)
        tr = sample_motion(m, sb, fr)
        tr2 = {tidx[sb[i].name]: v for i, v in tr.items() if sb[i].name in tidx}
        nm = build_motion(m.name, fr, tr2, m.frame_rate, extra=m.extra, header=m.header,
                          frame_start=m.frame_start, frame_end=m.frame_end)
        if m.name in by_name:
            out[by_name[m.name]] = nm
        else:
            out.append(nm)
        copied.append(m.name)
    open(out_path, 'wb').write(with_motions(tgt, out))
    return copied, missing


# ---------------------------------------------------------------- weapon slots
NOCTIS_KIT = {'wep15': 'sword', 'wep12': 'spear', 'wep13': 'greatsword', 'wep18': 'shuriken',
              'wep14': 'dagger', 'wep5': 'dagger', 'wep6': 'dagger', 'wep10': 'shield'}
# Bartz's twin-blade move (_631A) shows wep5 + wep14 for its first frames, then wep6 (one long
# double blade in the right hand) for the rest, so wep6 gets a dagger too.
FLAT_SLOTS = {'wep10', 'wep18'}


def _bone_world(arm, name):
    from .importer import _normalised
    return _normalised(np.array(arm.data.bones[name].matrix_local))


def _blade_of(arm, weapon_ob, hand):
    """Blade direction (grip -> farthest point) and length, in the hand bone's frame."""
    loc = _slot_local(arm, weapon_ob, hand)
    far = loc[np.argmax(np.linalg.norm(loc, axis=1))]
    d = far / np.linalg.norm(far)
    return d, float(np.max(loc @ d))


def _group_mask(ob, group):
    """Vertices of `ob` weighted mostly to vertex group `group`."""
    vg = ob.vertex_groups.get(group)
    if vg is None:
        return None
    m = np.zeros(len(ob.data.vertices), bool)
    for v in ob.data.vertices:
        for g in v.groups:
            if g.group == vg.index and g.weight > 0.5:
                m[v.index] = True
    return m


def _find_grip(L, n0, n1, bins=48):
    """Handle centre along the axis (L columns: along, side, thickness; tip at +). The guard is the
    widest point in the hilt half; the hand sits on the handle between the pommel and the guard."""
    span = n1 - n0
    if span <= 0:
        return None
    t = (L[:, 0] - n0) / span
    r = np.linalg.norm(L[:, 1:], axis=1)
    edges = np.linspace(0, 1, bins + 1)
    w = np.array([r[(t >= a) & (t < b)].max(initial=0) for a, b in zip(edges[:-1], edges[1:])])
    half = bins * 45 // 100
    g = int(np.argmax(w[:half]))
    filled = w[:half][w[:half] > 0]
    if g < 2 or not len(filled) or w[g] < 1.4 * np.median(filled):
        return None
    # the handle: the longest run of narrow bins below the guard (empty bins count: a plain
    # cylinder handle often has vertices only at its ends); the hand goes in its middle
    thin = w[:g] <= 0.6 * w[g]
    best, run = (0, 0), None
    for i, th in enumerate(list(thin) + [False]):
        if th and run is None:
            run = i
        elif not th and run is not None:
            if i - run > best[1] - best[0]:
                best = (run, i)
            run = None
    if best[1] - best[0] < 2:
        return None
    lo, hi = edges[best[0]], edges[best[1]]
    return n0 + 0.5 * (lo + hi) * span


def _slot_local(arm, slot_ob, hand):
    """The slot's current vertices in the holding bone's space."""
    me = slot_ob.data
    co = np.empty(len(me.vertices) * 3, np.float32); me.vertices.foreach_get('co', co)
    co = np.c_[co.reshape(-1, 3), np.ones(len(me.vertices))]
    return (co @ np.linalg.inv(_bone_world(arm, hand)).T)[:, :3]


def _install_mesh(arm, slot_ob, hand, me):
    """Swap a slot's mesh for `me` (vertices in the hand bone's space), rigid on that bone.
    The part keeps its slot, so every move that shows it now shows the new weapon; the exporter
    re-encodes it because its mesh signature changed. The warp dash's thrown copy follows."""
    M = _bone_world(arm, hand)
    co = np.empty(len(me.vertices) * 3, np.float32); me.vertices.foreach_get('co', co)
    co = np.c_[co.reshape(-1, 3), np.ones(len(me.vertices))] @ M.T
    me.vertices.foreach_set('co', co[:, :3].astype(np.float32).reshape(-1))
    me.update()
    old = slot_ob.data
    slot_ob.data = me
    slot_ob.vertex_groups.clear()
    slot_ob.vertex_groups.new(name=hand).add(list(range(len(me.vertices))), 1.0, 'REPLACE')
    if old.users == 0:
        bpy.data.meshes.remove(old)
    thrown = bpy.data.objects.get('warp_sword_mesh')
    if thrown is not None and slot_ob.name == thrown.get('dfx_copy_of', 'wep15'):
        tme = me.copy(); oldt = thrown.data; thrown.data = tme
        thrown.vertex_groups.clear()
        thrown.vertex_groups.new(name='warp_sword').add(list(range(len(tme.vertices))), 1.0, 'REPLACE')
        if oldt.users == 0:
            bpy.data.meshes.remove(oldt)


def _mesh_from_arrays(name, V, F, cells, img):
    from .dfx import placeholders as ph
    me = bpy.data.meshes.new(name)
    me.from_pydata([tuple(v) for v in V], [], [tuple(f) for f in F])
    uvl = me.uv_layers.new(name='UVMap')
    col = me.color_attributes.new('Col', 'BYTE_COLOR', 'CORNER')
    for poly, cell in zip(me.polygons, cells):
        u, v = ph.cell_uv(cell)
        for li in poly.loop_indices:
            uvl.data[li].uv = (u, 1.0 - v)
            col.data[li].color = (1, 1, 1, 1)
    me.materials.append(common.make_material(name, img))
    return me


def install_placeholders(arm, kit=None):
    """Give each slot in `kit` (slot object name -> shape) a plain placeholder weapon."""
    from .dfx import placeholders as ph
    kit = kit or NOCTIS_KIT
    img = bpy.data.images.get('noctis_placeholder.tga') or common.image_from_rgba('noctis_placeholder.tga', ph.palette_rgba())
    done = []
    for slot, kind in kit.items():
        ob = bpy.data.objects.get(slot)
        if ob is None or ob.parent != arm:
            continue
        hand = hand_bone_of(ob)
        V, F, C = ph.fit_to_slot(kind, _slot_local(arm, ob, hand))
        _install_mesh(arm, ob, hand, _mesh_from_arrays(f'{slot}_{kind}', V, F, C, img))
        ob['dfx_weapon_kind'] = kind
        done.append(f'{slot}={kind}')
    return done


def replace_weapon_from_file(context, arm, slot_ob, path, flip=False, keep_scale=False, max_tex=256, max_tris=1200, length=0.0,
                             grip_at_origin=False):
    """Import a weapon model (FBX / glTF / OBJ) into a slot. Its long axis is lined up with the old
    weapon's; the wider end is taken as the hilt (flip to reverse), and the model is fitted pommel-to-tip
    over the old weapon's span (keep_scale keeps its own size). Detailed models are decimated to about
    max_tris and textures shrunk to max_tex. Shields and the shuriken are fitted as discs.
    grip_at_origin: the hand goes where the model's own origin is (game rips such as War of the Visions or
    FFXV weapons are authored with the origin at the grip) instead of the handle found from the shape."""
    import os
    from .dfx import placeholders as ph
    ext = os.path.splitext(path)[1].lower()
    before = set(bpy.data.objects)
    if ext == '.fbx':
        bpy.ops.import_scene.fbx(filepath=path)
    elif ext in ('.glb', '.gltf'):
        bpy.ops.import_scene.gltf(filepath=path)
    elif ext == '.obj':
        bpy.ops.wm.obj_import(filepath=path)
    else:
        raise RuntimeError('use an .fbx, .glb/.gltf or .obj file')
    new = [o for o in bpy.data.objects if o not in before]
    meshes = [o for o in new if o.type == 'MESH']
    if not meshes:
        for o in new:
            bpy.data.objects.remove(o, do_unlink=True)
        raise RuntimeError('no mesh in that file')
    # one mesh, in the imported root's space (origin = grip)
    root = next((o for o in new if o.parent is None), meshes[0])
    inv_root = root.matrix_world.inverted()
    import bmesh
    bm = bmesh.new()
    mats, slot_of = [], {}
    for o in meshes:
        tmp = o.data.copy()
        tmp.transform(inv_root @ o.matrix_world)
        for m in tmp.materials:
            if m is not None and m.name not in slot_of:
                slot_of[m.name] = len(mats); mats.append(m)
        remap = [slot_of.get(m.name, 0) if m is not None else 0 for m in tmp.materials]
        for p in tmp.polygons:
            p.material_index = remap[p.material_index] if remap else 0
        bm.from_mesh(tmp)
        bpy.data.meshes.remove(tmp)
    bmesh.ops.remove_doubles(bm, verts=bm.verts, dist=1e-5)     # scanned/generated meshes are often unwelded
    me = bpy.data.meshes.new(slot_ob.name + '_import')
    bm.to_mesh(me); bm.free()
    _decimate(me, max_tris)
    for m in mats:
        img = _base_colour_image(m)                   # PSP materials are one texture: keep the colour map only
        me.materials.append(common.make_material(slot_ob.name + '_' + m.name[:20], img))
        if img is not None and max(img.size) > max_tex:
            k = max_tex / max(img.size)
            img.scale(max(8, int(img.size[0] * k)), max(8, int(img.size[1] * k)))
    for o in new:
        bpy.data.objects.remove(o, do_unlink=True)
    keep = {_base_colour_image(m2) for m2 in me.materials}
    for m in mats:                                    # the file's own materials and their extra maps
        imgs = {n.image for n in (m.node_tree.nodes if m.node_tree else []) if n.type == 'TEX_IMAGE' and n.image}
        bpy.data.materials.remove(m)
        for img in imgs - keep:
            bpy.data.images.remove(img)
    # line up with the slot
    hand = hand_bone_of(slot_ob)
    old = _slot_local(arm, slot_ob, hand)
    # leave out tassels / ribbons skinned to the slot's second bone (e.g. r_wep_acc): they hang behind the hand
    own = _group_mask(slot_ob, hand)
    if own is not None and own.sum() >= 0.5 * len(old):
        old = old[own]
    P = np.empty(len(me.vertices) * 3, np.float32); me.vertices.foreach_get('co', P); P = P.reshape(-1, 3).astype(float)
    c_new, ax_new, _ = ph.pca(P)
    c_old, ax_old, _ = ph.pca(old)
    if slot_ob.name in FLAT_SLOTS or slot_ob.get('dfx_weapon_kind') in ph.FLAT:
        rad_new = np.percentile(np.linalg.norm((P - c_new) @ ax_new[:, :2], axis=1), 95)
        rad_old = np.percentile(np.linalg.norm((old - c_old) @ ax_old[:, :2], axis=1), 95)
        k = 1.0 if keep_scale else rad_old / max(rad_new, 1e-6)
        Rn = np.stack([ax_new[:, 0], ax_new[:, 1], np.cross(ax_new[:, 0], ax_new[:, 1])], 1)
        Ro = np.stack([ax_old[:, 0], ax_old[:, 1], np.cross(ax_old[:, 0], ax_old[:, 1])], 1)
        Q = ((P - c_new) @ Rn) * k @ Ro.T + c_old
    else:
        # the hilt is the wider end of the long axis; the model is fitted pommel-to-tip over the old
        # weapon's span along its axis, so the hand lands on the grip whatever the model's origin is
        a_new = ax_new[:, 0]
        t = (P - c_new) @ a_new; t0, t1 = t.min(), t.max(); L = t1 - t0
        radial = np.linalg.norm((P - c_new) - np.outer(t, a_new), axis=1)
        lo = radial[t < t0 + 0.35 * L].max(initial=0); hi = radial[t > t1 - 0.35 * L].max(initial=0)
        if hi > lo:                                   # hilt at the + end: point the tip the other way
            a_new = -a_new
        if flip:
            a_new = -a_new
        far = old[np.argmax(np.linalg.norm(old, axis=1))]
        a_old = far / np.linalg.norm(far)             # grip -> farthest point: the axis the warp dash uses
        side = lambda ax, s_: (lambda v: v / np.linalg.norm(v))(s_ - (s_ @ ax) * ax)
        Rn = np.stack([a_new, side(a_new, ax_new[:, 1]), np.cross(a_new, side(a_new, ax_new[:, 1]))], 1)
        Ro = np.stack([a_old, side(a_old, ax_old[:, 1]), np.cross(a_old, side(a_old, ax_old[:, 1]))], 1)
        L_new = (P - c_new) @ Rn                      # columns: along, side, thickness
        n0, n1 = L_new[:, 0].min(), L_new[:, 0].max()
        o = old @ a_old; o0, o1 = o.min(), o.max()
        k = 1.0 if keep_scale else ((length if length > 0 else (o1 - o0)) / max(n1 - n0, 1e-6))
        grip = float((-c_new) @ a_new) if grip_at_origin else _find_grip(L_new, n0, n1)   # where the hand goes
        if grip is None:                              # no clear guard: keep the old weapon's grip fraction
            grip = n0 + (-o0 / max(o1 - o0, 1e-6)) * (n1 - n0)
        base = c_old - (c_old @ a_old) * a_old        # the old weapon's axis line, at the hand
        L_new[:, 0] = (L_new[:, 0] - grip) * k        # the handle on the hand
        L_new[:, 1:] *= k
        Q = base + L_new @ Ro.T
    me.vertices.foreach_set('co', Q.astype(np.float32).reshape(-1))
    if not me.color_attributes:
        ca = me.color_attributes.new('Col', 'BYTE_COLOR', 'CORNER')
        for c in ca.data:
            c.color = (1, 1, 1, 1)
    _install_mesh(arm, slot_ob, hand, me)
    return len(me.vertices), k


def _material_image_bl(mat):
    if mat is None or not mat.node_tree:
        return None
    return next((n.image for n in mat.node_tree.nodes if n.type == 'TEX_IMAGE' and n.image), None)


def _decimate(me, max_tris):
    """Reduce a mesh to about max_tris triangles (PSP weapons are a few hundred at most); keeps UVs."""
    tris = sum(len(p.vertices) - 2 for p in me.polygons)
    if tris <= max_tris:
        return tris
    tmp = bpy.data.objects.new('_dfx_decimate', me)
    bpy.context.scene.collection.objects.link(tmp)
    mod = tmp.modifiers.new('dec', 'DECIMATE')
    mod.decimate_type = 'COLLAPSE'; mod.ratio = max_tris / tris; mod.use_collapse_triangulate = True
    dg = bpy.context.evaluated_depsgraph_get()
    new = bpy.data.meshes.new_from_object(tmp.evaluated_get(dg))
    bpy.data.objects.remove(tmp, do_unlink=True)
    me.clear_geometry()
    import bmesh
    bm = bmesh.new(); bm.from_mesh(new); bm.to_mesh(me); bm.free()
    for m in new.materials:
        pass
    bpy.data.meshes.remove(new)
    return sum(len(p.vertices) - 2 for p in me.polygons)


def _base_colour_image(mat):
    """The image feeding a material's base colour (not its normal/roughness maps)."""
    if mat is None or not mat.node_tree:
        return None
    nt = mat.node_tree
    bsdf = next((n for n in nt.nodes if n.type == 'BSDF_PRINCIPLED'), None)
    if bsdf is not None:
        stack = [l.from_node for l in bsdf.inputs['Base Color'].links]
        while stack:
            n = stack.pop()
            if n.type == 'TEX_IMAGE' and n.image:
                return n.image
            stack += [l.from_node for i in n.inputs for l in i.links]
    imgs = [n.image for n in nt.nodes if n.type == 'TEX_IMAGE' and n.image]
    return next((i for i in imgs if not any(w in i.name.lower() for w in ('normal', '_rm', 'rough', 'metal'))), imgs[0] if imgs else None)


# ---------------------------------------------------------------- retarget from FBX / glTF (another game's rig)
FOREIGN_FORMATS = ('.fbx', '.glb', '.gltf')
_ID_KINDS = ('objects', 'meshes', 'armatures', 'materials', 'images', 'actions', 'cameras', 'lights',
             'collections', 'textures', 'node_groups', 'curves')


def _id_snapshot():
    return {k: {i.as_pointer() for i in getattr(bpy.data, k)} for k in _ID_KINDS}


def _import_foreign(path):
    """Import an FBX / glTF. -> (new datablocks per kind (lists), snapshot before)."""
    before = _id_snapshot()
    ext = os.path.splitext(path)[1].lower()
    if ext == '.fbx':
        bpy.ops.import_scene.fbx(filepath=path, use_anim=True, anim_offset=0.0)
    elif ext in ('.glb', '.gltf'):
        bpy.ops.import_scene.gltf(filepath=path)
    else:
        raise RuntimeError(f'Unsupported file type {ext}: use .fbx, .glb or .gltf')
    new = {k: [i for i in getattr(bpy.data, k) if i.as_pointer() not in before[k]] for k in _ID_KINDS}
    return new


def _remove_imported(new):
    for ob in new['objects']:
        if ob.name in bpy.data.objects:
            bpy.data.objects.remove(ob, do_unlink=True)
    for kind in ('collections', 'meshes', 'armatures', 'materials', 'images', 'actions', 'cameras', 'lights',
                 'textures', 'node_groups', 'curves'):
        for i in new[kind]:
            try:
                getattr(bpy.data, kind).remove(i)
            except (ReferenceError, RuntimeError):
                pass


def _action_has_curves(act):
    for slot in act.slots:
        cb = anim_utils.action_get_channelbag_for_slot(act, slot)
        if cb is not None and len(cb.fcurves):
            return True
    return False


def _use_action(ob, act):
    ad = ob.animation_data_create()
    for t in ad.nla_tracks:                       # glTF puts its clips in the NLA; only the active action is wanted
        t.mute = True
    ad.action = act
    if act.slots:
        try:
            ad.action_slot = act.slots[0]
        except (RuntimeError, TypeError):
            pass


def _hold_pose(act, names, edit_local, frame):
    """Game-local T, R(xyzw), S of every bone in `act` at `frame` (bones without keys: the bind pose)."""
    cb = anim_utils.action_get_channelbag_for_slot(act, act.slots[0]) if act.slots else None
    cur = {}
    for fc in (cb.fcurves if cb else []):
        if fc.data_path.startswith('pose.bones["'):
            cur[(fc.data_path.split('"')[1], fc.data_path.rsplit('.', 1)[1], fc.array_index)] = fc

    def val(n, prop, k, default):
        fc = cur.get((n, prop, k))
        return fc.evaluate(frame) if fc is not None else default
    nb = len(names)
    T, R, S = np.zeros((nb, 3)), np.zeros((nb, 4)), np.ones((nb, 3))
    for i, n in enumerate(names):
        loc = np.array([[val(n, 'location', k, 0.0) for k in range(3)]])
        rot = np.array([[val(n, 'rotation_quaternion', k, 1.0 if k == 0 else 0.0) for k in range(4)]])
        rot /= np.linalg.norm(rot)
        scl = np.array([[val(n, 'scale', k, 1.0) for k in range(3)]])
        t, r, s = common.basis_to_local_trs(loc, rot, scl, edit_local[i])
        T[i], R[i], S[i] = t[0], r[0], s[0]
    return T, R, S


def foreign_bone_names(arm_ob):
    """Bone names and parent indices of a foreign armature, ignoring nothing."""
    names = [b.name for b in arm_ob.data.bones]
    idx = {n: i for i, n in enumerate(names)}
    return names, [idx[b.parent.name] if b.parent else -1 for b in arm_ob.data.bones]


def retarget_from_file(context, arm, path, name='', action_name='', all_actions=False, frame_start=0, frame_end=0,
                       src_fps=0.0, out_fps=30.0, rest_align=True, orient='AUTO', yaw=0.0, translation='FULL',
                       scale=0.0, foot_fix=True, start_at_origin=True, base_action='', review=True, travel_bone='',
                       log=print):
    """Bring an animation from another game's rig (FBX / glTF) onto this Dissidia armature as a new game-keyed
    action.  The bone map comes from `<file>.bonemap.json` (written from a guess when it does not exist yet).
    With review=True a freshly guessed map that is unsure (required slots missing, or low confidence) stops
    before retargeting so the JSON can be fixed first.

    translation='IN_PLACE' keeps the hips' own sway and height but takes off the clip's travel, read from the
    source's root bone (the hips' ancestor that moves the most: FFXV `Trans`, a Mixamo root).  With travel_bone
    (e.g. 'trans_include', the game's root-motion bone) that travel is written onto that bone instead, which is
    how the game's own motions carry their travel.

    Returns {'actions': [names], 'bonemap': BoneMap, 'map_path', 'created', 'stopped', 'notes'}."""
    from .dfx import bonemap, retarget_ext
    from .importer import _normalised
    path = bpy.path.abspath(path)
    map_path = path + '.bonemap.json'
    ch = Character.load(bytes(arm['dfx_template']))
    dnames = [b.name for b in ch.bones]
    dparent = [b.parent for b in ch.bones]
    scene = context.scene
    keep_state = dict(frame=scene.frame_current, fps=scene.render.fps, fps_base=scene.render.fps_base,
                      start=scene.frame_start, end=scene.frame_end, active=context.view_layer.objects.active,
                      selected=[o for o in scene.objects if o.select_get()], act=arm.animation_data.action if arm.animation_data else None)
    handler_off = moveset_bl.frame_handler in bpy.app.handlers.frame_change_post
    if handler_off:
        bpy.app.handlers.frame_change_post.remove(moveset_bl.frame_handler)
    if context.mode != 'OBJECT':
        bpy.ops.object.mode_set(mode='OBJECT')
    new = _import_foreign(path)
    result = {'actions': [], 'map_path': map_path, 'created': False, 'stopped': False, 'notes': []}
    samples = []                                     # (action name, frames, src_world, T, Q, S, driven)
    try:
        srcs = [o for o in new['objects'] if o.type == 'ARMATURE']
        if not srcs:
            raise RuntimeError('The file has no armature')
        src = max(srcs, key=lambda o: len(o.data.bones))
        src.data.pose_position = 'POSE'
        snames, sparent = foreign_bone_names(src)
        bm, created = bonemap.load_or_create(map_path, snames, dnames, sparent, dparent)
        result['bonemap'], result['created'] = bm, created
        log(bm.describe())
        if review and created and (bm.missing_required or bm.confidence < 0.85):
            result['stopped'] = True
            result['notes'].append(f'The guessed bone map looks incomplete; edit {map_path} and run again.')
            return result
        pairs = bm.pairs()
        if not any(p[0] == 'hips' for p in pairs):
            raise RuntimeError('The bone map has no hips slot')
        uniq = sorted({a for _, a, _ in pairs})
        sidx = {n: i for i, n in enumerate(uniq)}
        didx = {n: i for i, n in enumerate(dnames)}
        slots = {s: (sidx[a], didx[b]) for s, a, b in pairs if s}
        extras = [(sidx[a], didx[b]) for s, a, b in pairs if not s]

        # --- source clips
        acts = [a for a in new['actions'] if _action_has_curves(a)]
        if action_name:
            acts = [a for a in acts if a.name == action_name] or acts
        if not acts:
            raise RuntimeError('The file contains no animation')
        if not all_actions:
            cur = src.animation_data.action if src.animation_data else None
            acts = [cur if cur in acts else max(acts, key=lambda a: a.frame_range[1] - a.frame_range[0])]
        X = np.array(arm.matrix_world.inverted() @ src.matrix_world)
        srest = np.array([X @ np.array(src.data.bones[n].matrix_local) for n in uniq])
        worlds = [_normalised(np.array(arm.data.bones[n].matrix_local)) for n in dnames]
        edit_local = [np.linalg.inv(worlds[p]) @ worlds[i] if p >= 0 else worlds[i] for i, p in enumerate(dparent)]
        inv_edit = [np.linalg.inv(m) for m in edit_local]
        hold = None
        base = bpy.data.actions.get(base_action) if base_action else None
        if base is not None:
            hold = _hold_pose(base, dnames, edit_local, base.frame_range[0])
        opts = retarget_ext.Options(rest_align=rest_align, orient='auto' if orient == 'AUTO' else 'none', yaw=yaw,
                                    translation=translation.lower(), scale=scale, foot_fix=foot_fix,
                                    start_at_origin=start_at_origin)
        # ancestors of the source hips: the one that travels the most is the clip's root (travel) bone
        hips_src = next(a for s_, a, _ in pairs if s_ == 'hips')
        anc = []
        b = src.data.bones[hips_src].parent
        while b is not None:
            anc.append(b.name); b = b.parent
        fps_src = src_fps if src_fps > 0 else scene.render.fps / scene.render.fps_base
        for act in acts:
            _use_action(src, act)
            f0, f1 = act.frame_range
            if frame_end > frame_start:
                f0, f1 = max(f0, frame_start), min(f1, frame_end)
            step = fps_src / out_fps                        # source frames per game frame
            n = int(math.floor((f1 - f0) / step + 1e-6)) + 1
            fsrc = f0 + np.arange(n) * step
            W = np.empty((n, len(uniq), 4, 4))
            # travel candidates: the hips' ancestors, then the armature object itself (FBX importers often turn
            # the root bone, e.g. FFXV `Trans`, into the object, so its travel is object animation)
            A = np.zeros((n, len(anc) + 1, 3))
            arm_inv = arm.matrix_world.inverted()
            for k, f in enumerate(fsrc):
                fi = math.floor(f)
                scene.frame_set(fi, subframe=float(f - fi))
                ev = src.evaluated_get(context.evaluated_depsgraph_get())
                Xk = np.array(arm_inv @ ev.matrix_world)         # per frame: the object may be animated
                for j, bn in enumerate(uniq):
                    W[k, j] = Xk @ np.array(ev.pose.bones[bn].matrix)
                for j, bn in enumerate(anc):
                    A[k, j] = (Xk @ np.array(ev.pose.bones[bn].matrix))[:3, 3]
                A[k, -1] = Xk[:3, 3]
            opts.travel = None
            if opts.translation == 'in_place':
                up = np.asarray(opts.up, np.float64)
                rel = A - A[:1]                                   # motion since the first frame
                hz = rel - (rel @ up)[..., None] * up
                reach = np.linalg.norm(hz, axis=-1).max(0)
                j = int(np.argmax(reach))
                if reach[j] > 1e-6:
                    opts.travel = A[:, j]
                    who = anc[j] if j < len(anc) else f'{src.name} (object)'
                    log(f'[retarget] {act.name}: travel taken off the hips from {who} ({reach[j]:.3f} units)')
            res = retarget_ext.retarget(W, srest, np.array(worlds), dparent, slots, extras, hold, opts)
            result['notes'] += res.notes
            samples.append((act.name, n, res))
            log(f'[retarget] {act.name}: {n} frames at {out_fps:g} fps, scale {res.scale:.3f}, '
                f'{int(res.driven.sum())} bones driven, foot lift max {res.lift.max():.3f} m')
    finally:
        _remove_imported(new)
        if handler_off:
            bpy.app.handlers.frame_change_post.append(moveset_bl.frame_handler)
        scene.render.fps, scene.render.fps_base = keep_state['fps'], keep_state['fps_base']
        scene.frame_start, scene.frame_end = keep_state['start'], keep_state['end']
        for o in scene.objects:
            o.select_set(o in keep_state['selected'])
        context.view_layer.objects.active = keep_state['active']
        scene.frame_set(keep_state['frame'])

    for src_name, n, res in samples:
        label = name if name and len(samples) == 1 else (f'{name}_{src_name}' if name else src_name)
        label = label.split('|')[-1][:31]
        old = bpy.data.actions.get(label)
        if old is not None and old.get('dfx_retarget') and old.get('dfx_armature') == arm.name:
            old.name = label + '.old'
            old.use_fake_user = False
            bpy.data.actions.remove(old)
        act = bpy.data.actions.new(label)
        act.use_fake_user = True
        act.use_frame_range = True
        act.frame_start, act.frame_end = 0.0, float(n - 1)
        act['dfx_armature'] = arm.name                       # no dfx_motion_index: appended as a new motion on export
        act['dfx_frame_rate'] = float(out_fps)
        act['dfx_rig'] = False
        act['dfx_retarget'] = os.path.basename(path)
        slot = act.slots.new(id_type='OBJECT', name=arm.name)
        cb = anim_utils.action_ensure_channelbag_for_slot(act, slot)
        frames = np.arange(n, dtype=np.float64)
        if travel_bone and res.travel is not None and np.abs(res.travel).max() > 0:
            if travel_bone not in dnames:
                result['notes'].append(f'travel bone {travel_bone} is not in this skeleton; the travel was dropped')
            else:
                ti = dnames.index(travel_bone)
                tp = dparent[ti]
                Rp = res.world[:, tp, :3, :3] if tp >= 0 else np.broadcast_to(np.eye(3), (n, 3, 3))
                res.T[:, ti] = res.T[:, ti] + np.einsum('fji,fj->fi', Rp, res.travel)    # parent-local offset
                act['dfx_travel'] = [round(float(x), 4) for x in res.travel[-1]]
                log(f'[retarget] {src_name}: travel {np.linalg.norm(res.travel[-1]):.3f} put on {travel_bone}')
        for i, bn in enumerate(dnames):
            loc, rot, scl = common.local_trs_to_basis(res.T[:, i], res.Q[:, i], res.S[:, i], inv_edit[i])
            common.write_bone_fcurves(cb, bn, frames, loc, rot, scl)
        result['actions'].append(act.name)
    return result
