"""Noctis-style warp dash, authored procedurally on any humanoid Dissidia skeleton.

Timeline (frames at 30 fps, all adjustable):
  0 .. windup         drop into a low crouch, twisted away from the throw, sword low across the body
  windup .. release   swing the sword forward underarm, low to high; it leaves the hand at `release`
  release .. vanish   follow-through held (right arm out front, left arm back) while the sword flies
  vanish .. reappear  body hidden (root scale 0); sword hovers where it landed
  reappear .. end     body comes back in a tumble landing (another animation's tail, re-timed)

The body is built from the character's idle stance plus key poses, solved per frame
with two-bone IK for both arms and both legs, so it adapts to each skeleton.
Model space: Y up, the character faces +Z, the right side is -X.
"""
import numpy as np

from .motion_encode import sample_motion

FLIGHT_ARC = 0.0                                # rise (m) at the middle of the sword's flight: dead straight
DEFAULTS = dict(windup=4, release=7, vanish=13, reappear=18, distance=3.0, height=1.1,
                land_motion='_150A', land_from=16, fade=6, catch=8, trail_len=1.0)        # fade = len(GHOST_FRAMES)

# The player's dash (script 103, motion _023A; the same motion on the ground and in the air) is cut by the
# engine after about 19 frames (38 ticks) at the latest, and earlier when the button is let go, so the whole
# warp has to fit in that window. The reappear frames play the motion's own pose (land_motion '') so the
# blend into the fall / idle motion that follows is smooth. `play_end` clamps every range to the real length.
DASH_PLAY_END = 19
# R+Triangle, the real dash (script 240, motion _500A): 34 frames of which 20..34 loop for as long as the button
# is held or the target is not reached; the engine then goes straight to the fall motion (script 101). So the
# throw and vanish fit in the start (frames 0..20), the body stays hidden through the loop (identical on every
# repeat, the sword hovering ahead), and the body comes back through the 16-frame blend into the fall motion.
RUN_DEFAULTS = dict(windup=4, release=7, vanish=13, reappear=None, catch=0, land_motion='', play_end=None, loop=True)
DASH_DEFAULTS = dict(windup=2, release=4, vanish=10, reappear=16, catch=3, land_motion='', play_end=DASH_PLAY_END)

# Key poses, authored for a LEFT-hand throw (from the FFXV reference, camera behind):
# crouch twisted right with the sword low across the body on the right; rise as the right leg
# steps forward while the left hand sweeps the sword round low across the front and lets go;
# follow-through with the left arm high to the front-left and the right arm low out to the side.
# A right-hand weapon gets the mirror image (see mirror_key).
# root drop/forward (m), torso yaw/lean (deg; +yaw turns to face left), hand offsets from each
# shoulder, blade direction, foot offsets from the idle stance. Model space: +Z forward, right = -X.
KEYS = {
    'start':  dict(drop=0.06, fwd=0.0, yaw=0, lean=8, lh=(0.08, -0.44, 0.06), rh=(-0.10, -0.44, 0.06),
                   blade=(0.0, -0.8, 0.5), lf=(0.0, 0.0, 0.0), rf=(0.0, 0.0, 0.0)),
    'windup': dict(drop=0.52, fwd=-0.04, yaw=-70, lean=46, lh=(-0.30, -0.16, -0.56), rh=(-0.20, -0.38, -0.08),
                   blade=(-0.3, -0.45, -1.0), lf=(0.10, 0.0, 0.18), rf=(-0.12, 0.0, -0.22)),
    'swing':  dict(drop=0.28, fwd=0.08, yaw=-18, lean=24, lh=(-0.24, -0.40, 0.36), rh=(-0.34, -0.18, -0.12),
                   blade=(-0.4, -0.5, 1.0), lf=(0.10, 0.0, 0.05), rf=(-0.12, 0.0, 0.15)),
    'throw':  dict(drop=0.16, fwd=0.18, yaw=10, lean=12, lh=(0.04, -0.16, 0.46), rh=(-0.40, -0.20, -0.08),
                   blade=(0.0, 0.1, 1.0), lf=(0.24, 0.0, -0.12), rf=(-0.12, 0.0, 0.40)),
    'follow': dict(drop=0.10, fwd=0.22, yaw=28, lean=2, lh=(0.46, 0.50, 0.20), rh=(-0.42, -0.24, -0.04),
                   blade=(0.6, 0.8, 0.6), lf=(0.30, 0.0, -0.22), rf=(-0.12, 0.0, 0.40)),
    'hold':   dict(drop=0.08, fwd=0.22, yaw=30, lean=-2, lh=(0.50, 0.56, 0.08), rh=(-0.44, -0.20, -0.06),
                   blade=(0.7, 0.9, 0.3), lf=(0.30, 0.0, -0.22), rf=(-0.12, 0.0, 0.40)),
}


def mirror_key(K):
    """Left-hand key -> right-hand key: swap sides and flip X / yaw."""
    fx = lambda v: np.array([-v[0], v[1], v[2]], float)
    out = dict(K)
    out['lh'], out['rh'] = fx(K['rh']), fx(K['lh'])
    out['lf'], out['rf'] = fx(K['rf']), fx(K['lf'])
    out['blade'] = fx(K['blade'])
    out['yaw'] = -K['yaw']
    return out


# ---------------------------------------------------------------- small maths
def qmat(q):
    x, y, z, w = q / np.linalg.norm(q)
    return np.array([[1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w)],
                     [2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w)],
                     [2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y)]])


def mquat(m):
    t = np.trace(m)
    if t > 0:
        s = np.sqrt(t + 1) * 2
        q = np.array([(m[2, 1] - m[1, 2]) / s, (m[0, 2] - m[2, 0]) / s, (m[1, 0] - m[0, 1]) / s, 0.25 * s])
    else:
        i = int(np.argmax([m[0, 0], m[1, 1], m[2, 2]])); j, k = (i + 1) % 3, (i + 2) % 3
        s = np.sqrt(max(1e-12, 1 + m[i, i] - m[j, j] - m[k, k])) * 2
        q = np.zeros(4); q[i] = 0.25 * s; q[j] = (m[j, i] + m[i, j]) / s; q[k] = (m[k, i] + m[i, k]) / s
        q[3] = (m[k, j] - m[j, k]) / s
    return q / np.linalg.norm(q)


def axis_rot(axis, deg):
    a = np.asarray(axis, float); a = a / np.linalg.norm(a); t = np.radians(deg)
    K = np.array([[0, -a[2], a[1]], [a[2], 0, -a[0]], [-a[1], a[0], 0]])
    return np.eye(3) + np.sin(t) * K + (1 - np.cos(t)) * K @ K


def rot_between(u, v):
    u = u / np.linalg.norm(u); v = v / np.linalg.norm(v)
    c = np.clip(np.dot(u, v), -1, 1); ax = np.cross(u, v); s = np.linalg.norm(ax)
    if s < 1e-8:
        if c > 0:
            return np.eye(3)
        ax = np.cross(u, [1, 0, 0] if abs(u[0]) < 0.9 else [0, 1, 0]); ax /= np.linalg.norm(ax)
        return axis_rot(ax, 180)
    return axis_rot(ax / s, np.degrees(np.arctan2(s, c)))


