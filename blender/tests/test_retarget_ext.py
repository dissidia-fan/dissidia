"""Self-test of dfx.retarget_ext / dfx.bonemap: numpy only, no Blender needed.
Run:  python3 tests/test_retarget_ext.py
"""
import os
import sys
import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, '..', 'io_scene_dissidia'))      # imports `dfx` without loading the add-on (bpy)
from dfx import retarget_ext as rx, bonemap as bmap

SLOT_LIST = ['hips', 'spine1', 'spine2', 'neck', 'head'] + \
    [f'{s}_{k}' for s in 'lr' for k in ('clavicle', 'upperarm', 'forearm', 'hand', 'thigh', 'shin', 'foot', 'toe')]


def make_skeleton(arm_drop_deg=0.0, frame=None, height=1.0, seed=0, yaw_roll=True):
    """Humanoid in canonical space (Y up, +Z forward, left = +X), moved into `frame` (3x3).
    arm_drop_deg: 0 = T-pose, 45 = A-pose.  Bone axes get random roll so no two skeletons share conventions."""
    rng = np.random.default_rng(seed)
    h = height
    P = {'hips': (0, 0.55 * h, 0), 'spine1': (0, 0.62 * h, 0), 'spine2': (0, 0.74 * h, 0),
         'neck': (0, 0.84 * h, 0.01), 'head': (0, 0.90 * h, 0.01)}
    parent = {'hips': None, 'spine1': 'hips', 'spine2': 'spine1', 'neck': 'spine2', 'head': 'neck'}
    d = np.radians(arm_drop_deg)
    for side, sx in (('l', 1), ('r', -1)):
        sh = np.array([sx * 0.03 * h, 0.80 * h, 0.0])
        out = np.array([sx * np.cos(d), -np.sin(d), 0.0])
        P[f'{side}_clavicle'] = tuple(sh)
        P[f'{side}_upperarm'] = tuple(sh + [sx * 0.10 * h, 0, 0])
        ua = np.array(P[f'{side}_upperarm'])
        P[f'{side}_forearm'] = tuple(ua + out * 0.17 * h)
        P[f'{side}_hand'] = tuple(np.array(P[f'{side}_forearm']) + out * 0.15 * h)
        P[f'{side}_thigh'] = (sx * 0.06 * h, 0.52 * h, 0)
        P[f'{side}_shin'] = (sx * 0.06 * h, 0.28 * h, 0.01)
        P[f'{side}_foot'] = (sx * 0.06 * h, 0.06 * h, 0)
        P[f'{side}_toe'] = (sx * 0.06 * h, 0.02 * h, 0.10 * h)
        parent.update({f'{side}_clavicle': 'spine2', f'{side}_upperarm': f'{side}_clavicle',
                       f'{side}_forearm': f'{side}_upperarm', f'{side}_hand': f'{side}_forearm',
                       f'{side}_thigh': 'hips', f'{side}_shin': f'{side}_thigh', f'{side}_foot': f'{side}_shin',
                       f'{side}_toe': f'{side}_foot'})
    names = list(SLOT_LIST)
    idx = {n: i for i, n in enumerate(names)}
    kid = {}
    for n in names:
        if parent[n]:
            kid.setdefault(parent[n], n)
    frame = np.eye(3) if frame is None else np.asarray(frame)
    rest = np.zeros((len(names), 4, 4))
    par = []
    for n in names:
        p = np.array(P[n])
        tgt = np.array(P[kid[n]]) - p if n in kid else p - np.array(P[parent[n]])
        y = tgt / np.linalg.norm(tgt)
        x0 = np.cross(y, [0, 0, 1.0])
        if np.linalg.norm(x0) < 1e-3:
            x0 = np.cross(y, [1.0, 0, 0])
        x0 /= np.linalg.norm(x0)
        R = np.stack([x0, y, np.cross(x0, y)], 1)
        R = R @ rx.axis_angle([0, 1, 0], rng.uniform(-np.pi, np.pi))
        rest[idx[n], :3, :3] = frame @ R
        rest[idx[n], :3, 3] = frame @ p
        rest[idx[n], 3, 3] = 1
        par.append(idx[parent[n]] if parent[n] else -1)
    return names, np.array(par), rest


