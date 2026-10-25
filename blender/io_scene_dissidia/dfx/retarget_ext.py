"""Retarget an animation from a foreign humanoid skeleton onto a game skeleton (numpy only, no Blender).

Everything is expressed in world matrices, so differing bone axes and rest poses do not matter:

    dst_pose = src_pose . (A . src_rest)^-1 . dst_rest          (all 3x3 world rotations)

`A` is the optional rest alignment: the smallest world rotation that turns the source limb direction
(joint -> next joint in the chain) at rest onto the destination's, so a Mixamo T-pose arm and an A-pose arm
end up pointing the same way in the same pose.  Bones are posed parent-first; bones nobody drives keep their
`hold` local transform (the rest pose unless the caller supplies one), and positions come from the
destination's own bone lengths.  Only the hips' motion is transferred as translation (scaled by the ratio of
hip heights above the feet) and applied to the hips and any ancestor sitting on the same spot (Bartz's `root`).

Conventions match the add-on: matrices are 4x4 column-vector (`M @ v`), quaternions are x, y, z, w.
"""
from dataclasses import dataclass, field
import numpy as np

from .bonemap import CHAINS

EYE3 = np.eye(3)


@dataclass
class Options:
    rest_align: bool = True        # rotate each source limb's rest direction onto the destination's
    orient: str = 'auto'           # 'auto': detect the source's left/up axes and rotate them onto the destination's
    yaw: float = 0.0               # extra turn about `up`, degrees (after `orient`)
    up: tuple = (0.0, 1.0, 0.0)    # destination up axis (game space is Y up)
    translation: str = 'full'      # 'full', 'vertical' (hips height only), 'in_place' (strip `travel`) or 'none'
    scale: float = 0.0             # translation scale; 0 = hip-height ratio
    foot_fix: bool = True          # lift the body when a foot sinks below its rest height
    foot_tol: float = 0.02         # metres a foot may sink below its rest height before the body is lifted
    start_at_origin: bool = False  # remove the first frame's horizontal hip offset (ripped clips start anywhere)
    travel: object = None          # (nf, 3) positions of the source's root (travel) bone, same space as src_world;
                                   # 'in_place' removes its horizontal motion from the hips (FFXV `Trans`, Mixamo root)


@dataclass
class Result:
    T: np.ndarray                  # (nf, nd, 3) local translation
    Q: np.ndarray                  # (nf, nd, 4) local rotation, xyzw
    S: np.ndarray                  # (nf, nd, 3) local scale
    driven: np.ndarray             # (nd,) bool: bones whose pose comes from the source
    world: np.ndarray              # (nf, nd, 4, 4) resulting world matrices
    scale: float = 1.0
    orient: np.ndarray = field(default_factory=lambda: np.eye(3))
    lift: np.ndarray = None        # (nf,) foot-contact lift that was applied
    travel: np.ndarray = None      # (nf, 3) horizontal travel 'in_place' took off the hips (destination space, scaled)
    notes: list = field(default_factory=list)


# ---------------------------------------------------------------- small maths
def unit(v):
    v = np.asarray(v, np.float64)
    n = np.linalg.norm(v)
    return v / n if n > 1e-12 else v


def orthonormal(R):
    """Nearest rotation to each 3x3 (strips scale and shear)."""
    u, _, vt = np.linalg.svd(np.asarray(R, np.float64))
    d = np.linalg.det(u @ vt)
    u[..., :, 2] *= np.where(d < 0, -1.0, 1.0)[..., None]
    return u @ vt


def rot_between(a, b):
    """Smallest rotation matrix taking direction a onto direction b."""
    a, b = unit(a), unit(b)
    c = float(np.dot(a, b))
    if c > 1 - 1e-9:
        return np.eye(3)
    if c < -1 + 1e-9:
        axis = np.cross(a, [1, 0, 0])
        if np.linalg.norm(axis) < 1e-6:
            axis = np.cross(a, [0, 1, 0])
        return axis_angle(unit(axis), np.pi)
    v = np.cross(a, b)
    k = np.array([[0, -v[2], v[1]], [v[2], 0, -v[0]], [-v[1], v[0], 0]])
    return np.eye(3) + k + k @ k / (1 + c)