def ortho(m):
    u, _, vt = np.linalg.svd(m)
    r = u @ vt
    return r if np.linalg.det(r) > 0 else u @ np.diag([1, 1, -1]) @ vt


def smooth(t):
    t = np.clip(t, 0, 1)
    return t * t * (3 - 2 * t)


def slerp(a, b, t):
    if np.dot(a, b) < 0:
        b = -b
    d = np.clip(np.dot(a, b), -1, 1)
    if d > 0.9995:
        q = a + (b - a) * t
    else:
        th = np.arccos(d)
        q = (np.sin((1 - t) * th) * a + np.sin(t * th) * b) / np.sin(th)
    return q / np.linalg.norm(q)


# ---------------------------------------------------------------- pose container
class Pose:
    """Local T/Q/S for every bone, with lazily recomputed world rotations/positions."""

    def __init__(self, bones, T, Q, S):
        self.b, self.T, self.Q, self.S = bones, np.array(T, float), np.array(Q, float), np.array(S, float)

    def copy(self):
        return Pose(self.b, self.T, self.Q, self.S)

    def worlds(self):
        n = len(self.b); R = np.zeros((n, 3, 3)); P = np.zeros((n, 3))
        for i, b in enumerate(self.b):
            r, p = qmat(self.Q[i]), self.T[i]
            if b.parent >= 0:
                R[i] = R[b.parent] @ r; P[i] = R[b.parent] @ p + P[b.parent]
            else:
                R[i], P[i] = r, p
        self.R, self.P = R, P
        return R, P

    def set_world_rot(self, i, Rw):
        par = self.b[i].parent
        loc = Rw if par < 0 else self.R[par].T @ Rw
        self.Q[i] = mquat(ortho(loc))
        self.worlds()

    def rotate_world(self, i, delta):
        self.set_world_rot(i, delta @ self.R[i])


def pose_from_tracks(bones, tracks, k):
    T = [tracks[i][0][k] if i in tracks else b.translate for i, b in enumerate(bones)]
    Q = [tracks[i][1][k] if i in tracks else b.rotate for i, b in enumerate(bones)]
    S = [tracks[i][2][k] if i in tracks else b.scale for i, b in enumerate(bones)]
    p = Pose(bones, T, Q, S); p.worlds(); return p


def two_bone_ik(p, a, b, c, target, pole):
    """Rotate bones a (upper) and b (lower) so bone c's origin reaches `target`, elbow/knee toward `pole`."""
    p.worlds()
    A, B, C = p.P[a], p.P[b], p.P[c]
    la, lb = np.linalg.norm(B - A), np.linalg.norm(C - B)
    d = target - A; dist = np.linalg.norm(d)
    dist = np.clip(dist, abs(la - lb) + 1e-3, (la + lb) * 0.999)
    d = d / np.linalg.norm(d); T = A + d * dist
    # elbow position in the plane spanned by d and the pole
    pv = pole - A; pv = pv - np.dot(pv, d) * d
    pv = pv / np.linalg.norm(pv) if np.linalg.norm(pv) > 1e-6 else np.cross(d, [1, 0, 0])
    x = (la * la - lb * lb + dist * dist) / (2 * dist); h = np.sqrt(max(0.0, la * la - x * x))
    E = A + d * x + pv * h
    # upper bone: align direction + bend-plane normal
    def frame(u, w):
        u = u / np.linalg.norm(u); n = np.cross(u, w)
        n = n / np.linalg.norm(n) if np.linalg.norm(n) > 1e-6 else np.cross(u, [0, 1, 0]) / max(1e-6, np.linalg.norm(np.cross(u, [0, 1, 0])))
        return np.stack([u, n, np.cross(u, n)], 1)
    old = frame(B - A, C - B); new = frame(E - A, T - E)
    R1 = new @ old.T
    p.rotate_world(a, R1)
    p.rotate_world(b, rot_between(p.P[c] - p.P[b], T - E))


# ---------------------------------------------------------------- the move
def _bones(names):
    g = {n: names.index(n) for n in names}
    need = ['root', 'waist', 'chest', 'r_uparm', 'r_foarm', 'r_hand', 'l_uparm', 'l_foarm', 'l_hand',
            'r_femur', 'r_tibia', 'r_foot', 'l_femur', 'l_tibia', 'l_foot']
    missing = [n for n in need if n not in g]
    if missing:
        raise ValueError('skeleton is missing ' + ', '.join(missing))
    return g


def blade_axis(ch, weapon_bone):
    """Blade direction (unit) in the holding bone's local frame, from the weapon mesh."""
    wb = ch.bones[weapon_bone]
    O = np.asarray(wb.blend_offsets[0]).T              # bind world -> holding-bone local
    pts = []
    for mi in range(len(ch.parts[wb.draw_part].meshes)):
        for g in ch.mesh_geometry(wb.draw_part, mi):
            pts.append(g['positions'])
    v = np.concatenate(pts)
    loc = (O[:3, :3] @ v.T).T + O[:3, 3]
    far = loc[np.argmax(np.linalg.norm(loc, axis=1))]
    return far / np.linalg.norm(far)


def _lerp_key(a, b, t):
    out = {}
    for k in a:
        if k == 'blade':
            v = (1 - t) * np.array(a[k]) + t * np.array(b[k]); out[k] = v / max(1e-6, np.linalg.norm(v))
        else:
            out[k] = (1 - t) * np.asarray(a[k], float) + t * np.asarray(b[k], float)
    return out


def key_at(f, p):
    seq = [(0, 'start'), (p['windup'], 'windup'), (p['release'] - 1.5, 'swing'), (p['release'], 'throw'),
           (p['release'] + 2, 'follow'), (p['vanish'], 'hold')]
    if f <= seq[0][0]:
        return {k: np.asarray(v, float) for k, v in KEYS['start'].items()}
    for (f0, k0), (f1, k1) in zip(seq, seq[1:]):
        if f <= f1:
            return _lerp_key(KEYS[k0], KEYS[k1], smooth((f - f0) / max(1, f1 - f0)))
    return {k: np.asarray(v, float) for k, v in KEYS['hold'].items()}


