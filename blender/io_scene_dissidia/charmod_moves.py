"""Character mod folders <-> Blender: the moves (View3D > Sidebar > Dissidia > Moves & events).

Character Selector (first section of the panel): retail characters (from the ISO) and character mod folders.
Open moves   imports a character folder's rig (mods/<name>/models/<rig>.gmo, with the chosen skin on it) and attaches
             its moveset, packed from mods/<name>/moveset/<p_code>/.
Save moves   writes the animations back into the folder's rig file (only the animation model: the body, weapons and
             textures in the folder stay exactly as they are) and the events / hits back into moveset/ as the usual
             readable files.
Events       a list of the active animation's events with friendly names, an editor for the selected one (timing,
             hit, part, effect, sound ...), add / duplicate / remove, and the hit (damage / knock-back) it uses.
"""
import json
import os
import re
import shutil
import struct
import sys
import tempfile

import bpy
from bpy.props import BoolProperty, EnumProperty, FloatProperty, IntProperty, PointerProperty, StringProperty

from . import moveset_bl
from .dfx.objx import Moveset, to_ticks, to_frame

FOREVER = 0xFFFF

# op -> (label, kind, names of the first args, default args, default length in ticks for ranged)
EVENTS = {
    # traced in the game code 2026-10-23 (project notes: dissidia-012-event-ops.md); "?" = not settled
    0x0200: ('Hit window', 'ranged', ['hit', 'group'], [0, 0], 8),
    0x0112: ('Weapon volume', 'ranged', [], [0], 8),
    0x0101: ('Chain window', 'ranged', [], [0], 30),
    0x0467: ('Chain + follow-up', 'ranged', [], [0, 0, 1, 1], 30),
    0x0120: ('Chase window', 'ranged', ['hit'], [0], FOREVER),
    0x0005: ('Can act', 'ranged', [], [], FOREVER),
    0x041E: ('Can act now', 'ranged', [], [0, 0, 0, 0], FOREVER),
    0x0028: ('Dodge-cancel allowed', 'ranged', [], [], FOREVER),
    0x0059: ('Dodge-cancel if foe not reeling', 'ranged', [], [], FOREVER),
    0x0022: ('Attack over (CPU)', 'ranged', [], [], FOREVER),
    0x0029: ('CPU hint', 'ranged', [], [], 20),
    0x0006: ('Invincible', 'ranged', [], [], 20),
    0x006A: ('No Assist / EX Revenge cancel', 'ranged', [], [], 30),
    0x0049: ("Can't be pushed", 'ranged', [], [], FOREVER),
    0x0402: ('Face target', 'ranged', [], [180, 0, 1, 0], 2),
    0x040B: ('Turn to opponent', 'ranged', [], [90, 0, 0, 0], 16),
    0x000A: ('Air motion', 'ranged', [], [], FOREVER),
    0x0015: ('Ground follow', 'ranged', [], [], FOREVER),
    0x0032: ('Follow floor slope', 'ranged', [], [], 4),
    0x002D: ('Landing allowed', 'ranged', [], [], FOREVER),
    0x0023: ('Keep motion when falling', 'ranged', [], [], FOREVER),
    0x011B: ('Show part', 'ranged', ['part'], [0], FOREVER),
    0x011C: ('Hide part', 'ranged', ['part'], [0], FOREVER),
    0x042A: ('Bone effect / trail', 'ranged', ['slot'], [0, 9999, 0, 0], 20),
    0x0214: ('Face texture', 'ranged', ['face part'], [2, 5], 20),
    0x0227: ('Pin physics bone', 'ranged', ['part'], [0, 180], FOREVER),
    0x030C: ('Motion camera', 'ranged', ['camera preset'], [5, 0, 0], FOREVER),
    0x075A: ('Hold prop', 'ranged', [], [0, 0, 0, 1, 10, 10, 0], 50),
    0x0C65: ('Summon object', 'ranged', [], [0] * 12, FOREVER),
    0x0219: ('Effect', 'keyed', ['effect'], [0, 30], None),
    0x0335: ('Summon effect', 'keyed', ['effect'], [0, 0, 0], None),
    0x0303: ('Sound', 'keyed', ['sound'], [0, 0, 0], None),
    0x0315: ('Voice', 'keyed', ['voice'], [0, 0, 0], None),
    0x060F: ('Footstep sound', 'keyed', ['kind'], [0, 1, 0, 1, 1, 1], None),
    0x0422: ('Ground dust', 'keyed', ['part'], [0, 0, 0x3F800000, 0], None),
    0x0306: ('Screen shake', 'keyed', [], [10, 30, 30], None),
    0x0204: ('Fire projectile', 'keyed', ['projectile'], [0, 0], None),
    0x0108: ('Forced hit', 'keyed', ['hit'], [0], None),
    0x0D10: ('Homing dash', 'keyed', [], [0] * 13, None),
    0x010B: ('Show part once', 'keyed', ['part'], [0], None),
    0x010C: ('Hide part once', 'keyed', ['part'], [0], None),
    0x0925: ('Ground homing dash', 'keyed', [], [0x3F000000, 12, 0, 0x40200000, 0, 0, 0, 0, 0], None),
    0x0002: ('End of move', 'keyed', [], [], None),
}
ADDABLE = [0x0200, 0x0112, 0x0101, 0x0467, 0x0005, 0x0028, 0x0022, 0x0006, 0x006A, 0x0049, 0x0402, 0x040B, 0x000A,
           0x002D, 0x011B, 0x011C, 0x042A, 0x0214, 0x0227, 0x030C, 0x0219, 0x0303, 0x0315, 0x060F, 0x0422, 0x0306,
           0x0204, 0x0108, 0x0D10, 0x010B, 0x010C, 0x0002]
MORE = {0x0811: 'Turn / body swivel', 0x0548: 'Straight lunge', 0x0062: '3D root motion',
        0x0047: 'Invincible (with ability)', 0x0069: 'Force Assist cancel', 0x004B: 'Heavy body',
        0x0013: 'No body collision', 0x0216: 'Pin physics group', 0x0925: 'Ground homing dash'}
# ops whose ranged and keyed forms do different things
KIND_LABEL = {(0x0037, 'ranged'): 'HP attack allowed', (0x0037, 'keyed'): "Release foe's hold ?",
              (0x0117, 'ranged'): 'Physics rest follows pose', (0x0117, 'keyed'): 'Eject cartridge'}
_NAMED = {}                      # charmod_objx.NAMED (the repo's names), filled at register
_NAMED_TRIED = []


def _load_named(context=None):
    """Event names from the repo's charmod_objx (found through the mods folder)."""
    _NAMED_TRIED.append(1)
    try:
        from . import charmods
        md = charmods.mods_dir(context or bpy.context) or charmods.auto_mods_dir()
        if md:
            CO, _G = tools(md)
            _NAMED.update(CO.NAMED)
            for op, (n, _a) in _NAMED.items():
                moveset_bl.OP_LABEL.setdefault(op, _pretty(n))
    except Exception as ex:
        print('[dfx] event names from charmod_objx not loaded:', ex)


def _pretty(n):
    return n.replace('_', ' ').capitalize().replace('Exburst', 'EX Burst').replace('Cpu', 'CPU').replace('Hp ', 'HP ')
