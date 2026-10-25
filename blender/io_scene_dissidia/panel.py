"""Dissidia Forge sidebar panel (View3D > Sidebar > Dissidia)."""
import json
import os
import bpy
from bpy.props import (StringProperty, EnumProperty, IntProperty, FloatProperty, BoolProperty)
from bpy_extras.io_utils import ImportHelper, ExportHelper

from . import moveset_bl, tools_bl
from .dfx import warp as warp_mod
from .dfx.iso import ROSTER, COSTUMES

ADDON = __package__


def prefs(context):
    return context.preferences.addons[ADDON].preferences


def active_arm(context):
    ob = context.active_object
    if ob is not None and ob.type in ('MESH', 'EMPTY') and ob.parent is not None and ob.parent.type == 'ARMATURE':
        ob = ob.parent
    if ob is not None and ob.type == 'ARMATURE' and 'dfx_template' in ob:
        return ob
    # nothing of a character active: the one picked in the Character Selector, else the only one in the scene
    arms = [o for o in context.view_layer.objects if o.type == 'ARMATURE' and 'dfx_template' in o]
    pick = context.scene.get('dfx_pick_id', '')
    for o in reversed(arms):
        if pick == 'mod:' + o.get('dfx_mod_name', '') or (pick.startswith('iso:') and o.name.startswith('p_' + pick[4:])):
            return o
    return arms[0] if len(arms) == 1 else None


def work_dir(context):
    p = prefs(context)
    d = bpy.path.abspath(p.work_dir) if p.work_dir else os.path.join(os.path.dirname(bpy.path.abspath(p.iso_path)), 'forge_work')
    os.makedirs(d, exist_ok=True)
    return d


def patches(scene):
    return json.loads(scene.get('dfx_patches', '{}'))


def add_patch(scene, pkg_path, local):
    p = patches(scene); p[pkg_path] = local; scene['dfx_patches'] = json.dumps(p)


# ---------------------------------------------------------------- preferences
class DFX_Prefs(bpy.types.AddonPreferences):
    bl_idname = ADDON
    iso_path: StringProperty(name='Game ISO', subtype='FILE_PATH',
                             description='Your Dissidia 012 [duodecim] ISO. It is only read; patched builds are written to a new file')
    work_dir: StringProperty(name='Working folder', subtype='DIR_PATH',
                             description='Where extracted and exported files go (default: forge_work next to the ISO)')
    mods_dir: StringProperty(name='Character mods folder', subtype='DIR_PATH',
                             description='The mods folder in the repo. Found by itself when the add-on runs from the repo')
    mod_sources: StringProperty(name='Recipe source folders', default='{}', options={'HIDDEN'},
                                description='The folder picked for each skin recipe (JSON)')

    def draw(self, context):
        self.layout.prop(self, 'iso_path')
        self.layout.prop(self, 'work_dir')
        self.layout.prop(self, 'mods_dir')


# ---------------------------------------------------------------- ISO
class DFX_OT_iso_import(bpy.types.Operator):
    """Extract the chosen character from the ISO and import model, animations and moveset"""
    bl_idname = 'dfx.iso_import'
    bl_label = 'Import from ISO'
    bl_options = {'UNDO'}

    def execute(self, context):
        from .dfx.iso import IsoPackage
        from . import importer
        p = prefs(context)
        iso = bpy.path.abspath(p.iso_path)
        if not iso or not os.path.isfile(iso):
            self.report({'ERROR'}, 'Set the game ISO first (Dissidia panel or add-on preferences).')
            return {'CANCELLED'}
        sc = context.scene
        code, cos = sc.dfx_char, sc.dfx_costume
        gmo_name = f'obj/p_{code}_{cos}.gmo'
        objx_name = f'obj/p_{code}_a.objx' if cos.endswith('_a') else f'obj/p_{code}.objx'
        try:
            pk = IsoPackage(iso)
        except Exception as e:
            self.report({'ERROR'}, str(e)); return {'CANCELLED'}
        if not pk.has(gmo_name):
            pk.close()
            self.report({'ERROR'}, f'{gmo_name} is not in this ISO (try another costume)')
            return {'CANCELLED'}
        d = os.path.join(work_dir(context), code)
        os.makedirs(d, exist_ok=True)
        gp = os.path.join(d, os.path.basename(gmo_name))
        open(gp, 'wb').write(pk.read(gmo_name))
        op = None
        if pk.has(objx_name):
            op = os.path.join(d, os.path.basename(objx_name))
            open(op, 'wb').write(pk.read(objx_name))
        pk.close()
        arm = importer.import_gmo(gp)
        if op and 'dfx_objx' not in arm:
            moveset_bl.attach_moveset(arm, op)
        arm['dfx_pkg_gmo'] = gmo_name
        if op:
            arm['dfx_pkg_objx'] = objx_name
        name = dict(ROSTER).get(code, code)
        self.report({'INFO'}, f'Imported {name} ({cos}) from the ISO')
        return {'FINISHED'}


class DFX_OT_export_all(bpy.types.Operator):
    """Export the model (and moveset, if loaded) to the working folder and queue them for the ISO build"""
    bl_idname = 'dfx.export_all'
    bl_label = 'Export model + moveset'

    @classmethod
    def poll(cls, context):
        return active_arm(context) is not None

    def execute(self, context):
        from . import exporter
        arm = active_arm(context)
        if 'dfx_marker_action' in arm:
            moveset_bl.write_markers(context.scene, arm)
        gname = arm.get('dfx_pkg_gmo') or 'obj/' + os.path.basename(arm.get('dfx_source', arm.name + '.gmo'))
        out = os.path.join(work_dir(context), 'out')
        os.makedirs(out, exist_ok=True)
        gp = os.path.join(out, os.path.basename(gname))
        try:
            msg = exporter.export_gmo(arm, gp)
        except Exception as e:
            self.report({'ERROR'}, f'Model export failed: {e}'); return {'CANCELLED'}
        add_patch(context.scene, gname, gp)
        parts = [msg]
        if moveset_bl.has_moveset(arm):
            oname = arm.get('dfx_pkg_objx') or 'obj/' + arm.get('dfx_objx_name', 'moveset.objx')
            opath = os.path.join(out, os.path.basename(oname))
            try:
                open(opath, 'wb').write(moveset_bl.build_objx(arm))
            except Exception as e:
                self.report({'ERROR'}, f'Moveset export failed: {e}'); return {'CANCELLED'}
            add_patch(context.scene, oname, opath)
            parts.append(f'moveset {os.path.basename(opath)}')
        self.report({'INFO'}, '; '.join(parts))
        return {'FINISHED'}


