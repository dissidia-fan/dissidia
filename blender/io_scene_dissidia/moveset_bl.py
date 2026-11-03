"""Moveset (.objx) support inside Blender.

The moveset is stored on the armature (original bytes + an editable JSON copy of
every animation's event timeline). Events are shown as timeline markers for the
active action, can be dragged and written back, and drive a weapon-visibility
preview (show-weapon event 0x011B). Saving rebuilds the .objx in the same motion
order the GMO exporter uses, so new Actions get their own event timelines.
"""
import json
import re
import struct
import os
import bpy
from bpy_extras import anim_utils
from bpy.app.handlers import persistent

from .dfx.objx import Moveset, decode_events, encode_events, to_ticks, to_frame, hide_draw_bones

OP_LABEL = {0x042A: 'Hitbox?', 0x0200: 'Attack?', 0x0101: 'Recovery?', 0x0402: 'Move?', 0x0315: 'Effect?',
            0x0925: 'Trail?', 0x060F: 'FX end?', 0x0311: 'Sound?', 0x0303: 'Sound?', 0x0422: 'Face?',
            0x011B: 'Show weapon', 0x011C: 'Hide weapon', 0x0219: 'Own effect', 0x0214: 'Window?'}
SHOW, OWN_FX = 0x011B, 0x0219
MARK = 'e:'                       # prefix of markers this add-on owns


def label(op):
    return OP_LABEL.get(op, f'0x{op:04x}')


# ---------------------------------------------------------------- storage
def has_moveset(arm):
    return arm is not None and 'dfx_objx' in arm


def attach_moveset(arm, path_or_bytes, name=None):
    import os
    if isinstance(path_or_bytes, str):
        data = open(path_or_bytes, 'rb').read()
        name = name or os.path.basename(path_or_bytes)
    else:
        data = bytes(path_or_bytes)
    ms = Moveset.load(data)
    if 'dfx_objx' in arm:                  # Blender 5 truncates bytes written over an existing property
        del arm['dfx_objx']
    arm['dfx_objx'] = data
    arm['dfx_objx_name'] = name or 'moveset.objx'
    ev = {str(i): decode_events(b) for i, b in enumerate(ms.events)}
    for i, d in ev.items():
        if not d['ok']:
            d['raw_all'] = ms.events[int(i)].hex()
    arm['dfx_events'] = json.dumps(ev)
    # which command steps use each motion
    users = {}
    for c in ms.commands:
        for si, s in enumerate(c.steps):
            a = ms.action(s[0])
            if a is None:
                continue
            for sid, mi, ph in ms.motion_map:
                if sid == a.script_id:
                    users.setdefault(str(mi), []).append(f'0x{c.id:03x}.{si + 1}')
    for sid, mi, ph in ms.motion_map:
        users.setdefault(str(mi), [])
        if sid < 10000:
            users[str(mi)].append(f'state {sid}')
    arm['dfx_motion_users'] = json.dumps({k: sorted(set(v)) for k, v in users.items()})
    arm['dfx_motion_map'] = json.dumps(ms.motion_map)
    return ms


def get_events(arm):
    return json.loads(arm.get('dfx_events', '{}'))


def set_events(arm, ev):
    arm['dfx_events'] = json.dumps(ev)


def action_key(act):
    mi = act.get('dfx_motion_index')
    return str(mi) if mi is not None else 'new:' + act.name


def motion_users(arm, act):
    if 'dfx_motion_index' not in act:
        return []
    return json.loads(arm.get('dfx_motion_users', '{}')).get(str(act['dfx_motion_index']), [])


def actions_for(arm):
    acts = [a for a in bpy.data.actions if a.get('dfx_armature') == arm.name]
    known = sorted([a for a in acts if 'dfx_motion_index' in a], key=lambda a: a['dfx_motion_index'])
    new = sorted([a for a in acts if 'dfx_motion_index' not in a], key=lambda a: a.name)
    return known + new          # same order the GMO exporter writes motions in


def event_arg(e, k=0):
    raw = bytes.fromhex(e['raw'])
    hdr = 12 if 'start' in e else 8
    return struct.unpack_from('<I', raw, hdr + 4 * k)[0]


