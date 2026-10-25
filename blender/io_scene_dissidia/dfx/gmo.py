"""Semantic view of a Dissidia 012 GMO: skeleton, skinned meshes, materials,
textures and animations, decoded from (and encodable back to) the chunk tree.

Conventions discovered from the game files:
  * A character GMO has two Model blocks: [0] holds the skeleton + all Motions,
    [1] ("model-0") holds the skeleton + Parts/Meshes/Arrays/Materials/Textures.
  * References are u32: (blockType << 16) | (scope << 12) | index.
  * Vertex arrays use the PSP GE vertex format word, stride in the top byte.
  * Positions/UVs are normalised ints, mapped through the Model's 0x8015
    commands: sel 0x180 -> pos = n*scale + offset (offset xyz, scale xyz);
    sel 0x003 -> uv = n*scale + offset (offset uv, scale uv).
  * FCurve keys are half floats: frame, then per-dimension values
    (Hermite curves add in/out tangents per dimension).
"""
from __future__ import annotations
import struct
from dataclasses import dataclass, field
import numpy as np

from .chunks import (Chunk, GmoFile, MODEL, BONE, PART, MESH, ARRAYS, MATERIAL,
                     LAYER, TEXTURE, MOTION, FCURVE)


def ref_index(ref: int) -> int:
    return ref & 0xFFF


def ref_type(ref: int) -> int:
    return ref >> 16


def make_ref(btype: int, index: int, scope: int = 1) -> int:
    return (btype << 16) | (scope << 12) | index


# ---------------------------------------------------------------- GE vertex format
@dataclass
class VertexFormat:
    word: int

    @property
    def tc(self): return self.word & 3            # 0 none, 1 u8, 2 u16, 3 f32
    @property
    def color(self): return (self.word >> 2) & 7  # 0 none, 4 5650, 5 5551, 6 4444, 7 8888
    @property
    def normal(self): return (self.word >> 5) & 3
    @property
    def pos(self): return (self.word >> 7) & 3
    @property
    def weight(self): return (self.word >> 9) & 3
    @property
    def nweights(self):
        return ((self.word >> 14) & 7) + 1 if self.weight else 0
    @property
    def stride(self): return self.word >> 24

    _SZ = {0: 0, 1: 1, 2: 2, 3: 4}
    _CSZ = {0: 0, 4: 2, 5: 2, 6: 2, 7: 4}

    def layout(self):
        """Return (offsets dict, computed stride) following GE alignment rules."""
        o = 0
        off = {}
        al = 1

        def place(key, count, size):
            nonlocal o, al
            if not size:
                return
            o = (o + size - 1) // size * size
            off[key] = o
            o += count * size
            al = max(al, size)
        place('w', self.nweights, self._SZ[self.weight])
        place('t', 2, self._SZ[self.tc])
        place('c', 1, self._CSZ[self.color])
        place('n', 3, self._SZ[self.normal])
        place('p', 3, self._SZ[self.pos])
        o = (o + al - 1) // al * al
        return off, o

    @classmethod
    def build(cls, nweights: int, tc=2, color=6, normal=0, pos=2, weight=1):
        word = tc | (color << 2) | (normal << 5) | (pos << 7) | (weight << 9) | ((max(nweights, 1) - 1) << 14)
        stride = cls(word | (1 << 9 if nweights else 0)).layout()[1]
        return cls(word | (stride << 24))


_NORM = {1: (np.uint8, 128.0), 2: (np.uint16, 32768.0), 3: (np.float32, 1.0)}
_SNORM = {1: (np.int8, 127.0), 2: (np.int16, 32767.0), 3: (np.float32, 1.0)}