def author(ch, abones, motions, mi, hand_i, blade_local, p=None):
    """Return (frames, tracks, sword_track) for motion `mi` with the warp dash applied.
    `abones`: animation skeleton (parse_bones of the motion model); `hand_i`: bone that holds
    the weapon (e.g. r_wep); `blade_local`: blade direction in that bone's frame."""
    p = dict(DEFAULTS, **(p or {}))
    names = [b.name for b in abones]
    g = _bones(names)
    m = motions[mi]
    f1 = int(np.ceil(m.frame_end)); frames = np.arange(0, f1 + 1, dtype=float); nf = len(frames)
    W, REL, V = int(p['windup']), int(p['release']), int(p['vanish'])
    PE = int(min(p.get('play_end') or f1, f1))                      # last frame the engine really plays
    loop = bool(p.get('loop'))
    LS = int(round(m.frame_start)) if loop else 0                    # first frame of the engine's loop
    R = int(min(p['reappear'] or f1, f1 - 4, PE - 1))
    R_out = R
    if loop:                                                          # never reappears inside the motion
        R_out = int(max(V + 2, min(LS, f1)))
        R = f1 + 1
    FS = max(REL + 1, V - int(p['fade']) + 1)                         # first ghost (fade-out) frame
    ghost_worlds = None
    side = 'l' if names[hand_i].startswith('l_') else 'r'          # throwing hand = the weapon's hand

    def full(mo, fr):
        tr = sample_motion(mo, abones, fr)
        for i, b in enumerate(abones):
            if i not in tr:
                tr[i] = (np.tile(b.translate, (len(fr), 1)), np.tile(b.rotate, (len(fr), 1)), np.tile(b.scale, (len(fr), 1)))
        return tr

    orig = full(m, frames)
    idle_m = next((x for x in motions if x.name == '_000A'), motions[0])
    stance = pose_from_tracks(abones, full(idle_m, np.array([float(idle_m.frame_start)])), 0)
    feet0 = {s: stance.P[g[f'{s}_foot']].copy() for s in 'lr'}
    landing = next((x for x in motions if x.name == p['land_motion']), m) if p['land_motion'] else m
    lf0 = float(p['land_from']) if p['land_motion'] and (landing is not m or p['land_from'] < f1) else float(R)
    land_fr = np.linspace(lf0, float(np.ceil(landing.frame_end)), max(2, f1 - R + 1))
    land = full(landing, land_fr)

    out_T = np.zeros((len(abones), nf, 3)); out_Q = np.zeros((len(abones), nf, 4)); out_S = np.zeros((len(abones), nf, 3))
    sword = np.zeros((nf, 4, 4))
    blade_w_rel = None; rel_mat = None
    target = np.array([0.0, p['height'], p['distance']])
    for k, f in enumerate(frames):
        if f >= R:                                    # reappear: tumble landing
            j = k - (len(frames) - len(land_fr))
            j = int(np.clip(j, 0, len(land_fr) - 1))
            pose = pose_from_tracks(abones, land, j)
        else:
            K = key_at(min(f, V), p)
            if side == 'r':
                K = mirror_key(K)
            pose = stance.copy(); pose.worlds()
            r = g['root']
            pose.T[r] = pose.T[r] + np.array([0.0, -K['drop'], K['fwd']]); pose.worlds()
            pose.rotate_world(names.index('hip') if 'hip' in names else r, axis_rot([0, 1, 0], K['yaw'] * 0.35))
            pose.rotate_world(g['waist'], axis_rot([0, 1, 0], K['yaw']) @ axis_rot([1, 0, 0], K['lean']))
            # legs: lunge stance, feet planted
            step = 'r' if side == 'l' else 'l'                              # the leg that steps through
            lift = {s_: 0.0 for s_ in 'lr'}
            lift[step] = 0.10 * np.sin(np.pi * (f - W) / max(1, REL - W)) if W < f < REL else 0.0
            for s, sgn in (('l', 1.0), ('r', -1.0)):
                ft = feet0[s] + K[s + 'f'] + np.array([0.0, lift[s], 0.0])
                hipp = pose.P[g[f'{s}_femur']]
                two_bone_ik(pose, g[f'{s}_femur'], g[f'{s}_tibia'], g[f'{s}_foot'], ft,
                            hipp + np.array([0.1 * sgn, -0.3, 0.6]))           # knees forward, slightly out
                pose.set_world_rot(g[f'{s}_foot'], stance.R[g[f'{s}_foot']])
            # arms
            for s, key, pole in (('r', 'rh', np.array([-0.45, -0.35, -0.25])), ('l', 'lh', np.array([0.45, -0.35, -0.25]))):
                sh = pose.P[g[f'{s}_uparm']]
                two_bone_ik(pose, g[f'{s}_uparm'], g[f'{s}_foarm'], g[f'{s}_hand'], sh + K[key], sh + pole)
            # throwing hand: aim the blade
            hand = g[f'{side}_hand']
            hR = pose.R[hand_i]
            cur = hR @ blade_local
            pose.rotate_world(hand, rot_between(cur, K['blade']))
        out_T[:, k], out_Q[:, k], out_S[:, k] = pose.T, pose.Q, pose.S
        if f == V:
            pose.worlds(); streak_from = pose.P[names.index('chest') if 'chest' in names else g['waist']].copy()
        if f == FS:                                     # the ghost is a snapshot of this pose
            pose.worlds()
            ghost_worlds = {}
            for i, nm in enumerate(names):
                M = np.eye(4); M[:3, :3] = pose.R[i]; M[:3, 3] = pose.P[i]; ghost_worlds[nm] = M
        # sword: in hand until release, then flies point-first to the target
        pose.worlds()
        hw = np.eye(4); hw[:3, :3] = pose.R[hand_i]; hw[:3, 3] = pose.P[hand_i]
        if f >= R:                                     # caught: back in his hand as he reappears
            sword[k] = hw
        elif f <= REL or rel_mat is None:
            sword[k] = hw
            if f <= REL:
                rel_mat = hw.copy(); blade_w_rel = pose.R[hand_i] @ blade_local
        else:
            t = np.clip((f - REL) / max(1, V - REL), 0, 1)
            e = 1 - (1 - t) ** 2
            start = rel_mat[:3, 3]
            pos = start + (target - start) * e + np.array([0, FLIGHT_ARC * np.sin(np.pi * t), 0])
            fly = target - start; fly = fly / np.linalg.norm(fly)
            align = rot_between(blade_w_rel, fly)
            a = smooth(min(1.0, (f - REL) / 2.0))
            Rm = slerp_mat(np.eye(3), align, a) @ rel_mat[:3, :3]
            if f > V:                                  # hovering: gentle bob
                if loop:                               # periodic over the loop, so it repeats without a jump
                    w_ = 2 * np.pi / max(1, f1 - LS); amp = smooth(min(1.0, (f - V) / 4.0)); ph = np.sin(w_ * (f - LS))
                    Rm = axis_rot([0, 0, 1], 6 * ph * amp) @ Rm
                    pos = target + np.array([0, 0.03 * ph * amp, 0])
                else:
                    Rm = axis_rot([0, 0, 1], 6 * np.sin((f - V) * 0.5)) @ Rm
                    pos = target + np.array([0, 0.03 * np.sin((f - V) * 0.4), 0])
            sword[k][:3, :3] = Rm; sword[k][:3, 3] = pos; sword[k][3, 3] = 1
    # body hidden from the first ghost frame until he reappears
    r = g['root']
    hide = (frames >= FS) & (frames < R)
    out_S[r, hide] = 0.0
    tracks = {i: (out_T[i], continuous(out_Q[i]), out_S[i]) for i in range(len(abones))}
    CE = min(R + int(p['catch']), PE, int(frames[-1]))
    sw_scale = np.where(frames < CE, 1.0, 0.0)
    if loop:
        CE = int(frames[-1]); sw_scale = np.ones(nf)            # the sword hovers for as long as the dash lasts
    streak = streak_track(frames, V, R_out, streak_from, target)
    return frames, tracks, sword, sw_scale, dict(windup=W, release=REL, vanish=V, reappear=R_out, streak=streak, play_end=PE,
                                                 fade_start=FS, ghost_worlds=ghost_worlds, catch_end=CE,
                                                 trail=(rel_mat[:3, 3].copy(), target.copy()))


