"""Make a Dissidia skeleton readable in Blender without changing it.

Game bones have no length, so they import as short stubs scattered around (weapon draw bones sit at their
parts' bind positions, the warp ghost skeleton sits in bind pose, etc.). This sorts the bones into bone
collections, hides everything except the body, and gives every body bone a wire shape: a line from its joint
to each child joint, so the rig reads as a stick figure. Display only: bone rest matrices are untouched, so
export is unaffected.
"""
import bpy
from mathutils import Vector

SIDE = {'l': 'THEME04', 'r': 'THEME01'}            # left blue, right red; centre yellow (THEME09)
BODY = {'root', 'hip', 'waist', 'chest', 'neck', 'head'} | {f'{s}_{n}' for s in 'lr' for n in
        ('clavicle', 'uparm', 'foarm', 'hand', 'femur', 'tibia', 'foot', 'toe')}

GROUPS = [   # (collection, visible, test); first match wins
    ('Warp ghost', False, lambda n: n.startswith('g_')),
    ('Body', True, lambda n: n in BODY),
    ('Fingers', True, lambda n: '_fing' in n),
    ('Cape & hair', True, lambda n: any(k in n for k in ('mant', 'hair', 'cape', 'scarf', 'skirt', 'ribbon')) and not n.startswith('g_')),
    ('Warp effects', False, lambda n: n.startswith('warp_')),
    ('Weapon grips', False, lambda n: n.startswith(('r_wep', 'l_wep'))),
    ('Weapon parts', False, lambda n: (n.startswith('wep') and n[3:].isdigit()) or n == 'star'),
    ('System', False, lambda n: 'include' in n or n.startswith('camera') or n.startswith('p_')),
    ('Helpers', False, lambda n: True),       # *_c, *_help, pad, anything else
]


def _group_of(name):
    for g, vis, test in GROUPS:
        if test(name):
            return g
    return 'Helpers'


def _shape_mesh(name, segs, diamond=0.0):
    me = bpy.data.meshes.get(name) or bpy.data.meshes.new(name)
    me.clear_geometry()
    verts, edges = [(0.0, 0.0, 0.0)], []
    for p in segs:
        verts.append(tuple(p)); edges.append((0, len(verts) - 1))
    if diamond:
        d = diamond; b = len(verts)
        verts += [(d, 0, 0), (-d, 0, 0), (0, d, 0), (0, -d, 0), (0, 0, d), (0, 0, -d)]
        for i, j in ((0, 2), (0, 3), (0, 4), (0, 5), (1, 2), (1, 3), (1, 4), (1, 5), (2, 4), (2, 5), (3, 4), (3, 5)):
            edges.append((b + i, b + j))
    me.from_pydata(verts, edges, [])
    return me


def tidy_rig(arm):
    """Bone collections + stick-figure shapes for the body. Safe to run again."""
    ad = arm.data
    colls = {}
    for g, vis, _ in GROUPS:
        c = ad.collections.get(g) or ad.collections.new(g)
        c.is_visible = vis
        colls[g] = c
    for b in ad.bones:
        g = _group_of(b.name)
        for c in list(b.collections):
            if c.name in colls and c.name != g:
                c.unassign(b)
        colls[g].assign(b)
        side = b.name[:2] if b.name[:2] in ('l_', 'r_') else ''
        b.color.palette = SIDE.get(side[:1], 'THEME09') if g in ('Body', 'Fingers', 'Cape & hair') else 'DEFAULT'
    # shapes live in a collection that is excluded from the view layer
    sc = bpy.context.scene
    holder = bpy.data.collections.get('DFX bone shapes')
    if holder is None:
        holder = bpy.data.collections.new('DFX bone shapes')
        sc.collection.children.link(holder)
    for lc in bpy.context.view_layer.layer_collection.children:
        if lc.collection == holder:
            lc.exclude = True
    shown = {b.name for b in ad.bones if _group_of(b.name) in ('Body', 'Fingers', 'Cape & hair')}

    def visible_children(b):
        out = []
        for c in b.children:
            if c.name in shown:
                out.append(c)
            elif _group_of(c.name) == 'Helpers':          # look through helper bones (e.g. *_c)
                out += visible_children(c)
        return out
    for b in ad.bones:
        if b.name not in shown:
            continue
        inv = b.matrix_local.inverted()
        segs = [inv @ c.head_local for c in visible_children(b)]
        segs = [p for p in segs if p.length > 1e-4]
        if not segs and b.get('dfx_distance') and _group_of(b.name) == 'Cape & hair':  # a spring bone's tip (cape hem): its game length
            segs = [Vector((float(b['dfx_distance']), 0.0, 0.0))]
        size = 0.09 if b.name == 'root' else (0.025 if segs else 0.035)
        me = _shape_mesh('dfx_shape_' + b.name, segs, diamond=size)
        ob = bpy.data.objects.get('dfx_shape_' + b.name)
        if ob is None:
            ob = bpy.data.objects.new('dfx_shape_' + b.name, me)
            holder.objects.link(ob)
        ob.data = me
        pb = arm.pose.bones[b.name]
        pb.custom_shape = ob
        pb.use_custom_shape_bone_size = False
        if hasattr(pb, 'custom_shape_wire_width'):
            pb.custom_shape_wire_width = 3.0
    ad.display_type = 'OCTAHEDRAL'
    ad.show_bone_custom_shapes = True
    arm.show_in_front = True
    return {g: sum(1 for b in ad.bones if _group_of(b.name) == g) for g, _, _ in GROUPS}