@dataclass
class VertexArray:
    name: str
    fmt: VertexFormat
    count: int
    raw: bytes                      # exact vertex bytes (count*stride)
    flags: int = 0
    nmorph: int = 1

    def _field(self, key, kind, n, dtypes):
        off, _ = self.fmt.layout()
        if key not in off or not kind:
            return None
        dt, scale = dtypes[kind]
        st = self.fmt.stride
        buf = np.frombuffer(self.raw, np.uint8).reshape(self.count, st)
        size = np.dtype(dt).itemsize
        cols = buf[:, off[key]:off[key] + n * size].copy()
        return cols.view(dt).reshape(self.count, n).astype(np.float64) / scale

    def weights(self):
        w = self._field('w', self.fmt.weight, self.fmt.nweights, _NORM)
        return w

    def uvs_norm(self):
        return self._field('t', self.fmt.tc, 2, _NORM)

    def positions_norm(self):
        return self._field('p', self.fmt.pos, 3, {1: (np.int8, 128.0), 2: (np.int16, 32768.0), 3: (np.float32, 1.0)})

    def normals(self):
        return self._field('n', self.fmt.normal, 3, _SNORM)

    def colors(self):
        """RGBA floats 0..1."""
        off, _ = self.fmt.layout()
        if 'c' not in off:
            return None
        st = self.fmt.stride
        buf = np.frombuffer(self.raw, np.uint8).reshape(self.count, st)
        c = self.fmt.color
        if c == 7:
            return buf[:, off['c']:off['c'] + 4].astype(np.float64) / 255.0
        v = buf[:, off['c']:off['c'] + 2].copy().view(np.uint16).reshape(-1).astype(np.uint32)
        if c == 6:
            ch = [(v >> s) & 15 for s in (0, 4, 8, 12)]
            return np.stack(ch, 1) / 15.0
        if c == 5:
            return np.stack([(v & 31) / 31, ((v >> 5) & 31) / 31, ((v >> 10) & 31) / 31, (v >> 15) & 1], 1).astype(np.float64)
        if c == 4:
            return np.stack([(v & 31) / 31, ((v >> 5) & 63) / 63, ((v >> 11) & 31) / 31, np.ones_like(v)], 1).astype(np.float64)
        return None

    # ---- encode -------------------------------------------------------
    @classmethod
    def encode(cls, name, positions_n, uvs_n, colors, weights, nweights, color_kind=6):
        """positions_n/uvs_n are already normalised (-1..1 / 0..2). weights: (N,nweights) summing to 1."""
        fmt = VertexFormat.build(nweights, color=color_kind if colors is not None else 0)
        off, st = fmt.layout()
        n = len(positions_n)
        buf = np.zeros((n, st), np.uint8)
        if nweights:
            w = np.clip(np.round(np.asarray(weights) * 128), 0, 255).astype(np.int32)
            # fix rounding so each row sums to 128
            diff = 128 - w.sum(1)
            w[np.arange(n), np.argmax(w, 1)] += diff
            buf[:, off['w']:off['w'] + nweights] = w.astype(np.uint8)
        t = np.clip(np.round(np.asarray(uvs_n) * 32768), 0, 65535).astype(np.uint16)
        buf[:, off['t']:off['t'] + 4] = t.view(np.uint8).reshape(n, 4)
        if colors is not None:
            c = np.clip(np.round(np.asarray(colors) * 15), 0, 15).astype(np.uint16)
            v = c[:, 0] | (c[:, 1] << 4) | (c[:, 2] << 8) | (c[:, 3] << 12)
            buf[:, off['c']:off['c'] + 2] = v.astype(np.uint16).view(np.uint8).reshape(n, 2)
        p = np.clip(np.round(np.asarray(positions_n) * 32768), -32768, 32767).astype(np.int16)
        buf[:, off['p']:off['p'] + 6] = p.view(np.uint8).reshape(n, 6)
        return cls(name, fmt, n, buf.tobytes())

    @classmethod
    def from_chunk(cls, c: Chunk):
        word, count, nmorph, flags = struct.unpack_from('<IIII', c.payload, 0)
        fmt = VertexFormat(word)
        raw = c.payload[16:16 + count * fmt.stride * nmorph]
        return cls(c.name, fmt, count, raw, flags, nmorph)

    def to_chunk(self) -> Chunk:
        body = struct.pack('<IIII', self.fmt.word, self.count, self.nmorph, self.flags) + self.raw
        body += b'\0' * ((-len(body)) % 4)
        ch = Chunk(ARRAYS)
        ch.name = self.name
        # Leaf-block header words as the game's loader expects them (and as every
        # retail Arrays chunk has them): word0 = total chunk size, word1 = offset of
        # the first vertex from the chunk start (header size + the 16-byte array
        # header). The generic name setter writes (hs, hs), which makes the loader
        # start every vertex stream 16 bytes early -> garbled geometry in-game.
        hs = 8 + len(ch.header)
        ch.header = struct.pack('<II', hs + len(body), hs + 16) + ch.header[8:]
        ch.payload = body
        return ch


