"""Character mods panel (View3D > Sidebar > Dissidia > Character mods).

The repo's character mod folders (mods/<name>/mod.json) are listed here. For the chosen character it
shows which skins it has and which one the game build uses, and the model and weapons it wants (mod.json
body.model_hint / body.weapon_groups), each replaced with any model file (own_model.py). Use sets mod.json body.skin;
Build writes the character into the game's mods folder (tools/charmod.py build).
"""
import json
import os
import subprocess
import sys
import tempfile
import threading
import time
from collections import OrderedDict

import bpy
from bpy.props import EnumProperty, StringProperty

ADDON = __package__
HERE = os.path.dirname(os.path.abspath(__file__))
DEFAULT_SKINS = {'manikin': 'the based-on character\'s manikin (from your disc)', 'as_is': 'the models as they are'}


# ---------------------------------------------------------------- folders and settings
def _prefs(context):
    a = context.preferences.addons.get(ADDON)
    return a.preferences if a else None


# where a mods folder may sit below a folder you point at (the repo, the mods folder itself, or the project folder
# that holds the ISO and dissidia-recomp/); the old places (psprecomp/games/dissidia/mods) still work
SUBPATHS = ('', 'mods', os.path.join('dissidia-recomp', 'mods'), os.path.join('games', 'dissidia', 'mods'),
            os.path.join('psprecomp', 'games', 'dissidia', 'mods'), os.path.join('dissidia-recomp', 'psprecomp', 'games', 'dissidia', 'mods'))


def is_mods_dir(d):
    try:
        return any(not n.startswith(('_', '.')) and os.path.isfile(os.path.join(d, n, 'mod.json')) for n in os.listdir(d))
    except OSError:
        return False


def find_mods_dir(base):
    for sp in SUBPATHS:
        d = os.path.normpath(os.path.join(base, sp))
        if is_mods_dir(d):
            return d
    return ''


def auto_mods_dir():
    """mods/ next to the add-on when it runs from the repo (blender/io_scene_dissidia)."""
    return find_mods_dir(os.path.normpath(os.path.join(os.path.realpath(HERE), '..', '..')))


def search_places(context):
    """Folders searched, in order: the preference, the add-on's own place in the repo, the ISO's folder."""
    p = _prefs(context)
    out = []
    if p is not None and getattr(p, 'mods_dir', ''):
        out.append(bpy.path.abspath(p.mods_dir))
    out.append(os.path.normpath(os.path.join(os.path.realpath(HERE), '..', '..')))
    iso = iso_path(context)
    if iso:
        out.append(os.path.dirname(iso))
    return out


def mods_dir(context):
    for base in search_places(context):
        d = find_mods_dir(base)
        if d:
            return d
    return ''


def tools_dir(md):
    """The repo's tools for a mods folder: psprecomp/games/dissidia/tools next to mods/ (or ../tools, the old layout)."""
    for d in (os.path.join(md, '..', 'psprecomp', 'games', 'dissidia', 'tools'), os.path.join(md, '..', 'tools')):
        d = os.path.normpath(d)
        if os.path.isfile(os.path.join(d, 'charmod.py')):
            return d
    return os.path.normpath(os.path.join(md, '..', 'psprecomp', 'games', 'dissidia', 'tools'))


def iso_path(context):
    p = _prefs(context)
    v = getattr(p, 'iso_path', '') if p is not None else ''
    return bpy.path.abspath(v) if v else ''


_SOURCES = {}                                   # fallback when the add-on has no preferences (scripts / tests)


def get_source(context, key):
    p = _prefs(context)
    if p is not None and hasattr(p, 'mod_sources'):
        try:
            return json.loads(p.mod_sources or '{}').get(key, '')
        except ValueError:
            return ''
    return _SOURCES.get(key, '')


def set_source(context, key, path):
    p = _prefs(context)
    if p is not None and hasattr(p, 'mod_sources'):
        try:
            d = json.loads(p.mod_sources or '{}')
        except ValueError:
            d = {}
        d[key] = path
        p.mod_sources = json.dumps(d)
        context.preferences.is_dirty = True
    else:
        _SOURCES[key] = path


# ---------------------------------------------------------------- reading the mod folders (cached by mtime)
_MODS = {}


