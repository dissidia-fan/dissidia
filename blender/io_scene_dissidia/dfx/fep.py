"""Dissidia 012 Duodecim (PSP) character effect package: FEP, FEPA, FXSE and the MPK wrapper.

Standard library only. Everything here was established from the game code (EBOOT, see
work/fx_re/FEP_NOTES.md) and from 14 character packages; fields marked "?" are inferred from data.

Layout model
------------
A FEP is a tree of C structs linked by u32 file offsets, relocated in place at load time by
FUN_0894351C. `pointer_slots()` transcribes that routine: every pointer slot and its target.
`Fep.from_bytes` cuts the file into Blocks at every pointer target; each Block keeps its raw bytes
(including trailing 0xCD padding) and a map {offset in block: target Block}. `to_bytes()` lays the
blocks out again (0xCD padding only where an edit broke alignment), rewrites every pointer and the
size field, so an unmodified file round-trips byte-identically and an edited one stays consistent.
Typed views (Effect, Body, Node, ParamSet, ResRef) and decoders (Tex, Anm, Mdl) sit on top, plus
edit helpers (clone_effect, clone_resource, set_res_bytes, relink, tint_effect).

Who plays what (event 0x0219 {u16 frame, u16 op, u32 x, u32 arg0, u32 arg1}):
  arg0 = FEPA record id (+0x24) -> record +0x28 = index in the FEP *name table* -> effect id ->
  t0 entry with the same group bits -> t0.body_index -> t1 body. FXSE[record +0x28] = sound cues.
"""
from __future__ import annotations

import bisect
import struct
import zlib

U32 = struct.Struct('<I'); I32 = struct.Struct('<i'); U16 = struct.Struct('<H'); F32 = struct.Struct('<f')
PAD = 0xCD
KINDS = ('TEX', 'MDL', 'ANM', 'VTL')     # resource tables at 0x30/0x34/0x38/0x3C, counts at 0x20..0x26


def u32(b, o): return U32.unpack_from(b, o)[0]
def i32(b, o): return I32.unpack_from(b, o)[0]
def u16(b, o): return U16.unpack_from(b, o)[0]
def f32(b, o): return F32.unpack_from(b, o)[0]
def _cstr(b): return bytes(b).split(b'\0')[0].decode('latin1')


# ====================================================================== channel knowledge
# Channel ("track") ids index the 162-entry descriptor table at FEP+0x08 (identical in every file and
# compared with the engine's own copy at 0x089D9A98 by FUN_089453F4). Descriptor 8 B:
# {u8, u8, u16, u16 param_size, u16 state_size}. Channel -> component type via the table at
# 0x089D95D0, type -> vtable via the registry built at 0x0894DD74. Names: "code" = from the
# component code, the rest from parameter values.
CHANNEL_NAMES = {
    1: 'position', 2: 'position+rate', 3: 'position+vel+acc',
    4: 'rotation (deg)', 5: 'rotation+rate', 6: 'rotation+vel+acc',
    7: 'scale', 8: 'scale+rate', 9: 'scale+vel+acc',
    10: 'texanim (ANM@12, code)', 11: 'model (MDL@12, code)', 14: 'flags?',
    15: 'colour rgba', 16: 'colour rgba+rate', 17: 'colour rgba+vel+acc',
    20: 'model emitter (MDL@12, code)', 21: 'particle emitter (ANM@12 MDL@244 VTL@246, code)',
    22: 'emit position range?', 23: 'emit rotation range?', 24: 'emit scale range?',
    26: 'trail/strip (MDL@14 VTL@16, code)', 30: 'trail/strip (MDL@14 VTL@16, code)',
    53: 'ANM@16 (code)', 54: 'ANM@160 (code)', 55: 'ANM@16 (code)', 56: 'ANM@16 (code)', 57: 'ANM@16 (code)',
    63: 'TEX@0 (code)', 78: 'MDL@4 VTL@6 (code)', 80: 'ANM@44 (code)', 105: 'TEX@44 (code)',
    108: 'MDL@16 VTL@18 (code)', 119: 'MDL@12 (code)', 150: 'trail/strip (MDL@14 VTL@16, code)',
}
COLOUR_CHANNELS = (15, 16, 17)   # vec4 RGBA (value[, rate[, acc]]), 128.0 = 1.0 (PSP convention)

# Parameter fields holding an index into a resource table (static analysis of each channel's
# component methods: lw x,456(ctx); lw y,{48,52,56,60}(x); index from the parameter block).
# (kind, byte offset in the parameter block, struct fmt); -1 = none. Covers every local resource of
# 14 characters except 8 TEX (library/unused) and 6 MDL.
LINKS = {
    10: [('ANM', 12, '<i')], 11: [('MDL', 12, '<i')], 20: [('MDL', 12, '<i')],
    21: [('ANM', 12, '<i'), ('MDL', 244, '<h'), ('VTL', 246, '<h')],
    26: [('MDL', 14, '<h'), ('VTL', 16, '<h')], 30: [('MDL', 14, '<h'), ('VTL', 16, '<h')],
    150: [('MDL', 14, '<h'), ('VTL', 16, '<h')],
    53: [('ANM', 16, '<i')], 54: [('ANM', 160, '<i')], 55: [('ANM', 16, '<i')],
    56: [('ANM', 16, '<i')], 57: [('ANM', 16, '<i')],
    63: [('TEX', 0, '<i')], 78: [('MDL', 4, '<h'), ('VTL', 6, '<h')], 80: [('ANM', 44, '<i')],
    105: [('TEX', 44, '<i')], 108: [('MDL', 16, '<h'), ('VTL', 18, '<h')], 119: [('MDL', 12, '<i')],
}

# Alignment each block kind gets in original files (14 characters); 16 = VFPU lv.q / texture data.
KIND_ALIGN = {'header': 16, 'hdr.t0': 16, 'hdr.x08': 8, 'names.arr': 8, 'x40.arr': 8,
              'res0.data': 16, 'res1.data': 16, 'res2.data': 16, 'res3.data': 16,
              't0.leaf': 16, 'node.p32': 16, 'node.p36.it': 16}