def slerp_mat(A, B, t):
    return qmat(slerp(mquat(A), mquat(B), t))


def continuous(q):
    q = np.array(q)
    for i in range(1, len(q)):
        if np.dot(q[i], q[i - 1]) < 0:
            q[i] = -q[i]
    return q


# ---------------------------------------------------------------- warp streak (the blue dash between vanish and reappear)
STREAK_W, STREAK_H = 64, 32


def streak_rgba():
    """Glow texture: bright white-blue core along the middle, fading to clear at the edges and the tail (u=0)."""
    u = np.linspace(0, 1, STREAK_W)[None, :]; v = np.linspace(-1, 1, STREAK_H)[:, None]
    core = np.exp(-(v / 0.18) ** 2); glow = np.exp(-(v / 0.55) ** 2)
    along = np.clip(u * 1.6, 0, 1) ** 1.5 * np.clip((1 - u) * 12, 0, 1) + np.clip((u - 0.92) * 12, 0, 1) * 0.0
    a = np.clip((0.55 * glow + 0.9 * core) * along, 0, 1)
    r = 0.35 + 0.65 * core; g_ = 0.65 + 0.35 * core; b = np.ones_like(core)
    rgba = np.stack([r * np.ones_like(u), g_ * np.ones_like(u), b * np.ones_like(u), a], -1)
    return np.round(rgba * 255).astype(np.uint8)


def streak_geometry(width=0.5):
    """Two crossed ribbons along +Z (z 0..1), each double-sided. Returns positions, uvs, triangle indices."""
    P, T, idx = [], [], []
    for axis in (np.array([1.0, 0, 0]), np.array([0, 1.0, 0])):
        base = len(P)
        for z, u in ((0.0, 0.0), (1.0, 1.0)):
            for side, vv in ((-1, 0.0), (1, 1.0)):
                P.append(axis * side * width / 2 + np.array([0, 0, z])); T.append((0.01 + 0.98 * u, 0.02 + 0.96 * vv))
        a, b, c, d = base, base + 1, base + 2, base + 3          # (z0,-) (z0,+) (z1,-) (z1,+)
        idx += [a, b, d, a, d, c, a, d, b, a, c, d]              # front and back
    return np.array(P), np.array(T), idx


def streak_track(frames, V, R, p0, p1, thick=1.0):
    """Bone track (T, quaternion, S) for the streak: hidden except between vanish and reappear, where a
    stretched ribbon shoots from p0 (where the body vanished) to p1 (the sword)."""
    n = len(frames); T = np.zeros((n, 3)); Q = np.tile([0, 0, 0, 1.0], (n, 1)); S = np.zeros((n, 3))
    d = p1 - p0; L = np.linalg.norm(d)
    q = mquat(rot_between(np.array([0, 0, 1.0]), d / max(L, 1e-6)))
    for k, f in enumerate(frames):
        T[k] = p0; Q[k] = q
        if V < f < R:
            t = (f - V) / max(1, R - V)
            head = min(1.0, 1.5 * t); tail = min(head, max(0.0, 1.5 * t - 0.55))
            T[k] = p0 + d * tail
            ln = max(1e-3, L * (head - tail))
            w = thick * (0.7 + 0.3 * np.sin(f * 2.1))       # slight flicker
            S[k] = (w, w, ln)
    return T, Q, S


class FxBuilder:
    """Appends effect resources to a Character's chunk tree. Bones go to both models in the same
    order, so indices stay in step; new draw bones are top level (not under root), so hiding the
    body with root scale 0 cannot hide the effects."""

    def __init__(self, ch):
        from .chunks import BONE, PART, MATERIAL, TEXTURE
        self.ch, self.mm = ch, ch.mesh_model
        self.n_bones = len(self.mm.find(BONE))
        self.G = np.asarray(ch.pos_offset, float)

    def _after_last(self, model, btype, chunks):
        i = max(k for k, c in enumerate(model.children) if c.type == btype)
        model.children[i + 1:i + 1] = chunks

    def texture(self, name, rgba):
        from . import gim
        return self.texture_raw(name, gim.wrap_for_gmo(gim.encode(rgba)))

    def texture_raw(self, name, payload):
        import copy, struct
        from .chunks import TEXTURE
        from . import gim
        texs = self.mm.find(TEXTURE)
        t = copy.deepcopy(next(x for x in texs if x.first(0x8013) is not None))
        t.name = f'file{len(texs)}'
        fn = (name + '.tga').encode() + b'\0'; fn += b'\0' * ((-len(fn)) % 4)
        for c in t.children:
            if c.type == 0x8012:
                c.payload = fn
            elif c.type == 0x8013:
                c.payload = payload
        self._after_last(self.mm, TEXTURE, [t])
        return len(texs)

    def material(self, name, tex_i):
        import copy, struct
        from .chunks import MATERIAL, TEXTURE
        from .gmo import make_ref
        mats = self.mm.find(MATERIAL)
        m = copy.deepcopy(next(x for x in mats if any(l.find(0x8091) for l in x.find(0x0009))))
        m.name = name
        for layer in m.find(0x0009):
            for c in layer.children:
                if c.type == 0x8091:
                    c.payload = struct.pack('<I', make_ref(TEXTURE, tex_i, scope=2))
        self._after_last(self.mm, MATERIAL, [m])
        return len(mats)

    def part(self, name, P, T, idx, mat_i, colors=None):
        """P in model space (already where the bound bone's bind pose puts it)."""
        import copy, struct
        from .chunks import Chunk, PART, MESH, ARRAYS, MATERIAL
        from .gmo import VertexArray, DrawCall, make_ref
        ch = self.ch
        parts = self.mm.find(PART)
        Pn = (np.asarray(P) - ch.pos_offset) / ch.pos_scale; Tn = (np.asarray(T) - ch.uv_offset) / ch.uv_scale
        if np.abs(Pn).max() > 1.02 or Tn.min() < -0.02 or Tn.max() > 2.02:
            raise ValueError(f'{name} does not fit this model\'s vertex range')
        Pn = np.clip(Pn, -1.0, 32767 / 32768); Tn = np.clip(Tn, 0.0, 65535 / 32768)
        n = len(P)
        arr = VertexArray.encode('arrays-0-0000', Pn, Tn, np.ones((n, 4)) if colors is None else colors, np.ones((n, 1)), 1)
        mesh = Chunk(MESH); mesh.name = f'16mesh-{len(parts)}-0000'
        mesh.children += [Chunk(0x8061, payload=struct.pack('<I', make_ref(MATERIAL, mat_i, scope=2))),
                          Chunk(0x8062, payload=struct.pack('<II', 1, 0)),
                          Chunk(0x8066, payload=DrawCall(make_ref(ARRAYS, 0), 3, len(idx), 1, list(map(int, idx))).to_payload())]
        part = copy.deepcopy(parts[-1]); part.name = name; part.children = [mesh, arr.to_chunk()]
        return self.add_part_chunk(part)

    def add_part_chunk(self, part):
        from .chunks import PART
        n = len(self.mm.find(PART))
        self._after_last(self.mm, PART, [part])
        return n

    def bone(self, name, T=(0, 0, 0), S=None, draw=None, R=None, parent=-1, draw_multi=None):
        """Bone in both models (top level unless `parent`). draw=(part_i, skin_bone_i, bind_world) or
        draw_multi=(part_i, [palette bones], [stored offset matrices]) makes it a draw bone."""
        import struct
        from .chunks import Chunk, BONE, PART
        from .gmo import make_ref

        def mk(extra):
            c = Chunk(BONE); c.name = name
            if parent >= 0:
                c.children.append(Chunk(0x8041, payload=struct.pack('<I', make_ref(BONE, parent))))
            c.children.append(Chunk(0x8048, payload=struct.pack('<3f', *T)))
            c.children.append(Chunk(0x804B, payload=struct.pack('<4f', *(R if R is not None else (0, 0, 0, 1)))))
            if S is not None:
                c.children.append(Chunk(0x804D, payload=struct.pack('<3f', *S)))
            c.children += extra
            return c
        extra = []
        if draw is not None:
            part_i, skin_i, bind = draw
            off = np.linalg.inv(bind).T                    # stored like the game's BlendOffsets
            extra = [Chunk(0x804E, payload=struct.pack('<I', make_ref(PART, part_i))),
                     Chunk(0x8044, payload=struct.pack('<II', 1, make_ref(BONE, skin_i))),
                     Chunk(0x8045, payload=struct.pack('<I', 1) + off.astype('<f4').tobytes())]
        if draw_multi is not None:
            part_i, pal, offs = draw_multi
            extra = [Chunk(0x804E, payload=struct.pack('<I', make_ref(PART, part_i))),
                     Chunk(0x8044, payload=struct.pack(f'<I{len(pal)}I', len(pal), *[make_ref(BONE, b) for b in pal])),
                     Chunk(0x8045, payload=struct.pack('<I', len(offs)) + b''.join(np.asarray(o, '<f4').tobytes() for o in offs))]
        self._after_last(self.mm, BONE, [mk(extra)])
        self._after_last(self.ch.anim_model, BONE, [mk([])])
        self.n_bones += 1
        return self.n_bones - 1

    def moving_effect(self, name, rgba, P, T, idx):
        """An animated effect: geometry bound at G (centre of the vertex range), bone `name` to animate
        (rest scale 0 = hidden) and draw bone `name`_draw. Returns (bone, draw bone)."""
        tex = self.texture(name, rgba); mat = self.material(name, tex)
        part = self.part(name, np.asarray(P) + self.G, T, idx, mat)
        bind = np.eye(4); bind[:3, 3] = self.G
        b = self.bone(name, T=self.G, S=(0, 0, 0))
        d = self.bone(name + '_draw', draw=(part, b, bind))
        return b, d