class DFX_OT_build_iso(bpy.types.Operator, ExportHelper):
    """Write a new ISO with every queued file. The original ISO is not changed"""
    bl_idname = 'dfx.build_iso'
    bl_label = 'Build patched ISO'
    filename_ext = '.iso'
    filter_glob: StringProperty(default='*.iso', options={'HIDDEN'})

    @classmethod
    def poll(cls, context):
        return bool(patches(context.scene))

    def invoke(self, context, event):
        self.filepath = os.path.join(work_dir(context), 'Dissidia012_Forge.iso')
        return super().invoke(context, event)

    def execute(self, context):
        from .dfx.iso import build_patched_iso
        iso = bpy.path.abspath(prefs(context).iso_path)
        if not os.path.isfile(iso):
            self.report({'ERROR'}, 'Set the game ISO first.'); return {'CANCELLED'}
        rep = {}
        for pkg, local in patches(context.scene).items():
            if not os.path.isfile(local):
                self.report({'ERROR'}, f'Missing exported file {local}'); return {'CANCELLED'}
            rep[pkg] = open(local, 'rb').read()
        wm = context.window_manager
        wm.progress_begin(0, 100)
        try:
            log = build_patched_iso(iso, self.filepath, replace=rep, progress=lambda x: wm.progress_update(int(x * 100)))
        except Exception as e:
            wm.progress_end(); self.report({'ERROR'}, f'Build failed: {e}'); return {'CANCELLED'}
        wm.progress_end()
        print('[dfx] ' + '\n[dfx] '.join(log))
        self.report({'INFO'}, f'Built {os.path.basename(self.filepath)} with {len(rep)} file(s)')
        return {'FINISHED'}


class DFX_OT_clear_patches(bpy.types.Operator):
    """Empty the list of files queued for the ISO build"""
    bl_idname = 'dfx.clear_patches'
    bl_label = 'Clear queue'

    def execute(self, context):
        context.scene['dfx_patches'] = '{}'
        return {'FINISHED'}


# ---------------------------------------------------------------- moveset + events
class DFX_OT_load_moveset(bpy.types.Operator, ImportHelper):
    """Attach a moveset (.objx) to the active Dissidia armature"""
    bl_idname = 'dfx.load_moveset'
    bl_label = 'Load moveset'
    filename_ext = '.objx'
    filter_glob: StringProperty(default='*.objx', options={'HIDDEN'})

    @classmethod
    def poll(cls, context):
        return active_arm(context) is not None

    def execute(self, context):
        arm = active_arm(context)
        try:
            moveset_bl.attach_moveset(arm, self.filepath)
        except Exception as e:
            self.report({'ERROR'}, str(e)); return {'CANCELLED'}
        arm['dfx_pkg_objx'] = 'obj/' + os.path.basename(self.filepath)
        moveset_bl.refresh_markers(context.scene, arm)
        return {'FINISHED'}


class DFX_OT_refresh_markers(bpy.types.Operator):
    """Show the active animation's events as timeline markers"""
    bl_idname = 'dfx.refresh_markers'
    bl_label = 'Show events'

    def execute(self, context):
        arm = active_arm(context)
        n = moveset_bl.refresh_markers(context.scene, arm) if arm else 0
        self.report({'INFO'}, f'{n} events')
        return {'FINISHED'}


class DFX_OT_write_markers(bpy.types.Operator):
    """Save moved event markers back into the moveset"""
    bl_idname = 'dfx.write_markers'
    bl_label = 'Keep marker timing'

    def execute(self, context):
        arm = active_arm(context)
        n = moveset_bl.write_markers(context.scene, arm) if arm else 0
        self.report({'INFO'}, f'Updated {n} event time(s)')
        return {'FINISHED'}


def _motion_items(self, context):
    arm = active_arm(context)
    if not arm:
        return [('-1', 'None', '')]
    return [(str(a['dfx_motion_index']), a.name, '') for a in moveset_bl.actions_for(arm) if 'dfx_motion_index' in a] or [('-1', 'None', '')]


class DFX_OT_copy_events(bpy.types.Operator):
    """Give the active animation a copy of another animation's event timeline (hitboxes, effects, sounds)"""
    bl_idname = 'dfx.copy_events'
    bl_label = 'Copy events from…'
    bl_property = 'source'
    source: EnumProperty(name='From', items=_motion_items)

    def invoke(self, context, event):
        context.window_manager.invoke_search_popup(self)
        return {'RUNNING_MODAL'}

    def execute(self, context):
        arm = active_arm(context)
        act = arm.animation_data.action if arm and arm.animation_data else None
        if not act:
            return {'CANCELLED'}
        ev = moveset_bl.get_events(arm)
        if self.source not in ev:
            return {'CANCELLED'}
        ev[moveset_bl.action_key(act)] = json.loads(json.dumps(ev[self.source]))
        moveset_bl.set_events(arm, ev)
        moveset_bl.refresh_markers(context.scene, arm, act)
        return {'FINISHED'}


def _any_action_items(self, context):
    """Every animation of this character, the game's and the ones made here."""
    arm = active_arm(context)
    if not arm:
        return [('', 'None', '')]
    items = []
    for a in moveset_bl.actions_for(arm):
        users = moveset_bl.motion_users(arm, a)
        items.append((a.name, a.name, ', '.join(users) if users else 'new animation'))
    return items or [('', 'None', '')]


