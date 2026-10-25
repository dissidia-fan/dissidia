"""Retarget motions between Dissidia characters that share the named core rig.

Rules (configurable):
  * rotations: copied directly for bones present in both rigs (same axis conventions);
  * translations: destination's own rest translation (keeps its proportions),
    except `root` (scaled by the ratio of root heights) and camera bones (copied);
  * `delta_bones` (e.g. r_wep): q_dst = q_dst_rest * inv(q_src_rest) * q_src(t);
  * bones only in the destination stay at their rest pose.
"""
import numpy as np
from .gmo import Character, parse_bones
from .motion_encode import sample_motion, build_motion


def qmul(a, b):
    ax, ay, az, aw = np.moveaxis(a, -1, 0)
    bx, by, bz, bw = np.moveaxis(b, -1, 0)
    return np.stack([aw * bx + ax * bw + ay * bz - az * by,
                     aw * by - ax * bz + ay * bw + az * bx,
                     aw * bz + ax * by - ay * bx + az * bw,
                     aw * bw - ax * bx - ay * by - az * bz], -1)


def qinv(q):
    q = np.array(q, np.float64)
    q[..., :3] *= -1
    return q


CAMERA_BONES = ('camera_include', 'camera2_include', 'trans_include')


def retarget(src: Character, dst: Character, delta_bones=('r_wep',), copy_translate=CAMERA_BONES,
             root_name='root', log=print):
    sb = parse_bones(src.anim_model)
    db = parse_bones(dst.anim_model)
    sidx = {b.name: i for i, b in enumerate(sb)}
    scale = 1.0
    if root_name in sidx:
        rs = np.linalg.norm(sb[sidx[root_name]].translate)
        rd = np.linalg.norm(next(b for b in db if b.name == root_name).translate)
        scale = rd / rs if rs > 1e-6 else 1.0
    log(f'[retarget] {len(sb)} -> {len(db)} bones, root scale {scale:.3f}')
    motions = []
    for m in src.motions:
        f0, f1 = int(np.floor(m.frame_start)), int(np.ceil(m.frame_end))
        frames = np.arange(f0, f1 + 1, dtype=np.float64)
        n = len(frames)
        st = sample_motion(m, sb, frames)
        tracks = {}
        for di, d in enumerate(db):
            si = sidx.get(d.name)
            T = np.tile(d.translate, (n, 1))
            R = np.tile(d.rotate, (n, 1))
            S = np.tile(d.scale, (n, 1))
            if si is not None and si in st:
                sT, sR, sS = st[si]
                if d.name in delta_bones:
                    R = qmul(np.tile(d.rotate, (n, 1)), qmul(np.tile(qinv(sb[si].rotate), (n, 1)), sR))
                else:
                    R = sR
                S = sS
                if d.name == root_name:
                    T = sT * scale
                elif d.name in copy_translate:
                    T = sT
            elif si is None and not (np.allclose(d.rotate, [0, 0, 0, 1]) and np.allclose(d.translate, 0)):
                pass   # destination-only bone: hold rest
            tracks[di] = (T, R, S)
        motions.append(build_motion(m.name, frames, tracks, m.frame_rate, extra=m.extra,
                                    header=m.header, frame_start=m.frame_start, frame_end=m.frame_end))
    return motions


def with_motions(dst: Character, motions):
    """Return GMO bytes of dst with its motion list replaced."""
    from .chunks import MOTION
    am = dst.anim_model
    first = next((i for i, c in enumerate(am.children) if c.type == MOTION), len(am.children))
    others = [c for c in am.children if c.type != MOTION]
    am.children = others[:first] + [m.to_chunk() for m in motions] + others[first:]
    return dst.gmo.to_bytes()