def add_fx_part(ch, name, rgba, P, T, idx, root_i=None, bone_i=None, draw_i=None):
    return FxBuilder(ch).moving_effect(name, rgba, P, T, idx)


def add_streak(fx):
    P, T, idx = streak_geometry()
    return fx.moving_effect('warp_streak', streak_rgba(), P, T, idx)


# ---------------------------------------------------------------- crystal shards (weapon summon / shatter)
CRYSTAL_W = CRYSTAL_H = 32


def crystal_rgba():
    """A faceted blue crystal diamond with a bright rim and a white glint."""
    y, x = np.mgrid[0:CRYSTAL_H, 0:CRYSTAL_W] / (CRYSTAL_W - 1) * 2 - 1
    d = np.abs(x) + np.abs(y) * 1.0                      # diamond distance
    inside = d < 0.95
    rim = np.clip(1 - np.abs(d - 0.85) / 0.12, 0, 1)
    facet = np.where(x * y > 0, 0.75, 1.0)
    glint = np.exp(-((x + 0.25) ** 2 + (y + 0.3) ** 2) / 0.02)
    r = np.clip(0.30 * facet + 0.7 * rim + glint, 0, 1); g = np.clip(0.60 * facet + 0.4 * rim + glint, 0, 1)
    b = np.clip(0.95 * facet + 0.3 * rim + glint, 0, 1)
    a = np.where(inside, np.clip(0.75 + 0.25 * rim + glint, 0, 1), 0.0)
    return np.round(np.stack([r, g, b, a], -1) * 255).astype(np.uint8)


def shard_geometry(n=22, seed=7):
    """Shards scattered along +X (blade direction) from -0.1 to 1.0, within 0.16 of the axis.
    Each shard is two crossed diamonds (double sided). Unit space: 1 = blade length."""
    rng = np.random.default_rng(seed)
    P, T, idx = [], [], []
    for i in range(n):
        c = np.array([rng.uniform(-0.1, 1.0), rng.normal(0, 0.07), rng.normal(0, 0.07)])
        c[1:] = np.clip(c[1:], -0.16, 0.16)
        size = rng.uniform(0.016, 0.036)
        ax = rng.normal(size=3); ax /= np.linalg.norm(ax)
        R = axis_rot(ax, rng.uniform(0, 180))
        for plane in ((np.array([1.0, 0, 0]), np.array([0, 1.0, 0])), (np.array([1.0, 0, 0]), np.array([0, 0, 1.0]))):
            u, v = R @ plane[0] * size * 1.6, R @ plane[1] * size     # elongated diamond
            base = len(P)
            for off, uv in ((u, (1, .5)), (v, (.5, 1)), (-u, (0, .5)), (-v, (.5, 0))):
                P.append(c + off); T.append((0.02 + 0.96 * uv[0], 0.02 + 0.96 * uv[1]))
            q = [base, base + 1, base + 2, base + 3]
            idx += [q[0], q[1], q[2], q[0], q[2], q[3], q[0], q[2], q[1], q[0], q[3], q[2]]
    return np.array(P), np.array(T), idx


def shard_track(frames, holder_worlds, blade_len, blade_dir, bursts):
    """Bone track for the shard cloud. holder_worlds: (nf,4,4) world of the bone that holds the weapon
    (or the thrown copy). bursts: list of (frame, kind) with kind 'in' (converge as it appears) or 'out'
    (shatter outwards as it vanishes); each lasts 5 frames. Hidden (scale 0) otherwise."""
    n = len(frames); T = np.zeros((n, 3)); Q = np.tile([0, 0, 0, 1.0], (n, 1)); S = np.zeros((n, 3))
    Rb = rot_between(np.array([1.0, 0, 0]), np.asarray(blade_dir, float))
    c = np.array([0.45, 0, 0])
    for k, f in enumerate(frames):
        for f0, kind in bursts:
            t = (f - f0) / 5.0
            if not (0 <= t < 1):
                continue
            k_ = blade_len * ((1.7 - 0.7 * smooth(t * 1.25)) if kind == 'in' else (1.0 + 0.9 * smooth(t)))
            spin = axis_rot([1, 0, 0], (1 - t) * 90 if kind == 'in' else t * 120)
            H = holder_worlds[min(k if kind == 'in' else k, n - 1)]
            Rw = H[:3, :3] @ Rb @ spin
            T[k] = H[:3, 3] + Rw @ (c * blade_len - c * k_)   # scale about the blade's middle
            Q[k] = mquat(ortho(Rw)); S[k] = (k_, k_, k_)
    return T, Q, S


