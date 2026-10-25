"""Cape bones that swing with the game's own spring-bone physics (Tools > Add cape bones).

Add cape bones   appends Cecil-style chains to the armature: an anchor `<s>_mant1c_c` on the chest and under it
                 `<s>_mant1c -> <s>_mant1b -> <s>_mant1a` (s = b back, l / r sides), local +X down the cape (the game's
                 convention), sized from the character's own chest, shoulders, legs and body mesh. Each physics bone
                 gets its length (bone property dfx_distance, written as the GMO's DISTANCE BlindData). The physics
                 list (records, parameter sets, collision spheres) is kept on the armature (dfx_cape) and written
                 into the moveset when the character is saved to its mod folder.
Weight to cape   weights the selected mesh (or the selected vertices) to the chains: nearest chains, blended
                 across rows and seams, the top row blended with the chest.
Saving           (charmod_moves.save_moves / own_model.save_skin call sync_folder): every rig in the folder's
                 models/ gets the new bones (appended, so every index stays), then moveset/<p_xxx>/sections/<type 6>
                 gets the cape's records, appended to the ones already there (spring.cape_section).

Format: psprecomp/games/dissidia/docs/spring-bones.md.
"""
import copy
import json
import os

import numpy as np
import bpy
from bpy.props import BoolProperty, EnumProperty, FloatProperty, IntProperty
from mathutils import Matrix

from .dfx import spring

CFG = 'dfx_cape'
CECIL_ANCHOR_Y = 1.617          # Cecil's cape anchor height (m): the scale his sphere sizes are given at


# ---------------------------------------------------------------- the character's own measures
def _bone(arm, *names):
    for n in names:
        if n in arm.data.bones:
            return arm.data.bones[n]
    return None


def _W(b):
    return np.array(b.matrix_local, np.float64)


def _pos(b):
    return _W(b)[:3, 3]


def _template_names(arm):
    from .dfx.chunks import GmoFile, BONE, PART
    if 'dfx_template' not in arm:
        return []
    g = GmoFile.from_bytes(bytes(arm['dfx_template']))
    mm = next((m for m in g.models if m.find(PART)), g.models[-1])
    return [c.name for c in mm.find(BONE)]


def to_arm_space(arm, o):
    """4x4 from the object's mesh space to the armature's (no depsgraph needed for a plain child)."""
    if o.parent == arm and o.parent_type == 'OBJECT':
        return np.array(o.matrix_parent_inverse @ o.matrix_basis, np.float64)
    return np.array(arm.matrix_world.inverted() @ o.matrix_world, np.float64)


def _body_points(arm):
    """Rest positions of the body mesh (part 0, or every mesh child) in armature space."""
    obs = [o for o in arm.children if o.type == 'MESH' and o.get('dfx_part_index') == 0]
    obs = obs or [o for o in arm.children if o.type == 'MESH' and 'dfx_part_index' not in o]
    pts = []
    for o in obs:
        me = o.data
        co = np.empty(len(me.vertices) * 3, np.float32)
        me.vertices.foreach_get('co', co)
        M = to_arm_space(arm, o)
        co = co.reshape(-1, 3).astype(np.float64)
        pts.append(co @ M[:3, :3].T + M[:3, 3])
    return np.concatenate(pts) if pts else np.zeros((0, 3))