REACTIONS = {0x0D: 'stagger', 0x0B: 'knock', 0x0C: 'blow', 0x04: 'blow away', 0x06: 'launch', 0x01: 'flinch / hold'}
# atkp record fields edited here: name -> (offset, struct format)
HIT = {'reaction': (0x05, 'B'), 'flags': (0x08, 'I'), 'hitstop': (0x0D, 'B'), 'power': (0x10, 'h'), 'rehit': (0x12, 'H'),
       'kb_yaw': (0x18, 'f'), 'kb_forward': (0x1C, 'f'), 'kb_up': (0x20, 'f'), 'stun': (0x24, 'f'), 'follow': (0x28, 'f')}


def set_bytes(ob, key, data):
    """Blender 5 truncates bytes written over an existing property at the first zero byte: delete it first."""
    if key in ob:
        del ob[key]
    ob[key] = bytes(data)


def op_label(op, kind=None):
    if (op, kind) in KIND_LABEL:
        return KIND_LABEL[(op, kind)]
    if op in EVENTS:
        return EVENTS[op][0]
    if op in MORE:
        return MORE[op]
    if not _NAMED and not _NAMED_TRIED:
        _load_named()
    if op in _NAMED:
        return _pretty(_NAMED[op][0])
    return '0x%04X' % op


# ---------------------------------------------------------------- the repo's tools (charmod_objx, charmod_gmo)
def tools(md):
    from . import charmods
    d = charmods.tools_dir(md)
    if d not in sys.path:
        sys.path.insert(0, d)
    import charmod_objx as CO
    import charmod_gmo as G
    return CO, G


def mods_dir(context):
    from . import charmods
    return charmods.mods_dir(context)


def _rj(p):
    with open(p) as f:
        return json.load(f)


def moveset_of(stem):
    """p_noc100_1p -> p_noc100, p_noc100_2p_a -> p_noc100_a, p_noc100_zako_a -> p_noc100_a"""
    return re.sub(r'_(\d+p|zako)(_a|_w)?$', r'\2', stem)


def rigs(md, name):
    """[(rig file relative to the folder, moveset folder relative to the folder)] of a character."""
    d = os.path.join(md, name, 'models')
    out = []
    if not os.path.isdir(d):
        return out
    for f in sorted(os.listdir(d)):
        if f.endswith('.gmo'):
            ms = os.path.join('moveset', moveset_of(f[:-4]))
            if os.path.isfile(os.path.join(md, name, ms, 'arc.json')):
                out.append(('models/' + f, ms))
    return out


def skin_file(md, name, skin, stem):
    d = os.path.join(md, name, 'skins', skin)
    for c in (stem + '.skin', 'body.skin'):
        if os.path.isfile(os.path.join(d, c)):
            return os.path.join(d, c)
    return None


# ---------------------------------------------------------------- open / save
# ---- the Character Selector (the first section of the Dissidia panel): retail characters from the ISO and the
# character mod folders, in one list
_PICK = []


def _pick_items(self, context):
    global _PICK
    md = mods_dir(context)
    from .dfx.iso import ROSTER
    from . import charmods
    it, n = [], 0
    mods = [(nm, lab + ('  (Modded)' if (m or {}).get('kind') == 'retail' else ''))
            for nm, lab, m in charmods.characters(md) if rigs(md, nm)] if md else []
    if mods:
        it.append(('', 'Character mods', '', 'NONE', n)); n += 1
        for nm, lab in mods:
            it.append(('mod:' + nm, lab, 'mods/%s (opened from its folder)' % nm, 'FILE_FOLDER', n)); n += 1
        it.append(('', 'From the ISO', '', 'NONE', n)); n += 1
    for code, lab in ROSTER:
        it.append(('iso:' + code, '%s (%s)' % (lab, code), 'from the game ISO', 'DISC', n)); n += 1
    _PICK = it
    return it


def picked(scene):
    """'mod:<folder>' or 'iso:<code>'"""
    return scene.get('dfx_pick_id') or 'iso:' + scene.dfx_char


def _pick_get(self):
    want = picked(self)
    items = _PICK or _pick_items(self, bpy.context)
    for i in items:
        if i[0] == want:
            return i[4]
    return next((i[4] for i in items if i[0]), 0)


def _pick_set(self, value):
    items = _PICK or _pick_items(self, bpy.context)
    ident = next((i[0] for i in items if i[4] == value), '')
    if not ident:
        return
    self['dfx_pick_id'] = ident
    kind, v = ident.split(':', 1)
    if kind == 'iso':
        self.dfx_char = v
    else:
        try:
            bpy.context.window_manager.dfx_mod_char = v       # the Character mods panel follows
        except Exception:
            pass


def picked_mod(context):
    k = picked(context.scene)
    return k[4:] if k.startswith('mod:') else ''


def draw_selector(box, context):
    from .panel import prefs
    from . import charmods
    sc, wm = context.scene, context.window_manager
    row = box.row(align=True)                          # the ISO and mods folder are set at the top (panel.draw_locations)
    row.prop(sc, 'dfx_pick', text='')
    row.operator('dfx.mod_refresh', text='', icon='FILE_REFRESH')
    row.operator('dfx.mod_new', text='', icon='ADD')
    name = picked_mod(context)
    if name:
        m = charmods.load_mod(mods_dir(context), name) or {}
        c = m.get('character') or {}
        if m.get('kind') == 'retail':
            box.label(text='%s: changes the retail character (%s)' % (c.get('name', name), c.get('code', '?')), icon='USER')
        else:
            box.label(text='%s, based on %s, slot %s' % (c.get('full_name') or c.get('name', name), c.get('based_on', '?'),
                                                        c.get('slot', '?')), icon='USER')
        box.prop(wm, 'dfx_moves_rig', text='Rig')
        box.prop(wm, 'dfx_moves_skin', text='Body')
        row = box.row(align=True)
        row.operator('dfx.mod_open_moves', text='Import from mod folder', icon='IMPORT')
        row.operator('dfx.open_studio', text='', icon='SEQUENCE')
        if (m or {}).get('kind') != 'retail':
            box.operator('dfx.own_model', text='Replace body with my model…', icon='OUTLINER_OB_ARMATURE')
        charmods.draw_mod(box, context, name)
    else:
        box.prop(sc, 'dfx_costume', text='')
        box.operator('dfx.iso_import', icon='IMPORT')
        if not mods_dir(context):
            charmods.draw_no_mods(box, context)


def _rig_items(self, context):
    md = mods_dir(context)
    name = picked_mod(context)
    it = [(r, os.path.basename(r)[:-4] + ('  (assist)' if r.endswith('_a.gmo') else ''), 'moves: ' + m)
          for r, m in rigs(md, name)] if md and name else []
    _rig_items.cache = it or [('', 'no rig with a moveset', '')]
    return _rig_items.cache


def _skin_items(self, context):
    md = mods_dir(context)
    name = picked_mod(context)
    rig = context.window_manager.dfx_moves_rig
    it = [('', 'Body as stored in the rig', 'the body the rig file carries (the manikin, for most characters)')]
    if md and name and rig:
        from . import charmods
        used = ((charmods.load_mod(md, name) or {}).get('body') or {}).get('skin')
        stem = os.path.basename(rig)[:-4]
        d = os.path.join(md, name, 'skins')
        for s in sorted(os.listdir(d)) if os.path.isdir(d) else []:
            if skin_file(md, name, s, stem):
                e = (s, 'Skin: %s%s' % (s, '  (used in builds)' if s == used else ''),
                     'skins/%s on the rig (only for looking at; it is not saved back)' % s)
                it.insert(0, e) if s == used else it.append(e)
    _skin_items.cache = it
    return it