def load_mod(md, name):
    p = os.path.join(md, name, 'mod.json')
    try:
        mt = os.path.getmtime(p)
    except OSError:
        return None
    c = _MODS.get(p)
    if c and c[0] == mt:
        return c[1]
    try:
        m = json.load(open(p), object_pairs_hook=OrderedDict)
    except ValueError as ex:
        m = {'_error': str(ex)}
    _MODS[p] = (mt, m)
    return m


def characters(md):
    out = []
    if not md:
        return out
    for n in sorted(os.listdir(md)):
        if n.startswith(('_', '.')):
            continue
        m = load_mod(md, n)
        if not m or m.get('enabled') is False:
            continue
        c = m.get('character') or {}
        out.append((n, c.get('name', n), m))
    return out


_ITEMS = []


def _char_items(self, context):
    global _ITEMS
    md = mods_dir(context)
    _ITEMS = [(n, '%s  (%s)' % (label, n) if label.lower() != n else label, '') for n, label, _ in characters(md)] or \
             [('', 'no character mods found', '')]
    return _ITEMS


def skins_of(md, name):
    d = os.path.join(md, name, 'skins')
    if not os.path.isdir(d):
        return []
    return sorted(n for n in os.listdir(d)
                  if os.path.isdir(os.path.join(d, n)) and any(f.endswith('.skin') for f in os.listdir(os.path.join(d, n))))


_ISO = {}


def iso_names(iso):
    """Set of package hashes in the ISO (cached), or None if it can't be read."""
    if not iso or not os.path.isfile(iso):
        return None
    key = (iso, os.path.getmtime(iso))
    if key not in _ISO:
        from .dfx.iso import IsoPackage
        try:
            pk = IsoPackage(iso)
            _ISO.clear()
            _ISO[key] = set(pk.index)
            pk.close()
        except Exception:
            _ISO[key] = None
    return _ISO[key]


JOB = {'proc': None, 'lines': [], 'status': '', 'result': None, 'error': None, 'what': '', 'log': ''}


def _reader(proc, log_path, more=()):
    with open(log_path, 'a') as log:
        log.write('$ %s\n' % ' '.join(proc.args))
        for line in proc.stdout:
            log.write(line)
            line = line.rstrip('\n')
            JOB['lines'].append(line)
            if line.startswith('DFX_PROGRESS '):
                JOB['status'] = line[13:]
            elif line.startswith('DFX_DONE '):
                try:
                    JOB['result'] = json.loads(line[9:])
                except ValueError:
                    JOB['result'] = {}
            elif line.startswith('DFX_ERROR '):
                JOB['error'] = line[10:]
            elif JOB.get('plain') and line.strip():               # tools without DFX_ lines: show what they print
                JOB['status'] = line.strip()
        proc.wait()
    if more and proc.returncode == 0:                         # the next command of a chain
        nxt = subprocess.Popen(more[0], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, bufsize=1)
        JOB['proc'] = nxt
        _reader(nxt, log_path, more[1:])
    JOB['chain_done'] = True


def running():
    p = JOB['proc']
    return p is not None and (p.poll() is None or not JOB.get('chain_done', True))


def _start(cmd, what, log_path, env=None, plain=False):
    """cmd: one command, or a list of commands run one after the other (stops at the first that fails)."""
    cmds = cmd if cmd and isinstance(cmd[0], (list, tuple)) else [cmd]
    JOB.update(lines=[], status='starting…', result=None, error=None, what=what, log=log_path, plain=plain,
               chain_done=False)
    open(log_path, 'w').close()
    JOB['proc'] = subprocess.Popen(cmds[0], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, bufsize=1,
                                   env=env)
    threading.Thread(target=_reader, args=(JOB['proc'], log_path, cmds[1:]), daemon=True).start()


# ---------------------------------------------------------------- a Python for the repo's tools


def tool_python():
    """The Python to start the repo's tools with: Blender's own. The tools bring their own numpy / Pillow / scipy
    (tools/pydeps.py sets them up in tools/.venv the first time, and the tool restarts itself there), so it does
    not matter which modules this Python has."""
    return sys.executable


def _redraw(context):
    for w in context.window_manager.windows:
        for a in w.screen.areas:
            if a.type == 'VIEW_3D':
                a.tag_redraw()