def set_event_frames(e, start=None, end=None, frame=None):
    """Set an event's timing, given in animation frames (stored as 60 fps timeline ticks)."""
    raw = bytearray.fromhex(e['raw'])
    if 'start' in e:
        if start is not None:
            e['start'] = to_ticks(start) & 0xFFFF; struct.pack_into('<H', raw, 0, e['start'])
        if end is not None:
            e['end'] = to_ticks(end) & 0xFFFF; struct.pack_into('<H', raw, 2, e['end'])
    elif frame is not None:
        e['frame'] = to_ticks(frame) & 0xFFFF; struct.pack_into('<H', raw, 0, e['frame'])
    e['raw'] = bytes(raw).hex()


# ---------------------------------------------------------------- markers
def clear_markers(scene):
    for m in list(scene.timeline_markers):
        if m.name.startswith(MARK):
            scene.timeline_markers.remove(m)


def refresh_markers(scene, arm, act=None):
    act = act or (arm.animation_data.action if arm.animation_data else None)
    clear_markers(scene)
    _marker_action[arm.name] = act.name if act else ''
    arm['dfx_marker_action'] = act.name if act else ''
    if act is None:
        return 0
    d = get_events(arm).get(action_key(act))
    if not d:
        return 0
    n = 0
    end_frame = int(act.frame_range[1])
    for i, e in enumerate(d.get('ranged', [])):
        lab = label(e['op'])
        if e['op'] == SHOW:
            lab += ' ' + (e.get('show_object') or f'#{event_arg(e)}')
        scene.timeline_markers.new(f'{MARK}r{i}> {lab}', frame=round(to_frame(e['start'])))
        if e['end'] != 0xFFFF:
            scene.timeline_markers.new(f'{MARK}r{i}<', frame=round(to_frame(e['end'])))
        n += 1
    for i, e in enumerate(d.get('keyed', [])):
        lab = label(e['op'])
        if e['op'] == OWN_FX:
            lab += f' #{event_arg(e)}'
        scene.timeline_markers.new(f'{MARK}k{i} {lab}', frame=round(to_frame(e['frame'])))
        n += 1
    return n


_MRE = re.compile(r'^e:([rk])(\d+)([<>]?)')


def write_markers(scene, arm):
    """Copy marker frames back into the event timeline of the action they were made for."""
    act = bpy.data.actions.get(arm.get('dfx_marker_action', ''))
    if act is None:
        return 0
    ev = get_events(arm)
    d = ev.get(action_key(act))
    if not d:
        return 0
    changed = 0
    for m in scene.timeline_markers:
        g = _MRE.match(m.name)
        if not g:
            continue
        kind, i, side = g.group(1), int(g.group(2)), g.group(3)
        if kind == 'r' and i < len(d['ranged']):
            e = d['ranged'][i]
            if side == '>' and round(to_frame(e['start'])) != m.frame:
                set_event_frames(e, start=max(0, m.frame)); changed += 1
            elif side == '<' and e['end'] != 0xFFFF and round(to_frame(e['end'])) != m.frame:
                set_event_frames(e, end=max(0, m.frame)); changed += 1
        elif kind == 'k' and i < len(d['keyed']) and round(to_frame(d['keyed'][i]['frame'])) != m.frame:
            set_event_frames(d['keyed'][i], frame=max(0, m.frame)); changed += 1
    set_events(arm, ev)
    return changed


# ---------------------------------------------------------------- weapon visibility preview
def _uses_show_events(arm):
    return arm.get('dfx_uses_show') if 'dfx_uses_show' in arm else None


SHOW_ONCE, HIDE_ONCE, HIDE = 0x010B, 0x010C, 0x011C


def part_states(d, tick, bone_of):
    """Which draw bones the game shows at this tick of a timeline: every part starts drawn; one-shot
    show / hide events (0x010B / 0x010C) switch it from their frame on; while a ranged show / hide event
    (0x011B / 0x011C) is active it wins. -> {bone: visible}; bones not mentioned are drawn."""
    st = {}
    for e in sorted((d or {}).get('keyed', []), key=lambda e: e['frame']):
        if e['op'] in (SHOW_ONCE, HIDE_ONCE) and e['frame'] <= tick:
            st[event_arg(e)] = e['op'] == SHOW_ONCE
    for e in (d or {}).get('ranged', []):
        if e['op'] in (SHOW, HIDE) and e['start'] <= tick and (e['end'] == 0xFFFF or tick < e['end']):
            b = bone_of(e)
            if b is not None:
                st[b] = e['op'] == SHOW
    return st