# ====================================================================== pointer map
def pointer_slots(d: bytes) -> dict:
    """Every pointer slot of a FEP: {slot offset: (target offset, kind)}.
    Transcription of FUN_0894351C (EBOOT), which relocates the file in place after loading."""
    n = len(d)
    slots = {}

    def U(o): return U32.unpack_from(d, o)[0]
    def H(o): return U16.unpack_from(d, o)[0]

    def P(slot, kind, allow0=False):
        v = U(slot)
        if v == 0 and allow0:
            return 0
        if not 0 < v <= n:
            raise ValueError('bad pointer %#x at %#x (%s)' % (v, slot, kind))
        slots[slot] = (v, kind)
        return v

    p = P(0x08, 'hdr.x08', True)                      # channel descriptors {u32 n, ptr}
    if p:
        P(p + 4, 'x08.arr')
    p = P(0x44, 'hdr.names', True)                    # names {u32 n, ptr} -> {u32 id, u32 len, ptr}
    if p:
        a = P(p + 4, 'names.arr')
        for i in range(U(p)):
            P(a + 12 * i + 8, 'name.str')
    p = P(0x40, 'hdr.x40', True)                      # ids with bit 0x200 (empty in characters)
    if p:
        a = P(p + 4, 'x40.arr')
        for i in range(H(p)):
            q = P(a + 4 * i, 'x40.item'); P(q + 8, 'x40.item.p8')
    for t in range(4):                                # TEX / MDL / ANM / VTL references
        a = P(0x30 + 4 * t, 'hdr.res%d' % t, True)
        if a:
            for i in range(H(0x20 + 2 * t)):
                r = P(a + 4 * i, 'res%d.ref' % t); P(r + 12, 'res%d.data' % t)
    a = P(0x28, 'hdr.t0')                             # effect entries
    for i in range(H(0x1C)):
        e = P(a + 4 * i, 't0.ent'); la = P(e + 8, 't0.leafarr')
        for j in range(H(e + 6)):
            lf = P(la + 4 * j, 't0.leaf'); P(lf + 12, 'leaf.p12', True)
    a = P(0x2C, 'hdr.t1')                             # effect bodies
    for i in range(H(0x1E)):
        e = P(a + 4 * i, 't1.ent')
        x = P(e + 28, 't1.spawn')
        for j in range(H(e + 16)):
            it = P(x + 4 * j, 't1.spawn.it'); P(it + 4, 't1.spawn.nodes')
        x = P(e + 32, 't1.nodes')
        for j in range(H(e + 18)):
            nd = P(x + 4 * j, 't1.node', True)
            if not nd or d[nd + 8] == 0:
                continue
            y = P(nd + 28, 'node.tracks')
            for k in range(d[nd + 8]):
                z = P(y + 4 * k, 'node.track'); P(z + 8, 'node.track.keys')
            if d[nd + 18]:
                z = P(nd + 32, 'node.p32'); P(z + 8, 'node.p32.p8')
            y = P(nd + 36, 'node.segs')
            for k in range(d[nd + 19]):
                z = P(y + 4 * k, 'node.p36.it'); P(z + 8, 'node.p36.it.p8')
            for k in range(8):
                P(nd + 40 + 4 * k, 'node.p%d' % (40 + 4 * k))
        P(e + 36, 't1.chans')
        x = P(e + 40, 't1.p40')
        for j in range(12):
            it = P(x + 4 * j, 't1.p40.it'); P(it + 4, 't1.p40.it.p4')
        x = P(e + 44, 't1.p44')
        for j in range(H(e + 24)):
            it = P(x + 4 * j, 't1.p44.it'); P(it + 4, 't1.p44.it.p4')
        x = P(e + 48, 't1.p48')
        for j in range(H(e + 26)):
            P(x + 4 * j, 't1.p48.it')
    return slots


# ====================================================================== block graph
class Block:
    __slots__ = ('data', 'ptrs', 'kinds', 'align', 'pos')

    def __init__(self, data, kinds=(), align=4, pos=None):
        self.data = bytearray(data)
        self.ptrs = {}            # offset in block -> Block
        self.kinds = set(kinds)
        self.align = align
        self.pos = pos            # offset in the source file (None for new blocks)

    def __repr__(self):
        return '<Block %s %s %d B>' % (hex(self.pos) if self.pos is not None else 'new',
                                       '|'.join(sorted(self.kinds)) or '-', len(self.data))

    def clone(self):
        b = Block(self.data, self.kinds, self.align, None)
        b.ptrs = dict(self.ptrs)
        return b


def _align_for(kinds):
    return max([KIND_ALIGN.get(k, 4) for k in kinds] or [4])


def _pad_to(data: bytearray, align: int):
    data += bytes([PAD]) * ((-len(data)) % align)
    return data