class DFX_OT_mod_open_moves(bpy.types.Operator):
    """Import this character's rig (skeleton, animations, weapons, effect parts) with the chosen body, and its moveset
    from the folder, so the animations and their events can be edited"""
    bl_idname = 'dfx.mod_open_moves'
    bl_label = 'Open moves'
    bl_options = {'UNDO'}

    def execute(self, context):
        wm = context.window_manager
        md, name, rig, skin = mods_dir(context), picked_mod(context), wm.dfx_moves_rig, wm.dfx_moves_skin
        if not (md and name and rig):
            self.report({'ERROR'}, 'Pick a modded character and a rig in the Character Selector')
            return {'CANCELLED'}
        CO, G = tools(md)
        if not _NAMED:
            _load_named(context)
        root = os.path.join(md, name)
        ms_rel = dict(rigs(md, name)).get(rig)
        data = open(os.path.join(root, rig), 'rb').read()
        stem = os.path.basename(rig)[:-4]
        if skin:
            data = G.apply_skin(data, G.skin_from_bytes(open(skin_file(md, name, skin, stem), 'rb').read()))
        tmp = tempfile.mkdtemp(prefix='dfx_moves_')
        gp = os.path.join(tmp, stem + '.gmo')
        open(gp, 'wb').write(data)
        from . import importer
        arm = importer.import_gmo(gp, True)
        objx = CO.pack(os.path.join(root, ms_rel))
        moveset_bl.attach_moveset(arm, objx, name=os.path.basename(ms_rel) + '.objx')
        set_bytes(arm, 'dfx_mod_objx_open', objx)          # what the folder held when opened (see save_moves)
        arm['dfx_mod_events_open'] = arm['dfx_events']
        arm['dfx_mod_dir'] = md
        arm['dfx_mod_name'] = name
        arm['dfx_mod_rig'] = rig
        arm['dfx_mod_moveset'] = ms_rel
        arm['dfx_mod_skin'] = skin
        m = _rj(os.path.join(root, 'mod.json'))
        for h in m.get('runtime_hooks') or []:             # armiger=ORBIT,FLOOR,FLASH,RINGDRAW,FXDRAW: EX Mode only
            if isinstance(h, str) and h.startswith('armiger='):
                v = [int(x) for x in h[8:].split(',') if x.strip().isdigit()]
                arm['dfx_ex_only'] = v[3:5]
        shutil.rmtree(tmp, ignore_errors=True)
        for o in context.view_layer.objects:
            o.select_set(False)
        context.view_layer.objects.active = arm
        arm.select_set(True)
        moveset_bl.refresh_markers(context.scene, arm)
        context.view_layer.update()                          # pose of the new action first
        moveset_bl.update_visibility(context.scene, arm)      # only the parts the game draws
        self.report({'INFO'}, 'Opened %s: %s with %s' % (name, stem, 'skin ' + skin if skin else 'its stored body'))
        return {'FINISHED'}


def _models(g):
    from .dfx.chunks import MODEL
    return [c for c in g.root.walk() if c.type == MODEL]


def transplant_anim(folder_rig, exported):
    """The folder rig with its animation model (bones + motions) taken from the exported GMO."""
    from .dfx.chunks import GmoFile, BONE
    a, b = GmoFile.from_bytes(folder_rig), GmoFile.from_bytes(exported)
    ma, mb = _models(a), _models(b)
    if len(ma) < 2 or len(mb) < 2:
        raise RuntimeError('the rig has no separate animation model')
    na, nb = [c.name for c in ma[0].find(BONE)], [c.name for c in mb[0].find(BONE)]
    if na != nb:
        extra = [n for n in nb if n not in na]
        raise RuntimeError('the skeleton changed (%s). Save moves only saves animations and events; export the '
                           'model to add bones' % ('new bones: ' + ', '.join(extra[:5]) if extra else 'bone order'))
    for c in a.root.walk():
        if ma[0] in c.children:
            c.children[c.children.index(ma[0])] = mb[0]
            break
    return a.to_bytes()


def sync_dir(src, dst):
    """Make dst hold exactly src's files; only files that differ are written. -> [changed relative paths]"""
    changed = []
    for dp, dns, fns in os.walk(src):
        rel = os.path.relpath(dp, src)
        os.makedirs(os.path.join(dst, rel), exist_ok=True)
        for f in fns:
            s, d = os.path.join(dp, f), os.path.join(dst, rel, f)
            new = open(s, 'rb').read()
            if not os.path.exists(d) or open(d, 'rb').read() != new:
                open(d, 'wb').write(new)
                changed.append(os.path.normpath(os.path.join(rel, f)))
    for dp, dns, fns in os.walk(dst):
        rel = os.path.relpath(dp, dst)
        for f in fns:
            if not os.path.exists(os.path.join(src, rel, f)):
                os.remove(os.path.join(dp, f))
                changed.append(os.path.normpath(os.path.join(rel, f)) + ' (removed)')
    return changed


def _atkp(objx):
    return Moveset.load(bytes(objx)).arc.get('atkp').data


def _with_new_timelines(arm, folder_objx):
    """The folder's moveset plus an empty event timeline for each new animation (an Action made here that the
    moveset has no timeline for yet). -> bytes, or None when there is nothing to add."""
    ms = Moveset.load(folder_objx)
    have = {struct.unpack_from('<H', b, 0)[0] for b in ms.events}
    new = [pos for pos, act in enumerate(moveset_bl.actions_for(arm)) if 'dfx_motion_index' not in act and pos not in have]
    if not new:
        return None
    ms.events = list(ms.events) + [struct.pack('<4H', pos, 0, 8, 8) for pos in new]
    return ms.to_bytes()