class _JobOperator:
    """Modal part shared by the jobs: polls the background process and reports when it ends."""
    _timer = None

    def modal(self, context, event):
        if event.type != 'TIMER':
            return {'PASS_THROUGH'}
        _redraw(context)
        if running():
            return {'PASS_THROUGH'}
        context.window_manager.event_timer_remove(self._timer)
        time.sleep(0.2)                                   # let the reader thread drain the pipe
        rc = JOB['proc'].returncode
        if rc != 0 and not JOB['error']:
            tail = [l for l in JOB['lines'] if l.strip()][-1:]
            JOB['error'] = tail[0].strip() if tail else None
        if rc == 0 and not JOB['error']:
            self.finished(context)
            return {'FINISHED'}
        err = JOB['error'] or 'stopped with code %s' % rc
        JOB['status'] = 'failed: %s (log: %s)' % (err, JOB['log'])
        self.report({'ERROR'}, '%s failed: %s. Full log: %s' % (JOB['what'], err, JOB['log']))
        return {'CANCELLED'}

    def start_modal(self, context):
        wm = context.window_manager
        self._timer = wm.event_timer_add(0.5, window=context.window)
        wm.modal_handler_add(self)
        return {'RUNNING_MODAL'}

    def finished(self, context):
        pass


def _work_dir():
    d = os.path.join(tempfile.gettempdir(), 'dissidia_charmods')
    os.makedirs(d, exist_ok=True)
    return d


# ---------------------------------------------------------------- operators
class DFX_OT_mod_refresh(bpy.types.Operator):
    """Read the character mod folders again"""
    bl_idname = 'dfx.mod_refresh'
    bl_label = 'Refresh'

    def execute(self, context):
        _MODS.clear(); _ISO.clear()
        _redraw(context)
        return {'FINISHED'}


class DFX_OT_mod_use_skin(bpy.types.Operator):
    """Use this body/weapons skin when the character is built into the game (mod.json body.skin)"""
    bl_idname = 'dfx.mod_use_skin'
    bl_label = 'Use'
    name: StringProperty(options={'HIDDEN'})
    skin: StringProperty(options={'HIDDEN'})

    def execute(self, context):
        md = mods_dir(context)
        p = os.path.join(md, self.name, 'mod.json')
        m = json.load(open(p), object_pairs_hook=OrderedDict)
        m.setdefault('body', OrderedDict())['skin'] = self.skin
        d = os.path.join(md, self.name, 'skins', self.skin)                 # every model the skin has a file for
        if os.path.isdir(d):
            from . import own_model
            have = [gp for gp, _ in own_model.character_models(md, self.name)
                    if os.path.isfile(os.path.join(d, os.path.splitext(os.path.basename(gp))[0] + '.skin'))]
            m['body']['models'] = list(OrderedDict.fromkeys(list(m['body'].get('models') or []) + have))
        with open(p, 'w') as f:
            json.dump(m, f, indent=1, ensure_ascii=False)
            f.write('\n')
        _MODS.clear()
        self.report({'INFO'}, '%s now uses "%s". Build to put it in the game (run.sh also builds at launch).'
                    % (self.name, self.skin))
        _redraw(context)
        return {'FINISHED'}


class DFX_OT_mod_build(_JobOperator, bpy.types.Operator):
    """Build this character into the game's mods folder (tools/charmod.py build), as run.sh does at launch"""
    bl_idname = 'dfx.mod_build'
    bl_label = 'Build into the game'
    name: StringProperty(options={'HIDDEN'})

    @classmethod
    def poll(cls, context):
        return not running()

    def execute(self, context):
        md = mods_dir(context)
        tool = os.path.join(tools_dir(md), 'charmod.py')
        if not os.path.isfile(tool):
            self.report({'ERROR'}, 'charmod.py not found next to the mods folder (%s)' % tool)
            return {'CANCELLED'}
        py = tool_python()
        wd = _work_dir()
        cmd = [py, tool, 'build', self.name]
        _start(cmd, 'Build %s' % self.name, os.path.join(wd, 'build_%s.txt' % self.name), plain=True)
        return self.start_modal(context)

    def finished(self, context):
        JOB['status'] = 'built %s into the game' % self.name
        self.report({'INFO'}, 'Built %s into the game.' % self.name)


def _like_items(self, context):
    from .dfx.iso import ROSTER
    return [(c, '%s (%s)' % (n, c), '') for c, n in ROSTER]