class DFX_OT_new_anim(bpy.types.Operator):
    """Make a new animation for this character as a copy of an existing one. It becomes a new motion slot on
    export (appended after the game's motions), with its own copy of the source's events, so a move can be pointed
    at it in the Move Studio"""
    bl_idname = 'dfx.new_anim'
    bl_label = 'New animation (from…)'
    bl_options = {'REGISTER', 'UNDO'}
    source: EnumProperty(name='Copy of', items=_any_action_items, description='The animation to start from')
    name: StringProperty(name='Name', default='', description='Name of the new animation (blank: "<source> copy"; 31 characters at most). '
                                                              'Game motions are named _NNNA; any name works for a new one')
    copy_events: BoolProperty(name='Copy its events', default=True,
                              description='Give the new animation a copy of the source\'s hits, effects, sounds and show/hide events '
                                          '(otherwise it starts with an empty timeline)')

    @classmethod
    def poll(cls, context):
        return active_arm(context) is not None

    def invoke(self, context, event):
        arm = active_arm(context)
        act = arm.animation_data.action if arm and arm.animation_data else None
        if act is not None and act.get('dfx_armature') == arm.name and not self.source:
            self.source = act.name
        return context.window_manager.invoke_props_dialog(self)

    def execute(self, context):
        arm = active_arm(context)
        src = bpy.data.actions.get(self.source)
        if src is None or src.get('dfx_armature') != arm.name:
            self.report({'ERROR'}, 'Pick an animation of this character'); return {'CANCELLED'}
        name = (self.name.strip() or f'{src.name} copy')[:31]
        if bpy.data.actions.get(name) is not None:
            self.report({'ERROR'}, f"An action called '{name}' exists already"); return {'CANCELLED'}
        new = src.copy()
        new.name = name
        for k in ('dfx_motion_index', 'dfx_hash'):          # no slot: the exporter appends it as a new motion
            if k in new:
                del new[k]
        new['dfx_armature'] = arm.name
        new.use_fake_user = True
        if moveset_bl.has_moveset(arm):
            ev = moveset_bl.get_events(arm)
            d = ev.get(moveset_bl.action_key(src)) if self.copy_events else None
            key = moveset_bl.action_key(new)
            if d is not None and d.get('ok'):
                ev[key] = json.loads(json.dumps(d))
            elif d is not None:
                self.report({'WARNING'}, 'The source events could not be decoded; the new animation starts empty')
                ev.pop(key, None)
            else:
                ev.pop(key, None)                            # empty timeline on save
            moveset_bl.set_events(arm, ev)
        context.scene.dfx_action_index = bpy.data.actions.find(new.name)
        self.report({'INFO'}, f"Made '{new.name}' from {src.name}. Save moves to folder, then point a move at it in the Move Studio")
        return {'FINISHED'}


# ---------------------------------------------------------------- tools
_WEP_CACHE = []


def _weapon_items(self, context):
    """Weapon parts, left-hand blades first (Noctis throws left-handed), longest first."""
    arm = active_arm(context)
    if not arm:
        return [('', 'None', '')]
    obs = [o for o in arm.children if o.type == 'MESH' and o.get('dfx_part_index', 0) != 0
           and not o.name.startswith('warp_sword') and not o.get('dfx_fx')]
    def side(o):
        g = [vg.name for vg in o.vertex_groups]
        return 'left' if any(n.startswith('l_') for n in g) else 'right' if any(n.startswith('r_') for n in g) else ''
    obs.sort(key=lambda o: (side(o) != 'left', -max(o.dimensions)))
    _WEP_CACHE[:] = [(o.name, f'{o.name} ({side(o) or "other"})', '') for o in obs] or [('', 'None', '')]
    return _WEP_CACHE


class DFX_OT_warp_dash(bpy.types.Operator):
    """Noctis-style warp dash: crouch, throw the weapon underarm, vanish while it flies ahead and come back holding it. Rewrites the dash animation as normal keyframes you can keep editing; the other dashes and the dodge go back to the game's animation"""
    bl_idname = 'dfx.warp_dash'
    bl_label = 'Warp dash'
    bl_options = {'REGISTER', 'UNDO'}
    weapon: EnumProperty(name='Weapon to throw', items=_weapon_items)
    kind: EnumProperty(name='Which dash', default='DASH',
                       items=[('DASH', 'Dash (R+Triangle)', 'The dash, script 240 (_500A), on the ground and in the air. The throw and vanish play in its start, he stays hidden through the loop while the sword hovers ahead, and he comes back through the blend into the fall'),
                              ('HOLDX', 'Hold-X dash (after a jump)', 'The short dash while X is held after a jump, script 103 (_023A). The engine cuts it after about 19 frames, so the whole warp is kept that short'),
                              ('DODGE', 'Dodge (R+X)', 'The four dodge motions (scripts 400 / 410 / 440 / 460), with the longer timing and a tumble landing')])
    distance: FloatProperty(name='Throw distance', default=3.0, min=0.5, max=10, unit='LENGTH')
    windup: IntProperty(name='Crouch until frame', default=0, min=0, max=30, description='0: the dash\'s own timing')
    release: IntProperty(name='Let go at frame', default=0, min=0, max=40, description='0: the dash\'s own timing')
    vanish: IntProperty(name='Gone after frame', default=0, min=0, max=60, description='0: the dash\'s own timing')
    reappear: IntProperty(name='Reappear at frame', default=0, min=0, max=120, description='0: the dash\'s own timing (the R+Triangle dash reappears when the game ends it)')
    land_motion: StringProperty(name='Reappear with', default='',
                                description='Animation whose second half plays as the tumble landing (empty: the dash\'s own pose; the dodge uses _150A)')
    land_from: IntProperty(name='Landing starts at its frame', default=16, min=0, max=200)
    effect: IntProperty(name='Sparkle (own effect #)', default=0x3C, min=0, max=255)
    ghost: BoolProperty(name='Fade out (ghost)', default=True, description='He turns blue, then to light, and dissolves')
    trail: BoolProperty(name='Warp trail', default=True, description='Wispy line behind the flying sword that breaks into particles when it lands')
    trail_len: FloatProperty(name='Trail max length', default=1.0, min=0.1, max=5, unit='LENGTH')
    bolts: BoolProperty(name='Lightning on the sword', default=False, description='Crackle round the hovering sword just before he arrives')
    streak: BoolProperty(name='Warp streak', default=False, description='Blue streak from where he vanishes to the sword')
    shards: BoolProperty(name='Shatter after the catch', default=False, description='The sword breaks into crystal shards after the roll')

    @classmethod
    def poll(cls, context):
        a = active_arm(context)
        return a is not None and moveset_bl.has_moveset(a)

    def invoke(self, context, event):
        return context.window_manager.invoke_props_dialog(self, width=320)

    def execute(self, context):
        arm = active_arm(context)
        acts = tools_bl.dash_actions(arm, self.kind)
        if not acts or not self.weapon:
            self.report({'ERROR'}, 'No dash animations or weapon found'); return {'CANCELLED'}
        try:
            base = {'DASH': warp_mod.RUN_DEFAULTS, 'HOLDX': warp_mod.DASH_DEFAULTS}.get(self.kind, warp_mod.DEFAULTS)
            val = lambda name: getattr(self, name) or base[name]
            w, r, v = val('windup'), val('release'), val('vanish')
            rp = self.reappear or base['reappear']
            if not (w < r < v):
                raise RuntimeError('Frames must go crouch < let go < vanish')
            if rp is not None and not v < rp:
                raise RuntimeError('Frames must go crouch < let go < vanish < reappear')
            if self.kind == 'HOLDX' and rp > warp_mod.DASH_PLAY_END - 1:
                raise RuntimeError(f'The engine cuts this dash after about {warp_mod.DASH_PLAY_END} frames: reappear by frame {warp_mod.DASH_PLAY_END - 1}')
            params = dict(distance=self.distance, windup=w, release=r, vanish=v, reappear=rp,
                          land_motion=self.land_motion or base['land_motion'], land_from=self.land_from, trail_len=self.trail_len)
            if self.kind == 'DODGE':
                params['land_motion'] = self.land_motion or '_150A'
            ob, done = tools_bl.apply_warp_dash(context, arm, bpy.data.objects[self.weapon], acts, params, self.effect,
                                                ghost=self.ghost, trail=self.trail, bolts=self.bolts, streak=self.streak, shards=self.shards, kind=self.kind)
            acts = tools_bl.dash_actions(arm, self.kind)
        except Exception as e:
            self.report({'ERROR'}, str(e)); return {'CANCELLED'}
        tools_bl.set_action(arm, acts[0])
        context.scene.frame_set(0)
        self.report({'INFO'}, 'Warp dash on ' + ', '.join(done))
        return {'FINISHED'}


