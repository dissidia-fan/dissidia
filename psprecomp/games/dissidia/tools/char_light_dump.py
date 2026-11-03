"""Dump a character model's body for char_light_preview (games/dissidia/tools/char_light_preview.sh).

  char_light_dump.py <model.gmo> [<skin.skin>] <out_dir> [--no-normals] [--hd <skins/<skin>/hd>] [--face 0..1]

Writes out_dir/mesh.bin (per texture: id u64, w, h, RGBA, triangle count, then per corner: model-space position xyz,
normal xyz (as stored: the GE draws it through the same matrices), uv, rgba floats) and out_dir/ids.txt (the body's
texture-pack ids). The body gets normals the way the build adds them (charmod_light.add_normals) unless --no-normals.
"""
import os
import struct
import sys

import numpy as np

here = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, here)
sys.path.insert(0, os.path.join(here, '..', '..', '..', '..', 'blender', 'io_scene_dissidia'))
import charmod_chunks as C          # noqa: E402
import charmod_gim                  # noqa: E402
import charmod_gmo as G             # noqa: E402
import charmod_light as L           # noqa: E402
import texpack_id as TP             # noqa: E402
from dfx import gmo                 # noqa: E402


def main(argv):
    hd, face = None, None
    if '--face' in argv:                                # mod.json lighting.face (default charmod_light.DEFAULTS)
        k = argv.index('--face'); face = float(argv[k + 1]); argv = argv[:k] + argv[k + 2:]
    if '--hd' in argv:                                  # skins/<skin>/hd: the native build's texture-pack pictures
        k = argv.index('--hd'); hd = argv[k + 1]; argv = argv[:k] + argv[k + 2:]
    flags = [a for a in argv if a.startswith('--')]
    args = [a for a in argv if not a.startswith('--')]
    data = open(args[0], 'rb').read()
    if len(args) == 3:
        data = G.apply_skin(data, G.skin_from_bytes(open(args[1], 'rb').read()))
    out = args[-1]
    os.makedirs(out, exist_ok=True)
    if '--no-normals' not in flags:
        data = L.light_model(data, {'face': L.DEFAULTS['face'] if face is None else face})
    m = G.Model(data)
    r = G.model_ranges(m)
    pi = m.slots()['body']['part']
    part = m.parts[pi].to_bytes()
    p = C.parse_chunks(part, 0, len(part))[0]
    arrays = p.find(C.ARRAYS)
    po, ps = np.array(r['pos'][0]), np.array(r['pos'][1])
    uo, us = np.array(r['uv'][0]), np.array(r['uv'][1])
    by_tex = {}
    for mesh in p.find(C.MESH):
        mat = G._ref_index(G._u32(mesh.first(G.SET_MATERIAL)))
        tex = m.mat_textures(mat)[0]
        for dc in mesh.find(0x8066):
            d = gmo.DrawCall.from_payload(dc.payload)
            va = gmo.VertexArray.from_chunk(arrays[d.array_ref & 0xFFF])
            P = va.positions_norm() * ps + po
            N = va.normals() if va.fmt.normal else np.zeros_like(P)
            T = va.uvs_norm() * us + uo
            K = va.colors()
            K = np.ones((va.count, 4)) if K is None else K
            for t in d.triangles():
                for i in t:
                    by_tex.setdefault(tex, []).append(np.concatenate([P[i], N[i], T[i], K[i]]))
    with open(os.path.join(out, 'mesh.bin'), 'wb') as f, open(os.path.join(out, 'ids.txt'), 'w') as ids:
        f.write(struct.pack('<I', len(by_tex)))
        for tex, corners in by_tex.items():
            gim = m.texture_image(tex)
            rgba = charmod_gim.decode(gim)
            body_n = m.slots()['body']['textures'].index(tex) if tex in m.slots()['body']['textures'] else -1
            if hd and os.path.exists(os.path.join(hd, '%d.png' % body_n)):
                from PIL import Image
                rgba = np.asarray(Image.open(os.path.join(hd, '%d.png' % body_n)).convert('RGBA'))
            tid = TP.gim_id(gim[4:])[0]
            ids.write('%016x\n' % tid)
            h, w = rgba.shape[:2]
            f.write(struct.pack('<QII', tid, w, h) + np.ascontiguousarray(rgba, np.uint8).tobytes())
            a = np.array(corners, np.float32)
            f.write(struct.pack('<I', len(a) // 3) + a.tobytes())
    print('%s: %d textures, %d triangles, normals %s' % (out, len(by_tex), sum(len(c) for c in by_tex.values()) // 3,
                                                          'off' if '--no-normals' in flags else 'on'))


if __name__ == '__main__':
    main(sys.argv[1:])
