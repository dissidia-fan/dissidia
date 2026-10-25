"""Plain, original placeholder weapons for slot testing (sword, greatsword, spear, dagger, shuriken, shield).

Each shape is built in a "weapon frame": +X runs from the grip (origin) to the tip, +Y is the blade's
width and +Z its thickness. `fit_to_slot` maps that frame onto the slot's old weapon (its long axis,
reach and orientation in the holding bone's space), so placeholders sit in the hand the same way.
All faces carry a cell index into one small palette texture (PALETTE).
"""
import numpy as np

# texture cells (4x4 grid of flat colours)
PALETTE = {'steel': (196, 204, 214), 'edge': (236, 240, 246), 'dark': (46, 50, 62), 'grip': (34, 30, 30),
           'blue': (92, 150, 230), 'trim': (150, 156, 170)}
CELLS = list(PALETTE)
TEX = 64


def palette_rgba():
    img = np.zeros((TEX, TEX, 4), np.uint8); img[..., 3] = 255
    c = TEX // 4
    for i, name in enumerate(CELLS):
        y, x = divmod(i, 4)
        img[y * c:(y + 1) * c, x * c:(x + 1) * c, :3] = PALETTE[name]
    return img


def cell_uv(name):
    i = CELLS.index(name); y, x = divmod(i, 4); c = 1 / 4
    return ((x + 0.5) * c, (y + 0.5) * c)        # centre of the cell (v down, like GMO)


class Shape:
    def __init__(self):
        self.V, self.F, self.C = [], [], []      # vertices, faces (index lists), colour cell per face

    def add(self, verts, faces, cell):
        b = len(self.V)
        self.V += [np.asarray(v, float) for v in verts]
        self.F += [[b + i for i in f] for f in faces]
        self.C += [cell] * len(faces)

    def box(self, x0, x1, hy, hz, cell, hy1=None, hz1=None):
        hy1 = hy if hy1 is None else hy1; hz1 = hz if hz1 is None else hz1
        v = [(x0, -hy, -hz), (x0, hy, -hz), (x0, hy, hz), (x0, -hy, hz),
             (x1, -hy1, -hz1), (x1, hy1, -hz1), (x1, hy1, hz1), (x1, -hy1, hz1)]
        f = [(0, 1, 2, 3), (4, 7, 6, 5), (0, 4, 5, 1), (1, 5, 6, 2), (2, 6, 7, 3), (3, 7, 4, 0)]
        self.add(v, f, cell)

    def prism(self, x0, x1, r0, r1, cell, n=8):
        """Round shaft / tapered cone along X."""
        a = np.linspace(0, 2 * np.pi, n, endpoint=False)
        ring = lambda x, r: [(x, r * np.cos(t), r * np.sin(t)) for t in a]
        v = ring(x0, r0) + ring(x1, r1)
        f = [(i, (i + 1) % n, n + (i + 1) % n, n + i) for i in range(n)]
        f += [tuple(range(n - 1, -1, -1)), tuple(range(n, 2 * n))]
        self.add(v, f, cell)

    def blade(self, x0, x1, w, t, tip, cell='steel', edge='edge', curve=0.0):
        """Flat blade: diamond cross-section (spine in the middle), pointed tip at x1 + tip."""
        n = 6
        xs = np.linspace(x0, x1, n)
        cs = [curve * ((x - x0) / max(1e-6, x1 - x0)) ** 2 for x in xs]
        v = []
        for x, c in zip(xs, cs):
            v += [(x, -w + c, 0), (x, c, t), (x, w + c, 0), (x, c, -t)]
        tipv = (x1 + tip, cs[-1] + w * 0.15, 0)
        v.append(tipv)
        f, cell_f = [], []
        for i in range(n - 1):
            a, b = 4 * i, 4 * (i + 1)
            for k in range(4):
                f.append((a + k, b + k, b + (k + 1) % 4, a + (k + 1) % 4))
        T = len(v) - 1; last = 4 * (n - 1)
        for k in range(4):
            f.append((last + k, T, last + (k + 1) % 4))
        f.append((3, 2, 1, 0))
        self.add(v, f, cell)

    def arrays(self):
        return np.array(self.V), self.F, self.C


# ---------------------------------------------------------------- shapes (weapon frame, metres; reach ~1)
def sword():
    s = Shape()
    s.prism(-0.20, -0.17, 0.028, 0.028, 'trim')          # pommel
    s.prism(-0.17, 0.06, 0.018, 0.018, 'grip')           # grip
    s.box(0.06, 0.10, 0.11, 0.025, 'dark', hy1=0.13)     # guard
    s.box(0.10, 0.16, 0.05, 0.02, 'blue', hy1=0.045)     # blue core under the guard
    s.blade(0.16, 0.92, 0.042, 0.009, 0.10)
    return s


def greatsword():
    s = Shape()
    s.prism(-0.38, -0.34, 0.032, 0.032, 'trim')
    s.prism(-0.34, 0.06, 0.02, 0.02, 'grip')
    s.box(0.06, 0.12, 0.16, 0.03, 'dark')
    s.box(0.12, 0.20, 0.10, 0.024, 'blue', hy1=0.09)
    s.blade(0.20, 0.95, 0.085, 0.014, 0.12)
    return s