def random_pose(rest, par, nf, seed, amp=0.7, hips_move=0.3):
    """Source world matrices for nf frames from random local rotations on the rest pose."""
    rng = np.random.default_rng(seed)
    nb = len(rest)
    Lrest = rx.rest_locals(rest, par)
    T = np.tile(Lrest[:, :3, 3], (nf, 1, 1))
    Q = np.zeros((nf, nb, 4))
    for f in range(nf):
        for b in range(nb):
            ax = rng.normal(size=3)
            Q[f, b] = rx.mat3_to_quat(rx.axis_angle(ax, rng.uniform(-amp, amp)) @ Lrest[b, :3, :3])
        Q[f] = rx.continuous(Q[f]) if False else Q[f]
    T[:, 0] += rng.uniform(-hips_move, hips_move, (nf, 3)) * [1, 0.3, 1]
    S = np.ones((nf, nb, 3))
    return rx.forward_kinematics(T, Q, S, par), (T, Q, S)


def rot_angle_deg(Ra, Rb):
    c = (np.trace(Ra @ Rb.T, axis1=-2, axis2=-1) - 1) / 2
    return np.degrees(np.arccos(np.clip(c, -1, 1)))


def test_identity():
    names, par, rest = make_skeleton(35, seed=1)
    nf = 12
    W, (T, Q, S) = random_pose(rest, par, nf, 2)
    slots = {n: (i, i) for i, n in enumerate(names)}
    r = rx.retarget(W, rest, rest, par, slots, opts=rx.Options(foot_fix=False))
    dw = np.abs(r.world - W).max()
    ang = max(rot_angle_deg(r.world[f, b, :3, :3], W[f, b, :3, :3]) for f in range(nf) for b in range(len(names)))
    # compare locals through the matrices (quaternion signs may differ)
    Lw = rx.compose_trs(r.T, r.Q, r.S)
    Lr = rx.compose_trs(T, Q, S)
    dl = np.abs(Lw - Lr).max()
    print(f'identity: max world error {dw:.2e} m, max rotation error {ang:.2e} deg, max local matrix error {dl:.2e}')
    assert dw < 1e-9 and dl < 1e-9 and r.scale == 1.0 and np.allclose(r.orient, np.eye(3))
    # fk of the returned locals reproduces the returned world
    fk = rx.forward_kinematics(r.T, r.Q, r.S, par)
    assert np.abs(fk - r.world).max() < 1e-9