def _slot_items(self, context):
    arm = active_arm(context)
    if not arm:
        return [('', 'None', '')]
    obs = sorted([o for o in arm.children if o.type == 'MESH' and o.get('dfx_part_index', 0) != 0 and not o.get('dfx_fx')
                  and not o.name.startswith('warp_')], key=lambda o: o.name)
    kit = tools_bl.NOCTIS_KIT
    _SLOT_CACHE[:] = [(o.name, f"{o.name}{' (' + kit[o.name] + ')' if o.name in kit else ''}", '') for o in obs] or [('', 'None', '')]
    return _SLOT_CACHE


_SLOT_CACHE = []


class DFX_OT_reference_clip(bpy.types.Operator, ImportHelper):
    """Show a reference video or image sequence beside the character and behind him in a 'Reference camera' view, synced to the timeline. For an image sequence, pick its first frame"""
    bl_idname = 'dfx.reference_clip'
    bl_label = 'Reference clip…'
    bl_options = {'REGISTER', 'UNDO'}
    filter_glob: StringProperty(default='*.jpg;*.jpeg;*.png;*.mov;*.mp4;*.m4v;*.avi;*.mkv;*.webm', options={'HIDDEN'})
    start_frame: IntProperty(name='Clip starts at frame', default=0,
                             description='Scene frame that shows the first picture of the clip')

    def execute(self, context):
        try:
            img, ob, cam = tools_bl.add_reference_clip(context, self.filepath, self.start_frame)
        except Exception as e:
            self.report({'ERROR'}, str(e)); return {'CANCELLED'}
        self.report({'INFO'}, f'Reference clip: {img.name} ({img.source.lower()}) from frame {self.start_frame}')
        return {'FINISHED'}


class DFX_OT_tidy_bones(bpy.types.Operator):
    """Sort the bones into collections (Body, Fingers, Cape & hair shown; weapons, warp ghost, effects, system hidden) and draw the body as a stick figure. Display only: export is unaffected"""
    bl_idname = 'dfx.tidy_bones'
    bl_label = 'Tidy bones'
    bl_options = {'REGISTER', 'UNDO'}

    @classmethod
    def poll(cls, context):
        return active_arm(context) is not None

    def execute(self, context):
        from . import rig_display
        counts = rig_display.tidy_rig(active_arm(context))
        self.report({'INFO'}, ', '.join(f'{k} {v}' for k, v in counts.items() if v))
        return {'FINISHED'}


class DFX_OT_build_rig(bpy.types.Operator):
    """Add a standard control rig (master, torso, hips, spine, FK/IK arms and legs with poles, fingers, toes, cape). The game bones follow it; export bakes it back onto them"""
    bl_idname = 'dfx.build_rig'
    bl_label = 'Build control rig'
    bl_options = {'REGISTER', 'UNDO'}

    @classmethod
    def poll(cls, context):
        return active_arm(context) is not None

    def execute(self, context):
        from . import rig_control
        arm = active_arm(context)
        act = arm.animation_data.action if arm.animation_data else None
        made = rig_control.build_rig(context, arm)
        if act is not None:
            tools_bl.set_action(arm, act)
        self.report({'INFO'}, f'Control rig: {len([n for n in made if not n.startswith("MCH-")])} controls')
        return {'FINISHED'}