# ---------------------------------------------------------------- meshes
@dataclass
class DrawCall:
    array_ref: int
    prim: int            # 3 = triangles, 4 = triangle strip
    verts_per_prim: int
    nprims: int
    indices: list

    @classmethod
    def from_payload(cls, p: bytes):
        ref, prim, vpp, n = struct.unpack_from('<IIII', p, 0)
        idx = list(struct.unpack_from(f'<{vpp * n}H', p, 16))
        return cls(ref, prim, vpp, n, idx)

    def to_payload(self) -> bytes:
        b = struct.pack('<IIII', self.array_ref, self.prim, self.verts_per_prim, self.nprims)
        b += struct.pack(f'<{len(self.indices)}H', *self.indices)
        return b + b'\0' * ((-len(b)) % 4)

    def triangles(self):
        tris = []
        for k in range(self.nprims):
            seq = self.indices[k * self.verts_per_prim:(k + 1) * self.verts_per_prim]
            if self.prim == 3:
                tris += [tuple(seq[i:i + 3]) for i in range(0, len(seq) - 2, 3)]
            elif self.prim == 4:
                for i in range(len(seq) - 2):
                    a, b, c = seq[i:i + 3]
                    if a == b or b == c or a == c:
                        continue
                    tris.append((a, b, c) if i % 2 == 0 else (b, a, c))
            elif self.prim == 5:
                tris += [(seq[0], seq[i], seq[i + 1]) for i in range(1, len(seq) - 1)]
        return tris


@dataclass
class Mesh:
    name: str
    material: int                 # material index
    blend_subset: list            # indices into owning bone's BlendBones list
    draws: list                   # DrawCall
    extra: list = field(default_factory=list)   # other commands, kept verbatim


@dataclass
class Part:
    name: str
    meshes: list
    arrays: list                  # VertexArray
    header: bytes = b''


# ---------------------------------------------------------------- skeleton
@dataclass
class Bone:
    name: str
    parent: int = -1
    translate: tuple = (0.0, 0.0, 0.0)
    rotate: tuple = (0.0, 0.0, 0.0, 1.0)      # quaternion x,y,z,w
    scale: tuple = (1.0, 1.0, 1.0)
    has_rotate: bool = False
    has_scale: bool = False
    draw_part: int = -1
    blend_bones: list = field(default_factory=list)
    blend_offsets: list = field(default_factory=list)   # 4x4 numpy matrices (row-major, GE convention)
    extra: list = field(default_factory=list)   # other commands verbatim (visibility, blind data)
    header: bytes = b''

    def local_matrix(self):
        x, y, z, w = self.rotate
        r = np.array([
            [1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w)],
            [2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w)],
            [2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y)]])
        m = np.eye(4)
        m[:3, :3] = r * np.array(self.scale)[None, :]
        m[:3, 3] = self.translate
        return m