def test_rotated_rest_and_frame():
    """Source: T-pose, 1.8 m tall, Z up facing -Y (Blender import).  Destination: A-pose, 1.6 m, Y up facing +Z."""
    zup = np.array([[1, 0, 0], [0, 0, -1], [0, 1, 0]], float)          # (x, y, z)_canon -> x, -z, y   (facing -Y)
    sn, sp, srest = make_skeleton(0, frame=zup, height=1.8, seed=3)
    dn, dp, drest = make_skeleton(45, height=1.6, seed=4)
    assert sn == dn
    nf = 10
    # source animation: arms hang down (rotate upper arms by a world-space swing), some random motion elsewhere
    W, _ = random_pose(srest, sp, nf, 5, amp=0.4, hips_move=0.2)
    slots = {n: (i, i) for i, n in enumerate(sn)}
    r = rx.retarget(W, srest, drest, dp, slots, opts=rx.Options(foot_fix=False))
    C = zup.T                                           # source frame -> canonical (destination) frame
    assert np.allclose(r.orient, C, atol=1e-9), r.orient
    errs = {}
    for k in ('l_upperarm', 'l_forearm', 'l_thigh', 'l_shin', 'r_upperarm', 'r_forearm', 'r_foot', 'head', 'spine2'):
        i = sn.index(k)
        j = sn.index(next_in_chain(k))
        # limb direction in the pose: source (rotated into destination space) vs destination
        ds = C @ (W[:, j, :3, 3] - W[:, i, :3, 3]).T
        dd = (r.world[:, j, :3, 3] - r.world[:, i, :3, 3]).T
        cs = (ds / np.linalg.norm(ds, axis=0)) * 1.0
        cd = dd / np.linalg.norm(dd, axis=0)
        errs[k] = np.degrees(np.arccos(np.clip((cs * cd).sum(0), -1, 1))).max()
    print('limb direction error vs source pose (deg): ' + ', '.join(f'{k} {v:.2f}' for k, v in errs.items()))
    assert max(errs.values()) < 1e-3
    # same test with rest alignment off: arms must be off by the 45 degree rest difference
    r0 = rx.retarget(W, srest, drest, dp, slots, opts=rx.Options(foot_fix=False, rest_align=False))
    i, j = sn.index('l_upperarm'), sn.index('l_forearm')
    ds = C @ (W[:, j, :3, 3] - W[:, i, :3, 3]).T
    dd = (r0.world[:, j, :3, 3] - r0.world[:, i, :3, 3]).T
    off = np.degrees(np.arccos(np.clip(((ds / np.linalg.norm(ds, axis=0)) * (dd / np.linalg.norm(dd, axis=0))).sum(0), -1, 1))).mean()
    print(f'without rest alignment the upper arm is off by {off:.1f} deg on average (expected about 45)')
    assert 20 < off < 70
    # hips: translation scaled by the hip-height ratio
    hs, hd = sn.index('hips'), sn.index('hips')
    ratio = (0.55 * 1.6 - 0.06 * 1.6) / (0.55 * 1.8 - 0.06 * 1.8)
    assert abs(r.scale - ratio) < 1e-9, (r.scale, ratio)
    d_src = C @ (W[:, hs, :3, 3] - srest[hs, :3, 3]).T
    d_dst = (r.world[:, hd, :3, 3] - drest[hd, :3, 3]).T
    print(f'hip translation scale {r.scale:.4f}, error {np.abs(d_dst - ratio * d_src).max():.2e} m')
    assert np.abs(d_dst - ratio * d_src).max() < 1e-9
    # yaw option: 180 degrees turns the character round
    ry = rx.retarget(W, srest, drest, dp, slots, opts=rx.Options(foot_fix=False, yaw=180))
    assert np.abs((ry.world[:, hd, :3, 3] - drest[hd, :3, 3]) - d_dst.T * [-1, 1, -1]).max() < 1e-9
    print('yaw 180: hips travel mirrored about the up axis: ok')


def next_in_chain(k):
    side = k[:2] if k[1] == '_' else ''
    base = k[2:] if side else k
    nxt = {'upperarm': 'forearm', 'forearm': 'hand', 'thigh': 'shin', 'shin': 'foot', 'foot': 'toe',
           'head': 'neck', 'spine2': 'neck'}
    return side + nxt[base]


def test_foot_fix():
    names, par, rest = make_skeleton(0, seed=7)
    slots = {n: (i, i) for i, n in enumerate(names)}
    nf = 6
    W, (T, Q, S) = random_pose(rest, par, nf, 8, amp=0.0, hips_move=0.0)
    T = T.copy(); T[:, 0, 1] -= np.linspace(0, 0.12, nf)               # body sinks 12 cm: feet go through the floor
    W = rx.forward_kinematics(T, Q, S, par)
    r = rx.retarget(W, rest, rest, par, slots, opts=rx.Options(foot_fix=True, foot_tol=0.005))
    toe = names.index('l_toe')
    sunk = (rest[toe, 1, 3] - W[:, toe, 1, 3])
    print(f'foot fix: lift per frame {np.round(r.lift, 3)} for a sink of {np.round(sunk, 3)}')
    assert np.allclose(r.lift[1:], np.maximum(sunk[1:] - 0.005, 0), atol=1e-9)
    after = rest[toe, 1, 3] - r.world[:, toe, 1, 3]
    assert after.max() <= 0.005 + 1e-9