class DFX_OT_rig_animate(bpy.types.Operator):
    """Animate this action with the controls: fits them to its current animation (every frame) and switches the rig on for it"""
    bl_idname = 'dfx.rig_animate'
    bl_label = 'Animate with controls'
    bl_options = {'REGISTER', 'UNDO'}

    @classmethod
    def poll(cls, context):
        from . import rig_control
        a = active_arm(context)
        return a is not None and rig_control.has_rig(a) and a.animation_data and a.animation_data.action

    def execute(self, context):
        from . import rig_control
        arm = active_arm(context); act = arm.animation_data.action
        n = rig_control.match_controls(context, arm, act)
        self.report({'INFO'}, f'{act.name}: {n} controls keyed from its animation')
        return {'FINISHED'}


class DFX_OT_rig_release(bpy.types.Operator):
    """Stop using the controls for this action: bakes their result onto the game bones' keys and switches the rig off"""
    bl_idname = 'dfx.rig_release'
    bl_label = 'Back to game keys'
    bl_options = {'REGISTER', 'UNDO'}

    @classmethod
    def poll(cls, context):
        a = active_arm(context)
        return a is not None and a.animation_data and a.animation_data.action and a.animation_data.action.get('dfx_rig')

    def execute(self, context):
        from . import rig_control
        arm = active_arm(context); act = arm.animation_data.action
        rig_control.bake_controls(context, arm, act)
        act['dfx_rig'] = False
        rig_control.set_rig_on(arm, False)
        self.report({'INFO'}, f'{act.name}: baked onto the game bones')
        return {'FINISHED'}


class DFX_OT_placeholders(bpy.types.Operator):
    """Put plain placeholder weapons in Noctis's slots (sword, spear, greatsword, shuriken, two daggers, shield)"""
    bl_idname = 'dfx.placeholder_weapons'
    bl_label = 'Placeholder weapons'
    bl_options = {'REGISTER', 'UNDO'}

    @classmethod
    def poll(cls, context):
        return active_arm(context) is not None

    def execute(self, context):
        done = tools_bl.install_placeholders(active_arm(context))
        self.report({'INFO'}, 'Placeholders: ' + ', '.join(done))
        return {'FINISHED'}


class DFX_OT_base_body(bpy.types.Operator):
    """Add a plain low-poly base body fitted to this skeleton (weighted, UV-unwrapped, flat colours) to model your character from"""
    bl_idname = 'dfx.base_body'
    bl_label = 'Add base body'
    bl_options = {'REGISTER', 'UNDO'}

    replace: BoolProperty(name='Use as the body', default=True,
                          description="Export the base body in place of the character's body. "
                                      "The original body is kept, hidden, as a reference")
    tex_size: EnumProperty(name='Texture', items=[('64', '64 × 64', ''), ('128', '128 × 128', ''),
                                                  ('256', '256 × 256', '')], default='128')

    @classmethod
    def poll(cls, context):
        return active_arm(context) is not None

    def invoke(self, context, event):
        return context.window_manager.invoke_props_dialog(self)

    def execute(self, context):
        from . import basebody
        arm = active_arm(context)
        if context.object is not None and context.object.mode != 'OBJECT':
            bpy.ops.object.mode_set(mode='OBJECT')
        try:
            ob, msg = basebody.create(arm, replace=self.replace, tex_size=int(self.tex_size))
        except RuntimeError as e:
            self.report({'ERROR'}, str(e))
            return {'CANCELLED'}
        for o in context.view_layer.objects:
            o.select_set(False)
        ob.select_set(True)
        context.view_layer.objects.active = ob
        self.report({'INFO'}, msg)
        return {'FINISHED'}


class DFX_OT_replace_weapon(bpy.types.Operator, ImportHelper):
    """Replace a weapon slot with your own model (FBX, glTF or OBJ). It is lined up with the old weapon, hilt end on the grip"""
    bl_idname = 'dfx.replace_weapon'
    bl_label = 'Replace weapon…'
    bl_options = {'REGISTER', 'UNDO'}
    filter_glob: StringProperty(default='*.fbx;*.glb;*.gltf;*.obj', options={'HIDDEN'})
    slot: EnumProperty(name='Slot', items=_slot_items)
    flip: BoolProperty(name='Flip direction', default=False, description='Use if it ends up held by the blade')
    keep_scale: BoolProperty(name='Keep model scale', default=False, description='Otherwise it is scaled to the old weapon\'s length')
    length: FloatProperty(name='Length', default=0.0, min=0.0, max=5.0, unit='LENGTH',
                          description='Total length, pommel to tip. 0 = the old weapon\'s length')
    max_tris: IntProperty(name='Max triangles', default=1200, min=50, max=20000, description='Detailed models are reduced to about this many triangles for the PSP')
    grip_at_origin: BoolProperty(name='Grip at the model\'s origin', default=False,
                                 description='Put the hand on the model\'s own origin (game rips are made that way) instead of the handle found from the shape')

    @classmethod
    def poll(cls, context):
        return active_arm(context) is not None

    def execute(self, context):
        arm = active_arm(context)
        try:
            n, k = tools_bl.replace_weapon_from_file(context, arm, bpy.data.objects[self.slot], self.filepath, self.flip, self.keep_scale, max_tris=self.max_tris, length=self.length,
                                                       grip_at_origin=self.grip_at_origin)
        except Exception as e:
            self.report({'ERROR'}, str(e)); return {'CANCELLED'}
        context.view_layer.objects.active = arm
        self.report({'INFO'}, f'{self.slot}: {n} vertices, scaled ×{k:.2f}')
        return {'FINISHED'}


class DFX_OT_retarget(bpy.types.Operator, ImportHelper):
    """Copy animations from another Dissidia character's model onto this skeleton (matched by bone name)"""
    bl_idname = 'dfx.retarget'
    bl_label = 'Retarget from character…'
    bl_options = {'UNDO'}
    filename_ext = '.gmo'
    filter_glob: StringProperty(default='*.gmo', options={'HIDDEN'})
    only_active: BoolProperty(name='Only the active animation', default=False)

    @classmethod
    def poll(cls, context):
        return active_arm(context) is not None

    def execute(self, context):
        arm = active_arm(context)
        only = None
        if self.only_active and arm.animation_data and arm.animation_data.action:
            only = {arm.animation_data.action.name}
        try:
            names = tools_bl.retarget_into(arm, self.filepath, only)
        except Exception as e:
            self.report({'ERROR'}, str(e)); return {'CANCELLED'}
        self.report({'INFO'}, f'Retargeted {len(names)} animation(s)')
        return {'FINISHED'}