def add_shards(fx):
    P, T, idx = shard_geometry()
    return fx.moving_effect('warp_shards', crystal_rgba(), P, T, idx)


def blade_length(ch, weapon_bone):
    wb = ch.bones[weapon_bone]
    O = np.asarray(wb.blend_offsets[0]).T
    v = np.concatenate([g['positions'] for mi in range(len(ch.parts[wb.draw_part].meshes))
                        for g in ch.mesh_geometry(wb.draw_part, mi)])
    loc = (O[:3, :3] @ v.T).T + O[:3, 3]
    return float(np.max(loc @ blade_axis(ch, weapon_bone)))


def bind_offset(G):
    """BlendOffsets matrix (as stored) for a bone whose bind pose is a pure translation G."""
    M = np.eye(4); M[:3, 3] = -np.asarray(G, float)
    return M.T


# ---------------------------------------------------------------- ghost fade (the body turns to light and dissolves, still moving)
# Stages (one part each, a copy of the body part skinned to a ghost copy of the skeleton, which plays
# the same animation as the body; the real body is hidden by root scale 0 from the first ghost frame):
#   0 dark blue, 1 bright white-blue, 2-3 dissolving. GHOST_FRAMES says which stage shows on each frame.
GHOST_STAGES = 4
GHOST_FRAMES = (0, 0, 1, 1, 2, 3)            # 6 frames
GHOST_TEX = 128


def _value_noise(n, cells, seed):
    rng = np.random.default_rng(seed)
    g = rng.random((cells + 1, cells + 1))
    x = np.linspace(0, cells, n, endpoint=False)
    i = x.astype(int); t = x - i; t = t * t * (3 - 2 * t)
    a = g[i][:, i]; b = g[i][:, i + 1]; c = g[i + 1][:, i]; d = g[i + 1][:, i + 1]
    ty, tx = t[:, None], t[None, :]
    return (a * (1 - tx) + b * tx) * (1 - ty) + (c * (1 - tx) + d * tx) * ty


def ghost_palette(stage):
    """256-entry RGBA palette: 0-253 brightness ramp in the stage's colour, 254 white glow, 255 clear."""
    t = np.linspace(0, 1, 254)[:, None]
    if stage == 0:
        rgb = np.c_[0.05 + 0.25 * t, 0.08 + 0.30 * t, 0.30 + 0.55 * t]
    else:
        rgb = np.c_[0.55 + 0.45 * t, 0.62 + 0.38 * t, 0.95 + 0.05 * t]
    pal = np.zeros((256, 4)); pal[:254, :3] = rgb; pal[:254, 3] = 1; pal[254] = 1
    return np.round(pal * 255).astype(np.uint8)


GHOST_CUTS = {0: None, 1: None, 2: 0.42, 3: 0.68}