def save_moves(arm, events=None):
    """Animations always go into the folder's rig. The moveset (events, hits) is the Move Studio's: it is only
    rewritten from Blender when events or hits were edited here (or events=True), and never over changes made in the
    folder since the moves were opened. -> (rig changed, [changed moveset files], note)"""
    md, name = arm['dfx_mod_dir'], arm['dfx_mod_name']
    CO, G = tools(md)
    root = os.path.join(md, name)
    rig_p = os.path.join(root, arm['dfx_mod_rig'])
    ms_d = os.path.join(root, arm['dfx_mod_moveset'])
    from . import exporter
    tmp = tempfile.mkdtemp(prefix='dfx_moves_')
    try:
        gp = os.path.join(tmp, 'export.gmo')
        exporter.export_gmo(arm, gp)
        from . import cape
        bone_notes = cape.sync_folder(arm, open(gp, 'rb').read(), physics=False)   # new bones (a cape): every rig
        old = open(rig_p, 'rb').read()
        new = transplant_anim(old, open(gp, 'rb').read())
        folder = CO.pack(ms_d)
        opened = bytes(arm['dfx_mod_objx_open']) if 'dfx_mod_objx_open' in arm else None
        edited_here = opened is None or arm.get('dfx_mod_events_open') != arm['dfx_events'] or _atkp(arm['dfx_objx']) != _atkp(opened)
        if events is None:
            events = edited_here
        note = ''
        if events:
            if opened is not None and folder != opened:
                raise RuntimeError('the moveset in the folder changed since you opened these moves (Move Studio?). '
                                   'Open the moves again to get those changes, or save animations only')
            objx = moveset_bl.build_objx(arm)
            note = 'events and hits from Blender'
        else:
            objx = _with_new_timelines(arm, folder)
            note = 'events kept as in the folder' + (' (+ empty timelines for new animations)' if objx else '')
        changed = []
        if objx is not None and objx != folder:
            mp = os.path.join(ms_d, 'moves.json')
            notes = {'commands': {int(c['id'], 16): c['_info'] for c in _rj(mp)['commands'] if '_info' in c}} if os.path.exists(mp) else {}
            bp = os.path.join(ms_d, 'bones.json')
            bones = _rj(bp)['bones'] if os.path.exists(bp) else None
            out = os.path.join(tmp, 'moveset')
            CO.unpack(objx, out, notes=notes, bones=bones)
            changed = sync_dir(out, ms_d)
        if new != old:
            open(rig_p, 'wb').write(new)
        try:
            phys = cape.write_physics(arm, open(gp, 'rb').read())   # after the events: the cape's spring records
        except Exception as ex:
            phys = ''
            bone_notes.append('cape physics NOT written: %s' % ex)
        if phys:
            changed.append(phys)
        if bone_notes:
            note += '; ' + ', '.join(bone_notes)
        now = CO.pack(ms_d)                                 # the folder is the new starting point
        set_bytes(arm, 'dfx_mod_objx_open', now)
        moveset_bl.attach_moveset(arm, now, name=arm.get('dfx_objx_name'))
        arm['dfx_mod_events_open'] = arm['dfx_events']
        return new != old, changed, note
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


class DFX_OT_open_studio(bpy.types.Operator):
    """Open the Move Studio (studio/ at the repo root) in Chrome: place events and tune hits for the character mod
    folders; Blender stays for the animations"""
    bl_idname = 'dfx.open_studio'
    bl_label = 'Open Move Studio'

    def execute(self, context):
        md = mods_dir(context)
        page = os.path.normpath(os.path.join(md, '..', 'studio', 'index.html')) if md else ''
        if not page or not os.path.isfile(page):
            self.report({'ERROR'}, 'Move Studio not found next to the mods folder (studio/index.html)')
            return {'CANCELLED'}
        import subprocess
        import webbrowser
        chrome = '/Applications/Google Chrome.app'
        try:
            if sys.platform == 'darwin' and os.path.isdir(chrome):      # Chrome can save into the folder in place
                subprocess.Popen(['open', '-a', chrome, page])
            else:
                webbrowser.open('file://' + page)
        except Exception as ex:
            self.report({'ERROR'}, 'Could not open %s: %s' % (page, ex))
            return {'CANCELLED'}
        self.report({'INFO'}, 'Move Studio: open the mods folder there (%s)' % md)
        return {'FINISHED'}


class DFX_OT_mod_save_moves(bpy.types.Operator):
    """Save the animations into the character folder's rig file (body, weapons and textures there stay as they are).
    Events and hits are saved too only if you edited them here; otherwise they stay as the Move Studio left them"""
    bl_idname = 'dfx.mod_save_moves'
    bl_label = 'Save moves to folder'
    events: EnumProperty(name='Events', items=[('auto', 'Automatic', 'Save events and hits only if you edited them here'),
                                               ('keep', 'Keep the folder\'s', 'Animations only; events stay as the Move Studio left them'),
                                               ('save', 'Save from Blender', 'Write the events and hits edited here')], default='auto')

    @classmethod
    def poll(cls, context):
        a = active_arm(context)
        return a is not None and 'dfx_mod_name' in a

    def execute(self, context):
        arm = active_arm(context)
        if arm.get('dfx_marker_action'):
            moveset_bl.write_markers(context.scene, arm)
        try:
            rig_changed, changed, note = save_moves(arm, {'auto': None, 'keep': False, 'save': True}[self.events])
        except Exception as ex:
            self.report({'ERROR'}, 'Save moves failed: %s' % ex)
            return {'CANCELLED'}
        what = []
        if rig_changed:
            what.append(arm['dfx_mod_rig'])
        if changed:
            what.append('%s (%d file%s)' % (arm['dfx_mod_moveset'], len(changed), '' if len(changed) == 1 else 's'))
        self.report({'INFO'}, ('Saved %s: %s' % (arm['dfx_mod_name'], ', '.join(what)) if what else 'Nothing changed') + '; ' + note)
        return {'FINISHED'}


# ---------------------------------------------------------------- events: reading and writing
def active_arm(context):
    from .panel import active_arm as aa
    return aa(context)


def current(context):
    """-> (arm, action, timeline dict or None)"""
    arm = active_arm(context)
    if arm is None or not moveset_bl.has_moveset(arm):
        return None, None, None
    act = arm.animation_data.action if arm.animation_data else None
    if act is None:
        return arm, None, None
    return arm, act, moveset_bl.get_events(arm).get(moveset_bl.action_key(act))