def measure(arm):
    """The numbers a cape is fitted to (armature space = the game's: Y up, the character faces +Z)."""
    b = arm.data.bones
    chest = _bone(arm, 'chest', 'spine2', 'spine1', 'spine')
    if chest is None:
        chest = next((x for x in b if 'chest' in x.name and not x.name.startswith('g_')), None)
    if chest is None:
        raise RuntimeError('the armature has no chest bone (needed to hang the cape on)')
    hip = _bone(arm, 'hip', 'waist', 'pelvis')
    cl = [x for x in (_bone(arm, 'l_clavicle'), _bone(arm, 'r_clavicle')) if x]
    up = [x for x in (_bone(arm, 'l_uparm', 'l_humerus', 'l_upperarm'), _bone(arm, 'r_uparm', 'r_humerus', 'r_upperarm')) if x]
    fem = [x for x in (_bone(arm, 'l_femur'), _bone(arm, 'r_femur')) if x]
    cp = _pos(chest)
    top = float(np.mean([_pos(x)[1] for x in cl])) if cl else cp[1] + 0.1
    half = float(np.mean([abs(_pos(x)[0] - cp[0]) for x in up])) if up else (
        float(np.mean([abs(_pos(x)[0] - cp[0]) for x in cl])) + 0.15 if cl else 0.17)
    knee = shin = None
    if fem:
        kids = [c for c in fem[0].children if not c.name.startswith('g_')]
        if kids:
            knee = _pos(kids[0])[1]
            k2 = [c for c in kids[0].children if not c.name.startswith('g_')]
            if k2:
                shin = (knee + _pos(k2[0])[1]) / 2
    pts = _body_points(arm)
    back = None
    if len(pts):
        band = pts[(np.abs(pts[:, 0] - cp[0]) < 0.08) & (pts[:, 1] > top - 0.18) & (pts[:, 1] < top + 0.02)]
        if len(band):
            back = float(band[:, 2].min())
    s = max(top, 0.3) / CECIL_ANCHOR_Y
    if back is None:
        back = float(cp[2]) - 0.12 * s
    return {'chest': chest.name, 'hip': hip.name if hip else None, 'femurs': [f.name for f in fem],
            'cx': float(cp[0]), 'top': top, 'half': half, 'back': back, 'knee': knee, 'shin': shin, 'scale': s}


# ---------------------------------------------------------------- plan
def chain_sides(chains):
    return {1: ['b'], 2: ['l', 'r'], 3: ['b', 'l', 'r']}[int(chains)]


def chain_names(side, per_chain):
    letters = 'abcdefgh'[:per_chain][::-1]          # top first: c, b, a for three
    return side + '_mant1' + letters[0] + '_c', [side + '_mant1' + x for x in letters]


def _frame(x_dir):
    X = x_dir / np.linalg.norm(x_dir)
    Z = np.array([1.0, 0, 0]) - X[0] * X              # the character's left, square to the bone
    Z /= np.linalg.norm(Z)
    Y = np.cross(Z, X)
    return X, Y, Z


def plan(arm, length=0.0, width=0.0, chains=3, per_chain=3, flare=15.0, spread=1.5):
    """-> (bones [(name, parent, 4x4 world, distance or None)], measures)"""
    m = measure(arm)
    L = float(length) if length > 0 else (m['top'] - (m['shin'] if m['shin'] is not None else 0.4 * m['top']))
    L = max(L, 0.1)
    m['length'] = L
    m['hem_y'] = m['top'] - L * np.cos(np.radians(flare))
    half = float(width) / 2 if width > 0 else m['half']
    seg = L / per_chain
    f = np.radians(flare)
    out = []
    for side in chain_sides(chains):
        sx = {'b': 0.0, 'l': 1.0, 'r': -1.0}[side]
        x0 = m['cx'] + sx * half
        z0 = m['back'] - 0.02 + (0.012 if side != 'b' else 0.0)
        p0 = np.array([x0, m['top'], z0])
        x1 = m['cx'] + sx * half * spread
        d = np.array([x1 - x0, -L * np.cos(f), -L * np.sin(f)])
        X, Y, Z = _frame(d)
        W = np.eye(4)
        W[:3, 0], W[:3, 1], W[:3, 2], W[:3, 3] = X, Y, Z, p0
        anchor, names = chain_names(side, per_chain)
        out.append((anchor, m['chest'], W.copy(), None))
        parent = anchor
        for k, n in enumerate(names):
            Wk = W.copy()
            Wk[:3, 3] = p0 + X * seg * k
            out.append((n, parent, Wk, seg))
            parent = n
    return out, m


def spheres(arm, m, long_cape=True):
    """Collision spheres from the character's own body, offsets in each bone's frame (Cecil's, scaled)."""
    s = m['scale']
    out = []

    def add(bone, world, r):
        W = _W(arm.data.bones[bone])
        loc = np.linalg.inv(W) @ np.r_[world, 1.0]
        out.append({'bone': bone, 'offset': [round(float(v), 4) for v in loc[:3]], 'radius': round(float(r), 4)})
    cp = _pos(arm.data.bones[m['chest']])
    r = 0.27 * s
    add(m['chest'], np.array([cp[0], cp[1] + 0.09 * s, m['back'] - 0.02 + r]), r)     # its back = the cape's top
    if m['hip']:
        hp = _pos(arm.data.bones[m['hip']])
        add(m['hip'], np.array([hp[0], hp[1] - 0.214 * s, hp[2]]), 0.2 * s)
    if long_cape:
        for fn in m['femurs']:
            fb = arm.data.bones[fn]
            kids = [c for c in fb.children if not c.name.startswith('g_')]
            fp = _pos(fb)
            kp = _pos(kids[0]) if kids else fp - np.array([0, 0.45 * s, 0])
            add(fn, fp + 0.4 * (kp - fp), 0.18 * s)
    return out