_base_items = []        # Blender keeps pointers to dynamic enum strings, so they must outlive the callback


def _base_action_items(self, context):
    items = [('AUTO', 'First game animation', "Hold the bones the clip does not drive (fingers, cape, weapon slots) in the first pose of the character's first animation"),
             ('NONE', 'Bind pose', 'Leave the bones the clip does not drive in the model\'s bind pose')]
    arm = active_arm(context)
    if arm is not None:
        items += [(a.name, a.name, '') for a in moveset_bl.actions_for(arm)]
    _base_items[:] = items
    return _base_items


class DFX_OT_retarget_fbx(bpy.types.Operator, ImportHelper):
    """Retarget an animation from another game's rig (FBX / glTF from Flagrum, Noesis, Mixamo...) onto this skeleton as a new action"""
    bl_idname = 'dfx.retarget_fbx'
    bl_label = 'Retarget from FBX/glTF…'
    bl_options = {'UNDO'}
    filename_ext = '.fbx'
    filter_glob: StringProperty(default='*.fbx;*.glb;*.gltf', options={'HIDDEN'})
    clip_name: StringProperty(name='Action name', default='', description='Name of the new action (blank: the clip\'s own name; 31 characters at most)')
    all_actions: BoolProperty(name='All clips in the file', default=False,
                              description='Make one action per animation in the file (otherwise the one that was playing, or the longest)')
    clip: StringProperty(name='Clip', default='', description='Only this clip of the file (blank: automatic)')
    frame_start: IntProperty(name='First frame', default=0, min=0, description='First source frame to use (0 and 0: the whole clip)')
    frame_end: IntProperty(name='Last frame', default=0, min=0)
    src_fps: FloatProperty(name='Source rate', default=0.0, min=0.0, description='Frames per second of the clip (0: the rate the importer set)')
    out_fps: FloatProperty(name='Game rate', default=30.0, min=1.0, description='Frames per second of the new action (the game plays motions at 30)')
    rest_align: BoolProperty(name='Align rest poses', default=True,
                             description='Turn each source limb\'s rest direction onto this skeleton\'s (T-pose arms onto an A-pose, for example)')
    orient: EnumProperty(name='Facing', items=[('AUTO', 'Detect', 'Find the source\'s left and up axes from its rest pose'),
                                               ('NONE', 'As imported', 'Use the importer\'s world axes as they are')], default='AUTO')
    yaw: FloatProperty(name='Extra turn', default=0.0, soft_min=-180.0, soft_max=180.0, description='Turn the source about the up axis (degrees)')
    translation: EnumProperty(name='Body travel', items=[('FULL', 'Full', 'Hips travel and height'), ('VERTICAL', 'Height only', 'Keep the hips\' height, drop the travel'),
                                                         ('IN_PLACE', 'In place', 'Keep the hips\' own sway and height, take off the clip\'s travel (its root bone, e.g. FFXV Trans)'),
                                                         ('NONE', 'None', 'Leave the body where it is')], default='FULL')
    scale: FloatProperty(name='Travel scale', default=0.0, min=0.0, description='0: the ratio of the two skeletons\' hip heights')
    travel_to_root: BoolProperty(name='Travel on trans_include', default=True,
                                 description='In place: put the clip\'s travel on the game\'s root-motion bone (trans_include), as the game\'s own motions do')
    foot_fix: BoolProperty(name='Keep feet above ground', default=True, description='Lift the body when a foot sinks well below its standing height')
    start_at_origin: BoolProperty(name='Start at origin', default=True, description='Remove the first frame\'s sideways offset (clips are often recorded far from the origin)')
    base_action: EnumProperty(name='Other bones from', items=_base_action_items)
    review: BoolProperty(name='Check unsure bone maps', default=True,
                         description='When the bone map had to be guessed and looks incomplete, write it next to the file and stop so it can be edited')

    @classmethod
    def poll(cls, context):
        return active_arm(context) is not None

    def execute(self, context):
        arm = active_arm(context)
        base = self.base_action
        if base == 'AUTO':
            known = [a for a in moveset_bl.actions_for(arm) if 'dfx_motion_index' in a]
            base = known[0].name if known else ''
        elif base == 'NONE':
            base = ''
        try:
            res = tools_bl.retarget_from_file(
                context, arm, self.filepath, name=self.clip_name, action_name=self.clip, all_actions=self.all_actions,
                frame_start=self.frame_start, frame_end=self.frame_end, src_fps=self.src_fps, out_fps=self.out_fps,
                rest_align=self.rest_align, orient=self.orient, yaw=self.yaw, translation=self.translation,
                scale=self.scale, foot_fix=self.foot_fix, start_at_origin=self.start_at_origin, base_action=base,
                review=self.review,
                travel_bone='trans_include' if (self.translation == 'IN_PLACE' and self.travel_to_root) else '')
        except Exception as e:
            self.report({'ERROR'}, str(e)); return {'CANCELLED'}
        bm = res['bonemap']
        txt = bpy.data.texts.get('Retarget bone map') or bpy.data.texts.new('Retarget bone map')
        txt.clear(); txt.write(bm.describe() + f"\n\nEdit {res['map_path']} to change the map.\n")
        if res['stopped']:
            miss = ', '.join(bm.missing_required) or 'none'
            self.report({'WARNING'}, f"Bone map written to {res['map_path']} (confidence {bm.confidence:.0%}, missing: {miss}). "
                                     f"Edit it and run again; the guess is in the Text Editor as 'Retarget bone map'.")
            return {'CANCELLED'}
        if res['actions']:
            context.scene.dfx_action_index = bpy.data.actions.find(res['actions'][0])
        notes = f" Unmapped: {', '.join(bm.unmapped)}." if bm.unmapped else ''
        self.report({'INFO'}, f"Created {', '.join(res['actions'])} (bone map {bm.confidence:.0%}).{notes}")
        return {'FINISHED'}