def axis_angle(axis, angle):
    x, y, z = unit(axis)
    c, s = np.cos(angle), np.sin(angle)
    C = 1 - c
    return np.array([[c + x * x * C, x * y * C - z * s, x * z * C + y * s],
                     [y * x * C + z * s, c + y * y * C, y * z * C - x * s],
                     [z * x * C - y * s, z * y * C + x * s, c + z * z * C]])


def mat3_to_quat(m):
    """Rotation matrices (..., 3, 3) -> quaternions (..., 4) xyzw."""
    m = np.asarray(m, np.float64)
    shp = m.shape[:-2]
    a = m.reshape(-1, 3, 3)
    q = np.empty((len(a), 4))
    m00, m11, m22 = a[:, 0, 0], a[:, 1, 1], a[:, 2, 2]
    tr = m00 + m11 + m22
    c0 = tr > 0
    c1 = ~c0 & (m00 > m11) & (m00 > m22)
    c2 = ~c0 & ~c1 & (m11 > m22)
    c3 = ~c0 & ~c1 & ~c2
    s = np.sqrt(np.maximum(tr + 1.0, 1e-12)) * 2
    q[c0] = np.stack([(a[:, 2, 1] - a[:, 1, 2]) / s, (a[:, 0, 2] - a[:, 2, 0]) / s, (a[:, 1, 0] - a[:, 0, 1]) / s, 0.25 * s], 1)[c0]
    s = np.sqrt(np.maximum(1.0 + m00 - m11 - m22, 1e-12)) * 2
    q[c1] = np.stack([0.25 * s, (a[:, 0, 1] + a[:, 1, 0]) / s, (a[:, 0, 2] + a[:, 2, 0]) / s, (a[:, 2, 1] - a[:, 1, 2]) / s], 1)[c1]
    s = np.sqrt(np.maximum(1.0 + m11 - m00 - m22, 1e-12)) * 2
    q[c2] = np.stack([(a[:, 0, 1] + a[:, 1, 0]) / s, 0.25 * s, (a[:, 1, 2] + a[:, 2, 1]) / s, (a[:, 0, 2] - a[:, 2, 0]) / s], 1)[c2]
    s = np.sqrt(np.maximum(1.0 + m22 - m00 - m11, 1e-12)) * 2
    q[c3] = np.stack([(a[:, 0, 2] + a[:, 2, 0]) / s, (a[:, 1, 2] + a[:, 2, 1]) / s, 0.25 * s, (a[:, 1, 0] - a[:, 0, 1]) / s], 1)[c3]
    q /= np.linalg.norm(q, axis=-1, keepdims=True)
    return q.reshape(shp + (4,))


def quat_to_mat3(q):
    q = np.asarray(q, np.float64)
    x, y, z, w = q[..., 0], q[..., 1], q[..., 2], q[..., 3]
    m = np.empty(q.shape[:-1] + (3, 3))
    m[..., 0, 0] = 1 - 2 * (y * y + z * z); m[..., 0, 1] = 2 * (x * y - z * w); m[..., 0, 2] = 2 * (x * z + y * w)
    m[..., 1, 0] = 2 * (x * y + z * w); m[..., 1, 1] = 1 - 2 * (x * x + z * z); m[..., 1, 2] = 2 * (y * z - x * w)
    m[..., 2, 0] = 2 * (x * z - y * w); m[..., 2, 1] = 2 * (y * z + x * w); m[..., 2, 2] = 1 - 2 * (x * x + y * y)
    return m


def compose_trs(T, Q, S):
    """(..., 3), (..., 4) xyzw, (..., 3) -> (..., 4, 4)."""
    T = np.asarray(T, np.float64)
    M = np.zeros(T.shape[:-1] + (4, 4))
    M[..., :3, :3] = quat_to_mat3(Q) * np.asarray(S, np.float64)[..., None, :]
    M[..., :3, 3] = T
    M[..., 3, 3] = 1
    return M


def decompose_trs(M):
    """(..., 4, 4) -> T, Q (xyzw), S."""
    T = M[..., :3, 3].copy()
    S = np.linalg.norm(M[..., :3, :3], axis=-2)
    R = M[..., :3, :3] / np.where(S == 0, 1, S)[..., None, :]
    return T, mat3_to_quat(R), S