# ---------------------------------------------------------------- Blender
def _edit_mode(context, arm):
    vl = context.view_layer
    if context.object is not None and context.object.mode != 'OBJECT':
        bpy.ops.object.mode_set(mode='OBJECT')
    prev = vl.objects.active
    hidden = arm.hide_get()
    arm.hide_set(False)
    vl.objects.active = arm
    bpy.ops.object.mode_set(mode='EDIT')
    return prev, hidden


def _back(context, arm, prev, hidden):
    bpy.ops.object.mode_set(mode='OBJECT')
    arm.hide_set(hidden)
    if prev is not None:
        context.view_layer.objects.active = prev


def add_cape(context, arm, length=0.0, width=0.0, chains=3, per_chain=3, flare=15.0, spread=1.5,
             preset='cecil', leg_spheres=True):
    context.view_layer.update()
    bones, m = plan(arm, length, width, chains, per_chain, flare, spread)
    want = [b[0] for b in bones]
    have = {n for n in want if n in arm.data.bones}
    tmpl = set(_template_names(arm))
    reuse = bool(have) and have == set(want) and have <= tmpl
    if have and not reuse and have & tmpl:
        raise RuntimeError('the armature already has some of these bones from its model file: %s. Pick another '
                           'number of chains / bones, or keep that cape' % ', '.join(sorted(have & tmpl)[:6]))
    if not reuse:
        old = set(cape_bones(json.loads(arm[CFG]))) if CFG in arm else set()
        redo = {n for n in have | old if n in arm.data.bones and n not in tmpl}
        prev, hidden = _edit_mode(context, arm)
        try:
            eb = arm.data.edit_bones
            for n in sorted(redo, key=lambda n: -len(arm.data.bones[n].parent_recursive)):
                eb.remove(eb[n])                           # made here before (not in the model file): redo them
            for name, parent, W, _d in bones:
                e = eb.new(name)
                e.length = 0.04
                e.matrix = Matrix(W.tolist())
                e.parent = eb[parent]
        finally:
            _back(context, arm, prev, hidden)
        for name, _p, _W_, d in bones:
            bl = arm.data.bones[name]
            if d is not None:
                bl['dfx_distance'] = float(d)
            elif 'dfx_distance' in bl:
                del bl['dfx_distance']
            arm.pose.bones[name].rotation_mode = 'QUATERNION'
        try:
            from . import rig_display
            rig_display.tidy_rig(arm)
        except Exception as ex:
            print('[dfx] tidy bones skipped:', ex)
    else:
        for name, _p, _W_, d in bones:                    # the model's own cape: keep its bones, lengths and all
            if d is not None and 'dfx_distance' not in arm.data.bones[name]:
                raise RuntimeError('%s has no length (DISTANCE) in the model file' % name)
    sides = chain_sides(chains)
    long_cape = m['knee'] is None or m['hem_y'] < m['knee'] + 0.15          # the hem reaches about the knees
    cfg = {'version': 1, 'preset': preset,
           'chains': [{'side': s, 'anchor': chain_names(s, per_chain)[0], 'bones': chain_names(s, per_chain)[1]} for s in sides],
           'spheres': spheres(arm, m, long_cape=leg_spheres and long_cape)}
    arm[CFG] = json.dumps(cfg)
    return cfg, reuse


def cape_bones(cfg):
    return [n for c in cfg['chains'] for n in [c['anchor']] + c['bones']]