def parse_bones(model: Chunk) -> list:
    bones = []
    for b in model.find(BONE):
        bone = Bone(b.name, header=b.header)
        for c in b.children:
            p = c.payload
            if c.type == 0x8041:
                bone.parent = ref_index(struct.unpack_from('<I', p)[0])
            elif c.type == 0x8048:
                bone.translate = struct.unpack_from('<3f', p)
            elif c.type == 0x804B:
                bone.rotate = struct.unpack_from('<4f', p); bone.has_rotate = True
            elif c.type == 0x804D:
                bone.scale = struct.unpack_from('<3f', p); bone.has_scale = True
            elif c.type == 0x804E:
                r = struct.unpack_from('<I', p)[0]
                bone.draw_part = ref_index(r) if r != 0xFFFFFFFF else -1
                bone.extra.append(c)
            elif c.type == 0x8044:
                n = struct.unpack_from('<I', p)[0]
                bone.blend_bones = [ref_index(x) for x in struct.unpack_from(f'<{n}I', p, 4)]
            elif c.type == 0x8045:
                n = struct.unpack_from('<I', p)[0]
                bone.blend_offsets = [np.array(struct.unpack_from('<16f', p, 4 + i * 64)).reshape(4, 4) for i in range(n)]
            else:
                bone.extra.append(c)
        bones.append(bone)
    return bones


def world_matrices(bones):
    out = []
    for b in bones:
        m = b.local_matrix()
        out.append(out[b.parent] @ m if b.parent >= 0 else m)
    return out


# ---------------------------------------------------------------- motions
@dataclass
class FCurveData:
    name: str
    interp: int          # 0 linear, 2 hermite, 4 quaternion slerp (flag 0x80 = half-float storage)
    dims: int
    keys: np.ndarray     # (nkeys, 1 + dims) or (nkeys, 1 + 3*dims) for hermite: frame, v..., [in..., out...]
    header: bytes = b''
    raw_flags: int = 0x80
    extra: int = 0

    @classmethod
    def from_chunk(cls, c: Chunk):
        fmt, dims, nk, extra = struct.unpack_from('<IIII', c.payload, 0)
        half = bool(fmt & 0x80)
        interp = fmt & 0x7F
        per = 1 + dims * (3 if interp == 2 else 1)
        if half:
            vals = np.frombuffer(c.payload, '<f2', count=nk * per, offset=16).astype(np.float32)
        else:
            vals = np.frombuffer(c.payload, '<f4', count=nk * per, offset=16)
        return cls(c.name, interp, dims, vals.reshape(nk, per), c.header, fmt & ~0x7F, extra)

    def to_chunk(self) -> Chunk:
        fmt = self.raw_flags | self.interp
        nk = len(self.keys)
        dt = '<f2' if fmt & 0x80 else '<f4'
        body = struct.pack('<IIII', fmt, self.dims, nk, self.extra) + self.keys.astype(dt).tobytes()
        body += b'\0' * ((-len(body)) % 4)
        ch = Chunk(FCURVE)
        nm = self.name.encode('latin1') + b'\0'
        nm += b'\0' * ((-len(nm)) % 4)
        hs = 8 + 8 + len(nm)
        ch.header = struct.pack('<II', hs + len(body), 0x2C) + nm   # (total size, 0x2c) as in game files
        ch.payload = body
        return ch

    # interp: 0 constant, 1 linear, 2 hermite (v,in,out interleaved per dim), 3 cubic, 4 spherical (quat)
    def values_at_keys(self):
        """(frames, values[nkeys, dims])"""
        if self.interp == 2:
            return self.keys[:, 0], self.keys[:, 1::3][:, :self.dims]
        return self.keys[:, 0], self.keys[:, 1:1 + self.dims]

    def evaluate(self, frames):
        """Sample the curve at the given frames -> (len(frames), dims)."""
        frames = np.asarray(frames, np.float64)
        kf, kv = self.values_at_keys()
        kf = kf.astype(np.float64)
        kv = kv.astype(np.float64)
        out = np.empty((len(frames), self.dims))
        if len(kf) == 1 or self.interp == 0:
            idx = np.clip(np.searchsorted(kf, frames, side='right') - 1, 0, len(kf) - 1)
            out[:] = kv[idx]
            return out
        i = np.clip(np.searchsorted(kf, frames, side='right') - 1, 0, len(kf) - 2)
        f0, f1 = kf[i], kf[i + 1]
        t = np.clip((frames - f0) / np.maximum(f1 - f0, 1e-9), 0, 1)[:, None]
        a, b = kv[i], kv[i + 1]
        if self.interp == 4 and self.dims == 4:
            dot = (a * b).sum(1, keepdims=True)
            b = np.where(dot < 0, -b, b)
            dot = np.abs(dot).clip(-1, 1)
            th = np.arccos(dot)
            s = np.sin(th)
            small = s < 1e-5
            wa = np.where(small, 1 - t, np.sin((1 - t) * th) / np.where(small, 1, s))
            wb = np.where(small, t, np.sin(t * th) / np.where(small, 1, s))
            q = wa * a + wb * b
            return q / np.linalg.norm(q, axis=1, keepdims=True)
        if self.interp == 2:
            dt = (f1 - f0)[:, None]
            m0 = self.keys[i + 1 - 1, 3::3][:, :self.dims] * dt   # out-tangent of key i
            m1 = self.keys[i + 1, 2::3][:, :self.dims] * dt       # in-tangent of key i+1
            t2, t3 = t * t, t * t * t
            return (2 * t3 - 3 * t2 + 1) * a + (t3 - 2 * t2 + t) * m0 + (-2 * t3 + 3 * t2) * b + (t3 - t2) * m1
        return a + (b - a) * t