def continuous(q):
    q = q.copy()
    for i in range(1, len(q)):
        if (q[i] * q[i - 1]).sum() < 0:
            q[i] = -q[i]
    return q


def safe_inv(M):
    try:
        return np.linalg.inv(M)
    except np.linalg.LinAlgError:
        return np.linalg.pinv(M)


def parent_first(parent):
    """Bone indices ordered so every parent precedes its children."""
    n = len(parent)
    order, seen = [], [False] * n
    kids = [[] for _ in range(n)]
    roots = []
    for i, p in enumerate(parent):
        (roots if p < 0 else kids[p]).append(i)
    stack = roots[::-1]
    while stack:
        i = stack.pop()
        order.append(i); seen[i] = True
        stack.extend(kids[i][::-1])
    return order + [i for i in range(n) if not seen[i]]


def rest_locals(rest, parent):
    """Local 4x4 of each bone in the rest pose."""
    out = np.empty_like(rest)
    for i, p in enumerate(parent):
        out[i] = safe_inv(rest[p]) @ rest[i] if p >= 0 else rest[i]
    return out


def forward_kinematics(T, Q, S, parent):
    """Local TRS (nf, nd, ...) -> world matrices (nf, nd, 4, 4)."""
    L = compose_trs(T, Q, S)
    W = np.empty_like(L)
    for i in parent_first(parent):
        p = parent[i]
        W[:, i] = W[:, p] @ L[:, i] if p >= 0 else L[:, i]
    return W


# ---------------------------------------------------------------- orientation
def _frame(pos, slots):
    """Orthonormal frame (columns left, up, forward) of a rest skeleton from its slot positions.
    `pos` maps slot -> position.  None when the slots are not there."""
    lefts = []
    for a, b in (('l_upperarm', 'r_upperarm'), ('l_thigh', 'r_thigh'), ('l_clavicle', 'r_clavicle')):
        if a in pos and b in pos:
            v = pos[a] - pos[b]
            if np.linalg.norm(v) > 1e-6:
                lefts.append(unit(v))
    if not lefts:
        return None
    left = unit(np.mean(lefts, 0))
    top = pos.get('head', pos.get('neck'))
    bottom = pos.get('hips')
    if top is not None and bottom is not None:
        up = top - bottom
    elif all(k in pos for k in ('l_upperarm', 'r_upperarm', 'l_thigh', 'r_thigh')):
        up = (pos['l_upperarm'] + pos['r_upperarm']) / 2 - (pos['l_thigh'] + pos['r_thigh']) / 2
    else:
        return None
    up = up - np.dot(up, left) * left
    if np.linalg.norm(up) < 1e-6:
        return None
    up = unit(up)
    return np.stack([left, up, np.cross(left, up)], 1)


def _snap(C):
    """Nearest signed axis permutation of C if it is a proper rotation and close to C, else None."""
    S = np.zeros((3, 3))
    for r in range(3):
        c = int(np.argmax(np.abs(C[r])))
        S[r, c] = np.sign(C[r, c])
    if abs(np.linalg.det(S) - 1) > 1e-6 or np.abs(S @ S.T - np.eye(3)).max() > 1e-6:
        return None
    ang = np.arccos(np.clip((np.trace(C @ S.T) - 1) / 2, -1, 1))
    return S if ang < np.radians(25) else None


def detect_orientation(src_rest, dst_rest, slots, notes=None):
    """Rotation taking the source's frame (left/up/forward from its rest pose) onto the destination's.
    Snapped to a multiple of 90 degrees when it is close to one (importers only ever permute axes)."""
    ps = {s: src_rest[a][:3, 3] for s, (a, b) in slots.items()}
    pd = {s: dst_rest[b][:3, 3] for s, (a, b) in slots.items()}
    Fs, Fd = _frame(ps, slots), _frame(pd, slots)
    if Fs is None or Fd is None:
        if notes is not None:
            notes.append('could not detect the skeletons\' facing; using the source axes as they are')
        return np.eye(3)
    C = Fd @ Fs.T
    S = _snap(C)
    if S is None:
        if notes is not None:
            notes.append('source orientation is not a multiple of 90 degrees from the destination\'s; used unsnapped')
        return C
    return S