# ---------------------------------------------------------------- weighting
def weight_mesh(arm, ob, chest_blend=True, selected=False):
    """Weights the vertices of `ob` (selected=True: its selected ones) to the cape chains.
    -> number of vertices weighted"""
    cfg = json.loads(arm[CFG])
    B = arm.data.bones
    chains = []
    for c in cfg['chains']:
        js = [_pos(B[n]) for n in c['bones']]
        lens = [float(B[n].get('dfx_distance', 0)) for n in c['bones']]
        X = _W(B[c['bones'][0]])[:3, 0]
        chains.append((c['bones'], js[0], X, np.cumsum([0.0] + lens)))
    chest = B[cfg['chains'][0]['anchor']].parent.name
    me = ob.data
    sel = [v.index for v in me.vertices if v.select or not selected]
    if not sel:
        return 0
    M = to_arm_space(arm, ob)
    co = np.array([tuple(me.vertices[i].co) for i in sel], np.float64)
    P = co @ M[:3, :3].T + M[:3, 3]
    weights = []
    for p in P:
        per = []
        for names, p0, X, cum in chains:
            s = float(np.dot(p - p0, X))
            d = float(np.linalg.norm(p - p0 - s * X))
            s = min(max(s, 0.0), cum[-1])
            k = int(np.clip(np.searchsorted(cum, s, side='right') - 1, 0, len(names) - 1))
            t = k + (s - cum[k]) / max(cum[k + 1] - cum[k], 1e-6)          # position in bones
            w = {}
            if t <= 0.5:
                w[names[0]] = 1.0
            elif t >= len(names) - 0.5:
                w[names[-1]] = 1.0
            else:
                i = int(np.floor(t - 0.5))
                fr = t - 0.5 - i
                w[names[i]] = 1.0 - fr
                w[names[i + 1]] = fr
            per.append((d, t, w))
        per.sort(key=lambda x: x[0])
        per = per[:2]
        if len(per) == 2:                        # between two chains: linear across the gap
            d1, d2 = per[0][0], per[1][0]
            share = [d2 / (d1 + d2), d1 / (d1 + d2)] if d1 + d2 > 1e-9 else [1.0, 0.0]
        else:
            share = [1.0]
        out = {}
        for (d, t, w), sh in zip(per, share):
            for n, v in w.items():
                out[n] = out.get(n, 0.0) + v * sh
        t_top = min(x[1] for x in per)
        c = max(0.0, 1.0 - t_top / 0.5) if chest_blend else 0.0
        if c > 0:
            out = {n: v * (1 - c) for n, v in out.items()}
            out[chest] = out.get(chest, 0.0) + c
        top = [(n, v) for n, v in sorted(out.items(), key=lambda kv: -kv[1])[:4] if v > 0.02]
        s_ = sum(v for _, v in top) or 1.0
        weights.append({n: v / s_ for n, v in top})
    groups = {g.name: g for g in ob.vertex_groups}
    for n in cape_bones(cfg) + [chest]:
        if n not in groups:
            groups[n] = ob.vertex_groups.new(name=n)
    for g in ob.vertex_groups:
        g.remove(sel)
    for vi, w in zip(sel, weights):
        for n, v in w.items():
            groups[n].add([vi], v, 'REPLACE')
    if not any(md.type == 'ARMATURE' for md in ob.modifiers):
        md = ob.modifiers.new('Armature', 'ARMATURE')
        md.object = arm
    return len(sel)


# ---------------------------------------------------------------- the mod folder
def _gmo_models(data):
    from .dfx.chunks import GmoFile
    g = GmoFile.from_bytes(data)
    return g, g.models


def new_bones_into(rig, exported):
    """`rig` (GMO bytes) with the exported model's new skeleton bones appended (bones past the rig's, up to the
    first draw bone of a new part), in the mesh model(s) and the animation model. -> (bytes, [names]) or
    (None, []) when the rig has them already."""
    from .dfx.chunks import BONE, PART, MOTION
    _gr, rms = _gmo_models(rig)
    _ge, ems = _gmo_models(exported)
    ex_mesh = next((m for m in ems if m.find(PART)), ems[-1])
    ex_anim = next((m for m in ems if m.find(MOTION)), ex_mesh)
    added = []
    for m in rms:
        src = ex_mesh if m.find(PART) or not m.find(MOTION) else ex_anim
        have = [c.name for c in m.find(BONE)]
        sb = src.find(BONE)
        names = [c.name for c in sb]
        if names[:len(have)] != have:
            raise RuntimeError('the skeleton differs from the exported one')
        extra = []
        mesh_bones = ex_mesh.find(BONE)
        for i in range(len(have), len(sb)):
            if mesh_bones[i].first(0x804E) is not None:      # a new part's draw bone: parts don't go this way
                break
            extra.append(copy.deepcopy(sb[i]))
        if extra:
            i = max(k for k, c in enumerate(m.children) if c.type == BONE)
            m.children[i + 1:i + 1] = extra
            added = [c.name for c in extra]
    if not added:
        return None, []
    return _gr.to_bytes(), added


def _models_in_folder(root):
    d = os.path.join(root, 'models')
    return sorted(os.path.join('models', f) for f in os.listdir(d) if f.endswith('.gmo')) if os.path.isdir(d) else []


