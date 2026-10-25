"""The Move Studio's "Open in Blender" button.

The Studio is a page in Chrome, so it can't start anything on the computer. While Blender runs with this add-on, it
listens on 127.0.0.1:47612 (this computer only) for two requests:

    GET /ping                                         -> {"ok": true, "addon": "0.24.0", "mods": <mods folder>}
    GET /open?mod=<folder>&rig=models/<rig>.gmo&motion=<index>&name=<motion name>&frame=<frame>
        -> {"ok": true, "message": ...} once Blender shows that animation (or {"ok": false, "message": why})
    GET /own_model?mod=<folder>&rig=models/<rig>.gmo&skin=<name>[&costume=2p&assist=models/<assist>.gmo]
        -> {"ok": true, "job": id}: Blender opens the character and asks for a model file (own_model.py), then fits it
           and saves it as a skin;   GET /job?id=<id> -> {"state": picking|working|done|error|cancelled, "message",
           "skin", "models"} (answered at once, even while Blender is busy fitting)
    GET /own_weapon?mod=<folder>&rig=models/<rig>.gmo&slot=wep0[&skin=<name>]
        -> {"ok": true, "job": id}: the same for one weapon (own_model.py DFX_OT_own_weapon); the job ends with "slots"

/open imports the character's rig from the mods folder (Character Selector > Import from mod folder) unless it is
already in the scene, makes that armature active, selects the animation in the Animations list and goes to the frame.
The work runs on Blender's main thread (a timer); the request waits for it.
"""
import json
import os
import queue
import sys
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import urlparse, parse_qs

import bpy

PORT = 47612
_Q = queue.Queue()
_SERVER = None
JOBS = {}


def _version():
    from . import bl_info
    return '.'.join(str(x) for x in bl_info['version'])