# ====================================================================== resources
class Tex:
    """'TEX\\0' u32 id, u32 1, u32 0, u32 fmt (4 = 8-bit CLUT; 5 assumed 4-bit), u32 swizzled (1),
    u32 clut_off, u32 w, u32 h, u32 pix_off (0x30) | w*h indices, PSP swizzle (16 B x 8 rows) |
    at clut_off: u32 n (256), u32 0, 8 pad, n x RGBA8888 (alpha 0..255)."""

    def __init__(self, raw: bytes):
        self.raw = bytes(raw)
        (self.magic, self.id, self.h0, self.h1, self.fmt, self.swizzled, self.clut_off,
         self.w, self.h, self.pix_off) = struct.unpack_from('<4s9I', raw, 0)
        self.n_colours = u32(raw, self.clut_off)
        po = self.clut_off + 16
        self.palette = [tuple(raw[po + 4 * i:po + 4 * i + 4]) for i in range(self.n_colours)]
        self.bpp = 8 if self.fmt == 4 else 4
        wb = self.w * self.bpp // 8
        px = raw[self.pix_off:self.pix_off + wb * self.h]
        self.indices = self.unswizzle(px, wb, self.h) if self.swizzled else bytes(px)

    @staticmethod
    def unswizzle(src: bytes, wb: int, h: int) -> bytes:
        out = bytearray(wb * h); bw = wb // 16; i = 0
        for ty in range(h // 8):
            for tx in range(bw):
                for r in range(8):
                    o = (ty * 8 + r) * wb + tx * 16
                    out[o:o + 16] = src[i:i + 16]; i += 16
        return bytes(out)

    @staticmethod
    def swizzle(src: bytes, wb: int, h: int) -> bytes:
        out = bytearray(wb * h); bw = wb // 16; i = 0
        for ty in range(h // 8):
            for tx in range(bw):
                for r in range(8):
                    o = (ty * 8 + r) * wb + tx * 16
                    out[i:i + 16] = src[o:o + 16]; i += 16
        return bytes(out)

    def to_rgba(self) -> bytes:
        pal = [bytes(c) for c in self.palette]
        if self.bpp == 8:
            return b''.join(pal[i] for i in self.indices)
        out = []
        for b in self.indices:
            out.append(pal[b & 15]); out.append(pal[b >> 4])
        return b''.join(out)

    def with_palette(self, palette) -> bytes:
        """Raw TEX bytes with a new palette (same size); store with Fep.set_res_bytes."""
        b = bytearray(self.raw); po = self.clut_off + 16
        for i, c in enumerate(list(palette)[:self.n_colours]):
            b[po + 4 * i:po + 4 * i + 4] = bytes(int(max(0, min(255, round(x)))) for x in c)
        return bytes(b)

    def with_indices(self, indices: bytes) -> bytes:
        """Raw TEX bytes with new (linear, unswizzled) pixel indices of the same size."""
        b = bytearray(self.raw); wb = self.w * self.bpp // 8
        px = self.swizzle(indices, wb, self.h) if self.swizzled else bytes(indices)
        b[self.pix_off:self.pix_off + len(px)] = px
        return bytes(b)

    def to_png(self) -> bytes:
        return png_rgba(self.w, self.h, self.to_rgba())


def _ge_vertex_layout(vtype):
    """PSP GE vertex type -> (size, [(name, off, count, comp size)], colour format)."""
    tcs = (0, 1, 2, 4)[vtype & 3]; col = (vtype >> 2) & 7
    nrm = (0, 1, 2, 4)[(vtype >> 5) & 3]; pos = (0, 1, 2, 4)[(vtype >> 7) & 3]
    cs = {0: 0, 4: 2, 5: 2, 6: 2, 7: 4}.get(col, 0)
    off = 0; al = 1; fields = []
    for name, cnt, sz in (('uv', 2, tcs), ('col', 1, cs), ('nrm', 3, nrm), ('pos', 3, pos)):
        if not sz:
            continue
        off = (off + sz - 1) // sz * sz
        fields.append((name, off, cnt, sz)); off += cnt * sz; al = max(al, sz)
    return (off + al - 1) // al * al, fields, col


def decode_vertices(raw, off, count, vtype):
    size, fields, col = _ge_vertex_layout(vtype)
    out = []
    for i in range(count):
        o = off + i * size; v = {}
        for name, fo, cnt, sz in fields:
            if name == 'uv':
                v['uv'] = struct.unpack_from('<%d%s' % (cnt, {1: 'B', 2: 'H', 4: 'f'}[sz]), raw, o + fo)
            elif name == 'col':
                c = u16(raw, o + fo) if sz == 2 else u32(raw, o + fo)
                if col == 6:
                    v['col'] = tuple(((c >> s) & 15) * 17 for s in (0, 4, 8, 12))
                elif col == 7:
                    v['col'] = tuple((c >> s) & 255 for s in (0, 8, 16, 24))
                else:
                    v['col'] = c
            else:
                v[name] = struct.unpack_from('<%d%s' % (cnt, {1: 'b', 2: 'h', 4: 'f'}[sz]), raw, o + fo)
        out.append(v)
    return out


class Anm:
    """Texture animation ('TexAnm' in the EBOOT), a flipbook of small textured quads.
    Header 0x50: 'ANM\\0' u32 id, u32 1, u32 1, u32 flags (0/0x80), u32 0, u32 0, u32 n?,
    u32 counts[4] (tex, clips, seqs, frames), u32 offs[4], f32[4] (?). Then tex ids (u32, padded to 8),
    clips 12 B {u32 flags, u32 index, u32 n}, seqs 12 B {u32 flags, u32 n, u32 off -> u32[n] frame
    indices}, frames 28 B {u32 0, u8 rgba[4] (0x80 = 1.0), u32 nverts, u32 vtx_off, u32 render, i32, i32},
    vertices (GE vtype assumed 0x81: u8 uv, s8 pos). Offsets block-relative."""
    FRAME = struct.Struct('<IIIIIii')

    def __init__(self, raw: bytes):
        self.raw = bytes(raw)
        h = struct.unpack_from('<4s7I4I4I4f', raw, 0)
        self.id = h[1]; self.head = h[2:8]; self.counts = h[8:12]; self.offs = h[12:16]; self.params = h[16:20]
        c, o = self.counts, self.offs
        self.tex_ids = [u32(raw, o[0] + 4 * i) for i in range(c[0])]
        self.clips = [struct.unpack_from('<3I', raw, o[1] + 12 * i) for i in range(c[1])]
        self.seqs = []
        for i in range(c[2]):
            fl, n, so = struct.unpack_from('<3I', raw, o[2] + 12 * i)
            self.seqs.append((fl, [u32(raw, so + 4 * k) for k in range(n)]))
        self.frames = []
        for i in range(c[3]):
            fo = o[3] + 28 * i
            fr = self.FRAME.unpack_from(raw, fo)
            self.frames.append({'colour': tuple(raw[fo + 4:fo + 8]), 'nverts': fr[2], 'vtx_off': fr[3],
                                'render': fr[4], 'x': fr[5], 'y': fr[6]})

    def colour_offset(self, i):
        return self.offs[3] + 28 * i + 4

    def vertices(self, i, vtype=0x81):
        f = self.frames[i]
        return decode_vertices(self.raw, f['vtx_off'], f['nverts'], vtype)


class Mdl:
    """Particle model. 'MDL\\0' u16 1, u16 8, u32 id, u32 n_mat, u32 mat_off (0x1C), u32 n_grp, u32 grp_off |
    materials 24 B {u32 tex_id, u32 0, u32 ?, u32 render, u32 0, i32 -1/0} |
    groups 52 B {u32 n_prim, u32 prim_off, u32 ?, f32 x10 (bounds/uv?)} |
    prims 24 B {u32 prim (3 tris, 4 strip), u32 GE vtype (0x99 = u8 uv, 4444 colour, s8 pos),
    u32 vsize, u32 0, u32 nverts, u32 vtx_off}. Offsets block-relative."""

    def __init__(self, raw: bytes):
        self.raw = bytes(raw)
        _, self.ver0, self.ver1, self.id, n_mat, mat_off, n_grp, grp_off = struct.unpack_from('<4sHHIIIII', raw, 0)
        self.mat_off = mat_off
        self.materials = [dict(zip(('tex_id', 'a', 'b', 'render', 'c', 'd'), struct.unpack_from('<5Ii', raw, mat_off + 24 * i)))
                          for i in range(n_mat)]
        self.groups = []
        for g in range(n_grp):
            go = grp_off + 52 * g
            n_prim, prim_off, flag = struct.unpack_from('<3I', raw, go)
            prims = []
            for k in range(n_prim):
                if prim_off + 24 * (k + 1) > len(raw):
                    break
                prims.append(dict(zip(('prim', 'vtype', 'vsize', 'z', 'nverts', 'vtx_off'),
                                      struct.unpack_from('<6I', raw, prim_off + 24 * k))))
            self.groups.append({'flag': flag, 'params': struct.unpack_from('<10f', raw, go + 12), 'prims': prims})

    def vertices(self, g, k):
        p = self.groups[g]['prims'][k]
        return decode_vertices(self.raw, p['vtx_off'], p['nverts'], p['vtype'])

    def colour_slots(self):
        """[(offset, colour format)] of every vertex colour (for tinting)."""
        out = []
        for g in self.groups:
            for p in g['prims']:
                size, fields, col = _ge_vertex_layout(p['vtype'])
                for name, fo, cnt, sz in fields:
                    if name == 'col':
                        out += [(p['vtx_off'] + i * size + fo, col) for i in range(p['nverts'])
                                if p['vtx_off'] + i * size + fo + sz <= len(self.raw)]
        return out


class Vtl:
    """'VTL\\0' u32 id, ... (used beside MDL by trail/emitter channels; layout not decoded)."""

    def __init__(self, raw: bytes):
        self.raw = bytes(raw)
        self.id = u32(raw, 4)
        self.head = struct.unpack_from('<10I', raw, 8)


DECODERS = {'TEX': Tex, 'MDL': Mdl, 'ANM': Anm, 'VTL': Vtl}


# ====================================================================== typed views
class ResRef:
    """20 B {f32 scale? (1.0), u32 id, u32 size, ptr data, i32 runtime handle (-1)}.
    size 0 (+ id 0xF0xxxxxx) = resource of the resident system library, looked up by id; its data
    pointer just points at the next local block."""

    def __init__(self, fep, kind, index, block):
        self.fep, self.kind, self.index, self.block = fep, kind, index, block

    scale = property(lambda s: f32(s.block.data, 0))
    id = property(lambda s: u32(s.block.data, 4))
    size = property(lambda s: u32(s.block.data, 8))
    handle = property(lambda s: i32(s.block.data, 16))
    system = property(lambda s: s.size == 0)
    data_block = property(lambda s: s.block.ptrs.get(12))

    @property
    def raw(self):
        return bytes(self.data_block.data[:self.size]) if self.size else None

    def decode(self):
        r = self.raw
        return DECODERS[self.kind](r) if r else None

    def __repr__(self):
        return '<%s #%d %#010x %s>' % (self.kind, self.index, self.id, 'system' if self.system else '%d B' % self.size)


class ParamSet:
    """12 B {u16 frame0, u16 frame1, u32 n, ptr table}; table n x {u16 track, u16 off, u16 a, u16 b},
    parameter blocks follow (off is relative to the ParamSet start; the table always sits at +12).
    node+32 = initial values; node+36 = keyframe segments [frame0, frame1) (values interpolated)."""

    def __init__(self, node, block):
        self.node, self.block = node, block
        self.table = block.ptrs[8]

    frame0 = property(lambda s: u16(s.block.data, 0))
    frame1 = property(lambda s: u16(s.block.data, 2))
    count = property(lambda s: u32(s.block.data, 4))

    def entries(self):
        """[(track index, channel, off, (a, b), (block, rel, size))] - bytes are block.data[rel:rel+size]."""
        out = []
        for q in range(self.count):
            ti, off, a, b = struct.unpack_from('<4H', self.table.data, 8 * q)
            ch = self.node.tracks[ti].chan
            out.append((ti, ch, off, (a, b), (self.table, off - 12, self.node.fep.param_size(ch))))
        return out


class Track:
    """node+28 item 12 B {u16 channel, u16 state_off, u32 n, ptr u16 data}."""

    def __init__(self, node, block):
        self.node, self.block = node, block

    chan = property(lambda s: u16(s.block.data, 0))
    state_off = property(lambda s: u16(s.block.data, 2))
    n_keys = property(lambda s: u32(s.block.data, 4))


class Node:
    """Emitter/element, 72 B: u16 flags, u16 state_size, u16 duration, u16 index, u8 n_tracks, u8 ?,
    u8 ?, u8 kind?, u16 ?, u16 ?, u8 ?, u8 life?, u8 has_init, u8 n_segments, u8 list_counts[8],
    ptr tracks[], ptr init ParamSet, ptr segments[], ptr lists[8] (u16 track indices per engine phase)."""
    FIELDS = struct.Struct('<4H4B2H4B8B')

    def __init__(self, fep, body, index, block):
        self.fep, self.body, self.index, self.block = fep, body, index, block
        f = self.FIELDS.unpack_from(block.data, 0)
        (self.flags, self.state_size, self.duration, self.serial_index, self.n_tracks, self.b9, self.b10,
         self.kind, self.h12, self.h14, self.b16, self.life, self.has_init, self.n_segments) = f[:14]
        self.list_counts = f[14:22]
        self.tracks, self.init, self.segments = [], None, []
        if self.n_tracks:
            arr = block.ptrs[28]
            self.tracks = [Track(self, arr.ptrs[4 * k]) for k in range(self.n_tracks)]
            if self.has_init:
                self.init = ParamSet(self, block.ptrs[32])
            arr = block.ptrs[36]
            self.segments = [ParamSet(self, arr.ptrs[4 * k]) for k in range(self.n_segments)]

    def lists(self):
        out = []
        for k in range(8):
            b = self.block.ptrs.get(40 + 4 * k)
            out.append([u16(b.data, 2 * i) for i in range(self.list_counts[k])] if b else [])
        return out

    def param_sets(self):
        return ([self.init] if self.init else []) + self.segments

    def resources(self):
        """[(kind, index, channel)] referenced by this node."""
        out = []
        for ps in self.param_sets():
            for ti, ch, off, ab, (blk, rel, size) in ps.entries():
                for kind, fo, fmt in LINKS.get(ch, ()):
                    if fo + struct.calcsize(fmt) <= size:
                        v = struct.unpack_from(fmt, blk.data, rel + fo)[0]
                        if v >= 0:
                            out.append((kind, v, ch))
        return out


class Body:
    """t1 entry 52 B: u32 a (0/2), u32 duration (frames), u32 c, u32 d (0/2/3), u16 n_spawn, u16 n_nodes,
    u16 n_chans, u16 n_p40 (12), u16 n_serial, u16 n_p48 (0), ptr spawn[], ptr nodes[], ptr chans
    (u16[]), ptr p40[12] (authoring-tool records with 0xFD/0xDD fill), ptr serial[], ptr p48[]."""

    def __init__(self, fep, index, block):
        self.fep, self.index, self.block = fep, index, block
        d = block.data
        self.a, self.duration, self.c, self.d = struct.unpack_from('<4I', d, 0)
        self.n_spawn, self.n_nodes, self.n_chans, self.n_p40, self.n_serial, self.n_p48 = struct.unpack_from('<6H', d, 16)
        arr = block.ptrs[32]
        self.nodes = []
        for j in range(self.n_nodes):
            nb = arr.ptrs.get(4 * j)
            self.nodes.append(Node(fep, self, j, nb) if nb is not None else None)

    def spawns(self):
        """[(frame, [node indices])]: the nodes started at that frame of the effect."""
        arr = self.block.ptrs[28]; out = []
        for j in range(self.n_spawn):
            it = arr.ptrs[4 * j]
            fr, n = struct.unpack_from('<HH', it.data, 0)
            lst = it.ptrs[4]
            out.append((fr, [u16(lst.data, 2 * k) for k in range(n)]))
        return out

    def channels(self):
        b = self.block.ptrs[36]
        return [u16(b.data, 2 * i) for i in range(self.n_chans)]

    def serials(self):
        """[(spawn frame, global node serial)] (body+44)."""
        arr = self.block.ptrs[44]; out = []
        for j in range(self.n_serial):
            it = arr.ptrs[4 * j]
            n = u16(it.data, 0); lst = it.ptrs[4]
            out += [struct.unpack_from('<HH', lst.data, 4 * k) for k in range(n)]
        return out


class Effect:
    """t0 entry 12 B {u32 id, u16 body_index, u16 n_leaves, ptr leaves[]}; leaf 156 B {u32 id, u32 1,
    u8 type, .., ptr p12 (0), f32 transform defaults ..., f32 300, f32 500}.
    id = package base (FEP+0x10) | group << 10 | leaf; group = bits 10-16 (0..127), leaf = bits 0-8."""

    def __init__(self, fep, index, block):
        self.fep, self.index, self.block = fep, index, block

    id = property(lambda s: u32(s.block.data, 0))
    body_index = property(lambda s: u16(s.block.data, 4))
    n_leaves = property(lambda s: u16(s.block.data, 6))
    group = property(lambda s: (s.id >> 10) & 0x7F)

    @property
    def leaves(self):
        arr = self.block.ptrs[8]
        return [arr.ptrs[4 * j] for j in range(self.n_leaves)]

    @property
    def body(self):
        return self.fep.body(self.body_index)

    @property
    def name_index(self):
        return self.fep.name_index(self.id)

    @property
    def name(self):
        i = self.name_index
        return self.fep.names()[i][1] if i is not None else None

    def resources(self, transitive=True):
        """{kind: sorted indices} used by this effect (MDL/ANM -> TEX by id when transitive)."""
        res = {k: set() for k in KINDS}
        for n in self.body.nodes:
            if n:
                for kind, v, ch in n.resources():
                    res[kind].add(v)
        if transitive:
            res['TEX'] |= self.fep.textures_of(res['MDL'], res['ANM'])
        return {k: sorted(v) for k, v in res.items()}

    def __repr__(self):
        return '<Effect #%d %#010x %s>' % (self.index, self.id, self.name)


# ====================================================================== FEP
class Fep:
    """Header 0x48: 'FEP\\xcd', u16 1, u16 3 (checked against the engine), ptr channel table
    {u32 162, ptr}, u32 runtime flags (0; relocation state bits), u32 package id base, u32 file size,
    u16 3 (heap/pool id; FUN_08943124 masks it with 0xF), u16 0, u16 n_effects, u16 n_bodies,
    u16 n_tex, u16 n_mdl, u16 n_anm, u16 n_vtl, ptr effects[], ptr bodies[], ptr tex[], ptr mdl[],
    ptr anm[], ptr vtl[], ptr x40 {u16 n, u16, ptr}, ptr names {u32 n, ptr}."""

    def __init__(self, blocks):
        self.blocks = blocks

    # ---------------------------------------------------------------- io
    @classmethod
    def from_bytes(cls, d: bytes) -> 'Fep':
        d = bytes(d)
        if d[:4] != b'FEP\xcd':
            raise ValueError('not a FEP file')
        slots = pointer_slots(d)
        kinds = {}
        for s, (v, k) in slots.items():
            kinds.setdefault(v, set()).add(k)
        starts = sorted(set(kinds) | {0})
        blocks = []
        for i, t in enumerate(starts):
            end = starts[i + 1] if i + 1 < len(starts) else len(d)
            ks = kinds.get(t, {'header'} if t == 0 else set())
            blocks.append(Block(d[t:end], ks, _align_for(ks), t))
        by_pos = {b.pos: b for b in blocks}
        for s, (v, k) in slots.items():
            b = blocks[bisect.bisect_right(starts, s) - 1]
            b.ptrs[s - b.pos] = by_pos[v]
        return cls(blocks)

    @classmethod
    def load(cls, path):
        with open(path, 'rb') as f:
            return cls.from_bytes(f.read())

    def to_bytes(self) -> bytes:
        out = bytearray(); pos = {}
        for b in self.blocks:
            _pad_to(out, b.align)
            pos[id(b)] = len(out)
            out += b.data
        _pad_to(out, 16)
        for b in self.blocks:
            base = pos[id(b)]
            for rel, t in b.ptrs.items():
                U32.pack_into(out, base + rel, pos[id(t)])
            # ParamSet tables are addressed implicitly (entry offsets are relative to the ParamSet)
            if b.kinds & {'node.p32', 'node.p36.it'} and 8 in b.ptrs and pos[id(b.ptrs[8])] != base + 12:
                raise ValueError('ParamSet table must follow its header directly')
        U32.pack_into(out, 0x14, len(out))
        return bytes(out)

    def save(self, path):
        with open(path, 'wb') as f:
            f.write(self.to_bytes())

    # ---------------------------------------------------------------- header
    @property
    def header(self):
        return self.blocks[0]

    def _h16(self, o): return u16(self.header.data, o)
    def _set_h16(self, o, v): U16.pack_into(self.header.data, o, v)

    version = property(lambda s: (u16(s.header.data, 4), u16(s.header.data, 6)))
    id_base = property(lambda s: u32(s.header.data, 0x10))
    heap = property(lambda s: u16(s.header.data, 0x18))
    n_effects = property(lambda s: s._h16(0x1C))
    n_bodies = property(lambda s: s._h16(0x1E))

    def count(self, kind):
        return self._h16(0x20 + 2 * KINDS.index(kind))

    def local_id(self, n):
        """Local resource id = 0x98000000 + package << 10 + n, n < 0x400 (by convention TEX 0x00-,
        ANM 0x80-, MDL 0x100-, VTL 0x180-, but gst130's MDL reach 0x182). The package part equals
        ((id_base >> 16) & 0x3FF) << 8 in every file seen."""
        if not 0 <= n < 0x400:
            raise ValueError('local resource number out of range')
        return 0x98000000 + ((((self.id_base >> 16) & 0x3FF) << 8) & ~0x3FF) + n

    # ---------------------------------------------------------------- tables
    def _table(self, hoff, n):
        t = self.header.ptrs.get(hoff)
        return [t.ptrs[4 * i] for i in range(n)] if t else []

    def channel_table(self):
        p = self.header.ptrs[0x08]; arr = p.ptrs[4]
        return [struct.unpack_from('<BBHHH', arr.data, 8 * i) for i in range(u32(p.data, 0))]

    def param_size(self, chan):
        return u16(self.header.ptrs[0x08].ptrs[4].data, 8 * chan + 4)

    def effects(self):
        return [Effect(self, i, b) for i, b in enumerate(self._table(0x28, self.n_effects))]

    def effect(self, i):
        return Effect(self, i, self._table(0x28, self.n_effects)[i])

    def body(self, i):
        return Body(self, i, self._table(0x2C, self.n_bodies)[i])

    def refs(self, kind):
        k = KINDS.index(kind)
        return [ResRef(self, kind, i, b) for i, b in enumerate(self._table(0x30 + 4 * k, self.count(kind)))]

    def names(self):
        """[(effect id, name)] in name-table order (the index FEPA +0x28 and FXSE use)."""
        ns = self.header.ptrs[0x44]; arr = ns.ptrs[4]; out = []
        for i in range(u32(ns.data, 0)):
            eid, ln = struct.unpack_from('<II', arr.data, 12 * i)
            out.append((eid, bytes(arr.ptrs[12 * i + 8].data[:ln]).decode('latin1')))
        return out

    def name_index(self, eid):
        for i, (n_id, _) in enumerate(self.names()):
            if n_id == eid:
                return i
        return None

    def textures_of(self, mdl_idx=(), anm_idx=()):
        tex_by_id = {r.id: r.index for r in self.refs('TEX')}
        out = set(); mdl = self.refs('MDL'); anm = self.refs('ANM')
        for i in mdl_idx:
            m = mdl[i].decode()
            if m:
                out |= {tex_by_id[x['tex_id']] for x in m.materials if x['tex_id'] in tex_by_id}
        for i in anm_idx:
            a = anm[i].decode()
            if a:
                out |= {tex_by_id[t] for t in a.tex_ids if t in tex_by_id}
        return out

    # ---------------------------------------------------------------- lookups (mirror the engine)
    def lookup(self, eid):
        """Body index the engine plays for effect id `eid` (FUN_089444B0): the first t0 entry whose
        group (bits 10-16) matches, then its leaf whose id & 0x1FF matches. None = not found."""
        if eid & 0x200:
            return None                                # x40 table path (unused by characters)
        grp, leaf = (eid & 0x1FC00) >> 10, eid & 0x1FF
        for e in self.effects():
            if (e.id & 0x1FC00) >> 10 == grp:
                for lf in e.leaves:
                    if u32(lf.data, 0) & 0x1FF == leaf:
                        return e.body_index
                return None
        return None

    def effect_index_for_name(self, ni):
        eid = self.names()[ni][0]
        return next((e.index for e in self.effects() if e.id == eid), None)

    # ---------------------------------------------------------------- editing helpers
    def _insert(self, pred, new_blocks, before=False):
        idx = [i for i, b in enumerate(self.blocks) if pred(b)]
        at = (idx[0] if before else idx[-1] + 1) if idx else len(self.blocks)
        self.blocks[at:at] = new_blocks

    def _grow_table(self, hoff, new_block):
        t = self.header.ptrs[hoff]
        n = len(t.ptrs)
        t.data = bytearray(t.data[:4 * n]) + b'\0\0\0\0'
        t.ptrs[4 * n] = new_block
        return n

    def effect_blocks(self, index):
        """Blocks owned by one effect (t0 entry, leaves, body and its whole graph), in file order."""
        e = self.effect(index)
        body = self._table(0x2C, self.n_bodies)[e.body_index]
        seen = []; memo = set()

        def add(b):
            if b is not None and id(b) not in memo:
                memo.add(id(b)); seen.append(b)
            return b
        add(e.block); la = add(e.block.ptrs[8])
        for j in range(e.n_leaves):
            lf = add(la.ptrs[4 * j]); add(lf.ptrs.get(12))
        add(body)
        n_sp, n_nd, n_ch, n_40, n_44, n_48 = struct.unpack_from('<6H', body.data, 16)
        if n_sp:
            a = add(body.ptrs[28])
            for j in range(n_sp):
                it = add(a.ptrs[4 * j])
                if u16(it.data, 2):
                    add(it.ptrs[4])
        if n_nd:
            a = add(body.ptrs[32])
            for j in range(n_nd):
                nd = add(a.ptrs.get(4 * j))
                if nd is None or not nd.data[8]:
                    continue
                t = add(nd.ptrs[28])
                for k in range(nd.data[8]):
                    it = add(t.ptrs[4 * k])
                    if u32(it.data, 4):
                        add(it.ptrs[8])
                if nd.data[18]:
                    p = add(nd.ptrs[32])
                    if u32(p.data, 4):
                        add(p.ptrs[8])
                if nd.data[19]:
                    s = add(nd.ptrs[36])
                    for k in range(nd.data[19]):
                        p = add(s.ptrs[4 * k])
                        if u32(p.data, 4):
                            add(p.ptrs[8])
                for k in range(8):
                    if nd.data[20 + k]:
                        add(nd.ptrs[40 + 4 * k])
        if n_ch:
            add(body.ptrs[36])
        a = add(body.ptrs[40])
        for j in range(n_40):
            it = add(a.ptrs[4 * j]); add(it.ptrs[4])
        if n_44:
            a = add(body.ptrs[44])
            for j in range(n_44):
                it = add(a.ptrs[4 * j])
                if u16(it.data, 0):
                    add(it.ptrs[4])
        if n_48:
            a = add(body.ptrs[48])
            for j in range(n_48):
                add(a.ptrs[4 * j])
        order = {id(b): i for i, b in enumerate(self.blocks)}
        return sorted(seen, key=lambda b: order.get(id(b), 1 << 30))

    def free_group(self):
        used = {e.group for e in self.effects()}
        return next(g for g in range(128) if g not in used)

    def clone_effect(self, src, name=None, group=None):
        """Deep-copy effect `src` into a new t0 entry + t1 body + name. Resources stay shared (use
        clone_resource + relink to give the copy its own). Returns the new effect index; because the
        name is appended too, it is also the new name-table index (the value FEPA +0x28 needs) as
        long as names and effects are kept in the same order (true for all shipped files)."""
        e = self.effect(src)
        owned = self.effect_blocks(src)
        copies = {id(b): b.clone() for b in owned}
        for c in copies.values():
            c.ptrs = {r: copies.get(id(t), t) for r, t in c.ptrs.items()}
        group = self.free_group() if group is None else group
        if not 0 <= group < 128 or group in {x.group for x in self.effects()}:
            raise ValueError('effect group %r is not free' % group)
        new_id = (self.id_base & 0xFFFC0000) | (group << 10)
        ne = copies[id(e.block)]
        U32.pack_into(ne.data, 0, new_id)
        U16.pack_into(ne.data, 4, self.n_bodies)
        for b in owned:
            if 't0.leaf' in b.kinds:
                c = copies[id(b)]
                U32.pack_into(c.data, 0, new_id | (u32(c.data, 0) & 0x3FF))
        # body+44 pairs (spawn frame, node serial): serials are unique file-wide in every shipped
        # file, so give the copy fresh numbers
        top = max([s for i in range(self.n_bodies) for _, s in self.body(i).serials()] or [-1]) + 1
        cb = copies[id(self._table(0x2C, self.n_bodies)[e.body_index])]
        for j in range(u16(cb.data, 24)):
            it = cb.ptrs[44].ptrs[4 * j]
            lst = it.ptrs.get(4)
            for k in range(u16(it.data, 0) if lst is not None else 0):
                U16.pack_into(lst.data, 4 * k + 2, top); top += 1
        leafs = [b for b in owned if b.kinds & {'t0.leafarr', 't0.leaf', 'leaf.p12'}]
        rest = [b for b in owned if b is not e.block and b not in leafs]
        self._insert(lambda b: 't0.ent' in b.kinds, [ne])
        self._insert(lambda b: b.kinds & {'t0.leafarr', 't0.leaf'}, [copies[id(b)] for b in leafs])
        self._insert(lambda b: any(k.startswith('res') and k.endswith('.data') for k in b.kinds),
                     [copies[id(b)] for b in rest], before=True)
        self._grow_table(0x28, ne)
        self._grow_table(0x2C, copies[id(self._table(0x2C, self.n_bodies)[e.body_index])])
        self._set_h16(0x1C, self.n_effects + 1)
        self._set_h16(0x1E, self.n_bodies + 1)
        name = name or (e.name or 'EFFECT') + '_COPY'
        sb = Block(_pad_to(bytearray(name.encode('latin1') + b'\0'), 4), {'name.str'}, 4)
        self._insert(lambda b: 'name.str' in b.kinds, [sb])
        ns = self.header.ptrs[0x44]; arr = ns.ptrs[4]
        n = u32(ns.data, 0)
        arr.data = bytearray(arr.data[:12 * n]) + struct.pack('<II', new_id, len(name)) + b'\0\0\0\0'
        arr.ptrs[12 * n + 8] = sb
        U32.pack_into(ns.data, 0, n + 1)
        return self.n_effects - 1

    def rebase(self, new_base):
        """Give the package a new identity (for a new character built from a donor's FEP).
        FUN_0894349C treats packages with the same (id_base & 0xFFFC0000) as one already-loaded
        package, and resources are registered per kind by id, so a donor copy must not keep the
        donor's ids. Rewrites id_base, every effect / leaf / name id and every *local* resource id
        (refs, block headers, MDL material and ANM texture references). System ids (0xF0xxxxxx) stay."""
        if new_base & 0x3FFFF:
            raise ValueError('id base must be a multiple of 0x40000')
        old_eff = self.id_base & 0xFFFC0000
        old_loc = self.local_id(0); U32.pack_into(self.header.data, 0x10, new_base)
        new_loc = self.local_id(0)

        def eff(v): return (v - old_eff + new_base) if (v & 0xFFFC0000) == old_eff else v

        def loc(v): return (v - old_loc + new_loc) if (v & ~0x3FF) == old_loc else v
        for e in self.effects():
            U32.pack_into(e.block.data, 0, eff(e.id))
            for lf in e.leaves:
                U32.pack_into(lf.data, 0, eff(u32(lf.data, 0)))
        ns = self.header.ptrs[0x44]; arr = ns.ptrs[4]
        for i in range(u32(ns.data, 0)):
            U32.pack_into(arr.data, 12 * i, eff(u32(arr.data, 12 * i)))
        for kind in KINDS:
            for r in self.refs(kind):
                U32.pack_into(r.block.data, 4, loc(r.id))
                if r.system:
                    continue
                blk = r.data_block
                io = 8 if kind == 'MDL' else 4
                U32.pack_into(blk.data, io, loc(u32(blk.data, io)))
                if kind == 'MDL':
                    m = Mdl(bytes(blk.data[:r.size]))
                    for i in range(len(m.materials)):
                        o = m.mat_off + 24 * i
                        U32.pack_into(blk.data, o, loc(u32(blk.data, o)))
                elif kind == 'ANM':
                    a = Anm(bytes(blk.data[:r.size]))
                    for i in range(len(a.tex_ids)):
                        o = a.offs[0] + 4 * i
                        U32.pack_into(blk.data, o, loc(u32(blk.data, o)))

    def set_res_bytes(self, kind, index, raw):
        """Replace a local resource's bytes (size may change; the ref's size field follows)."""
        r = self.refs(kind)[index]
        if r.system:
            raise ValueError('system resource has no local data')
        blk = r.data_block
        tail = blk.data[r.size:] if len(raw) == r.size else b''
        blk.data = bytearray(raw) + tail
        U32.pack_into(r.block.data, 8, len(raw))

    def clone_resource(self, kind, index, tex_map=None):
        """Copy a local resource under a new local id; returns the new table index.
        tex_map {old TEX id: new TEX id} rewrites MDL materials / ANM texture lists of the copy."""
        k = KINDS.index(kind)
        src = self.refs(kind)[index]
        if src.system:
            raise ValueError('cannot clone a system resource')
        raw = bytearray(src.raw)
        # ids are registered per kind with the engine's resource managers (FUN_08947004 and siblings),
        # so they must be unique within the kind and stay inside this package's 0x400 id window
        lo = (0x00, 0x100, 0x80, 0x180)[k]
        used = {r.id & 0x3FF for r in self.refs(kind) if r.id >> 24 == 0x98}
        n = max(used | {lo - 1}) + 1
        if n >= 0x400:
            n = next(x for x in range(0x400) if x not in used)
        new_id = self.local_id(n)
        U32.pack_into(raw, 8 if kind == 'MDL' else 4, new_id)
        if tex_map:
            if kind == 'MDL':
                m = Mdl(raw)
                for i, mat in enumerate(m.materials):
                    if mat['tex_id'] in tex_map:
                        U32.pack_into(raw, m.mat_off + 24 * i, tex_map[mat['tex_id']])
            elif kind == 'ANM':
                a = Anm(raw)
                for i, t in enumerate(a.tex_ids):
                    if t in tex_map:
                        U32.pack_into(raw, a.offs[0] + 4 * i, tex_map[t])
        size = len(raw)
        data = Block(_pad_to(raw, 16), {'res%d.data' % k}, 16)
        ref = Block(src.block.data[:20], {'res%d.ref' % k}, 4)
        U32.pack_into(ref.data, 4, new_id); U32.pack_into(ref.data, 8, size); I32.pack_into(ref.data, 16, -1)
        ref.ptrs = {12: data}
        refs = self.refs(kind)
        self.blocks.insert(self.blocks.index(refs[-1].block) + 1, ref)
        last = max((r.data_block for r in refs if r.size), key=self.blocks.index)
        self.blocks.insert(self.blocks.index(last) + 1, data)
        ni = self._grow_table(0x30 + 4 * k, ref)
        self._set_h16(0x20 + 2 * k, ni + 1)
        return ni

    def relink(self, effect_index, kind, old, new):
        """Point every LINKS field of one effect from resource index `old` to `new`; returns #fields."""
        nchg = 0
        for n in self.effect(effect_index).body.nodes:
            if not n:
                continue
            for ps in n.param_sets():
                for ti, ch, off, ab, (blk, rel, size) in ps.entries():
                    for kd, fo, fmt in LINKS.get(ch, ()):
                        if kd == kind and fo + struct.calcsize(fmt) <= size and \
                                struct.unpack_from(fmt, blk.data, rel + fo)[0] == old:
                            struct.pack_into(fmt, blk.data, rel + fo, new); nchg += 1
        return nchg

    def colours(self, effect_index):
        """[(node, set (-1 initial / k segment), channel, block, rel, n_vec4)] of colour channels."""
        out = []
        for n in self.effect(effect_index).body.nodes:
            if not n:
                continue
            for si, ps in enumerate(n.param_sets()):
                for ti, ch, off, ab, (blk, rel, size) in ps.entries():
                    if ch in COLOUR_CHANNELS:
                        out.append((n.index, si - (1 if n.init else 0), ch, blk, rel, size // 16))
        return out

    def tint_effect(self, effect_index, rgb=(1.0, 1.0, 1.0), alpha=1.0):
        """Multiply R, G, B (and A) of every vec4 in the effect's colour channels (value, rate and
        acceleration alike, so curves keep their shape). Only affects this effect's own blocks."""
        nchg = 0
        for node, si, ch, blk, rel, nv in self.colours(effect_index):
            for v in range(nv):
                o = rel + 16 * v
                r, g, b, a = struct.unpack_from('<4f', blk.data, o)
                struct.pack_into('<4f', blk.data, o, r * rgb[0], g * rgb[1], b * rgb[2], a * alpha); nchg += 1
        return nchg


# ====================================================================== FEPA / FXSE
class _RecordFile:
    MAGIC = b''
    REC = 0

    def __init__(self, version, records):
        self.version, self.records = version, [bytearray(r) for r in records]

    @classmethod
    def from_bytes(cls, d):
        d = bytes(d)
        if d[:4] != cls.MAGIC:
            raise ValueError('not a %r file' % cls.MAGIC)
        ver, size, n = struct.unpack_from('<3I', d, 4)
        if size != len(d) or 16 + n * cls.REC != len(d):
            raise ValueError('unexpected %r size' % cls.MAGIC)
        return cls(ver, [d[16 + cls.REC * i:16 + cls.REC * (i + 1)] for i in range(n)])

    @classmethod
    def load(cls, path):
        with open(path, 'rb') as f:
            return cls.from_bytes(f.read())

    def to_bytes(self):
        body = b''.join(bytes(r) for r in self.records)
        return self.MAGIC + struct.pack('<3I', self.version, 16 + len(body), len(self.records)) + body


class Fepa(_RecordFile):
    """'FEPA' u32 0x20100205, u32 size, u32 n, n x 260 B "effect placement" records:
    f32 pos[3], f32 rot[3] (deg), f32 scale[3], u32 id (+0x24; event 0x0219 arg0 and the other
    callers of FUN_088AA5B8; 9999/8888/9000 = disabled), u32 effect (+0x28; FEP name-table index,
    also the FXSE index), u32 (+0x30), u32 (+0x38), u32 (+0x3C flags), ... u32 attach mode (+0x7C:
    0x80/0x100-0x103/0x200), u32 (+0x80), char model[32] (+0x88), char bone[32] (+0xA8),
    char motion[32] (+0xC8), f32 (+0xEC), f32 (+0xF0), f32 (+0xF4), u32 (+0xF8)."""
    MAGIC = b'FEPA'
    REC = 260

    def rec_id(self, i): return u32(self.records[i], 0x24)
    def rec_effect(self, i): return u32(self.records[i], 0x28)

    def info(self, i):
        r = self.records[i]
        return {'id': u32(r, 0x24), 'effect': u32(r, 0x28), 'pos': struct.unpack_from('<3f', r, 0),
                'rot': struct.unpack_from('<3f', r, 12), 'scale': struct.unpack_from('<3f', r, 24),
                'attach': u32(r, 0x7C), 'model': _cstr(r[0x88:0xA8]), 'bone': _cstr(r[0xA8:0xC8]),
                'motion': _cstr(r[0xC8:0xE8]), 'f4': f32(r, 0xF4)}

    def records_for_id(self, rid):
        return [i for i in range(len(self.records)) if self.rec_id(i) == rid]

    def free_id(self, start=500):
        used = {self.rec_id(i) for i in range(len(self.records))}
        i = start
        while i in used:
            i += 1
        return i

    def add(self, copy_from, rid=None, effect=None):
        """Append a copy of record `copy_from` (new id unless given); returns its record index."""
        r = bytearray(self.records[copy_from])
        U32.pack_into(r, 0x24, self.free_id() if rid is None else rid)
        if effect is not None:
            U32.pack_into(r, 0x28, effect)
        self.records.append(r)
        return len(self.records) - 1


class Fxse(_RecordFile):
    """'FXSE' u32 0x20070422, u32 size, u32 n (= FEP effect count), n x 320 B, indexed like the FEP
    name table: 16 x 20 B {u32 bank? (0x1B7740 / 0x14689C0 / 0), u32 cue?, u32 ?, u32 ?, u32 ?}
    (sound triggers; fields inferred). Grow it together with the FEP name table."""
    MAGIC = b'FXSE'
    REC = 320

    def cues(self, i):
        r = self.records[i]
        return [c for c in (struct.unpack_from('<5I', r, 20 * k) for k in range(16)) if any(c)]

    def add(self, copy_from=None):
        self.records.append(bytearray(self.records[copy_from]) if copy_from is not None else bytearray(self.REC))
        return len(self.records) - 1


# ====================================================================== MPK (the objx 'effe' entry)
class Mpk:
    """'MPK ' 06 06 07 20, u32 n, u32 0, n x {u32 id, u32 off, u32 size, u32 0}, names (\\0-separated),
    data 16-aligned with zero padding. 'effe': 0x40 *.fep, 0x4D *.feb (FEPA), 0x5A *.fxseb (FXSE)."""

    def __init__(self, ver, entries, names_blob):
        self.ver, self.entries, self.names_blob = ver, entries, names_blob   # entries: [[id, bytes]]

    @classmethod
    def from_bytes(cls, d):
        d = bytes(d)
        if d[:4] != b'MPK ':
            raise ValueError('not an MPK')
        n = u32(d, 8)
        ents = [struct.unpack_from('<4I', d, 16 + 16 * i) for i in range(n)]
        first = min([e[1] for e in ents if e[2]] or [len(d)])
        m = cls(d[4:8], [[e[0], d[e[1]:e[1] + e[2]]] for e in ents], d[16 + 16 * n:first])
        m.tail = d[max([e[1] + e[2] for e in ents if e[2]] or [first]):]
        return m

    def names(self):
        return [x.decode('latin1') for x in self.names_blob.split(b'\0') if x]

    def get(self, eid):
        return next((b for i, b in self.entries if i == eid), None)

    def set(self, eid, data):
        for e in self.entries:
            if e[0] == eid:
                e[1] = bytes(data); return
        raise KeyError(eid)

    def to_bytes(self):
        n = len(self.entries)
        head = 16 + 16 * n + len(self.names_blob)
        table = b''; body = bytearray(); o = head
        for eid, data in self.entries:
            if not data:                                 # empty slot (e.g. arrow-effe has no FXSE)
                table += struct.pack('<4I', eid, 0, 0, 0); continue
            pad = (-o) % 16
            body += b'\0' * pad; o += pad
            table += struct.pack('<4I', eid, o, len(data), 0)
            body += data; o += len(data)
        return b'MPK ' + self.ver + struct.pack('<II', n, 0) + table + self.names_blob + bytes(body) + getattr(self, 'tail', b'')


class EffectPackage:
    """The objx 'effe' entry: MPK with FEP + FEPA + FXSE."""

    def __init__(self, mpk):
        self.mpk = mpk
        self.fep = Fep.from_bytes(mpk.get(0x40))
        self.fepa = Fepa.from_bytes(mpk.get(0x4D))
        self.fxse = Fxse.from_bytes(mpk.get(0x5A))

    @classmethod
    def from_bytes(cls, d):
        return cls(Mpk.from_bytes(d))

    def to_bytes(self):
        self.mpk.set(0x40, self.fep.to_bytes())
        self.mpk.set(0x4D, self.fepa.to_bytes())
        self.mpk.set(0x5A, self.fxse.to_bytes())
        return self.mpk.to_bytes()

    def play_0219(self, arg0):
        return play_0219(self.fep, self.fepa, arg0)

    def clone_effect(self, src_effect, name=None, fepa_template=None):
        """Clone effect + FXSE row + a FEPA record. Returns (effect index, FEPA id for event 0x0219)."""
        src_ni = self.fep.effect(src_effect).name_index
        ni = self.fep.clone_effect(src_effect, name)
        self.fxse.add(src_ni)
        if fepa_template is None:
            cands = [i for i in range(len(self.fepa.records)) if self.fepa.rec_effect(i) == src_ni]
            fepa_template = cands[0] if cands else 0
        ri = self.fepa.add(fepa_template, effect=ni)
        return ni, self.fepa.rec_id(ri)


# ====================================================================== misc
def png_rgba(w, h, rgba: bytes) -> bytes:
    """Minimal PNG writer (stdlib only)."""
    def chunk(t, b):
        return struct.pack('>I', len(b)) + t + b + struct.pack('>I', zlib.crc32(t + b) & 0xFFFFFFFF)
    raw = b''.join(b'\0' + rgba[y * w * 4:(y + 1) * w * 4] for y in range(h))
    return (b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 6, 0, 0, 0)) +
            chunk(b'IDAT', zlib.compress(raw, 9)) + chunk(b'IEND', b''))


def play_0219(fep: Fep, fepa: Fepa, arg0: int):
    """[(effect index, name, FEPA record)] that event 0x0219 with this arg0 starts, resolved like the
    engine (FUN_088A9DE8 -> FUN_088AA5B8 (match rec+0x24) -> FUN_088A4D7C -> FUN_08944694
    (names[rec+0x28].id) -> FUN_089444B0 (group/leaf lookup))."""
    out = []
    names = fep.names()
    for ri in fepa.records_for_id(arg0)[:1]:        # FUN_088A9DE8 asks for 1 match: first record wins
        ni = fepa.rec_effect(ri)
        if ni < len(names):
            bi = fep.lookup(names[ni][0])
            idx = next((e.index for e in fep.effects() if e.body_index == bi), None) if bi is not None else None
            out.append((idx, names[ni][1], ri))
    return out