def test_hierarchy_and_extras():
    """Unmapped intermediate bones keep their hold pose; extras copy world rotation."""
    names, par, rest = make_skeleton(20, seed=9)
    nb = len(names)
    # add two more bones: a helper below the left upper arm (unmapped) and a "cape" under spine2 (extra)
    rest2 = np.concatenate([rest, rest[[names.index('l_forearm'), names.index('spine2')]]])
    par2 = np.concatenate([par, [names.index('l_upperarm'), names.index('spine2')]])
    slots = {n: (i, i) for i, n in enumerate(names)}
    W, _ = random_pose(rest2, par2, 5, 10)
    r = rx.retarget(W, rest2, rest2, par2, slots, extras=[(nb + 1, nb + 1)], opts=rx.Options(foot_fix=False))
    assert not r.driven[nb] and r.driven[nb + 1]
    L = rx.rest_locals(rest2, par2)
    assert np.abs(rx.compose_trs(r.T[:, nb], r.Q[:, nb], r.S[:, nb]) - L[nb]).max() < 1e-9
    assert np.abs(r.world[:, nb + 1] - W[:, nb + 1]).max() < 1e-9
    print('hold + extras: ok')


def test_bonemap():
    bartz = "root hip l_femur_c l_femur l_tibia l_foot l_toe r_femur_c r_femur r_tibia r_foot r_toe waist chest " \
            "l_clavicle l_uparm_c l_uparm l_foarm l_hand neck head r_clavicle r_uparm r_foarm r_hand g_hip wep1".split()
    luminous = "C_Hip C_Spine1 C_Spine2 C_Spine3 C_Neck1 C_Head L_Shoulder L_UpperArm L_Forearm L_Hand R_Shoulder " \
               "R_UpperArm R_Forearm R_Hand L_Thigh L_Knee L_Ankle L_Toe R_Thigh R_Knee R_Ankle R_Toe".split()
    m = bmap.auto_map(luminous, bartz)
    assert m.slots['hips'] == ('C_Hip', 'hip') and m.slots['l_shin'] == ('L_Knee', 'l_tibia')
    assert m.slots['spine2'] == ('C_Spine3', 'chest') and m.missing_required == [], m.describe()
    same = bmap.auto_map(bartz, bartz)
    assert same.slots['hips'] == ('hip', 'hip') and ('root', 'root') in same.extras and same.confidence > 0.95
    m2 = bmap.BoneMap.from_json(m.to_json(), luminous, bartz)
    assert m2.slots == m.slots
    # hierarchy fallback with unfamiliar names
    odd = ['Hips', 'sp_a', 'sp_b', 'Head', 'ua_L', 'fa_L', 'LeftHand', 'ua_R', 'fa_R', 'RightHand',
           'th_L', 'sh_L', 'LeftFoot', 'th_R', 'sh_R', 'RightFoot']
    pp = [-1, 0, 1, 2, 2, 4, 5, 2, 7, 8, 0, 10, 11, 0, 13, 14]
    o = bmap.resolve_best(odd, pp)[0]
    print('hierarchy fallback found:', {k: o[k] for k in sorted(o)})
    assert o['l_forearm'] == 'fa_L' and o['l_upperarm'] == 'ua_L' and o['r_shin'] == 'sh_R' and o['r_thigh'] == 'th_R'
    assert o['spine1'] == 'sp_a' and o['spine2'] == 'sp_b'
    print('bonemap: ok')


if __name__ == '__main__':
    for name, fn in list(globals().items()):
        if name.startswith('test_'):
            fn()
            print(f'{name}: PASS')
