#!/usr/bin/env python3
"""Re-encode the body part of the retail Bartz GMO with N weights per vertex (N = 4, 6, 8, ...) and
BlendSubsets of N bones, keeping the geometry and the skinning mathematically identical to retail.
A test vehicle for "does the runtime / the game handle N-weight models?" (answer: yes, see the
"Model files" paragraph of games/dissidia/README.md, "Modding: loose files"); not needed for normal modding.

usage: synth_weight_variants.py DFX_PARENT_DIR RETAIL.gmo OUT.gmo N [--merge] [--bad-headers]
  DFX_PARENT_DIR  directory that contains the dfx package (the Dissidia Forge library; its code is run)
  --merge         merge neighbouring meshes (same material) while the union of their bones fits in N
  --bad-headers   keep the Arrays chunk header words the dfx writer produces (W0 = W1 = header size);
                  without it they are written like retail (W0 = chunk size, W1 = header size + 16)
Meshes with fewer than N bones are padded with other (distinct) bones of the owner's BlendBones list at
weight 0, so every subset really has N distinct bones. Run with `python3 -I`.
"""
import struct, sys


def build(dfx_parent, src, dst, N, merge, retail_headers=True):
    sys.path.insert(0, dfx_parent)
    import numpy as np
    from dfx.gmo import Character, VertexFormat, VertexArray, DrawCall, ref_index, make_ref
    from dfx.chunks import Chunk, MESH, ARRAYS, MATERIAL, PART

    c = Character.load(src)
    part = c.parts[0]
    owner = c.bones[c.part_owner(0)]
    nbb = len(owner.blend_bones)
    items = []
    for m in part.meshes:
        d = m.draws[0]
        arr = part.arrays[ref_index(d.array_ref)]
        tris = d.triangles()
        used = sorted({i for t in tris for i in t})
        remap = {o: n for n, o in enumerate(used)}
        buf = np.frombuffer(arr.raw, np.uint8).reshape(arr.count, arr.fmt.stride)[used]
        off, _ = arr.fmt.layout()
        assert arr.fmt.tc == 2 and arr.fmt.color == 6 and arr.fmt.pos == 2 and arr.fmt.normal == 0
        items.append(dict(mat=m.material, subset=list(m.blend_subset), w=buf[:, :arr.fmt.nweights].copy(),
                          rest=buf[:, off['t']:].copy(), tris=[tuple(remap[i] for i in t) for t in tris]))
    groups = []
    for it in items:
        for g in (groups if merge else []):
            if g['mat'] == it['mat'] and len(set(g['bones']) | set(it['subset'])) <= N:
                g['items'].append(it)
                g['bones'] += [b for b in it['subset'] if b not in g['bones']]
                break
        else:
            groups.append(dict(mat=it['mat'], bones=list(it['subset']), items=[it]))
    fmt = VertexFormat.build(N, tc=2, color=6, pos=2)
    off, st = fmt.layout()
    meshes, arrays = [], []
    for gi, g in enumerate(groups):
        bones = list(g['bones'])
        for b in range(nbb):
            if len(bones) >= N:
                break
            if b not in bones:
                bones.append(b)
        assert len(bones) == N, 'owner has too few blend bones to pad a subset to %d' % N
        n = sum(len(it['w']) for it in g['items'])
        buf = np.zeros((n, st), np.uint8)
        idx, base = [], 0
        for it in g['items']:
            k = len(it['w'])
            for j, b in enumerate(it['subset']):
                buf[base:base + k, bones.index(b)] += it['w'][:, j]
            buf[base:base + k, off['t']:off['t'] + it['rest'].shape[1]] = it['rest']
            for t in it['tris']:
                idx += [base + i for i in t]
            base += k
        ach = VertexArray('arrays-%d-0000' % gi, fmt, n, buf.tobytes()).to_chunk()
        if retail_headers:
            ach.header = struct.pack('<II', 8 + len(ach.header) + len(ach.payload), 8 + len(ach.header) + 16) + ach.header[8:]
        arrays.append(ach)
        mc = Chunk(MESH)
        mc.name = '16mesh-0-%04d' % gi
        mc.children = [Chunk(0x8061, payload=struct.pack('<I', make_ref(MATERIAL, g['mat'], scope=2))),
                       Chunk(0x8062, payload=struct.pack('<I%dI' % N, N, *bones)),
                       Chunk(0x8066, payload=DrawCall(make_ref(ARRAYS, gi), 3, len(idx), 1, idx).to_payload())]
        meshes.append(mc)
    c.mesh_model.find(PART)[0].children = meshes + arrays
    c.gmo.save(dst)
    print('%s: N=%d, %d meshes (retail %d), stride %d, retail-style headers: %s' % (dst, N, len(meshes), len(part.meshes), st, retail_headers))


if __name__ == '__main__':
    import os
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    import pydeps                    # numpy at the pinned version (tools/pydeps.py)
    pydeps.require()
    if len(sys.argv) < 5:
        print(__doc__)
        sys.exit(2)
    build(sys.argv[1], sys.argv[2], sys.argv[3], int(sys.argv[4]), '--merge' in sys.argv, '--bad-headers' not in sys.argv)