# ---------------------------------------------------------------- the retarget
def retarget(src_world, src_rest, dst_rest, dst_parent, slots, extras=(), hold=None, opts=None):
    """
    src_world : (nf, ns, 4, 4) world matrices of the source bones per frame
    src_rest  : (ns, 4, 4)     source rest world matrices
    dst_rest  : (nd, 4, 4)     destination rest world matrices (game space)
    dst_parent: (nd,)          parent index or -1
    slots     : {slot: (src index, dst index)}      (see bonemap.SLOTS)
    extras    : [(src index, dst index)]            extra bones: world rotation delta, no limb alignment
    hold      : None or (T, Q, S) local transforms (nd, ...) for bones nothing drives and for the offsets of the
                bones that are driven; default is the destination rest pose
    """
    opts = opts or Options()
    notes = []
    src_world = np.array(src_world, np.float64)
    src_rest = np.array(src_rest, np.float64)
    dst_rest = np.asarray(dst_rest, np.float64)
    dst_parent = list(dst_parent)
    nf, nd = len(src_world), len(dst_rest)
    up = unit(opts.up)

    # clean rotations (imports often carry scale), then bring the source frame onto the destination's
    src_world[..., :3, :3] = orthonormal(src_world[..., :3, :3])
    src_rest[..., :3, :3] = orthonormal(src_rest[..., :3, :3])
    C = np.eye(3)
    if opts.orient == 'auto':
        C = detect_orientation(src_rest, dst_rest, slots, notes)
    if opts.yaw:
        C = axis_angle(up, np.radians(opts.yaw)) @ C
    if not np.allclose(C, np.eye(3)):
        src_world[..., :3, :] = np.einsum('ij,...jk->...ik', C, src_world[..., :3, :])
        src_rest[..., :3, :] = np.einsum('ij,...jk->...ik', C, src_rest[..., :3, :])
    trav = None if opts.travel is None else np.asarray(opts.travel, np.float64)[:, :3] @ C.T

    # per-bone constant K so that dst rotation = src rotation @ K
    pairs = [(s, a, b) for s, (a, b) in slots.items()] + [(None, a, b) for a, b in extras]
    A = {}
    present = {s for s in slots}
    if opts.rest_align:
        for chain in CHAINS:
            run = [s for s in chain if s in present]
            for j, s in enumerate(run):
                if s == 'hips':
                    A[s] = EYE3
                elif j + 1 < len(run):
                    nx = run[j + 1]
                    us = src_rest[slots[nx][0]][:3, 3] - src_rest[slots[s][0]][:3, 3]
                    ud = dst_rest[slots[nx][1]][:3, 3] - dst_rest[slots[s][1]][:3, 3]
                    ls, ld = np.linalg.norm(us), np.linalg.norm(ud)
                    if min(ls, ld) < 1e-4 or min(ls, ld) < 0.02 * max(ls, ld):
                        A[s] = EYE3                      # no usable direction (e.g. Bartz's hip -> waist)
                    else:
                        A[s] = rot_between(us, ud)
                else:
                    A[s] = A.get(run[j - 1], EYE3) if j else EYE3      # hand, toe, head follow the bone before
    dst_to_src, K = {}, {}
    for s, a, b in pairs:
        Rs = src_rest[a][:3, :3]
        Rd = dst_rest[b][:3, :3]
        Aa = A.get(s, EYE3) if s else EYE3
        K[b] = Rs.T @ Aa.T @ Rd
        dst_to_src[b] = a

    # hold (unmapped / offset) local transforms
    if hold is None:
        Lh = rest_locals(dst_rest, dst_parent)
        Th, Qh, Sh = decompose_trs(Lh)
    else:
        Th, Qh, Sh = (np.asarray(x, np.float64) for x in hold)
        Lh = compose_trs(Th, Qh, Sh)

    # translation of the hips
    Tr = np.zeros((nf, 3))
    removed = np.zeros((nf, 3))
    trans_set = set()
    scale = 1.0
    if 'hips' in slots and opts.translation != 'none':
        hs, hd = slots['hips']
        trans_set.add(hd)
        p = dst_parent[hd]
        while p >= 0 and np.linalg.norm(dst_rest[p][:3, 3] - dst_rest[hd][:3, 3]) < 0.02:
            trans_set.add(p)
            p = dst_parent[p]
        feet = [k for k in ('l_foot', 'r_foot') if k in slots] or [k for k in ('l_toe', 'r_toe') if k in slots]
        if opts.scale > 0:
            scale = opts.scale
        elif feet:
            hsrc = src_rest[hs][:3, 3] @ up - np.mean([src_rest[slots[k][0]][:3, 3] @ up for k in feet])
            hdst = dst_rest[hd][:3, 3] @ up - np.mean([dst_rest[slots[k][1]][:3, 3] @ up for k in feet])
            scale = hdst / hsrc if hsrc > 1e-3 and hdst > 1e-3 else 1.0
        d = src_world[:, hs, :3, 3] - src_rest[hs][:3, 3]
        if opts.translation == 'in_place':
            if trav is None:
                notes.append('in place: no root bone travel was given; the hips keep the full travel')
            else:
                tv = trav - trav[:1]
                removed = tv - (tv @ up)[:, None] * up[None]
                d = d - removed
        if opts.start_at_origin and nf:
            h0 = d[0] - (d[0] @ up) * up
            d = d - h0[None]
        if opts.translation == 'vertical':
            d = (d @ up)[:, None] * up[None]
        Tr = d * scale
        removed = removed * scale
    elif 'hips' in slots:
        notes.append('hip translation switched off')

    order = parent_first(dst_parent)
    foot_ids = [slots[k][1] for k in ('l_foot', 'r_foot', 'l_toe', 'r_toe') if k in slots]
    # parentless extras (camera / trans bones, floating effects) follow their source's travel as well
    free = {b: a for a, b in extras if dst_parent[b] < 0 and b not in trans_set}

    def solve(shift):
        W = np.zeros((nf, nd, 4, 4))
        for i in order:
            p = dst_parent[i]
            base = W[:, p] @ Lh[i] if p >= 0 else np.broadcast_to(Lh[i], (nf, 4, 4)).copy()
            if i in K:
                base[:, :3, :3] = (src_world[:, dst_to_src[i], :3, :3] @ K[i]) * Sh[i][None, None, :]
            if i in trans_set and (p < 0 or p not in trans_set):
                base[:, :3, 3] += shift
            elif i in free:
                base[:, :3, 3] = dst_rest[i][:3, 3] + scale * (src_world[:, free[i], :3, 3] - src_rest[free[i]][:3, 3])
            W[:, i] = base
        return W

    shift = np.zeros((nf, 3))
    if trans_set:
        # put the hips where the source's travel (scaled) says, wherever the parents' hold pose leaves them
        hd = slots['hips'][1]
        shift = (dst_rest[hd][:3, 3][None] + Tr) - solve(shift)[:, hd, :3, 3]
    W = solve(shift)
    lift = np.zeros(nf)
    if opts.foot_fix and foot_ids and trans_set:
        h0 = np.array([dst_rest[i][:3, 3] @ up for i in foot_ids])
        # a foot joint's rest height is its height on the ground
        h = np.stack([W[:, i, :3, 3] @ up for i in foot_ids], 1)
        lift = np.maximum(0.0, (h0[None] - opts.foot_tol - h).max(1))
        if lift.max() > 0:
            W = solve(shift + lift[:, None] * up[None])

    driven = np.zeros(nd, bool)
    for i in list(K) + list(trans_set):
        driven[i] = True
    T = np.empty((nf, nd, 3)); Q = np.empty((nf, nd, 4)); S = np.empty((nf, nd, 3))
    for i in range(nd):
        if driven[i]:
            p = dst_parent[i]
            L = safe_inv(W[:, p]) @ W[:, i] if p >= 0 else W[:, i]
            T[:, i], Q[:, i], S[:, i] = decompose_trs(L)
            Q[:, i] = continuous(Q[:, i])
        else:
            T[:, i], Q[:, i], S[:, i] = Th[i], Qh[i], Sh[i]
    return Result(T, Q, S, driven, W, scale, C, lift, travel=removed, notes=notes)