def add_bones_to_folder(arm, exported):
    """Every rig in the folder's models/ gets the armature's new skeleton bones. -> [notes]"""
    root = os.path.join(arm['dfx_mod_dir'], arm['dfx_mod_name'])
    notes = []
    for rel in _models_in_folder(root):
        p = os.path.join(root, rel)
        old = open(p, 'rb').read()
        try:
            new, names = new_bones_into(old, exported)
        except Exception as ex:
            notes.append('%s: not given the new bones (%s)' % (rel, ex))
            continue
        if new is not None:
            from .dfx.chunks import GmoFile
            GmoFile.from_bytes(new)
            open(p, 'wb').write(new)
            notes.append('%s: +%d bones' % (rel, len(names)))
    return notes


def _spring_file(ms_dir):
    with open(os.path.join(ms_dir, 'arc.json')) as f:
        arc = json.load(f)
    e = next((e for e in arc['entries'] if e['type'] == spring.SPRING_ARC_TYPE), None)
    return os.path.join(ms_dir, e['file']) if e else None


def write_physics(arm, exported):
    """The cape's records into the moveset's spring section (only when every rig of that moveset has the bones at the
    same numbers). -> note or ''"""
    if CFG not in arm:
        return ''
    from .dfx.chunks import BONE, PART
    cfg = json.loads(arm[CFG])
    _g, ems = _gmo_models(exported)
    ex_mesh = next((m for m in ems if m.find(PART)), ems[-1])
    names = [c.name for c in ex_mesh.find(BONE)]
    index_of = {}
    for n in cape_bones(cfg) + [s['bone'] for s in cfg['spheres']]:
        if n not in names:
            raise RuntimeError('bone %s is not in the exported model' % n)
        index_of[n] = names.index(n)
    md, name = arm['dfx_mod_dir'], arm['dfx_mod_name']
    root = os.path.join(md, name)
    ms_rel = arm['dfx_mod_moveset']
    from . import charmod_moves as CM
    for rig, ms in CM.rigs(md, name):
        if os.path.normpath(ms) != os.path.normpath(ms_rel):
            continue
        _r, rms = _gmo_models(open(os.path.join(root, rig), 'rb').read())
        for m in rms:
            rn = [c.name for c in m.find(BONE)]
            bad = [n for n in index_of if index_of[n] >= len(rn) or rn[index_of[n]] != n]
            if bad:
                raise RuntimeError('%s lacks the cape bones (%s); physics not written' % (rig, ', '.join(bad[:4])))
    ms_dir = os.path.join(root, ms_rel)
    p = _spring_file(ms_dir)
    if p is None:
        raise RuntimeError('%s has no spring-bone section (ARC type 6) in arc.json' % ms_rel)
    old = open(p, 'rb').read() if os.path.isfile(p) else b''
    new = spring.cape_section(old, cfg, index_of)
    if new == old:
        return ''
    CO, _G = CM.tools(md)
    before = CO.pack(ms_dir)
    with open(p, 'wb') as f:
        f.write(new)
    after = CO.pack(ms_dir)
    if 'dfx_mod_objx_open' in arm and bytes(arm['dfx_mod_objx_open']) == before:
        CM.set_bytes(arm, 'dfx_mod_objx_open', after)       # our own change: not "changed in the folder"
    if 'dfx_objx' in arm:                                    # the moveset kept in Blender carries it too
        from .dfx.objx import Arc
        a = Arc.from_bytes(bytes(arm['dfx_objx']))
        e = a.get(type=spring.SPRING_ARC_TYPE)
        if e is not None:
            e.data = new
            CM.set_bytes(arm, 'dfx_objx', a.to_bytes())
    n = len(spring.parse(new)['records'])
    return '%s: cape physics (%d spring bones)' % (os.path.relpath(p, root), n)


def sync_folder(arm, exported, physics=True):
    """Rigs get the new bones; then the physics (Save moves calls write_physics itself, after the events)."""
    if 'dfx_mod_name' not in arm:
        return []
    notes = add_bones_to_folder(arm, exported)
    if physics:
        try:
            w = write_physics(arm, exported)
        except Exception as ex:
            w = 'cape physics NOT written: %s' % ex
        if w:
            notes.append(w)
    for n in notes:
        print('[dfx] cape:', n)
    return notes