def spear():
    s = Shape()
    s.prism(-0.95, -0.90, 0.02, 0.02, 'trim')
    s.prism(-0.90, 0.72, 0.015, 0.015, 'dark')
    s.prism(0.72, 0.76, 0.024, 0.024, 'blue')
    s.blade(0.76, 0.98, 0.04, 0.01, 0.12)
    return s


def dagger():
    s = Shape()
    s.prism(-0.13, -0.11, 0.02, 0.02, 'trim')
    s.prism(-0.11, 0.03, 0.015, 0.015, 'grip')
    s.box(0.03, 0.06, 0.06, 0.018, 'dark')
    s.blade(0.06, 0.62, 0.03, 0.007, 0.08, curve=0.03)
    return s


def shuriken():
    """Four-point star in the X-Y plane, centred on the origin, with a hub."""
    s = Shape()
    pts = []
    for k in range(8):
        a = np.pi / 4 * k
        r = 1.0 if k % 2 == 0 else 0.32
        pts.append((r * np.cos(a), r * np.sin(a)))
    for z, sign in ((0.035, 1), (-0.035, -1)):
        v = [(0, 0, z * 1.4)] + [(x, y, 0) for x, y in pts]
        f = [(0, 1 + i, 1 + (i + 1) % 8) if sign > 0 else (0, 1 + (i + 1) % 8, 1 + i) for i in range(8)]
        s.add(v, f, 'steel')
    s.prism(-0.05, 0.05, 0.12, 0.12, 'blue', n=12)        # hub (axis X; rotated to Z below)
    V, F, C = s.arrays()
    hub = np.arange(len(V) - 24, len(V))
    V[hub] = V[hub][:, [1, 2, 0]] * [1, 1, 1]             # hub axis X -> Z
    s.V = list(V)
    return s


def shield():
    """Round shield in the X-Y plane (face +Z), radius 1, slight dome, with rim and boss."""
    s = Shape()
    n = 20; a = np.linspace(0, 2 * np.pi, n, endpoint=False)
    rim = [(np.cos(t), np.sin(t), 0) for t in a]
    inner = [(0.86 * np.cos(t), 0.86 * np.sin(t), 0.06) for t in a]
    back = [(0.95 * np.cos(t), 0.95 * np.sin(t), -0.04) for t in a]
    v = rim + inner + back + [(0, 0, 0.14), (0, 0, -0.02)]
    C0, C1 = 3 * n, 3 * n + 1
    f_rim = [(i, (i + 1) % n, n + (i + 1) % n, n + i) for i in range(n)]
    f_face = [(n + i, n + (i + 1) % n, C0) for i in range(n)]
    f_back = [((i + 1) % n, i, 2 * n + i, 2 * n + (i + 1) % n) for i in range(n)] + \
             [(2 * n + (i + 1) % n, 2 * n + i, C1) for i in range(n)]
    s.add(v, f_rim, 'trim'); s.F, s.C = s.F, s.C
    b = len(s.V) - len(v)
    s.F += [[b + i for i in f] for f in f_face]; s.C += ['steel'] * len(f_face)
    s.F += [[b + i for i in f] for f in f_back]; s.C += ['dark'] * len(f_back)
    s.prism(-0.08, 0.08, 0.16, 0.16, 'blue', n=12)        # boss, axis X -> Z
    V = np.array(s.V); hub = np.arange(len(V) - 24, len(V))
    V[hub] = V[hub][:, [1, 2, 0]] + [0, 0, 0.12]
    s.V = list(V)
    return s


SHAPES = {'sword': sword, 'greatsword': greatsword, 'spear': spear, 'dagger': dagger,
          'shuriken': shuriken, 'shield': shield}
FLAT = {'shuriken', 'shield'}                               # fitted as discs, not blades
NATIVE = {'dagger'}                                         # keep their own length (short)


# ---------------------------------------------------------------- fitting to a slot
def pca(P):
    c = P.mean(0)
    w, v = np.linalg.eigh(np.cov((P - c).T))
    order = np.argsort(w)[::-1]
    return c, v[:, order], np.sqrt(np.maximum(w[order], 0))


def fit_to_slot(kind, old_local):
    """Place a placeholder in the holding bone's space, matching the old weapon there.
    old_local: (N,3) old weapon vertices in that bone's space. Returns (verts, faces, cells)."""
    V, F, C = SHAPES[kind]().arrays()
    c, axes, _ = pca(old_local)
    if kind in FLAT:
        proj = (old_local - c) @ axes[:, :2]
        radius = 0.5 * float(np.mean(np.percentile(np.abs(proj), 98, axis=0))) * 1.6   # fills the old shape's width
        R = np.stack([axes[:, 0], axes[:, 1], np.cross(axes[:, 0], axes[:, 1])], 1)
        return (V * radius) @ R.T + c, F, C
    far = old_local[np.argmax(np.linalg.norm(old_local, axis=1))]   # tip: farthest from the hand
    ax = far / np.linalg.norm(far)                                    # grip -> tip, as the warp dash uses
    reach = float(np.max(old_local @ ax))
    side = axes[:, 1] - (axes[:, 1] @ ax) * ax; side /= np.linalg.norm(side)
    R = np.stack([ax, side, np.cross(ax, side)], 1)
    k = 1.0 if kind in NATIVE else reach / float(V[:, 0].max())
    Vs = V * [k, 1.0, 1.0]                                    # stretch along the reach only
    return Vs @ R.T, F, C