@dataclass
class Binding:
    bone: int
    command: int         # 0x48 translate, 0x4B rotateQ, 0x4D scale
    index: int
    fcurve: int
    raw: bytes = b''


@dataclass
class Motion:
    name: str
    frame_start: float
    frame_end: float
    frame_rate: float
    bindings: list
    fcurves: list
    extra: list = field(default_factory=list)
    header: bytes = b''

    @classmethod
    def from_chunk(cls, c: Chunk):
        fs = fe = 0.0
        rate = 30.0
        binds, fcs, extra = [], [], []
        for ch in c.children:
            if ch.type == 0x80B1:
                fs, fe = struct.unpack_from('<2f', ch.payload)
            elif ch.type == 0x80B2:
                rate = struct.unpack_from('<f', ch.payload)[0]
            elif ch.type == 0x80B3:
                blk, cmd, idx, fc = struct.unpack_from('<4I', ch.payload)
                binds.append(Binding(ref_index(blk), cmd, idx, ref_index(fc), ch.payload))
            elif ch.type == FCURVE:
                fcs.append(FCurveData.from_chunk(ch))
            else:
                extra.append(ch)
        return cls(c.name, fs, fe, rate, binds, fcs, extra, c.header)

    def to_chunk(self) -> Chunk:
        m = Chunk(MOTION, header=self.header)
        if not self.header:
            m.name = self.name
        m.children.append(Chunk(0x80B1, payload=struct.pack('<2f', self.frame_start, self.frame_end)))
        m.children.append(Chunk(0x80B2, payload=struct.pack('<f', self.frame_rate)))
        m.children += self.extra
        for b in self.bindings:
            m.children.append(Chunk(0x80B3, payload=struct.pack('<4I', make_ref(BONE, b.bone), b.command, b.index, (FCURVE << 16) | b.fcurve)))
        m.children += [f.to_chunk() for f in self.fcurves]
        return m


# ---------------------------------------------------------------- materials / textures
@dataclass
class TextureRef:
    name: str
    filename: str
    gim: bytes
    chunk: Chunk = None


@dataclass
class MaterialInfo:
    name: str
    textures: list       # texture indices from layers
    chunk: Chunk = None


def parse_textures(model: Chunk):
    out = []
    for t in model.find(TEXTURE):
        fn, img = '', b''
        for c in t.children:
            if c.type == 0x8012:
                fn = c.payload.split(b'\0')[0].decode('latin1')
            elif c.type == 0x8013:
                img = c.payload
        out.append(TextureRef(t.name, fn, img, t))
    return out