class DFX_OT_copy_to_costume(bpy.types.Operator, ImportHelper):
    """Write this character's animations into another costume's model file (e.g. 2p, 3p, manikin)"""
    bl_idname = 'dfx.copy_to_costume'
    bl_label = 'Copy animations to costume…'
    filename_ext = '.gmo'
    filter_glob: StringProperty(default='*.gmo', options={'HIDDEN'})
    only_active: BoolProperty(name='Only the active animation', default=False)

    @classmethod
    def poll(cls, context):
        return active_arm(context) is not None

    def execute(self, context):
        arm = active_arm(context)
        names = None
        if self.only_active and arm.animation_data and arm.animation_data.action:
            names = {arm.animation_data.action.name}
        base = os.path.basename(self.filepath)
        out_dir = os.path.join(work_dir(context), 'out'); os.makedirs(out_dir, exist_ok=True)
        out = os.path.join(out_dir, base)
        try:
            copied, missing = tools_bl.copy_to_costume(arm, self.filepath, out, names)
        except Exception as e:
            self.report({'ERROR'}, str(e)); return {'CANCELLED'}
        add_patch(context.scene, 'obj/' + base, out)
        note = f'; bones not in that costume were skipped: {", ".join(missing[:4])}' if missing else ''
        self.report({'INFO'}, f'Copied {len(copied)} animation(s) into {base}{note}')
        return {'FINISHED'}


class DFX_OT_iso_extract_costume(bpy.types.Operator):
    """Extract another costume's model from the ISO into the working folder (for Copy animations to costume)"""
    bl_idname = 'dfx.iso_extract_costume'
    bl_label = 'Extract costume file'
    costume: EnumProperty(name='Costume', items=[(c, n, '') for c, n in COSTUMES])

    def execute(self, context):
        from .dfx.iso import IsoPackage
        arm = active_arm(context)
        code = context.scene.dfx_char
        if arm and arm.get('dfx_pkg_gmo'):
            code = arm['dfx_pkg_gmo'].split('p_')[1].split('_')[0]
        name = f'obj/p_{code}_{self.costume}.gmo'
        pk = IsoPackage(bpy.path.abspath(prefs(context).iso_path))
        if not pk.has(name):
            self.report({'ERROR'}, f'{name} not found'); return {'CANCELLED'}
        d = os.path.join(work_dir(context), code); os.makedirs(d, exist_ok=True)
        open(os.path.join(d, os.path.basename(name)), 'wb').write(pk.read(name)); pk.close()
        self.report({'INFO'}, f'Extracted {os.path.basename(name)} to {d}')
        return {'FINISHED'}


# ---------------------------------------------------------------- animation list
def _set_action_from_index(self, context):
    arm = active_arm(context)
    if arm is None or not (0 <= self.dfx_action_index < len(bpy.data.actions)):
        return
    act = bpy.data.actions[self.dfx_action_index]
    if act.get('dfx_armature') != arm.name:
        return
    tools_bl.set_action(arm, act)
    context.scene.frame_start = int(act.frame_range[0])
    context.scene.frame_end = int(act.frame_range[1])
    moveset_bl.refresh_markers(context.scene, arm, act)
    context.view_layer.update()                          # pose of the new action first
    moveset_bl.update_visibility(context.scene, arm)


class DFX_UL_actions(bpy.types.UIList):
    def draw_item(self, context, layout, data, item, icon, active_data, active_propname, index):
        arm = active_arm(context)
        row = layout.row(align=True)
        row.label(text=item.name, icon='ACTION')
        users = moveset_bl.motion_users(arm, item) if arm else []
        sub = row.row(); sub.alignment = 'RIGHT'
        tag = ', '.join(users[:2]) + ('…' if len(users) > 2 else '') if users else ('new' if 'dfx_motion_index' not in item else '')
        sub.label(text=f'{tag}  {int(item.frame_range[1])}f')

    def filter_items(self, context, data, propname):
        arm = active_arm(context)
        items = getattr(data, propname)
        flags = [0] * len(items)
        order = list(range(len(items)))
        if arm:
            for i, a in enumerate(items):
                if a.get('dfx_armature') == arm.name and (not self.filter_name or self.filter_name.lower() in a.name.lower()):
                    flags[i] = self.bitflag_filter_item
            key = [(0, a.get('dfx_motion_index', 10 ** 6), a.name) for a in items]
            srt = sorted(range(len(items)), key=lambda i: key[i])
            order = [0] * len(items)
            for pos, i in enumerate(srt):
                order[i] = pos
        return flags, order


# ---------------------------------------------------------------- panel
def section(layout, idname, title, icon='NONE', closed=False):
    """A fold-out section (Blender remembers whether it is open). Returns its layout, or None while it is closed."""
    header, body = layout.panel(idname, default_closed=closed)
    header.label(text=title, icon=icon)
    return body


def draw_locations(L, context):
    """Where the files are, at the top as in the Move Studio: the game ISO and the mods folder (found by itself from
    the repo when the add-on runs from it)."""
    from . import charmods
    p = prefs(context)
    if p is None:
        return
    iso, md = charmods.iso_path(context), charmods.mods_dir(context)
    col = L.column(align=True)
    sp = col.split(factor=0.25, align=True)
    sp.label(text='ISO', icon='CHECKMARK' if iso and os.path.isfile(iso) else 'DISC')
    sp.prop(p, 'iso_path', text='')
    sp = col.split(factor=0.25, align=True)
    sp.label(text='Mods', icon='CHECKMARK' if md else 'FILE_FOLDER')
    sp.prop(p, 'mods_dir', text='')
    if md and not p.mods_dir:
        col.label(text='Found: ' + md, icon='INFO')
    elif not md:
        col.label(text='Set the repo\'s mods folder for your characters', icon='INFO')


