"""Key-pose animation: author a move as a short list of poses, solved per frame with IK.

A move is a list of keys. Each key is a dict with a frame `f` plus any of the fields below; a field
left out holds the previous key's value (the first key falls back to the idle stance). Between keys
every field follows a smooth curve (cubic Hermite, Catmull-Rom tangents), so a swing keyed at
wind-up, impact and follow-through moves through a natural arc.

Body frame: Y up, +Z towards the opponent, +X the character's LEFT (right = -X), metres / degrees.
  drop        lower the hips by this much (negative = jump)
  fwd, side   move the hips forward / to the left
  spin        turn the whole body about Y (+ = turn to the left; 360 = a full turn, in the spun frame
              the hands, feet and blade below turn with him)
  yaw         twist the torso against the hips (+ = chest turns left)
  lean        bend forward (+) / arch back (-)
  tilt        side bend (+ = lean to the left)
  rh, lh      hand targets: offset from that shoulder
  blade       direction the weapon points (grip -> tip), for the hand holding it
  bw          the same, but relative to the opponent rather than the spun body (for a sword that
              lags behind a turn, e.g. still pointing at the foe while he spins away)
  rf, lf      foot targets: offset from the idle stance foot (y = lift off the ground)
  rpole, lpole  where the elbow points (offset from the shoulder), when the default looks wrong
  ('rest' as a value returns that field to the idle stance)
  lpin, rpin  plant that foot (0..1): it stays where it was when the pin began while the body
              turns or moves over it (a pivot, or the leg he pushes off from)
  over        how far the hips move over a planted foot (0..1), to put his weight on it
  look        how much the head keeps facing the opponent while the torso twists (0..1, default 0.5)
  grip        slide the weapon through the hand towards its far end by this much (metres; 0 = held as
              modelled), e.g. to swing a staff from its foot end for a long sweeping arc

The same solver drives every skeleton that has the usual Dissidia bone names.
"""
import numpy as np

from .motion_encode import sample_motion
from .warp import (Pose, pose_from_tracks, two_bone_ik, axis_rot, rot_between, continuous, _bones, slerp_mat)


def rot_slerp(A, B, t):
    return slerp_mat(A, B, t)

VEC = ('rh', 'lh', 'blade', 'rf', 'lf', 'rpole', 'lpole')
SCAL = ('drop', 'fwd', 'side', 'spin', 'yaw', 'lean', 'tilt', 'look', 'lpin', 'rpin', 'over', 'grip')
POLES = {'r': np.array([-0.45, -0.35, -0.25]), 'l': np.array([0.45, -0.35, -0.25])}


def _full(motion, abones, frames):
    tr = sample_motion(motion, abones, frames)
    n = len(frames)
    for i, b in enumerate(abones):
        if i not in tr:
            tr[i] = (np.tile(b.translate, (n, 1)), np.tile(b.rotate, (n, 1)), np.tile(b.scale, (n, 1)))
    return tr


def _hermite(ts, vals, t):
    """Piecewise cubic Hermite with Catmull-Rom tangents (holds flat at both ends)."""
    ts = np.asarray(ts, float); V = np.asarray(vals, float)
    if t <= ts[0]:
        return V[0].copy()
    if t >= ts[-1]:
        return V[-1].copy()
    i = int(np.searchsorted(ts, t, side='right') - 1)
    t0, t1 = ts[i], ts[i + 1]; h = t1 - t0
    def tangent(k):
        if k == 0 or k == len(ts) - 1:
            return np.zeros_like(V[0])
        return (V[k + 1] - V[k - 1]) / (ts[k + 1] - ts[k - 1])
    m0, m1 = tangent(i) * h, tangent(i + 1) * h
    s = (t - t0) / h; s2, s3 = s * s, s * s * s
    return (2 * s3 - 3 * s2 + 1) * V[i] + (s3 - 2 * s2 + s) * m0 + (-2 * s3 + 3 * s2) * V[i + 1] + (s3 - s2) * m1


