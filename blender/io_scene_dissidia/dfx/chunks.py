"""Lossless chunk tree for PSP GMO files (as used by Dissidia 012).

Every GMO chunk is: u16 type, u16 header_size, u32 total_size, then
  - "block" (type < 0x8000): header bytes (name etc.) up to header_size,
    followed by child chunks (or raw payload for leaf blocks);
  - "command" (type >= 0x8000): header_size is 0; args fill the rest.

Parsing then serialising an unmodified tree reproduces the file byte for byte.
"""
from __future__ import annotations
import struct
from dataclasses import dataclass, field

MAGIC = b'OMG.00.1PSP\x00\x00\x00\x00\x00'

# Block types (GMS names)
FILE, MODEL, BONE, PART, MESH, ARRAYS, MATERIAL, LAYER, TEXTURE, MOTION, FCURVE, BLIND = \
    0x0002, 0x0003, 0x0004, 0x0005, 0x0006, 0x0007, 0x0008, 0x0009, 0x000A, 0x000B, 0x000C, 0x000F
# Blocks whose body is raw data, not child chunks
LEAF_BLOCKS = {ARRAYS, FCURVE}

NAMES = {
    0x0002: 'File', 0x0003: 'Model', 0x0004: 'Bone', 0x0005: 'Part', 0x0006: 'Mesh',
    0x0007: 'Arrays', 0x0008: 'Material', 0x0009: 'Layer', 0x000A: 'Texture',
    0x000B: 'Motion', 0x000C: 'FCurve', 0x000F: 'BlindData',
    0x8012: 'FileName', 0x8013: 'FileImage', 0x8015: 'VertexOffset/TexOffset',
    0x8041: 'ParentBone', 0x8042: 'VisibilityBone', 0x8044: 'BlendBones', 0x8045: 'BlendOffsets',
    0x8048: 'Translate', 0x804B: 'RotateQ', 0x804D: 'Scale', 0x804E: 'DrawPart',
    0x8061: 'SetMaterial', 0x8062: 'BlendSubset', 0x8066: 'DrawArrays',
    0x8081: 'RenderState', 0x8082: 'Diffuse', 0x8083: 'Specular', 0x8085: 'Ambient?',
    0x8091: 'SetTexture', 0x80B1: 'FrameLoop', 0x80B2: 'FrameRate', 0x80B3: 'Animate',
    0x80B4: 'FrameRepeat', 0x80F1: 'BlindData',
}


@dataclass
class Chunk:
    type: int
    header: bytes = b''          # bytes 8..header_size (blocks only)
    children: list = field(default_factory=list)
    payload: bytes = b''         # args (commands) or raw body (leaf blocks)

    # ---- convenience -------------------------------------------------
    @property
    def is_command(self) -> bool:
        return bool(self.type & 0x8000)

    @property
    def name(self) -> str:
        """Block name, stored after two u32s in the header."""
        if self.is_command or len(self.header) <= 8:
            return ''
        return self.header[8:].split(b'\0')[0].decode('latin1')

    @name.setter
    def name(self, value: str):
        raw = value.encode('latin1') + b'\0'
        raw += b'\0' * ((-len(raw)) % 4)
        hs = 8 + 8 + len(raw)
        self.header = struct.pack('<II', hs, hs) + raw

    def find(self, t):
        return [c for c in self.children if c.type == t]

    def first(self, t):
        for c in self.children:
            if c.type == t:
                return c
        return None

    def walk(self):
        yield self
        for c in self.children:
            yield from c.walk()

    @property
    def label(self):
        return NAMES.get(self.type, f'0x{self.type:04x}')

    def __repr__(self):
        n = f' "{self.name}"' if self.name else ''
        return f'<{self.label}{n} children={len(self.children)} payload={len(self.payload)}>'

    # ---- (de)serialisation --------------------------------------------
    def to_bytes(self) -> bytes:
        if self.is_command:
            return struct.pack('<HHI', self.type, 0, 8 + len(self.payload)) + self.payload
        body = self.payload if self.type in LEAF_BLOCKS else b''.join(c.to_bytes() for c in self.children)
        hs = 8 + len(self.header)
        return struct.pack('<HHI', self.type, hs, hs + len(body)) + self.header + body


def parse_chunks(data: bytes, start: int, end: int) -> list:
    out = []
    o = start
    while o + 8 <= end:
        t, hs, sz = struct.unpack_from('<HHI', data, o)
        if sz < 8 or o + sz > end:
            raise ValueError(f'bad chunk 0x{t:04x} at 0x{o:x} size {sz}')
        if t & 0x8000:
            out.append(Chunk(t, payload=data[o + 8:o + sz]))
        else:
            c = Chunk(t, header=data[o + 8:o + hs])
            if t in LEAF_BLOCKS:
                c.payload = data[o + hs:o + sz]
            else:
                c.children = parse_chunks(data, o + hs, o + sz)
            out.append(c)
        o += sz
    if o != end:
        raise ValueError(f'trailing bytes at 0x{o:x}..0x{end:x}')
    return out


class GmoFile:
    def __init__(self, root: Chunk, magic: bytes = MAGIC):
        self.root = root
        self.magic = magic

    @classmethod
    def from_bytes(cls, data: bytes) -> 'GmoFile':
        if data[:11] != MAGIC[:11]:
            raise ValueError('not a GMO file')
        roots = parse_chunks(data, 16, len(data))
        if len(roots) != 1 or roots[0].type != FILE:
            raise ValueError('expected a single File block')
        return cls(roots[0], data[:16])

    @classmethod
    def load(cls, path) -> 'GmoFile':
        with open(path, 'rb') as f:
            return cls.from_bytes(f.read())

    def to_bytes(self) -> bytes:
        return self.magic + self.root.to_bytes()

    def save(self, path):
        with open(path, 'wb') as f:
            f.write(self.to_bytes())

    @property
    def models(self):
        return self.root.find(MODEL)

    def dump(self, max_depth=99) -> str:
        lines = []

        def rec(c, dep):
            if dep > max_depth:
                return
            extra = c.payload[:24].hex() if c.is_command else ''
            lines.append('  ' * dep + f'{c.label} {c.name} {extra}'.rstrip())
            for ch in c.children:
                rec(ch, dep + 1)
        rec(self.root, 0)
        return '\n'.join(lines)