def ev_parse(e):
    raw = bytes.fromhex(e['raw'])
    if 'start' in e:
        s, en, op, pad, x = struct.unpack_from('<HHHHI', raw, 0)
        n = 12
    else:
        s, op, x = struct.unpack_from('<HHI', raw, 0)
        en, pad, n = None, 0, 8
    args = list(struct.unpack_from('<%dI' % ((len(raw) - n) // 4), raw, n))
    return {'start': s, 'end': en, 'op': op, 'pad': pad, 'x': x, 'args': args}


def ev_make(p, keep=None):
    a = b''.join(struct.pack('<I', v & 0xFFFFFFFF) for v in p['args'])
    if p['end'] is not None:
        e = {'start': p['start'], 'end': p['end'], 'op': p['op'],
             'raw': (struct.pack('<HHHHI', p['start'], p['end'], p['op'], p['pad'], p['x']) + a).hex()}
    else:
        e = {'frame': p['start'], 'op': p['op'], 'raw': (struct.pack('<HHI', p['start'], p['op'], p['x']) + a).hex()}
    if keep and keep.get('show_object'):
        e['show_object'] = keep['show_object']
    return e


def val_text(v):
    if 0x10000 <= v < 0xFFFF0000:
        f = struct.unpack('<f', struct.pack('<I', v))[0]
        if 1e-6 < abs(f) < 1e7:
            return ('%g' % f) + 'f'
    return str(v - (1 << 32) if v >= 0x80000000 else v)


def val_parse(t):
    t = t.strip()
    if t.endswith('f'):
        return struct.unpack('<I', struct.pack('<f', float(t[:-1])))[0]
    return int(t, 0) & 0xFFFFFFFF


def sel_of(context):
    s = context.scene.dfx_ev.sel
    if not s:
        return None, None
    return s[0], int(s[1:])


# ---- moveset tables (cached on the objx bytes)
_CACHE = {}


def hits(arm):
    """[(id, name, record index)] from the armature's objx."""
    d = bytes(arm['dfx_objx'])
    k = ('hits', arm.name, hash(d))
    if k not in _CACHE:
        ms = Moveset.load(d)
        _CACHE[k] = [(struct.unpack_from('<H', a.raw, 0)[0], a.name, i) for i, a in enumerate(ms.attacks)]
    return _CACHE[k]


def bones(arm):
    k = ('bones', arm.name, len(arm['dfx_template']))
    if k not in _CACHE:
        from .dfx.gmo import Character
        _CACHE[k] = [b.name for b in Character.load(bytes(arm['dfx_template'])).bones]
    return _CACHE[k]


def part_bones(arm):
    """[(bone index, name)] of the bones that draw a part (weapons, ghosts, effects), the body's left out."""
    k = ('parts', arm.name, len(arm['dfx_template']))
    if k not in _CACHE:
        from .dfx.gmo import Character
        ch = Character.load(bytes(arm['dfx_template']))
        _CACHE[k] = [(i, b.name) for i, b in enumerate(ch.bones)
                     if getattr(b, 'draw_part', -1) is not None and getattr(b, 'draw_part', -1) > 0]
        _CACHE[k].sort(key=lambda t: [int(x) if x.isdigit() else x for x in re.split(r'(\d+)', t[1])])
    return _CACHE[k]


def start_hides(d):
    """{bone: [keyed index]} of the frame-0 'hide part once' events."""
    out = {}
    for k, e in enumerate(d.get('keyed', []) if d else []):
        if _is_setup(e):
            a = ev_parse(e)['args']
            if a:
                out.setdefault(a[0], []).append(k)
    return out


def hit_users(arm, hid):
    n = 0
    for d in moveset_bl.get_events(arm).values():
        for e in d.get('ranged', []):
            if e['op'] == 0x0200 and ev_parse(e)['args'][:1] == [hid]:
                n += 1
    return n


# ---- the editor's fields (scene.dfx_ev) mirror the selected event; editing one writes the event back
_LOADING = [False]


def load_fields(context):
    arm, act, d = current(context)
    f = context.scene.dfx_ev
    kind, i = sel_of(context)
    if d is None or kind is None:
        return
    lst = d['ranged' if kind == 'r' else 'keyed']
    if i >= len(lst):
        f.sel = ''
        return
    p = ev_parse(lst[i])
    names = EVENTS.get(p['op'], ('', '', [], [], 0))[2][:len(p['args'])]
    _LOADING[0] = True
    try:
        f.start = to_frame(p['start'])
        f.forever = p['end'] == FOREVER
        f.end = to_frame(p['end']) if p['end'] not in (None, FOREVER) else f.start
        f.phase = p['pad'] if p['end'] is not None else p['x']
        f.value = 0
        f.group = 0
        for k, n in enumerate(names):
            v = p['args'][k]
            if n == 'hit':
                ids = {h[0] for h in hits(arm)}
                f.hit = str(v) if v in ids else 'none'
            elif n == 'part':
                f.part = str(v) if v < len(bones(arm)) else 'none'
            elif n == 'group':
                f.group = v
            else:
                f.value = v
        f.args = ', '.join(val_text(v) for v in p['args'][len(names):])
    finally:
        _LOADING[0] = False
    load_hit(context)


def write_fields(self, context):
    if _LOADING[0]:
        return
    arm, act, d = current(context)
    f = context.scene.dfx_ev
    kind, i = sel_of(context)
    if d is None or kind is None:
        return
    ev = moveset_bl.get_events(arm)
    d = ev[moveset_bl.action_key(act)]
    lst = d['ranged' if kind == 'r' else 'keyed']
    old = lst[i]
    p = ev_parse(old)
    p['start'] = min(to_ticks(max(0.0, f.start)), 0xFFFE)
    if p['end'] is not None:
        p['end'] = FOREVER if f.forever else max(p['start'], min(to_ticks(f.end), 0xFFFE))
        p['pad'] = f.phase
    else:
        p['x'] = f.phase
    names = EVENTS.get(p['op'], ('', '', [], [], 0))[2][:len(p['args'])]
    try:
        rest = [val_parse(t) for t in f.args.split(',') if t.strip()]
    except ValueError:
        rest = p['args'][len(names):]
    first = []
    for k, n in enumerate(names):
        v = p['args'][k]
        if n == 'hit' and f.hit != 'none':
            v = int(f.hit)
        elif n == 'part' and f.part != 'none':
            v = int(f.part)
        elif n == 'group':
            v = f.group
        elif n not in ('hit', 'part'):
            v = f.value & 0xFFFFFFFF
        first.append(v)
    want = (p['op'] >> 8) - len(first)
    rest = (rest + p['args'][len(names):][len(rest):] + [0] * want)[:want]
    p['args'] = first + rest
    lst[i] = ev_make(p, old)
    moveset_bl.set_events(arm, ev)
    moveset_bl.refresh_markers(context.scene, arm, act)
    load_hit(context)


def _hit_items(self, context):
    arm = active_arm(context)
    it = [('none', '(not in this moveset)', '')]
    if arm is not None and moveset_bl.has_moveset(arm):
        it += [(str(h), '%s  0x%04X' % (n or '?', h), '') for h, n, _ in hits(arm)]
    _hit_items.cache = it
    return it


def _part_items(self, context):
    arm = active_arm(context)
    it = [('none', '(other)', '')]
    if arm is not None and 'dfx_template' in arm:
        it += [(str(i), n, 'bone %d' % i) for i, n in enumerate(bones(arm))]
    _part_items.cache = it
    return it


class DFX_EventFields(bpy.types.PropertyGroup):
    sel: StringProperty()                                  # 'r3' / 'k5'
    show_setup: BoolProperty(name='Show start-up hides', default=False,
                             description='List the "hide this part at frame 0" events most animations start with')
    start: FloatProperty(name='Start', description='Frame the event starts (keyed events: fires)', min=0, step=50,
                         precision=1, update=write_fields)
    end: FloatProperty(name='End', description='Frame the event ends', min=0, step=50, precision=1, update=write_fields)
    forever: BoolProperty(name='To the end', description='Lasts until the move ends', update=write_fields)
    phase: IntProperty(name='Filter', min=0, max=0xFFFF, update=write_fields,
                       description='When the event runs (keyed: "x", ranged: "pad"). Low byte = phase: 0 = always, else '
                                   'only when the playing script\'s motion_map phase matches (one shared animation, '
                                   'several moves). High byte = mode: 0 = always, 1 = outside EX Mode, 2 = only in EX Mode')
    hit: EnumProperty(name='Hit', items=_hit_items, update=write_fields, description='Hit profile (damage, knock-back)')
    group: IntProperty(name='Group', min=0, update=write_fields,
                       description='Hit group: windows of one group hit the same foe only once')
    part: EnumProperty(name='Part', items=_part_items, update=write_fields, description='Draw bone (weapon / effect part)')
    value: IntProperty(name='Id', update=write_fields)
    args: StringProperty(name='Other values', update=write_fields,
                         description='The remaining values, comma separated: numbers, 0x.. hex, or 1.5f for floats')


# ---- hit profile fields (scene.dfx_hit)
def _hit_record(arm, hid):
    ms = Moveset.load(bytes(arm['dfx_objx']))
    for a in ms.attacks:
        if struct.unpack_from('<H', a.raw, 0)[0] == hid:
            return ms, a
    return ms, None


def selected_hit(context):
    arm, act, d = current(context)
    kind, i = sel_of(context)
    if d is None or kind != 'r' or i >= len(d['ranged']):
        return arm, None
    p = ev_parse(d['ranged'][i])
    if p['op'] != 0x0200 or not p['args']:
        return arm, None
    return arm, p['args'][0]


def load_hit(context):
    arm, hid = selected_hit(context)
    h = context.scene.dfx_hit
    h.id = -1
    if hid is None:
        return
    ms, a = _hit_record(arm, hid)
    if a is None:
        return
    _LOADING[0] = True
    try:
        h.id = hid
        h.name = a.name
        for k, (o, fmt) in HIT.items():
            v = struct.unpack_from('<' + fmt, a.raw, o)[0]
            if k == 'flags':
                h.hp_attack = bool(v & 2)
                h.random_power = bool(v & 1)
            else:
                setattr(h, k, v)
    finally:
        _LOADING[0] = False


def write_hit(self, context):
    if _LOADING[0]:
        return
    arm = active_arm(context)
    h = context.scene.dfx_hit
    if arm is None or h.id < 0:
        return
    ms, a = _hit_record(arm, h.id)
    if a is None:
        return
    a.name = h.name
    for k, (o, fmt) in HIT.items():
        if k == 'flags':
            v = struct.unpack_from('<I', a.raw, o)[0] & ~3
            v |= (2 if h.hp_attack else 0) | (1 if h.random_power else 0)
        else:
            v = getattr(h, k)
        struct.pack_into('<' + fmt, a.raw, o, v)
    set_bytes(arm, 'dfx_objx', ms.to_bytes())


class DFX_HitFields(bpy.types.PropertyGroup):
    id: IntProperty(default=-1)
    name: StringProperty(name='Name', maxlen=15, update=write_hit)
    power: IntProperty(name='Power', min=0, max=9999, update=write_hit,
                       description='Brave damage per hit (x the attack/defence factor). Retail: single hits 3-8, '
                                   'finishers 10-16')
    rehit: IntProperty(name='Re-hit', min=0, max=0xFFFF, update=write_hit,
                       description='0 = one hit per window; N = hits the same foe again every N ticks (60 a second)')
    reaction: IntProperty(name='Reaction', min=0, max=255, update=write_hit,
                          description='How the foe reacts: ' + ', '.join('0x%02X %s' % kv for kv in REACTIONS.items()))
    kb_forward: FloatProperty(name='Forward', update=write_hit, step=10,
                              description='Where the foe is sent: distance in front of you (m; negative = behind)')
    kb_up: FloatProperty(name='Up', update=write_hit, step=10, description='Where the foe is sent: height (m)')
    kb_yaw: FloatProperty(name='Yaw', update=write_hit, description='Knock-back direction turn')
    stun: FloatProperty(name='Stun', update=write_hit, description='Ticks to reach that spot = hit-stun (60 = 1 s)')
    follow: FloatProperty(name='Follow', update=write_hit, description='Ticks the foe keeps following you')
    hitstop: IntProperty(name='Hit-stop ?', min=0, max=255, update=write_hit, description='Probably hit-stop')
    hp_attack: BoolProperty(name='HP attack', update=write_hit,
                            description='Deals your brave as HP damage (power is ignored)')
    random_power: BoolProperty(name='Random power', update=write_hit, description='Random power 1..power')


# ---------------------------------------------------------------- event operators
def _refresh(context, arm, act):
    moveset_bl.refresh_markers(context.scene, arm, act)
    context.view_layer.update()                          # pose of the new action first
    moveset_bl.update_visibility(context.scene, arm)
    load_fields(context)
    for a in context.screen.areas if context.screen else []:
        a.tag_redraw()


class DFX_OT_ev_select(bpy.types.Operator):
    """Edit this event (and jump to its frame)"""
    bl_idname = 'dfx.ev_select'
    bl_label = 'Select event'
    sel: StringProperty()

    def execute(self, context):
        context.scene.dfx_ev.sel = self.sel
        arm, act, d = current(context)
        load_fields(context)
        if d is not None:
            e = d['ranged' if self.sel[0] == 'r' else 'keyed'][int(self.sel[1:])]
            context.scene.frame_set(int(round(to_frame(e.get('start', e.get('frame', 0))))))
        return {'FINISHED'}


def _template(arm, op, act_key):
    """A retail-shaped event of this op: one in this animation, else the most common in the moveset, else defaults."""
    ev = moveset_bl.get_events(arm)
    best = {}
    for k, d in ev.items():
        for e in d.get('ranged', []) + d.get('keyed', []):
            if e['op'] == op:
                p = ev_parse(e)
                if k == act_key:
                    return p
                sig = (p['pad'], p['x'], tuple(p['args'][1:]), p['end'] is None,
                       None if p['end'] in (None, FOREVER) else p['end'] - p['start'])
                best.setdefault(sig, [0, p])[0] += 1
    if best:
        return max(best.values(), key=lambda v: v[0])[1]
    lab, kind, names, args, dur = EVENTS[op]
    if op == 0x0200 and hits(arm):
        args = [hits(arm)[0][0], 0]
    return {'start': 0, 'end': (dur if kind == 'ranged' else None), 'op': op, 'pad': 0, 'x': 0, 'args': list(args)}


class DFX_OT_ev_add(bpy.types.Operator):
    """Add an event at the current frame (shaped like the moveset's own events of that kind)"""
    bl_idname = 'dfx.ev_add'
    bl_label = 'Add event'
    bl_options = {'UNDO'}
    op: EnumProperty(name='Event', items=[(str(o), EVENTS[o][0], '0x%04X' % o) for o in ADDABLE])

    @classmethod
    def poll(cls, context):
        arm, act, d = current(context)
        return act is not None

    def execute(self, context):
        arm, act, d = current(context)
        op = int(self.op)
        key = moveset_bl.action_key(act)
        ev = moveset_bl.get_events(arm)
        if key not in ev:
            tpl = next(v for v in ev.values() if v.get('ok'))
            ev[key] = moveset_bl._empty_script(0, tpl)
        d = ev[key]
        if not d.get('ok'):
            self.report({'ERROR'}, 'This animation\'s events could not be decoded; they can\'t be edited here')
            return {'CANCELLED'}
        p = dict(_template(arm, op, key))
        t0 = to_ticks(context.scene.frame_current - act.frame_range[0] if act.frame_range[0] > 0 else
                      context.scene.frame_current)
        t0 = max(0, min(t0, 0xFFFE))
        if p['end'] is not None and p['end'] != FOREVER:
            p['end'] = min(t0 + max(p['end'] - p['start'], 1), 0xFFFE)
        p['start'] = t0
        lst = d['keyed' if p['end'] is None else 'ranged']
        lst.append(ev_make(p))
        moveset_bl.set_events(arm, ev)
        context.scene.dfx_ev.sel = ('k' if p['end'] is None else 'r') + str(len(lst) - 1)
        _refresh(context, arm, act)
        return {'FINISHED'}


class DFX_OT_ev_remove(bpy.types.Operator):
    """Remove the selected event"""
    bl_idname = 'dfx.ev_remove'
    bl_label = 'Remove event'
    bl_options = {'UNDO'}

    @classmethod
    def poll(cls, context):
        return bool(context.scene.dfx_ev.sel) and current(context)[2] is not None

    def execute(self, context):
        arm, act, d = current(context)
        kind, i = sel_of(context)
        ev = moveset_bl.get_events(arm)
        lst = ev[moveset_bl.action_key(act)]['ranged' if kind == 'r' else 'keyed']
        if i < len(lst):
            lst.pop(i)
        moveset_bl.set_events(arm, ev)
        context.scene.dfx_ev.sel = ''
        _refresh(context, arm, act)
        return {'FINISHED'}


class DFX_OT_ev_duplicate(bpy.types.Operator):
    """Copy the selected event to the current frame"""
    bl_idname = 'dfx.ev_duplicate'
    bl_label = 'Duplicate event'
    bl_options = {'UNDO'}

    @classmethod
    def poll(cls, context):
        return bool(context.scene.dfx_ev.sel) and current(context)[2] is not None

    def execute(self, context):
        arm, act, d = current(context)
        kind, i = sel_of(context)
        ev = moveset_bl.get_events(arm)
        lst = ev[moveset_bl.action_key(act)]['ranged' if kind == 'r' else 'keyed']
        p = ev_parse(lst[i])
        t0 = max(0, min(to_ticks(context.scene.frame_current), 0xFFFE))
        if p['end'] is not None and p['end'] != FOREVER:
            p['end'] = min(t0 + p['end'] - p['start'], 0xFFFE)
        p['start'] = t0
        lst.append(ev_make(p, lst[i]))
        moveset_bl.set_events(arm, ev)
        context.scene.dfx_ev.sel = kind + str(len(lst) - 1)
        _refresh(context, arm, act)
        return {'FINISHED'}


class DFX_OT_ev_toggle_hide(bpy.types.Operator):
    """Hidden at the start of this animation (a 'hide part once' event at frame 0). Click to switch"""
    bl_idname = 'dfx.ev_toggle_hide'
    bl_label = 'Hide part at start'
    bl_options = {'UNDO'}
    bone: IntProperty()

    def execute(self, context):
        arm, act, d = current(context)
        if act is None:
            return {'CANCELLED'}
        key = moveset_bl.action_key(act)
        ev = moveset_bl.get_events(arm)
        if key not in ev:
            ev[key] = moveset_bl._empty_script(0, next(v for v in ev.values() if v.get('ok')))
        d = ev[key]
        have = start_hides(d).get(self.bone)
        if have:
            for k in sorted(have, reverse=True):
                d['keyed'].pop(k)
        else:
            p = dict(_template(arm, 0x010C, key))
            p.update(start=0, end=None, args=[self.bone] + p['args'][1:])
            d['keyed'].append(ev_make(p))
        moveset_bl.set_events(arm, ev)
        context.scene.dfx_ev.sel = ''
        _refresh(context, arm, act)
        return {'FINISHED'}


class DFX_OT_ev_new_hit(bpy.types.Operator):
    """Give this hit window its own copy of the hit, so changing its damage doesn't change the other moves using it"""
    bl_idname = 'dfx.ev_new_hit'
    bl_label = 'Own copy of this hit'
    bl_options = {'UNDO'}

    @classmethod
    def poll(cls, context):
        return selected_hit(context)[1] is not None

    def execute(self, context):
        arm, hid = selected_hit(context)
        ms, a = _hit_record(arm, hid)
        if a is None:
            return {'CANCELLED'}
        from .dfx.objx import Attack
        ids = {struct.unpack_from('<H', x.raw, 0)[0] for x in ms.attacks}
        new = max(ids) + 1
        while new in ids:
            new += 1
        b = Attack(bytearray(a.raw))
        struct.pack_into('<H', b.raw, 0, new)
        base = a.name[:11] or 'HIT'
        names = {x.name for x in ms.attacks}
        k = 2
        while '%s_%d' % (base, k) in names:
            k += 1
        b.name = '%s_%d' % (base, k)
        ms.attacks.append(b)
        set_bytes(arm, 'dfx_objx', ms.to_bytes())
        context.scene.dfx_ev.hit = str(new)                # writes the event (update)
        load_fields(context)
        self.report({'INFO'}, 'New hit %s (0x%04X)' % (b.name, new))
        return {'FINISHED'}


class DFX_OT_ev_reload(bpy.types.Operator):
    """Save the dragged “e:” markers' frames into the events"""
    bl_idname = 'dfx.ev_reload'
    bl_label = 'Keep marker timing'

    def execute(self, context):
        arm, act, d = current(context)
        if arm is not None and arm.get('dfx_marker_action'):
            moveset_bl.write_markers(context.scene, arm)
        load_fields(context)
        return {'FINISHED'}


# ---------------------------------------------------------------- panel
def ev_text(e, arm):
    p = ev_parse(e)
    s = '%g' % to_frame(p['start'])
    if p['end'] is not None:
        s += '–' + ('end' if p['end'] == FOREVER else '%g' % to_frame(p['end']))
    lab = op_label(p['op'], 'ranged' if p['end'] is not None else 'keyed')
    names = EVENTS.get(p['op'], ('', '', [], [], 0))[2]
    if names and p['args']:
        v = p['args'][0]
        if names[0] == 'hit':
            lab += '  ' + next((n for h, n, _ in hits(arm) if h == v), '0x%04X' % v)
        elif names[0] == 'part':
            b = bones(arm)
            lab += '  ' + (e.get('show_object') or (b[v] if v < len(b) else '#%d' % v))
        else:
            lab += '  #%d' % v
    return s, lab


def _is_setup(e):
    """The 'hide this part at frame 0' events nearly every retail animation starts with."""
    return e.get('frame') == 0 and e['op'] == 0x010C


class DFX_PT_mod_moves(bpy.types.Panel):
    bl_label = 'Moves & events'
    bl_space_type = 'VIEW_3D'
    bl_region_type = 'UI'
    bl_category = 'Dissidia'

    def draw(self, context):
        from .panel import section
        L = self.layout
        f = context.scene.dfx_ev
        arm = active_arm(context)
        if arm is None:
            L.label(text='Import a character (Character Selector)', icon='INFO')
            return
        if 'dfx_mod_name' in arm:
            box = section(L, 'dfx_mv_folder', 'Mod folder: %s · %s' % (arm['dfx_mod_name'],
                                                                         os.path.basename(arm['dfx_mod_rig'])), 'FILE_FOLDER')
            if box:
                box.label(text='Moves: ' + arm['dfx_mod_moveset'])
                row = box.row(align=True)
                row.operator('dfx.mod_save_moves', icon='FILE_TICK')
                row.operator_menu_enum('dfx.mod_save_moves', 'events', text='', icon='DOWNARROW_HLT')
                box.operator('dfx.open_studio', icon='SEQUENCE')
                box.label(text='Events and hits: edit them in the Move Studio', icon='INFO')
        if not moveset_bl.has_moveset(arm):
            return
        arm, act, d = current(context)
        if act is None:
            L.label(text='Pick an animation (Animations list)', icon='INFO')
            return
        users = moveset_bl.motion_users(arm, act)
        box = section(L, 'dfx_mv_anim', 'Animation %s' % act.name, 'ACTION')
        if box:
            box.label(text='Used by: ' + (', '.join(users) if users else 'no move yet'))
            row = box.row(align=True)
            row.operator('dfx.refresh_markers', icon='FILE_REFRESH')
            row.operator('dfx.ev_reload', icon='CHECKMARK')
            row = box.row(align=True)
            row.operator('dfx.new_anim', icon='ADD')
            row.operator('dfx.copy_events', icon='DUPLICATE')
            if 'dfx_wep_preview' in arm:
                row.prop(arm, '["dfx_wep_preview"]', text='Weapon preview')
            else:
                row.operator('dfx.enable_preview', text='Weapon preview', icon='HIDE_OFF')
        if d is not None and not d.get('ok'):
            L.label(text='These events could not be decoded (kept as they are)', icon='ERROR')
            return
        kind, i = sel_of(context)
        lst = d['ranged' if kind == 'r' else 'keyed'] if d is not None and kind else []
        if kind and i < len(lst):
            self.draw_event(context, L, arm, kind, ev_parse(lst[i]))
        # the list
        rows = [('r%d' % k, e) for k, e in enumerate(d.get('ranged', []))] + \
               [('k%d' % k, e) for k, e in enumerate(d.get('keyed', []))] if d is not None else []
        rows = [r for r in rows if not _is_setup(r[1])]
        parts = part_bones(arm)
        hides = start_hides(d)
        names = dict(parts)
        extra = [(b, '#%d' % b) for b in hides if b not in names]
        box = section(L, 'dfx_mv_parts', 'Parts hidden at start (%d of %d)' % (len(hides), len(parts) + len(extra)),
                      'HIDE_ON', closed=True)
        if box:
            box.label(text='Ticked = not drawn when the animation starts (a Show part event can still show it)')
            g = box.grid_flow(columns=2, even_columns=True, align=True)
            for b, n in parts + extra:
                g.operator('dfx.ev_toggle_hide', text=n, emboss=False,
                           icon='CHECKBOX_HLT' if b in hides else 'CHECKBOX_DEHLT').bone = b
        rows.sort(key=lambda r: (r[1].get('start', r[1].get('frame', 0)), r[0]))
        box = section(L, 'dfx_mv_events', 'Events (%d)' % len(rows), 'MARKER_HLT')
        if not box:
            return
        row = box.row(align=True)
        row.operator_menu_enum('dfx.ev_add', 'op', text='Add', icon='ADD')
        row.operator('dfx.ev_duplicate', text='Duplicate', icon='DUPLICATE')
        row.operator('dfx.ev_remove', text='', icon='X')
        if not rows:
            box.label(text='No events yet')
            return
        col = box.column(align=True)
        for key, e in rows:
            t, lab = ev_text(e, arm)
            sp = col.split(factor=0.25, align=True)
            sp.operator('dfx.ev_select', text=t, depress=(key == f.sel)).sel = key
            sp.operator('dfx.ev_select', text=lab, depress=(key == f.sel)).sel = key
        if not f.sel:
            box.label(text='Click an event to edit it, or drag its “e:” marker in the timeline.', icon='INFO')

    def draw_event(self, context, L, arm, kind, p):
        from .panel import section
        f = context.scene.dfx_ev
        box = section(L, 'dfx_mv_event', 'Event: %s' % op_label(p['op'], 'ranged' if kind == 'r' else 'keyed'), 'PROPERTIES')
        if box:
            box.label(text='0x%04X, %s' % (p['op'], 'lasts start → end' if kind == 'r' else 'fires once'))
            r = box.row(align=True)
            r.prop(f, 'start', text='Start' if kind == 'r' else 'Frame')
            if kind == 'r':
                sub = r.row(align=True)
                sub.enabled = not f.forever
                sub.prop(f, 'end')
                r.prop(f, 'forever', text='', icon='FF')
            names = EVENTS.get(p['op'], ('', '', [], [], 0))[2][:len(p['args'])]
            for n in names:
                if n == 'hit':
                    box.prop(f, 'hit')
                elif n == 'part':
                    box.prop(f, 'part', text='Part' if p['op'] in (0x011B, 0x011C, 0x010B, 0x010C) else 'Bone')
                elif n == 'group':
                    box.prop(f, 'group')
                else:
                    box.prop(f, 'value', text=n.capitalize())
            if len(p['args']) > len(names):
                box.prop(f, 'args')
            box.prop(f, 'phase')
        h = context.scene.dfx_hit
        if p['op'] != 0x0200 or h.id < 0:
            return
        n = hit_users(arm, h.id)
        hb = section(L, 'dfx_mv_hit', 'Hit: %s  (damage, knock-back)' % h.name, 'MOD_PHYSICS')
        if not hb:
            return
        hb.label(text='0x%04X, used by %d hit window%s' % (h.id, n, '' if n == 1 else 's'))
        if n > 1:
            hb.operator('dfx.ev_new_hit', icon='DUPLICATE')
        hb.prop(h, 'name')
        r = hb.row(align=True)
        r.prop(h, 'power')
        r.prop(h, 'rehit')
        r = hb.row(align=True)
        r.prop(h, 'hp_attack', toggle=True)
        r.prop(h, 'random_power', toggle=True)
        r = hb.row(align=True)
        r.prop(h, 'reaction')
        r.label(text=REACTIONS.get(h.reaction, '?'))
        c = hb.column(align=True)
        c.label(text='Knock-back')
        r = c.row(align=True)
        r.prop(h, 'kb_forward')
        r.prop(h, 'kb_up')
        r = c.row(align=True)
        r.prop(h, 'stun')
        r.prop(h, 'follow')
        r = c.row(align=True)
        r.prop(h, 'kb_yaw')
        r.prop(h, 'hitstop')


# ---------------------------------------------------------------- registration
classes = (DFX_EventFields, DFX_HitFields, DFX_OT_mod_open_moves, DFX_OT_mod_save_moves, DFX_OT_open_studio, DFX_OT_ev_select,
           DFX_OT_ev_add, DFX_OT_ev_remove, DFX_OT_ev_duplicate, DFX_OT_ev_new_hit, DFX_OT_ev_toggle_hide, DFX_OT_ev_reload, DFX_PT_mod_moves)


def register():
    for c in classes:
        bpy.utils.register_class(c)
    bpy.types.Scene.dfx_ev = PointerProperty(type=DFX_EventFields)
    bpy.types.Scene.dfx_hit = PointerProperty(type=DFX_HitFields)
    bpy.types.Scene.dfx_pick = EnumProperty(name='Character', items=_pick_items, get=_pick_get, set=_pick_set,
                                            description='A retail character from the ISO, or a character mod folder')
    bpy.types.WindowManager.dfx_moves_rig = EnumProperty(name='Rig', items=_rig_items)
    bpy.types.WindowManager.dfx_moves_skin = EnumProperty(name='Body', items=_skin_items)
    for op, (lab, *_rest) in EVENTS.items():                # friendlier marker names
        moveset_bl.OP_LABEL[op] = lab
    for op, lab in MORE.items():
        moveset_bl.OP_LABEL[op] = lab
    moveset_bl.OP_LABEL[0x0117] = 'Cartridge / physics rest'
    moveset_bl.OP_LABEL[0x0037] = 'HP attack allowed / release hold'


def unregister():
    del bpy.types.WindowManager.dfx_moves_skin
    del bpy.types.WindowManager.dfx_moves_rig
    del bpy.types.Scene.dfx_hit
    del bpy.types.Scene.dfx_pick
    del bpy.types.Scene.dfx_ev
    for c in reversed(classes):
        bpy.utils.unregister_class(c)