_EX_PARTS = {}               # mod.json path -> (mtime, its body.ex_parts)


def ex_parts(arm):
    """mod.json body.ex_parts: the weapons the game draws in EX Mode only, in place of the others (retail Cloud's
    wep1, the Ultima Weapon). The game swaps them in its own code, not the moveset, so the preview needs telling."""
    import json
    md, name = arm.get('dfx_mod_dir'), arm.get('dfx_mod_name')
    if not md or not name:
        return set()
    p = os.path.join(md, name, 'mod.json')
    try:
        t = os.path.getmtime(p)
    except OSError:
        return set()
    c = _EX_PARTS.get(p)
    if c is None or c[0] != t:
        try:
            body = json.load(open(p)).get('body') or {}
        except Exception:
            body = {}
        c = _EX_PARTS[p] = (t, set(body.get('ex_parts') or []))
    return c[1]


def update_visibility(scene, arm):
    if not has_moveset(arm) or not arm.get('dfx_wep_preview', True):
        return
    exp, ex_on = ex_parts(arm), bool(arm.get('dfx_ex_preview'))
    act = arm.animation_data.action if arm.animation_data else None
    d = get_events(arm).get(action_key(act)) if act else None
    by_name = {ob.name: ob.get('dfx_draw_bone') for ob in arm.children if ob.type == 'MESH'}
    bone_of = lambda e: by_name.get(e['show_object']) if e.get('show_object') else event_arg(e)
    st = part_states(d, to_ticks(scene.frame_current), bone_of)
    ex_only = set(arm.get('dfx_ex_only', []))           # parts the runtime shows in EX Mode only
    pb = arm.pose.bones
    for ob in arm.children:
        if ob.type != 'MESH':
            continue
        vis = ob.get('dfx_part_index') == 0 or (st.get(ob.get('dfx_draw_bone', -99), True)
                                                and ob.get('dfx_draw_bone') not in ex_only)
        part = ob.name.split('.')[0]
        if vis and exp and ob.get('dfx_part_index') != 0:   # EX weapons: only in EX Mode, and then in the others' place
            if part in exp:
                vis = ex_on
            elif ex_on and re.match(r'^wep\d+$', part):
                vis = False
        if vis and ob.get('dfx_part_index') != 0:
            # effect parts are switched off by holding their bones at scale 0
            bones = [pb[g.name] for g in ob.vertex_groups if g.name in pb]
            if bones and all(b.matrix.to_scale().length < 1e-4 for b in bones):
                vis = False
        try:
            if ob.hide_get() == vis:
                ob.hide_set(not vis)
        except RuntimeError:
            pass


_rest0 = {}                  # armature name -> bones whose game rest scale is 0


def rest_scale0_bones(arm):
    """Bones the game rests at scale 0: prop and effect bones (bow, flask, spell effects, warp dash) that only
    their own motions scale up. Read once from the import template; bones the add-on added carry dfx_rest_scale0."""
    names = _rest0.get(arm.name)
    if names is None:
        names = {b.name for b in arm.data.bones if b.get('dfx_rest_scale0')}
        tpl = arm.get('dfx_template')
        if tpl:
            try:
                from .dfx.gmo import Character, parse_bones
                ch = Character.load(bytes(tpl))
                ab = parse_bones(ch.anim_model) if ch.anim_model else ch.bones
                names |= {ch.bones[i].name for i, b in enumerate(ab)
                          if i < len(ch.bones) and max(abs(x) for x in b.scale) < 1e-6}
            except Exception as e:
                print('[dfx] rest scale 0 bones:', e)
        names = _rest0[arm.name] = sorted(n for n in names if n in arm.pose.bones)
    return names