def parse_materials(model: Chunk):
    out = []
    for m in model.find(MATERIAL):
        tex = []
        for layer in m.find(LAYER):
            for c in layer.children:
                if c.type == 0x8091:
                    tex.append(ref_index(struct.unpack_from('<I', c.payload)[0]))
        out.append(MaterialInfo(m.name, tex, m))
    return out


# ---------------------------------------------------------------- whole character
@dataclass
class Character:
    gmo: GmoFile
    anim_model: Chunk
    mesh_model: Chunk
    bones: list
    parts: list
    materials: list
    textures: list
    motions: list
    pos_offset: np.ndarray
    pos_scale: np.ndarray
    uv_offset: np.ndarray
    uv_scale: np.ndarray

    @classmethod
    def load(cls, path_or_bytes):
        g = GmoFile.load(path_or_bytes) if isinstance(path_or_bytes, str) else GmoFile.from_bytes(path_or_bytes)
        models = g.models
        anim = next((m for m in models if m.find(MOTION)), None)
        mesh = next((m for m in models if m.find(PART)), models[-1])
        po, ps, uo, us = np.zeros(3), np.ones(3), np.zeros(2), np.ones(2)
        for c in mesh.find(0x8015):
            sel = struct.unpack_from('<I', c.payload)[0]
            f = np.array(struct.unpack_from(f'<{(len(c.payload) - 4) // 4}f', c.payload, 4))
            if sel == 0x180:
                po, ps = f[:3], f[3:6]
            elif sel == 0x3:
                uo, us = f[:2], f[2:4]
        parts = []
        for p in mesh.find(PART):
            arrays = [VertexArray.from_chunk(a) for a in p.find(ARRAYS)]
            meshes = []
            for m in p.find(MESH):
                mat, subset, draws, extra = -1, [], [], []
                for c in m.children:
                    if c.type == 0x8061:
                        mat = ref_index(struct.unpack_from('<I', c.payload)[0])
                    elif c.type == 0x8062:
                        n = struct.unpack_from('<I', c.payload)[0]
                        subset = list(struct.unpack_from(f'<{n}I', c.payload, 4))
                    elif c.type == 0x8066:
                        draws.append(DrawCall.from_payload(c.payload))
                    else:
                        extra.append(c)
                meshes.append(Mesh(m.name, mat, subset, draws, extra))
            parts.append(Part(p.name, meshes, arrays, p.header))
        motions = [Motion.from_chunk(m) for m in anim.find(MOTION)] if anim else []
        return cls(g, anim, mesh, parse_bones(mesh), parts, parse_materials(mesh),
                   parse_textures(mesh), motions, po, ps, uo, us)

    # convenience
    def part_owner(self, part_index):
        for i, b in enumerate(self.bones):
            if b.draw_part == part_index:
                return i
        return -1

    def mesh_geometry(self, part_index, mesh_index):
        """Yield per draw call: dict(positions, uvs, colors, weights(bone index -> w), tris)."""
        part = self.parts[part_index]
        mesh = part.meshes[mesh_index]
        owner = self.bones[self.part_owner(part_index)] if self.part_owner(part_index) >= 0 else None
        for dc in mesh.draws:
            arr = part.arrays[ref_index(dc.array_ref)]
            pos = arr.positions_norm() * self.pos_scale + self.pos_offset
            uv = arr.uvs_norm()
            uv = uv * self.uv_scale + self.uv_offset if uv is not None else None
            w = arr.weights()
            bone_ids = []
            if owner is not None and mesh.blend_subset:
                bone_ids = [owner.blend_bones[s] for s in mesh.blend_subset]
            yield dict(array=arr, positions=pos, uvs=uv, colors=arr.colors(), weights=w,
                       bone_ids=bone_ids, tris=dc.triangles(), draw=dc)