# ---------------------------------------------------------------- operators
def _arm(context):
    from .panel import active_arm
    return active_arm(context)


class DFX_OT_add_cape(bpy.types.Operator):
    """Add cape bones on the chest (Cecil-style chains) that swing with the game's own spring physics. The physics
    list is written into the moveset when you save the character to its mod folder (Save moves / Make skin)"""
    bl_idname = 'dfx.add_cape'
    bl_label = 'Add cape bones'
    bl_options = {'REGISTER', 'UNDO'}

    length: FloatProperty(name='Length', default=0.0, min=0.0, max=4.0, unit='LENGTH',
                          description='Cape length from the shoulders down (0: to mid-shin)')
    width: FloatProperty(name='Width', default=0.0, min=0.0, max=2.0, unit='LENGTH',
                         description='Distance between the side chains at the shoulders (0: the shoulders\' width)')
    chains: EnumProperty(name='Chains', default='3', items=[('1', '1 (back)', ''), ('2', '2 (left, right)', ''),
                                                             ('3', '3 (back, left, right)', '')])
    per_chain: IntProperty(name='Bones per chain', default=3, min=2, max=6)
    flare: FloatProperty(name='Hang back', default=15.0, min=0.0, max=60.0,
                         description='How far the cape hangs back from vertical (degrees)')
    spread: FloatProperty(name='Hem spread', default=1.5, min=0.5, max=3.0,
                          description='Side chains: hem width / shoulder width')
    stiffness: EnumProperty(name='Stiffness', default='cecil', items=[
        ('cecil', 'Loose (Cecil)', 'No stiffness: a free pendulum, drag 0.1'),
        ('graded', 'Graded (Exdeath)', 'Stiffness 0.1 at the top down to 0 at the hem: heavier, less floaty')])
    leg_spheres: BoolProperty(name='Leg spheres', default=True,
                              description='Thigh collision spheres for a cape that reaches past the knees')

    @classmethod
    def poll(cls, context):
        a = _arm(context)
        return a is not None and 'dfx_template' in a

    def invoke(self, context, event):
        return context.window_manager.invoke_props_dialog(self, width=320)

    def execute(self, context):
        arm = _arm(context)
        try:
            cfg, reuse = add_cape(context, arm, self.length, self.width, int(self.chains), self.per_chain,
                                  self.flare, self.spread, self.stiffness, self.leg_spheres)
        except Exception as ex:
            self.report({'ERROR'}, 'Add cape bones: %s' % ex)
            return {'CANCELLED'}
        n = sum(len(c['bones']) for c in cfg['chains'])
        where = ('saved to the folder with Save moves / Make skin' if 'dfx_mod_name' in arm else
                 'written when the character is saved to a mod folder')
        self.report({'INFO'}, '%s %d physics bones in %d chains (%s); physics %s. Weight the cape mesh to them' % (
            'Kept the model\'s' if reuse else 'Added', n, len(cfg['chains']), self.stiffness, where))
        return {'FINISHED'}


class DFX_OT_weight_cape(bpy.types.Operator):
    """Weight the selected mesh to the cape bones (in Edit Mode: only the selected vertices): nearest chains, blended
    across rows and seams, the top row blended with the chest"""
    bl_idname = 'dfx.weight_cape'
    bl_label = 'Weight to cape bones'
    bl_options = {'REGISTER', 'UNDO'}

    chest_blend: BoolProperty(name='Blend top with chest', default=True)

    @classmethod
    def poll(cls, context):
        a = _arm(context)
        return a is not None and CFG in a and any(o.type == 'MESH' for o in context.selected_objects)

    def execute(self, context):
        arm = _arm(context)
        edit = context.object is not None and context.object.mode == 'EDIT'
        obs = [o for o in context.selected_objects if o.type == 'MESH'] or (
            [context.object] if context.object and context.object.type == 'MESH' else [])
        if edit:
            bpy.ops.object.mode_set(mode='OBJECT')
        total = 0
        try:
            for ob in obs:
                total += weight_mesh(arm, ob, self.chest_blend, selected=edit)
        finally:
            if edit:
                bpy.ops.object.mode_set(mode='EDIT')
        self.report({'INFO'}, 'Weighted %d vertices to the cape bones' % total)
        return {'FINISHED'}


classes = (DFX_OT_add_cape, DFX_OT_weight_cape)


def register():
    for c in classes:
        bpy.utils.register_class(c)


def unregister():
    for c in reversed(classes):
        bpy.utils.unregister_class(c)