class _Handler(BaseHTTPRequestHandler):
    def log_message(self, *a):
        pass

    def _send(self, obj, code=200):
        body = json.dumps(obj).encode()
        self.send_response(code)
        self.send_header('Content-Type', 'application/json')
        self.send_header('Access-Control-Allow-Origin', '*')            # the Studio is a file:// page (origin "null")
        self.send_header('Access-Control-Allow-Private-Network', 'true')
        self.send_header('Content-Length', str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_OPTIONS(self):
        self.send_response(204)
        self.send_header('Access-Control-Allow-Origin', '*')
        self.send_header('Access-Control-Allow-Methods', 'GET')
        self.send_header('Access-Control-Allow-Private-Network', 'true')
        self.end_headers()

    def do_GET(self):
        u = urlparse(self.path)
        q = {k: v[0] for k, v in parse_qs(u.query).items()}
        if u.path == '/job':                                 # no Blender call: read while the main thread works
            return self._send(dict(JOBS.get(q.get('id', ''), {'state': 'unknown', 'message': 'no such job'})))
        if u.path == '/ping':
            job = {'kind': 'ping', 'done': threading.Event()}
        elif u.path in ('/open', '/own_model', '/own_weapon'):
            job = {'kind': u.path[1:], 'q': q, 'done': threading.Event()}
        else:
            return self._send({'ok': False, 'message': 'unknown request'}, 404)
        _Q.put(job)
        if not job['done'].wait(120):
            return self._send({'ok': False, 'message': 'Blender is busy (a dialog or a long job?): try again'})
        self._send(job.get('result') or {'ok': False, 'message': 'no answer'})


def _mods_dir():
    from .charmod_moves import mods_dir
    return mods_dir(bpy.context)


def _bring_to_front():
    try:
        if sys.platform == 'darwin':
            import subprocess
            subprocess.Popen(['osascript', '-e', 'tell application id "org.blenderfoundation.blender" to activate'],
                             stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        else:
            w = bpy.context.window_manager.windows[0] if bpy.context.window_manager.windows else None
            if w is not None and hasattr(w, 'raise'):
                getattr(w, 'raise')()
    except Exception:
        pass


def _find_arm(md, name, rig):
    for o in bpy.context.view_layer.objects:
        if (o.type == 'ARMATURE' and o.get('dfx_mod_name') == name and o.get('dfx_mod_rig') == rig
                and os.path.normpath(o.get('dfx_mod_dir', '')) == os.path.normpath(md)):
            return o
    return None


def _open(q, need_action=True):
    from . import charmod_moves as CM, moveset_bl
    md = _mods_dir()
    name, rig = q.get('mod', ''), q.get('rig', '')
    if not md:
        return {'ok': False, 'message': "Blender doesn't know the mods folder yet: set it in the Dissidia panel (Character Selector)"}
    if not os.path.isdir(os.path.join(md, name)):
        return {'ok': False, 'message': "Blender's mods folder (%s) has no %s: point Blender and the Studio at the same mods folder" % (md, name)}
    if rig not in dict(CM.rigs(md, name)):
        return {'ok': False, 'message': '%s has no rig %s with a moveset in %s' % (name, rig, md)}
    ctx = bpy.context
    arm = _find_arm(md, name, rig)
    opened = False
    if arm is None:
        ctx.scene['dfx_pick_id'] = 'mod:' + name
        wm = ctx.window_manager
        try:
            wm.dfx_mod_char = name
        except Exception:
            pass
        wm.dfx_moves_rig = rig
        try:
            r = bpy.ops.dfx.mod_open_moves()
        except Exception as ex:
            return {'ok': False, 'message': 'Import failed: %s' % ex}
        if 'FINISHED' not in r:
            return {'ok': False, 'message': 'Import failed (see the Info area in Blender)'}
        arm = _find_arm(md, name, rig)
        opened = True
        if arm is None:
            return {'ok': False, 'message': 'Imported, but the armature was not found'}
    for o in ctx.view_layer.objects:
        o.select_set(False)
    ctx.view_layer.objects.active = arm
    arm.select_set(True)
    ctx.scene['dfx_pick_id'] = 'mod:' + name
    acts = moveset_bl.actions_for(arm)
    act = None
    if q.get('motion', '').lstrip('-').isdigit():
        mi = int(q['motion'])
        act = next((a for a in acts if a.get('dfx_motion_index') == mi), None)
    if act is None and q.get('name'):
        act = next((a for a in acts if a.name == q['name'] or a.name.split('.')[0] == q['name']), None)
    if act is None and not need_action:
        _bring_to_front()
        return {'ok': True, 'message': name, 'imported': opened}
    if act is None:
        return {'ok': False, 'message': '%s is open, but has no animation %s (%s): save in the Studio, then Import from mod folder again'
                % (name, q.get('name', '?'), q.get('motion', '?'))}
    ctx.scene.dfx_action_index = bpy.data.actions.find(act.name)     # the Animations list: sets the action, range, markers
    try:
        f = float(q.get('frame', ''))
        ctx.scene.frame_set(int(round(max(act.frame_range[0], min(act.frame_range[1], f)))))
    except ValueError:
        pass
    _bring_to_front()
    return {'ok': True, 'message': '%s%s: %s' % ('Imported ' if opened else '', name, act.name), 'imported': opened}


class _Null:
    def __enter__(self):
        return self

    def __exit__(self, *a):
        return False


def _pump():
    while True:
        try:
            job = _Q.get_nowait()
        except queue.Empty:
            break
        try:
            wm = bpy.context.window_manager
            win = wm.windows[0] if wm and wm.windows else None
            with (bpy.context.temp_override(window=win, screen=win.screen) if win else _Null()):   # a timer has no window
                job['result'] = (_ping() if job['kind'] == 'ping' else _open(job['q']) if job['kind'] == 'open'
                                 else _own_weapon(job['q']) if job['kind'] == 'own_weapon' else _own_model(job['q']))
        except Exception as ex:
            job['result'] = {'ok': False, 'message': 'Blender: %s' % ex}
        job['done'].set()
    return 0.2


def _own_model(q):
    """open the character, then the file browser of dfx.own_model (it runs when the user picks the file)"""
    r = _open(dict(q, motion='', name=''), need_action=False)
    if not r.get('ok'):
        return r
    import uuid
    jid = uuid.uuid4().hex[:12]
    JOBS[jid] = {'state': 'picking', 'message': 'Pick the model in Blender'}
    try:
        bpy.ops.dfx.own_model('INVOKE_DEFAULT', job=jid, skin=q.get('skin', ''), costume=q.get('costume', ''), assist=q.get('assist', ''))
    except Exception as ex:
        JOBS[jid] = {'state': 'error', 'message': 'Blender could not ask for the file: %s' % ex}
    return {'ok': True, 'job': jid, 'imported': r.get('imported', False)}


def _own_weapon(q):
    """open the character, then the file browser of dfx.own_weapon for that weapon"""
    r = _open(dict(q, motion='', name=''), need_action=False)
    if not r.get('ok'):
        return r
    import uuid
    jid = uuid.uuid4().hex[:12]
    JOBS[jid] = {'state': 'picking', 'message': 'Pick the weapon model in Blender'}
    try:
        bpy.ops.dfx.own_weapon('INVOKE_DEFAULT', job=jid, slot=q.get('slot', ''), skin=q.get('skin', ''))
    except Exception as ex:
        JOBS[jid] = {'state': 'error', 'message': 'Blender could not ask for the file: %s' % ex}
    return {'ok': True, 'job': jid, 'imported': r.get('imported', False)}


def _ping():
    return {'ok': True, 'addon': _version(), 'mods': _mods_dir() or ''}


def register():
    global _SERVER
    try:
        _SERVER = ThreadingHTTPServer(('127.0.0.1', PORT), _Handler)
        _SERVER.daemon_threads = True
        threading.Thread(target=_SERVER.serve_forever, daemon=True).start()
    except OSError as ex:                                   # another Blender has the port: that one answers
        print('Dissidia: Move Studio link not started (%s)' % ex)
        _SERVER = None
    if not bpy.app.timers.is_registered(_pump):
        bpy.app.timers.register(_pump, first_interval=0.5, persistent=True)


def unregister():
    global _SERVER
    if bpy.app.timers.is_registered(_pump):
        bpy.app.timers.unregister(_pump)
    if _SERVER is not None:
        _SERVER.shutdown()
        _SERVER.server_close()
        _SERVER = None