class DFX_PT_main(bpy.types.Panel):
    bl_label = 'Dissidia Forge'
    bl_space_type = 'VIEW_3D'
    bl_region_type = 'UI'
    bl_category = 'Dissidia'

    def draw(self, context):
        L = self.layout
        sc = context.scene
        p = prefs(context)
        draw_locations(L, context)
        box = section(L, 'dfx_iso', 'Character Selector', 'COMMUNITY')
        if box:
            from . import charmod_moves
            charmod_moves.draw_selector(box, context)
        arm = active_arm(context)
        if arm is None:
            L.label(text='Import a character, or select its armature.', icon='INFO')
            return
        box = section(L, 'dfx_char', arm.name, 'ARMATURE_DATA')
        if box:
            if moveset_bl.has_moveset(arm):
                box.label(text=f"Moveset: {arm.get('dfx_objx_name')}", icon='CHECKMARK')
            else:
                box.operator('dfx.load_moveset', icon='FILE')
            box.operator('dfx.export_all', icon='EXPORT')

        box = section(L, 'dfx_anims', 'Animations', 'ACTION')
        if box:
            box.template_list('DFX_UL_actions', '', bpy.data, 'actions', sc, 'dfx_action_index', rows=8)
            box.operator('dfx.new_anim', icon='ADD')
            act = arm.animation_data.action if arm.animation_data else None
            if act is not None:
                users = moveset_bl.motion_users(arm, act)
                box.label(text=f"{act.name}: {', '.join(users) if users else 'not used by a move yet'}")
        from . import rig_control
        box = section(L, 'dfx_rig', 'Rig', 'ARMATURE_DATA')
        if box:
            if not rig_control.has_rig(arm):
                box.operator('dfx.build_rig', icon='ADD')
            else:
                act = arm.animation_data.action if arm.animation_data else None
                if act is not None and act.get('dfx_rig'):
                    box.label(text=f'{act.name}: animated with controls', icon='CHECKMARK')
                    box.operator('dfx.rig_release', icon='LOOP_BACK')
                else:
                    box.label(text=(f'{act.name}: game keys' if act else 'No action'), icon='INFO')
                    box.operator('dfx.rig_animate', icon='POSE_HLT')
                m = arm.pose.bones.get('CTL-master')
                if m is not None:
                    col = box.column(align=True)
                    for k, lab in (('ik_arm_L', 'Arm L'), ('ik_arm_R', 'Arm R'), ('ik_leg_L', 'Leg L'), ('ik_leg_R', 'Leg R')):
                        if k in m:
                            col.prop(m, f'["{k}"]', text=f'{lab}  FK ↔ IK', slider=True)
                box.operator('dfx.build_rig', text='Rebuild control rig', icon='FILE_REFRESH')

        box = section(L, 'dfx_tools', 'Tools', 'TOOL_SETTINGS')
        if box:
            box.operator('dfx.base_body', icon='OUTLINER_OB_ARMATURE')
            box.operator('dfx.reference_clip', icon='FILE_MOVIE')
            box.operator('dfx.tidy_bones', icon='BONE_DATA')
            row = box.row(align=True)
            row.operator('dfx.add_cape', icon='MOD_CLOTH')
            row.operator('dfx.weight_cape', text='Weight to cape', icon='MOD_VERTEX_WEIGHT')
            row = box.row(align=True)
            row.operator('dfx.replace_weapon', icon='IMPORT')
            row.operator('dfx.placeholder_weapons', icon='MESH_CUBE')
            box.operator('dfx.retarget', icon='ARMATURE_DATA')
            box.operator('dfx.retarget_fbx', icon='ARMATURE_DATA')
            row = box.row(align=True)
            row.operator('dfx.copy_to_costume', icon='COPYDOWN')
            row.operator_menu_enum('dfx.iso_extract_costume', 'costume', text='', icon='DISC')

        q = patches(sc)
        box = section(L, 'dfx_isobuild', f'ISO build ({len(q)} file{"s" if len(q) != 1 else ""} queued)', 'PACKAGE')
        if box:
            for k in list(q)[:8]:
                box.label(text=k)
            row = box.row(align=True)
            row.operator('dfx.build_iso', icon='DISC')
            row.operator('dfx.clear_patches', text='', icon='X')


class DFX_OT_enable_preview(bpy.types.Operator):
    """Hide weapons that the current animation's show-weapon events don't show"""
    bl_idname = 'dfx.enable_preview'
    bl_label = 'Weapon visibility preview'

    def execute(self, context):
        arm = active_arm(context)
        arm['dfx_wep_preview'] = True
        moveset_bl.update_visibility(context.scene, arm)
        return {'FINISHED'}


classes = (DFX_Prefs, DFX_OT_iso_import, DFX_OT_export_all, DFX_OT_build_iso, DFX_OT_clear_patches,
           DFX_OT_load_moveset, DFX_OT_refresh_markers, DFX_OT_write_markers, DFX_OT_copy_events, DFX_OT_new_anim,
           DFX_OT_warp_dash, DFX_OT_reference_clip, DFX_OT_tidy_bones, DFX_OT_build_rig, DFX_OT_rig_animate, DFX_OT_rig_release, DFX_OT_placeholders, DFX_OT_base_body, DFX_OT_replace_weapon, DFX_OT_retarget, DFX_OT_retarget_fbx, DFX_OT_copy_to_costume, DFX_OT_iso_extract_costume,
           DFX_OT_enable_preview, DFX_UL_actions, DFX_PT_main)


def register():
    for c in classes:
        bpy.utils.register_class(c)
    bpy.types.Scene.dfx_char = EnumProperty(name='Character', items=[(c, f'{n} ({c})', '') for c, n in ROSTER], default='fiv100')
    bpy.types.Scene.dfx_costume = EnumProperty(name='Costume', items=[(c, n, '') for c, n in COSTUMES])
    bpy.types.Scene.dfx_action_index = IntProperty(update=_set_action_from_index)
    if moveset_bl.frame_handler not in bpy.app.handlers.frame_change_post:
        bpy.app.handlers.frame_change_post.append(moveset_bl.frame_handler)


def unregister():
    if moveset_bl.frame_handler in bpy.app.handlers.frame_change_post:
        bpy.app.handlers.frame_change_post.remove(moveset_bl.frame_handler)
    for p in ('dfx_char', 'dfx_costume', 'dfx_action_index'):
        delattr(bpy.types.Scene, p)
    for c in reversed(classes):
        bpy.utils.unregister_class(c)
