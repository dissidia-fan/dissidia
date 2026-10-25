"""Build GMO Motion data from per-frame local TRS samples (shared by the
Blender exporter and the retargeter)."""
import numpy as np
from .gmo import FCurveData, Binding, Motion


def reduce_keys(frames, vals, tol, slerp=False):
    """Indices of the minimal key set so linear/slerp interpolation stays within tol."""
    n = len(frames)
    if np.abs(vals - vals[0]).max() <= tol:
        return [0]
    if n <= 2:
        return list(range(n))
    keep = [0]
    i = 0
    while i < n - 1:
        j = i + 2
        while j < n:
            t = (frames[i + 1:j] - frames[i]) / (frames[j] - frames[i])
            interp = vals[i] + (vals[j] - vals[i]) * t[:, None]
            if slerp:
                interp /= np.linalg.norm(interp, axis=1, keepdims=True)
            if np.abs(interp - vals[i + 1:j]).max() > tol:
                break
            j += 1
        i = j - 1
        keep.append(i)
    return keep


def continuous_quats(q):
    q = np.array(q, np.float64)
    for i in range(1, len(q)):
        if (q[i] * q[i - 1]).sum() < 0:
            q[i] = -q[i]
    return q


def build_motion(name, frames, tracks, frame_rate=30.0, extra=None, header=b'',
                 frame_start=None, frame_end=None, tol_t=1e-4, tol_r=2e-4, tol_s=1e-4):
    """tracks: {bone_index: (T[n,3], R[n,4] xyzw, S[n,3])} sampled at `frames`."""
    frames = np.asarray(frames, np.float64)
    fcurves, bindings = [], []

    def add(bi, cmd, vals, tol, slerp):
        keep = reduce_keys(frames, vals, tol, slerp)
        keys = np.c_[frames[keep], vals[keep]]
        interp = 0 if len(keep) == 1 else (4 if slerp else 1)
        fcurves.append(FCurveData(f'fcurve-{len(fcurves)}', interp, vals.shape[1], keys))
        bindings.append(Binding(bi, cmd, 0, len(fcurves) - 1))

    for bi in sorted(tracks):
        T, R, S = tracks[bi]
        add(bi, 0x48, np.asarray(T, np.float64), tol_t, False)
        add(bi, 0x4B, continuous_quats(R), tol_r, True)
        add(bi, 0x4D, np.asarray(S, np.float64), tol_s, False)
    fs = frames[0] if frame_start is None else frame_start
    fe = frames[-1] if frame_end is None else frame_end
    return Motion(name, float(fs), float(fe), float(frame_rate), bindings, fcurves, list(extra or []), header)


def sample_motion(motion, rest_bones, frames):
    """Evaluate a parsed Motion -> {bone: (T,R,S)} using rest TRS for missing channels."""
    frames = np.asarray(frames, np.float64)
    chans = {}
    for b in motion.bindings:
        chans.setdefault(b.bone, {})[b.command] = motion.fcurves[b.fcurve].evaluate(frames)
    out = {}
    n = len(frames)
    for bi, c in chans.items():
        rb = rest_bones[bi]
        T = c.get(0x48, np.tile(rb.translate, (n, 1)))
        R = c.get(0x4B, np.tile(rb.rotate, (n, 1)))
        S = c.get(0x4D, np.tile(rb.scale, (n, 1)))
        R = R / np.linalg.norm(R, axis=1, keepdims=True)
        out[bi] = (np.asarray(T), continuous_quats(R), np.asarray(S))
    return out