def pose_unkeyed_rest(arm, act):
    """The game poses a bone the motion doesn't animate with its rest values; Blender keeps whatever the previous
    action left there. Put rest-scale-0 bones the action doesn't key back to scale 0, so props such as the bow
    don't float around in moves that never show them."""
    keyed = set()
    slot = act.slots[0] if act.slots else None
    cb = anim_utils.action_get_channelbag_for_slot(act, slot) if slot else None
    for fc in (cb.fcurves if cb else ()):
        if fc.data_path.startswith('pose.bones["'):
            keyed.add(fc.data_path.split('"')[1])
    for nm in rest_scale0_bones(arm):
        if nm not in keyed:
            pb = arm.pose.bones[nm]
            if tuple(pb.scale) != (0.0, 0.0, 0.0):
                pb.scale = (0.0, 0.0, 0.0)


_marker_action = {}          # armature name -> action whose markers are shown (kept out of ID data)
_rest_action = {}            # armature name -> action whose unkeyed bones were last put at rest
_pending = set()


def sync_scene(scene):
    """Refresh markers when the active action changes and apply the weapon preview.
    Runs outside depsgraph evaluation (from a timer); changing data inside a
    frame-change handler can crash Blender."""
    for arm in scene.objects:
        if arm.type == 'ARMATURE':
            try:
                from . import rig_control
                rig_control.sync_action(arm)          # control rig on for rig actions only
            except Exception as e:
                print('[dfx] rig sync:', e)
        if arm.type != 'ARMATURE' or not has_moveset(arm):
            continue
        act = arm.animation_data.action if arm.animation_data else None
        if act is not None and _rest_action.get(arm.name) != act.name:
            _rest_action[arm.name] = act.name
            try:
                pose_unkeyed_rest(arm, act)
            except Exception as e:
                print('[dfx] rest pose:', e)
        if arm.get('dfx_auto_markers', True) and act is not None and _marker_action.get(arm.name) != act.name:
            refresh_markers(scene, arm, act)
        update_visibility(scene, arm)


def _run_pending():
    for name in list(_pending):
        _pending.discard(name)
        sc = bpy.data.scenes.get(name)
        if sc is not None:
            try:
                sync_scene(sc)
            except Exception as e:      # never let a preview error break playback
                print('[dfx] preview:', e)
    return None


@persistent
def frame_handler(scene, depsgraph=None):
    _pending.add(scene.name)
    if not bpy.app.timers.is_registered(_run_pending):
        bpy.app.timers.register(_run_pending, first_interval=0.0)


# ---------------------------------------------------------------- saving
def _empty_script(motion_index, template):
    d = json.loads(json.dumps(template))
    d['header'][0] = motion_index
    d['ranged'], d['keyed'] = [], []
    d['preamble'] = '00000000'
    d['ok'] = True
    return d


def build_objx(arm, drawbones=None):
    """Rebuild the .objx: original tables + edited events, in the exporter's motion order."""
    ms = Moveset.load(bytes(arm['dfx_objx']))
    ev = get_events(arm)
    drawbones = drawbones if drawbones is not None else json.loads(arm.get('dfx_drawbones', '{}'))
    template = next(d for d in ev.values() if d.get('ok'))
    blobs = []
    for pos, act in enumerate(actions_for(arm)):
        d = ev.get(action_key(act))
        if d is None:
            d = _empty_script(pos, template)
        if not d.get('ok'):
            blobs.append(bytes.fromhex(d['raw_all']))
            continue
        d = json.loads(json.dumps(d))
        d['header'][0] = pos
        for e in d['ranged']:
            if e.get('show_object'):
                if e['show_object'] not in drawbones:
                    raise RuntimeError(f"'{e['show_object']}' has no draw bone yet; export the model first")
                raw = bytearray.fromhex(e['raw'])
                struct.pack_into('<I', raw, 12, drawbones[e['show_object']])
                e['raw'] = bytes(raw).hex()
        blobs.append(encode_events(d))
    ms.events = blobs
    new_bones = json.loads(arm.get('dfx_new_drawbones', '[]'))
    if new_bones:
        # extra draw bones (warp ghosts, thrown sword, trails) start hidden; motions whose own
        # 0x011B/0x011C events show or hide them are left alone
        hide_draw_bones(ms, new_bones)
    return ms.to_bytes()