class DFX_OT_mod_new(_JobOperator, bpy.types.Operator):
    """Start a new character: a mod folder from the template, built on a retail character (its moves, model and
    pictures until you change them), with the next free roster slot. Then Build into the game"""
    bl_idname = 'dfx.mod_new'
    bl_label = 'New character'
    folder: StringProperty(name='Folder name', description='Lower case letters, digits, - and _, e.g. terra-esper')
    display: StringProperty(name='Name', description='Name shown in the menus (default: from the folder name)')
    full_name: StringProperty(name='Full name', description='VS screen and battle banner (default: the name)')
    series: StringProperty(name='Series', default='FINAL FANTASY', description='e.g. FINAL FANTASY VI')
    like: EnumProperty(name='Built on', items=_like_items,
                       description='The retail character whose moveset, model and pictures it starts from')
    picture: StringProperty(name='Picture', subtype='FILE_PATH',
                            description='Optional: a big full-body picture; the menu and HUD pictures are cut from it')

    @classmethod
    def poll(cls, context):
        return bool(mods_dir(context)) and not running()

    def invoke(self, context, event):
        return context.window_manager.invoke_props_dialog(self, width=420)

    def draw(self, context):
        L = self.layout
        L.prop(self, 'folder'); L.prop(self, 'display'); L.prop(self, 'full_name'); L.prop(self, 'series')
        L.prop(self, 'like'); L.prop(self, 'picture')
        L.label(text='Code and roster slot are picked automatically (the next free ones).', icon='INFO')

    def execute(self, context):
        import re
        md = mods_dir(context)
        name = self.folder.strip()
        if not re.match(r'^[a-z0-9][a-z0-9_-]*$', name):
            self.report({'ERROR'}, 'Folder name: lower case letters, digits, - and _ (e.g. terra-esper)')
            return {'CANCELLED'}
        if os.path.exists(os.path.join(md, name)):
            self.report({'ERROR'}, 'mods/%s already exists' % name)
            return {'CANCELLED'}
        pic = bpy.path.abspath(self.picture) if self.picture else ''
        if pic and not os.path.isfile(pic):
            self.report({'ERROR'}, 'Picture not found: %s' % pic)
            return {'CANCELLED'}
        py = tool_python()
        tool = os.path.join(tools_dir(md), 'charmod.py')
        cmd = [py, tool, 'new', name, '--like', self.like, '--series', self.series or 'FINAL FANTASY']
        for flag, v in (('--display', self.display), ('--full-name', self.full_name)):
            if v:
                cmd += [flag, v]
        cmds = [cmd]
        if pic:
            cmds.append([py, tool, 'avatars', name, '--from-picture', pic, '--names'])
        self._name = name
        _start(cmds, 'New character %s' % name, os.path.join(_work_dir(), 'new_%s.txt' % name), plain=True)
        return self.start_modal(context)

    def finished(self, context):
        _MODS.clear()
        try:
            context.window_manager.dfx_mod_char = self._name
            context.scene['dfx_pick_id'] = 'mod:' + self._name   # the Character Selector shows it
        except Exception:
            pass
        JOB['status'] = 'made %s: now Build into the game' % self._name
        self.report({'INFO'}, 'Made mods/%s. Build into the game to play it (run.sh also builds it at launch).'
                    % self._name)


class DFX_OT_mod_stop(bpy.types.Operator):
    """Stop the running job"""
    bl_idname = 'dfx.mod_stop'
    bl_label = 'Stop'

    def execute(self, context):
        if running():
            JOB['proc'].terminate()
        return {'FINISHED'}


# ---------------------------------------------------------------- panel
def section(layout, idname, title, icon='NONE', closed=False):
    header, body = layout.panel(idname, default_closed=closed)
    header.label(text=title, icon=icon)
    return body


def draw_no_mods(L, context):
    """Shown in the Character Selector when no mods folder is found."""
    p = _prefs(context)
    col = L.column()
    col.label(text='No character mods found.', icon='ERROR')
    col.label(text='Set the mods folder at the top (the repo or its mods folder).')
    col.label(text='Looked in:')
    for b in search_places(context):
        col.label(text='  ' + b)


def draw_mod(L, context, name):
    """The chosen character mod's details (Character Selector): skins, recipes, build."""
    try:
        _draw_mod(L, context, name)
    except Exception as ex:                              # show the problem instead of a blank section
        import traceback
        col = L.column()
        col.alert = True
        col.label(text='Character mods: %s: %s' % (type(ex).__name__, ex), icon='ERROR')
        col.label(text='If you just installed or updated the add-on, restart Blender.')
        traceback.print_exc()