class Rig:
    """The idle stance of a skeleton, measured once, that keys are offsets from."""

    def __init__(self, abones, motions, hand_i, blade_local):
        self.b = abones
        self.names = [b.name for b in abones]
        self.g = _bones(self.names)
        idle = next((x for x in motions if x.name == '_000A'), motions[0])
        self.stance = pose_from_tracks(abones, _full(idle, abones, np.array([float(idle.frame_start)])), 0)
        st, g = self.stance, self.g
        # weapon bones sit at their bind pose relative to the hand, so the weapon mesh looks as modelled
        # (the idle stance may leave a folded / stowed weapon pose there)
        for i, b in enumerate(abones):
            if 'wep' in b.name:
                st.T[i], st.Q[i], st.S[i] = b.translate, b.rotate, b.scale
        st.worlds()
        self.root0 = st.P[g['root']].copy()
        self.ground = np.array([self.root0[0], 0.0, self.root0[2]])
        self.feet0 = {s: st.P[g[f'{s}_foot']] - self.ground for s in 'lr'}
        self.hand_i, self.blade_local = hand_i, np.asarray(blade_local, float)
        self.side = 'l' if self.names[hand_i].startswith('l_') else 'r'
        # defaults for hand keys: where the stance already has them
        self.defaults = dict(drop=0.0, fwd=0.0, side=0.0, spin=0.0, yaw=0.0, lean=0.0, tilt=0.0, look=0.5, lpin=0.0, rpin=0.0, over=0.0, grip=0.0,
                             rf=np.zeros(3), lf=np.zeros(3), rpole=POLES['r'], lpole=POLES['l'])
        for s in 'lr':
            self.defaults[s + 'h'] = st.P[g[f'{s}_hand']] - st.P[g[f'{s}_uparm']]
        self.defaults['blade'] = st.R[hand_i] @ self.blade_local
        # bones animated by the move: everything under root; the rest (camera, etc.) keeps the source motion
        self.body = set()
        for i, b in enumerate(abones):
            j = i
            while abones[j].parent >= 0:
                j = abones[j].parent
            if self.names[j] == 'root':
                self.body.add(i)

    def resolve(self, keys):
        """Fill every key's missing fields from the previous key (the first from the stance)."""
        out, cur = [], dict(self.defaults)
        for k in sorted(keys, key=lambda k: k['f']):
            cur = dict(cur)
            for name in VEC + SCAL:
                if name in k and isinstance(k[name], str) and k[name] == 'rest':
                    cur[name] = np.asarray(self.defaults[name], float)      # back to the idle stance
                elif name in k and k[name] is not None:
                    cur[name] = np.asarray(k[name], float)
            if k.get('bw') is not None:            # blade given facing the opponent, whatever the spin
                cur['blade'] = axis_rot([0, 1, 0], -float(cur['spin'])) @ np.asarray(k['bw'], float)
            cur['f'] = float(k['f'])
            out.append(cur)
        return out

    def sample(self, keys, f):
        ts = [k['f'] for k in keys]
        K = {n: _hermite(ts, [k[n] for k in keys], f) for n in VEC + SCAL}
        K['blade'] = K['blade'] / max(1e-6, np.linalg.norm(K['blade']))
        return K

    def solve(self, K):
        g, st = self.g, self.stance
        pose = st.copy(); pose.worlds()
        S = axis_rot([0, 1, 0], float(K['spin']))
        r = g['root']
        pose.T[r] = pose.T[r] + S @ np.array([K['side'], 0, K['fwd']]) + np.array([0.0, -K['drop'], 0.0])
        anchors = getattr(self, '_anchors', None)
        over = float(np.clip(K['over'], 0, 1))
        if anchors and over > 0:                 # hips over the planted foot (its weight-bearing leg)
            for s_ in 'lr':
                if s_ in anchors:
                    w = over * float(np.clip(K[s_ + 'pin'], 0, 1))
                    tgt = anchors[s_][0]
                    pose.T[r][0] += w * (tgt[0] - pose.T[r][0]); pose.T[r][2] += w * (tgt[2] - pose.T[r][2])
                    break
        pose.worlds()
        pose.rotate_world(r, S)
        if 'hip' in g or 'hip' in self.names:
            hp = self.names.index('hip')
            pose.rotate_world(hp, S @ axis_rot([0, 1, 0], K['yaw'] * 0.3) @ S.T)
        D = axis_rot([0, 1, 0], K['yaw'] * 0.7) @ axis_rot([1, 0, 0], K['lean']) @ axis_rot([0, 0, 1], -K['tilt'])
        pose.rotate_world(g['waist'], S @ D @ S.T)
        # head keeps looking at the opponent
        hd = self.names.index('neck') if 'neck' in self.names else None
        if hd is not None and K['look']:
            pose.rotate_world(hd, S @ axis_rot([0, 1, 0], -K['yaw'] * K['look']) @ axis_rot([1, 0, 0], -K['lean'] * 0.4 * K['look']) @ S.T)
        # legs
        base = np.array([pose.P[r][0], 0.0, pose.P[r][2]]) - S @ (self.root0 - self.ground) * np.array([1, 0, 1])
        for s, sgn in (('l', 1.0), ('r', -1.0)):
            ft = base + S @ (self.feet0[s] + K[s + 'f'])
            ft[1] = self.feet0[s][1] + K[s + 'f'][1]
            pin = float(np.clip(K[s + 'pin'], 0, 1))
            if anchors is not None:
                if pin > 1e-3:
                    if s not in anchors:
                        anchors[s] = (ft.copy(), np.array(S))      # planted where the foot is now
                    ft = (1 - pin) * ft + pin * anchors[s][0]
                else:
                    anchors.pop(s, None)
            hp_ = pose.P[g[f'{s}_femur']]
            two_bone_ik(pose, g[f'{s}_femur'], g[f'{s}_tibia'], g[f'{s}_foot'], ft,
                        hp_ + S @ np.array([0.1 * sgn, -0.3, 0.6]))
            Sf = S
            if anchors is not None and s in anchors and pin > 1e-3:   # a planted foot keeps its heading
                Sf = rot_slerp(S, anchors[s][1], pin)
            pose.set_world_rot(g[f'{s}_foot'], Sf @ st.R[g[f'{s}_foot']])
        # arms
        for s in 'rl':
            sh = pose.P[g[f'{s}_uparm']]
            two_bone_ik(pose, g[f'{s}_uparm'], g[f'{s}_foarm'], g[f'{s}_hand'], sh + S @ K[s + 'h'], sh + S @ K[s + 'pole'])
        # weapon hand: aim the blade
        hand = g[f'{self.side}_hand']
        cur = pose.R[self.hand_i] @ self.blade_local
        pose.rotate_world(hand, rot_between(cur, S @ K['blade']))
        if abs(float(K['grip'])) > 1e-4:           # slide the weapon through the hand towards its far end
            i = self.hand_i; par = self.b[i].parent
            disp = -float(K['grip']) * (pose.R[i] @ self.blade_local)
            pose.T[i] = pose.T[i] + (pose.R[par].T @ disp if par >= 0 else disp)
        pose.worlds()
        return pose

    def author(self, keys, motion, frames=None):
        """Tracks {bone: (T, Q, S)} for `motion`'s frame range; non-body bones keep `motion`'s own tracks."""
        if frames is None:
            frames = np.arange(0, int(np.ceil(motion.frame_end)) + 1, dtype=float)
        keys = self.resolve(keys)
        src = _full(motion, self.b, frames)
        nb, nf = len(self.b), len(frames)
        T = np.zeros((nb, nf, 3)); Q = np.zeros((nb, nf, 4)); Sc = np.zeros((nb, nf, 3))
        poses = []
        self._anchors = {}                     # planted feet (lpin / rpin), carried from frame to frame
        for k, f in enumerate(frames):
            p = self.solve(self.sample(keys, f))
            poses.append(p)
            T[:, k], Q[:, k], Sc[:, k] = p.T, p.Q, p.S
        self._anchors = None
        tracks = {}
        for i in range(nb):
            tracks[i] = (T[i], continuous(Q[i]), Sc[i]) if i in self.body else src[i]
        return frames, tracks, poses