def ghost_indices(rgba, stage, size=GHOST_TEX):
    """Index image for a stage from one of the body's textures: brightness keeps the detail,
    noise-threshold holes (with a white edge) dissolve the later stages."""
    h, w = rgba.shape[:2]
    ys = (np.arange(size) * h // size); xs = (np.arange(size) * w // size)
    src = rgba[ys][:, xs].astype(float) / 255
    lum = np.clip(0.3 * src[..., 0] + 0.59 * src[..., 1] + 0.11 * src[..., 2], 0, 1)
    idx = np.round(lum * 253).astype(np.uint8)
    idx[src[..., 3] < 0.5] = 255
    cut = GHOST_CUTS[stage]
    if cut is not None:
        noise = 0.6 * _value_noise(size, 6, 3) + 0.4 * _value_noise(size, 14, 5)
        idx[(noise < cut + 0.04) & (idx != 255)] = 254
        idx[noise < cut] = 255
    return idx


def add_ghost(fx, abones):
    """Ghost copy of the body: skeleton copies ('g_' + name) for the body's palette bones and their
    ancestors, plus one copy of the body part per stage with tinted textures.
    Returns (ghost bone index by original bone index, [draw bone per stage])."""
    import copy, struct
    from .chunks import PART, MATERIAL, TEXTURE
    from .gmo import make_ref
    from . import gim
    ch = fx.ch
    names = [b.name for b in abones]; parents = [b.parent for b in abones]
    rest_of = lambda i: dict(T=abones[i].translate, R=abones[i].rotate, S=abones[i].scale if abones[i].has_scale else None)
    pi = body_part_index(ch)
    owner = ch.bones[ch.part_owner(pi)]
    need = set()
    for b in owner.blend_bones:
        while b >= 0 and b not in need:
            need.add(b); b = parents[b]
    order = sorted(need)
    gmap = {}
    for b in order:                                   # parents come first (bones are stored parent-first)
        gmap[b] = fx.bone('g_' + names[b][:29], parent=gmap.get(parents[b], -1), **rest_of(b))
    # textures per stage, for each material the body uses
    mats = ch.mesh_model.find(MATERIAL); texs = ch.mesh_model.find(TEXTURE)
    used = sorted({m.material for m in ch.parts[pi].meshes})
    body_part = ch.mesh_model.find(PART)[pi]
    draws = []
    for st in range(GHOST_STAGES):
        remap = {}
        for mi in used:
            tex_i = ch.materials[mi].textures[0] if ch.materials[mi].textures else None
            rgba = gim.decode(texs[tex_i].first(0x8013).payload[4:]) if tex_i is not None else np.full((8, 8, 4), 255, np.uint8)
            raw = gim.wrap_for_gmo(gim.encode_indexed(ghost_indices(rgba, st), ghost_palette(st)))
            t = fx.texture_raw(f'g{st}_{mi}', raw)
            remap[mi] = fx.material(f'warp_ghost{st}_{mi}', t)
        part = copy.deepcopy(body_part); part.name = f'warp_ghost{st}'
        for mesh in part.children:
            for c in mesh.children:
                if c.type == 0x8061:
                    old = struct.unpack_from('<I', c.payload)[0] & 0xFFF
                    c.payload = struct.pack('<I', make_ref(MATERIAL, remap.get(old, old), scope=2))
        part_i = fx.add_part_chunk(part)
        draws.append(fx.bone(f'warp_ghost{st}_draw', draw_multi=(part_i, [gmap[b] for b in owner.blend_bones], owner.blend_offsets)))
    return gmap, draws


def body_part_index(ch):
    return ch.bones[next(i for i, b in enumerate(ch.bones) if b.draw_part == 0)].draw_part


# ---------------------------------------------------------------- lightning crackle around the hovering sword
def bolt_rgba():
    v = np.linspace(-1, 1, 16)[:, None]; u = np.ones((1, 64))
    core = np.exp(-(v / 0.25) ** 2); glow = np.exp(-(v / 0.7) ** 2)
    rgb = np.stack([0.55 + 0.45 * core, 0.70 + 0.30 * core, np.ones_like(core)], -1) * np.ones((16, 64, 1))
    a = np.clip(0.4 * glow + core, 0, 1) * u
    return np.round(np.dstack([rgb, a]) * 255).astype(np.uint8)


def bolt_geometry(n=5, segs=7, seed=19):
    """Jagged lightning ribbons along +X (blade direction, unit length), each two crossed strips."""
    rng = np.random.default_rng(seed)
    P, T, idx = [], [], []
    for b in range(n):
        x0, x1 = rng.uniform(-0.25, 0.3), rng.uniform(0.7, 1.15)
        pts = [np.array([x0 + (x1 - x0) * i / segs, rng.normal(0, 0.06), rng.normal(0, 0.06)]) for i in range(segs + 1)]
        w = rng.uniform(0.008, 0.016)
        for side in (np.array([0, 1.0, 0]), np.array([0, 0, 1.0])):
            base = len(P)
            for i, p in enumerate(pts):
                P += [p - side * w, p + side * w]; T += [(0.02 + 0.96 * i / segs, 0.02), (0.02 + 0.96 * i / segs, 0.98)]
            for i in range(segs):
                a_, b_, c_, d_ = base + 2 * i, base + 2 * i + 1, base + 2 * i + 2, base + 2 * i + 3
                idx += [a_, b_, d_, a_, d_, c_, a_, d_, b_, a_, c_, d_]
    return np.array(P), np.array(T), idx


def bolt_track(frames, holder_worlds, blade_len, blade_dir, f0, f1, seed=23):
    """Visible (flickering) on frames f0 <= f < f1, following the sword."""
    rng = np.random.default_rng(seed)
    n = len(frames); T = np.zeros((n, 3)); Q = np.tile([0, 0, 0, 1.0], (n, 1)); S = np.zeros((n, 3))
    Rb = rot_between(np.array([1.0, 0, 0]), np.asarray(blade_dir, float))
    for k, f in enumerate(frames):
        ang, jit = rng.uniform(0, 360), rng.uniform(0.9, 1.25)
        if f0 <= f < f1:
            H = holder_worlds[k]
            Rw = H[:3, :3] @ Rb @ axis_rot([1, 0, 0], ang)
            T[k] = H[:3, 3]; Q[k] = mquat(ortho(Rw)); S[k] = (blade_len, blade_len * jit, blade_len * jit)
    return T, Q, S


def add_bolts(fx):
    P, T, idx = bolt_geometry()
    return fx.moving_effect('warp_bolts', bolt_rgba(), P, T, idx)


def ghost_tracks(tracks, gmap, root_i, nf):
    """Ghost skeleton tracks = the body's own (so the ghost keeps moving), with the root never hidden."""
    out = {}
    for b, gb in gmap.items():
        T, Q, S = tracks[b]
        if b == root_i:
            S = np.where(S == 0, 1.0, S)
        out[gb] = (T, Q, S)
    return out


def ghost_show_ranges(fs, draws):
    """[(draw bone, start, end)] for the stage sequence starting at frame fs."""
    out = []
    for j, st in enumerate(GHOST_FRAMES):
        if out and out[-1][0] == draws[st] and out[-1][2] == fs + j:
            out[-1] = (draws[st], out[-1][1], fs + j + 1)
        else:
            out.append((draws[st], fs + j, fs + j + 1))
    return out


# ---------------------------------------------------------------- warp trail
# A single thin, wispy line following the flying sword's grip, at most `trail_len` long. Each flight frame
# has its own stage part (the line behind the sword at that moment); once the sword arrives the line
# breaks up from its tail into tiny drifting particles over SCATTER frames.
SCATTER = 5
TRAIL_SHRINK = 0.5


class Lcg:
    """Tiny deterministic generator, identical in the studio (so both build the same trail)."""
    def __init__(self, seed):
        self.s = seed & 0xFFFFFFFF

    def __call__(self):
        self.s = (self.s * 1664525 + 1013904223) & 0xFFFFFFFF
        return self.s / 4294967296.0


def flight_pos(a, b, t):
    """Where the sword's grip is at flight fraction t (same curve as author())."""
    a = np.asarray(a, float); b = np.asarray(b, float)
    t = np.clip(np.asarray(t, float), 0, 1)[..., None]
    e = 1 - (1 - t) ** 2
    return a + (b - a) * e + np.array([0, FLIGHT_ARC, 0]) * np.sin(np.pi * t)


def _trail_basis(a, b):
    d = np.asarray(b, float) - np.asarray(a, float); d /= np.linalg.norm(d)
    u = np.cross(d, [0, 1, 0]); u /= max(np.linalg.norm(u), 1e-6)
    return u, np.cross(u, d)


TRAIL_GAP = 0.35                                # the trail starts this far (m) behind the sword's grip


def trail_line(a, b, t_head, length, frame):
    """Polyline (head first) of the trail: a straight line along the flight path starting TRAIL_GAP
    behind the grip, at most `length` long, tapering towards the tail."""
    ts = np.linspace(0, t_head, 240)
    pts = flight_pos(a, b, ts)[::-1]                     # head first
    seg = np.r_[0, np.cumsum(np.linalg.norm(np.diff(pts, axis=0), axis=1))]
    keep = (seg >= TRAIL_GAP) & (seg <= TRAIL_GAP + length)
    pts, seg = pts[keep], seg[keep] - TRAIL_GAP
    if len(pts) < 2:
        return None, None
    sig = seg / length                                    # 0 at the head, 1 at the max-length tail
    w = 0.028 * (1 - sig) ** 0.8 + 0.007
    return pts, w


def _ribbon_w(P, T, idx, pts, widths):
    """Crossed ribbons (double-sided) along a polyline, per-point width; u runs head (1) to tail (0)."""
    n = len(pts)
    for fixed_up in (True, False):
        base = len(P)
        for i, p in enumerate(pts):
            if fixed_up:
                sd = np.array([0, 1.0, 0])
            else:
                tan = pts[min(i + 1, n - 1)] - pts[max(i - 1, 0)]
                sd = np.cross(tan, [0, 1, 0]); sd = sd / max(np.linalg.norm(sd), 1e-6)
            u = 0.98 - 0.96 * i / max(1, n - 1)
            P += [p - sd * widths[i], p + sd * widths[i]]; T += [(u, 0.02), (u, 0.98)]
        for i in range(n - 1):
            a_, b_, c_, d_ = base + 2 * i, base + 2 * i + 1, base + 2 * i + 2, base + 2 * i + 3
            idx += [a_, b_, d_, a_, d_, c_, a_, d_, b_, a_, c_, d_]


def _dot(P, T, idx, c, size):
    for ax in (np.array([1.0, 0, 0]), np.array([0, 0, 1.0])):
        base = len(P)
        up = np.array([0, 1.0, 0]) * size; sd = ax * size
        P += [c - sd - up, c + sd - up, c + sd + up, c - sd + up]
        T += [(0.02, 0.02), (0.98, 0.02), (0.98, 0.98), (0.02, 0.98)]
        idx += [base, base + 1, base + 2, base, base + 2, base + 3, base, base + 2, base + 1, base, base + 3, base + 2]


def trail_stages(a, b, rel, v, length=1.0, seed=11):
    """[(line P,T,idx), (dots P,T,idx)] per stage: one per flight frame rel+1..v, then SCATTER stages."""
    out = []
    as_arr = lambda P, T, I: (np.array(P).reshape(-1, 3), np.array(T).reshape(-1, 2), I)
    nfl = max(1, v - rel)
    for k in range(1, nfl + 1):                          # in flight
        lP, lT, lI = [], [], []
        pts, w = trail_line(a, b, k / nfl, length, rel + k)
        if pts is not None:
            _ribbon_w(lP, lT, lI, pts, w)
        out.append((as_arr(lP, lT, lI), as_arr([], [], [])))
    pts, w = trail_line(a, b, 1.0, length, v)            # the line as the sword lands
    # 1-2: it unravels into two crackling strands corkscrewing round it; 3-5: those break into particles
    st1 = unravel(pts, 0.05, seed)
    st2 = unravel(pts, 0.10, seed)
    st3 = unravel(pts, 0.12, seed)
    rnd = Lcg(seed)
    dirs = []                                            # random unit direction x random speed, per particle
    for _ in range(2 * len(pts)):
        dv = np.array([rnd() - 0.5, rnd() - 0.5, rnd() - 0.5]); dv /= max(np.linalg.norm(dv), 1e-6)
        dirs.append(dv * (0.3 + 1.4 * rnd()))
    for j in range(1, SCATTER + 1):
        lP, lT, lI, dP, dT, dI = [], [], [], [], [], []
        if j <= 2:
            strands = st1 if j == 1 else st2
            for sp in strands:
                _ribbon_w(lP, lT, lI, sp, np.full(len(sp), 0.02 - 0.003 * j))
            if j == 2:
                r2 = Lcg(seed + 200)
                for k, sp in enumerate(strands):
                    for i in range(0, len(sp), 5):
                        if r2() < 0.6:
                            _dot(dP, dT, dI, sp[i] + dirs[2 * i + k] * 0.06, 0.012)
        else:
            age = j - 2                                    # 1..3
            r2 = Lcg(seed + 100 * j)
            for k, sp in enumerate(st3):
                for i in range(len(sp)):
                    if r2() > (0.9, 0.6, 0.3)[age - 1]:
                        continue
                    drift = dirs[2 * i + k] * 0.08 * age + np.array([0, 0.01 * age, 0])
                    _dot(dP, dT, dI, sp[i] + drift, 0.02 * (1 - age / 3.6))      # shrink as they fade
        out.append((as_arr(lP, lT, lI), as_arr(dP, dT, dI)))
    return out


def unravel(pts, radius, seed=11):
    """Two jagged strands corkscrewing round a straight polyline (head first), `radius` at their widest."""
    pts = np.asarray(pts, float); n = len(pts)
    d = pts[-1] - pts[0]; L = max(np.linalg.norm(d), 1e-6); d /= L
    u = np.cross(d, [0, 1, 0]); u /= max(np.linalg.norm(u), 1e-6); v = np.cross(u, d)
    rw = Lcg(seed + 7); wob = [rw() for _ in range(12)]
    out = []
    for k in range(2):
        rnd = Lcg(seed + 31 + k)
        sp = []
        for i in range(n):
            t = i / max(1, n - 1)
            noise = sum(0.5 * (wob[j] - 0.5) * np.sin((j + 2) * 3.1 * t + wob[j + 6] * 6) for j in range(6))
            r = radius * (0.55 + 0.45 * np.sin(np.pi * t) + 0.6 * noise)
            th = 2 * np.pi * (4.0 * L * t + 0.5 * k) + 4 * noise
            jit = np.array([rnd() - 0.5, rnd() - 0.5, rnd() - 0.5]) * radius * 0.35       # crackle
            sp.append(pts[i] + r * (np.cos(th) * u + np.sin(th) * v) + jit)
        out.append(np.array(sp))
    return out


def dot_rgba():
    y, x = np.mgrid[0:16, 0:16] / 15.0 * 2 - 1
    r2 = x * x + y * y
    core = np.exp(-r2 / 0.1); glow = np.exp(-r2 / 0.5)
    a = np.clip(0.85 * core + 0.4 * glow, 0, 1) * (r2 < 1)
    rgb = np.stack([0.55 + 0.45 * core, 0.65 + 0.35 * core, np.ones_like(core)], -1)
    return np.round(np.dstack([rgb, a]) * 255).astype(np.uint8)


def add_trail(fx, a, b, rel, v, length=1.0):
    """Stage parts on one bone, stored half size around G (the trail reaches past the model's encodable
    vertex range); the bone (path middle, scale 2) puts it back. Returns (bone, (T, S), draws per stage)."""
    M = (np.asarray(a, float) + np.asarray(b, float)) / 2
    k = TRAIL_SHRINK
    bone = fx.bone('warp_trail', T=M, S=(1 / k,) * 3)
    bind = np.eye(4); bind[:3, 3] = fx.G
    line_mat = fx.material('warp_trail_line', fx.texture('warp_trail_line', trail_line_rgba()))
    dot_mat = fx.material('warp_trail_dot', fx.texture('warp_trail_dot', dot_rgba()))
    draws = []
    for st, ((lP, lT, lI), (dP, dT, dI)) in enumerate(trail_stages(a, b, rel, v, length)):
        ds = []
        for nm, (P, T, I), mat in ((f'warp_trail{st}', (lP, lT, lI), line_mat), (f'warp_trail{st}d', (dP, dT, dI), dot_mat)):
            if len(I):
                part = fx.part(nm, fx.G + (np.asarray(P) - M) * k, T, I, mat)
                ds.append(fx.bone(nm + '_draw', draw=(part, bone, bind)))
        draws.append(ds)
    return bone, (M, 1 / k), draws


def trail_show_ranges(rel, draws):
    """Stage i shows on frame rel + 1 + i."""
    return [(d, rel + 1 + i, rel + 2 + i) for i, ds in enumerate(draws) for d in ds]


def trail_line_rgba():
    """Ethereal: a soft bright core with a faint wide blue haze."""
    v = np.linspace(-1, 1, 16)[:, None]; u = np.ones((1, 64))
    core = np.exp(-(v / 0.3) ** 2); glow = np.exp(-(v / 0.9) ** 2)
    rgb = np.stack([0.50 + 0.50 * core, 0.62 + 0.38 * core, np.ones_like(core)], -1) * np.ones((16, 64, 1))
    a = np.clip(0.75 * core + 0.3 * glow, 0, 1) * u
    return np.round(np.dstack([rgb, a]) * 255).astype(np.uint8)