def _draw_mod(L, context, name):
        md = mods_dir(context)
        m = load_mod(md, name) if name else None
        if not m:
            return
        if m.get('_error'):
            L.label(text='mod.json: ' + m['_error'], icon='ERROR')
            return
        c = m.get('character') or {}
        body = m.get('body') or {}
        if m.get('kind') == 'retail':                      # changes a retail character: no skins / avatars / slot
            L.label(text='Builds only the files that differ from the retail ones.', icon='INFO')
            row = L.row(align=True)
            row.enabled = not running()
            row.operator('dfx.mod_build', icon='EXPORT').name = name
            return
        # skins
        cur = body.get('skin', 'manikin')
        wspec = body.get('weapons')
        options = [('manikin' if body.get('models') else 'as_is', None)] + [(s, None) for s in skins_of(md, name)]
        box = section(L, 'dfx_mod_skin', 'Skin (body and weapons): %s' % cur, 'MOD_CLOTH')
        for s, _ in (options if box else []):
            row = box.row(align=True)
            label = s + ('  (default: %s)' % DEFAULT_SKINS[s] if s in DEFAULT_SKINS else '')
            if s == cur:
                row.label(text=label, icon='CHECKBOX_HLT')
            else:
                row.label(text=label, icon='CHECKBOX_DEHLT')
                op = row.operator('dfx.mod_use_skin', text='Use')
                op.name, op.skin = name, s
        if box and cur not in [s for s, _ in options]:
            box.label(text='mod.json uses "%s", which is not in skins/' % cur, icon='ERROR')
        if box and wspec and cur in ('manikin',):
            box.label(text='Default weapons: %s\'s own' % c.get('based_on', 'the based-on character'), icon='INFO')

        # the model and the weapons it wants: any model file for each (Blender fits it)
        box = section(L, 'dfx_mod_look', 'Model and weapons', 'OUTLINER_OB_ARMATURE')
        if box:
            from .panel import active_arm
            arm = active_arm(context)
            mine = arm is not None and arm.get('dfx_mod_name') == name
            if not mine:
                box.label(text='Import from mod folder first.', icon='INFO')
            col = box.column(align=True)
            if body.get('model_hint'):
                col.label(text='Body: ' + body['model_hint'])
            r = col.row()
            r.enabled = mine
            r.operator('dfx.own_model', text='Replace body with my model…', icon='OUTLINER_OB_ARMATURE')
            groups = body.get('weapon_groups') or []
            if not groups and mine:
                groups = [{'name': o.name.split('.')[0], 'parts': [o.name.split('.')[0]]} for o in sorted(arm.children, key=lambda o: o.name)
                          if o.type == 'MESH' and o.name.split('.')[0].startswith('wep') and o.name.split('.')[0][3:].isdigit()]
            for g in groups:
                col = box.column(align=True)
                r = col.row(align=True)
                r.label(text=g.get('name', g['parts'][0]))
                sub = r.row(align=True)
                sub.enabled = mine
                op = sub.operator('dfx.own_weapon', text='Use my model…', icon='IMPORT')
                op.slot = g['parts'][0]
                if g.get('hint'):
                    col.label(text=g['hint'])

        # build
        row = L.row(align=True)
        row.enabled = not running()
        row.operator('dfx.mod_build', icon='EXPORT').name = name
        if JOB['what'] or running():
            box = L.box()
            row = box.row(align=True)
            row.label(text=('%s: %s' % (JOB['what'], JOB['status'])) if running() else JOB['status'],
                      icon='SORTTIME' if running() else ('ERROR' if JOB['error'] else 'INFO'))
            if running():
                row.operator('dfx.mod_stop', text='', icon='CANCEL')


classes = (DFX_OT_mod_refresh, DFX_OT_mod_use_skin, DFX_OT_mod_build,
           DFX_OT_mod_new, DFX_OT_mod_stop)


def register():
    for c in classes:
        bpy.utils.register_class(c)
    bpy.types.WindowManager.dfx_mod_char = EnumProperty(name='Character', items=_char_items,
                                                        description='A character mod folder in the repo')


def unregister():
    del bpy.types.WindowManager.dfx_mod_char
    for c in reversed(classes):
        bpy.utils.unregister_class(c)
