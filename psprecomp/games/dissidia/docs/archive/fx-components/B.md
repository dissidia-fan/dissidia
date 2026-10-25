# B: effect meshes (channel 11, channel 119, the MDL resource)

Agent B, 2026-10-24. Source: the static recompilation (`work/output/generated/batch_*.cpp`) and the
EBOOT image (`work/eboot_image.bin`, base 0x08804000), checked against all 254 FEPs on the disc
(every `obj/*.objx` `effe` section plus `effect/system/*.mpk`, `effect/region/*.mpk`).
Scratch scripts: `work/fx_re/scratch_B/` (`fc.py`/`at.py` print a recompiled function from an
address with register names; `scan.py`, `allfep.py`, `ex.py` scan the data).

Labels: **confirmed** = read end to end in the game code; **strong** = code read, but one link
depends on PSP GE hardware behaviour or on state set elsewhere; **guess** = inferred.

## Summary

- Channel 11 is the "draw one MDL" component. Its 26-byte block holds a **depth-bias offset**
  (+0..+8), the **MDL index** (+12), a bias scale (+16), a scissor flag (+21), and the
  **depth-bias enable** (+24) and mode (+25). The component keeps no state and does nothing per frame. Its only
  real method is draw (vtable 0x089DBA90, method 3 = 0x08955E94).
- An MDL is drawn **all groups, all prims, in file order** (no group selection). Each group sets
  its own model matrix `T(f3,f4,f5)·S(f0,f1,f2)`, its own **texture scale (f6,f7) and offset
  (f8,f9)**, and its front-face winding (+8). The studio currently uses f0..f2 only. It ignores the
  translation and the UV transform, so it shows the wrong part of the texture atlas.
- Vertex position = s8/128, UV = u8/128 (GE fixed point). World position = `W · (t + s ⊙ p/128)`.
- Each prim has its **own material** (prim +12 = material index, -1 = untextured). The material's
  +12 word is the **GE blend function** `{u8 op, u8 src, u8 dst}`. +16 and +20 are the src and dst
  FIX colours (that is the "-1/0" field), and +8 holds flags (bit 2 = no depth write, bit 3 = cull).
  Seen: `0x000A0600` additive (Cs·2As + Cd), `0x000A0602` **reverse-subtract**
  (Cd − Cs·2As), `0x00070600` **alpha blend** (Cs·2As + Cd·(1−2As)). With dstfix 0, `0x000A0600` writes
  Cs·2As over the destination. So "everything additive" is wrong for about 20 % of materials.
- Colour: the model is drawn with **GE lighting on**. The material ambient colour comes from the vertex colour
  (RGBA4444), and **both** the global ambient light and light 0's ambient are set to the node colour
  (channels 15–17, bytes, 128 = neutral). The texture function is MODULATE, and the effect pass enables
  **GU_FRAGMENT_2X** (RGB ×2). Net result for the typical vertex colour 0x8888 and node colour 128:
  texture × ~1.07, so the halves and doublings cancel. Alpha: node alpha ×2 (128 → 255) × vertex alpha × texel
  alpha, then the blend uses **2·alpha**.
- Global effect-pass state (FUN_08940CCC): depth test on (GEQUAL, PSP's inverted Z), depth writes
  off by default, blending on, alpha test **off**, fog off, culling off, lighting off (MDL turns it
  on), texture on, GU_FRAGMENT_2X on. Texture filter linear, wrap **repeat** in U and V (UV scale > 1 is used to tile).
- Channel 119 is a separate model-drawing component (58-byte block, its own blend selector), but
  **no FEP on the disc uses it** (0 of 254 files). The studio can ignore it.

## 1. The MDL resource

### 1.1 Header (0x1C bytes) — confirmed (reader FUN_08972194)
| off | type | meaning |
|---|---|---|
| 0x00 | char[4] | 'MDL\0' |
| 0x04 | u16, u16 | 1, 8 (version; not read by the draw) |
| 0x08 | u32 | resource id |
| 0x0C | u32 | n_mat (not read by the draw: materials are indexed from prims) |
| 0x10 | u32 | mat_off (block-relative; 0x1C) — read at 0x089722E8 |
| 0x14 | u32 | n_grp — loop bound at 0x089721E4 / 0x0897244C |
| 0x18 | u32 | grp_off — read at 0x08972204 |

### 1.2 Group (52 bytes) — confirmed (FUN_08972194, 0x08972204..0x089722B4, 0x08972430)
| off | type | meaning | code |
|---|---|---|---|
| +0 | u32 | n_prim | 0x089722B4 |
| +4 | u32 | prim_off (block-relative) | 0x089722CC |
| +8 | u32 | winding flag: `sceGuFrontFace(flag != 0 ? GU_CCW : GU_CW)` (0x089A0CF8 → 0x089A1E44, GE cmd 0x9B). It only matters when the material sets cull | 0x08972290 |
| +12..+20 | f32×3 | **scale** (sx,sy,sz): `sceGumScale` (0x089A26C8) | 0x08972214.. |
| +24..+32 | f32×3 | **translation** (tx,ty,tz): `sceGumTranslate` (0x089A2678) | 0x0897222C.. |
| +36, +40 | f32×2 | **texture scale** (su, sv): `sceGuTexScale` (0x089A001C, GE 0x48/0x49) | 0x08972268 |
| +44, +48 | f32×2 | **texture offset** (ou, ov): `sceGuTexOffset` (0x089A0F84, GE 0x4A/0x4B); also stored to texmgr+40/+44 | 0x08972274 |

Per group the code runs `sceGumMatrixMode(GU_MODEL)` (once, 0x089721D8), then `sceGumPushMatrix`
(0x089A29C4), `sceGumLoadIdentity` (0x089A250C), translate, scale, and `sceGumUpdateMatrix` (0x089A2568).
After the prims come `sceGumPopMatrix` (0x089A2A40) and an update. So **MODEL = T · S** (scale first, then
translate). At the end, TexScale(1,1) and TexOffset(0,0) are restored (0x08972464..0x08972488).

**All groups are drawn**, in order. No parameter selects a group (confirmed: the loop at
0x08972204/0x08972460 runs `g = 0..n_grp-1` unconditionally). Data: in all 1202 local MDLs the
first three floats are the half-extent of the s8 grid and the next three are its centre. For example,
Cloud's FIGA_BALL MDL 15 has `t = (0, 2.4057, 0)`. The uv floats map 0..1 onto an atlas cell (0.4844 = 62/128).

### 1.3 Prim (24 bytes) — confirmed (0x089722C8..0x08972414)
| off | type | meaning |
|---|---|---|
| +0 | u32 | GE primitive: 3 = GU_TRIANGLES, 4 = GU_TRIANGLE_STRIP (the value goes straight to `sceGuDrawArray`, 0x089A0520) |
| +4 | u32 | GE vertex type (straight to DrawArray) |
| +8 | u32 | vertex size (not read by the draw) |
| +12 | s32 | **material index** (0, 1; -1 = no material → `sceGuDisable(GU_TEXTURE_2D)` at 0x089723E0). fep.py calls it `z`; it is not padding |
| +16 | u32 | vertex count |
| +20 | u32 | vertex offset (block-relative) |

### 1.4 Vertices — strong (GE hardware rules; vertex types from data)
- `0x99` = u8 UV, RGBA4444 colour, s8 XYZ; 8 bytes: `u8 u, u8 v, u16 col, s8 x, s8 y, s8 z, pad`. 1488 prims.
- `0x98` = **no UV**, RGBA4444 colour, s8 XYZ; 6 bytes: `u16 col, s8 x,y,z, pad`. 67 prims. (FEP_NOTES says
  "0x98 = no colour". That is wrong: bits 0–1 = 0 means no texture coordinates. The generic layout code in
  fep.py and fx.js already decodes it correctly.) These prims still have a textured material.
  The GE then uses texcoord 0, so the texel is constant at (ou, ov) (guess).
- Normalisation (GE, not through-mode): s8 position / 128, u8 UV / 128 (128 = 1.0). Data agrees:
  positions are within ±127 and UVs within 0..128 in all 201 187 vertices. The studio divides positions by 127; use 128.
- Vertex colour RGBA4444: nibble n → n·17/255. Common values: 0x8888 (68 k vertices; nibble 8 ≈ 0.533 ≈ "1.0"
  once doubled), 0x0000 (50 k; black and transparent edges), 0x4444, 0x2222. Low nibble = R.

### 1.5 Material (24 bytes) — confirmed (0x089722E8..0x089723C0, texture bind 0x089C2AF0)
| off | type | meaning |
|---|---|---|
| +0 | u32 | texture id (local `0x98…` or resident `0xF0…`) |
| +4 | s32 | runtime texture handle (0 on disc; -1 = untextured → `sceGuDisable(9)`, 0x089C2B10). Resolved at load (strong) |
| +8 | u32 | flags. **bit 2 (0x4)**: no depth write (see 3.3). **bit 3 (0x8)**: `sceGuEnable(GU_CULL_FACE)`, else disable (0x08972384). Bits 0, 1 and 5 (0x1, 0x2, 0x20) are not read by this path (unknown) |
| +12 | u32 | **blend**: byte0 = op, byte1 = src factor, byte2 = dst factor → `sceGuBlendFunc(op, src, dst, srcfix, dstfix)` (0x08972340 → 0x089A12A4, GE 0xDF/0xE0/0xE1). Byte 3 = 0. The special value 0x00070600 also feeds the depth-write rule |
| +16 | u32 | src FIX colour (GE 0xE0, low 24 bits) |
| +20 | u32 | dst FIX colour (GE 0xE1, low 24 bits). 0xFFFFFFFF = white (1.0); 0 = black |

GE factor codes: src 0 DST_COLOR, 1 1−DST_COLOR, 2 SRC_ALPHA, 3 1−SRC_ALPHA, 4 DST_ALPHA,
5 1−DST_ALPHA, **6 2·SRC_ALPHA**, **7 1−2·SRC_ALPHA**, 8 2·DST_ALPHA, 9 1−2·DST_ALPHA, **10 FIX**; dst
uses the same codes with 0/1 = SRC_COLOR. Ops: **0 ADD, 1 SUBTRACT (src−dst), 2 REVERSE_SUBTRACT (dst−src)**, 3 MIN,
4 MAX, 5 ABS.

Distribution (1218 materials in the local MDLs of the 14 extracted characters; resident MDLs not counted):
`0x000A0600 dstfix -1` 945 (additive), `0x00070600` 160 (alpha blend), `0x000A0602 dstfix -1` 90
(**subtractive**), `0x000A0600 dstfix 0` 23 (src only, overwrites). Flags: 0x7 (most), 0x4, 0xF, 0x27, 0xB, 0x2B, 0x2F, 0x3, 0xC, 0x23.

Resource reference float (ResRef +0, "f32 1.0"): confirmed. It is a **uniform scale applied to the node's
rotation part** in FUN_08946AC4 (read at 0x08955F5C and passed as f12, `vmscl` at 0x08946B18). 1200 refs are 1.0 and 2 are 10.0.

## 2. Channel 11 and channel 119

### 2.1 Component lookup — confirmed
`0x089D95D0[channel]` is a pointer to a 4-byte slot. FUN_0894DCC8, from 0x0894DD74, fills each slot with a
vtable (97 slots). Vtable layout `{0, 0, then 9 × {u32 adjust, u32 fn}}` (80 B). Channel 11: slot
0x089D9884 → vtable **0x089DBA90**: m0 0x08955E84 (`return 1`), m1 0x08955E8C (nop), m2 0x0894E388
(default nop), **m3 0x08955E94 = draw**, m4 0x0894E398 and the rest are defaults. Descriptor
(0x089D9A98 + 8·11) = `09 00 02 00 1a 00 00 00` → param_size 26, **state_size 0**.
Channel 119: slot 0x089D9968 → vtable 0x089DBCC0 (m0 0x0895981C, m1 0x08959A10, m2 0x08959A48,
**m3 0x08959AF4 draw**, m4 0x08959A7C, m5 0x08959AB8); param 58 B, state 4 B.

### 2.2 Channel 11 parameter block (26 B) — confirmed unless marked
| off | type | meaning | read at | data (4248 nodes, all discs) |
|---|---|---|---|---|
| +0, +4, +8 | f32×3 | depth-bias offset (x, y, z); used as (x, −y, −z) in **view space** (mode 3) | FUN_08947B88 0x08947BB4..BF8 | x = y = 0 always; z ∈ {0, −0.1 … −2, +0.5, +1} |
| +12 | s32 | MDL resource-table index (-1 = draw nothing) | 0x08955EA0 | — |
| +16 | f32 | scale of the bias matrix (it only affects the bias depth) | 0x08956034 | always 1.0 |
| +20 | u8 | not read by channel 11 | — | 0 (32× 1) |
| +21 | u8 | `sceGuEnable/Disable(GU_SCISSOR_TEST)` | 0x08955F8C | always 0 |
| +22, +23 | u8 | not read | — | 0 |
| +24 | u8 | depth-bias enable | 0x08956028 | 1 in 2607, 0 in 1290 blocks |
| +25 | u8 | depth-bias mode (0..3; 3 = view-space offset) | 0x08956060 | 3 in 3895 of 3897 |

Per-frame behaviour: none. The state size is 0 and the update methods are empty. The draw reads the
parameter block of the current segment. In the data, the MDL index never changes inside a node; only
the bias z changes, in 4 nodes. No UV scroll, no group or frame selection, and no colour inside this channel.

### 2.3 What the draw does (FUN m3 0x08955E94) — confirmed
```
idx = p.i32[12]; if (idx < 0) return
ref = fep.MDLrefs[idx]; h = ref.handle; if (h == -1) return; s = ref.scale      // 0x08955ED4..F5C
col = nodeState(+96, a vec4 kept by channels 15-17, clamped 0..255 in their update 0x0894E540)
      ⊙ instance.tint (instance+64)                                             // 0x08956010 vmul
sceGuEnable(TEXTURE_2D); scissor(p.u8[21])
W   = FUN_08946AC4(elem, s, nodeRec[rec0,rec1,rec2])   // node world matrix (agent A)
VIEW = camView · W                                     // FUN_0896FB5C: sceGuSetMatrix(GU_VIEW, 0x089DA278 · W)
if (p.u8[24]) {                                        // depth bias only, no visible move
   B = FUN_08947B88(..., mode = p.u8[25], off = p.f32[0..2], scale = p.f32[16]·s)
   dz = ndcZ(B.translation) − ndcZ(W.translation)      // FUN_0896FD08 (uses 0x089DA284)
   PROJ' = PROJ with m[2][2] −= dz                     // FUN_0896FD88
   drawMDL(h, col); restore PROJ                       // FUN_0896FE50
} else drawMDL(h, col)                                 // FUN_08972080 → FUN_089716D4
sceGuDepthMask(1)                                      // back to "no depth writes"
```
For mode 3 with node record byte 1 = 0 (the common case), the bias point is
`B_view = View·Parent·t_local + (x, −y, −z)`, projected with PROJ (0x08947D84..0x08947E34). The
offset is in view-space units: −0.8 moves the depth **toward the camera** (strong; sign by PSP
convention, view space looks down −z). WebGL equivalent: draw normally, then shift NDC z by
`ndcZ(pivot_view + (0,0,−z)) − ndcZ(pivot_view)`, or approximate with polygonOffset (guess for the
exact size).

### 2.4 Channel 119 — strong (code); unused by every shipped FEP
m3 0x08959AF4: byte +55 picks the blend (0: `BlendFunc(ADD, 2As, 1−2As)`, 1: `(ADD, 2As, FIX white)`,
2: `(REVERSE_SUBTRACT, 2As, FIX white)`). +56 is a flag (written to renderer+64). +12 is the MDL index. Colour is
built the same way as channel 11 (lighting, ambient = light0 ambient = colour, alpha ×2). The draw goes
through a model-object vtable kept in the 4-byte state (FUN_08972180, FUN_0895E798, FUN_089477B4).
Since no FEP uses it, it is not decoded further.

## 3. Draw path and GE state

### 3.1 Effect-pass state, set once per pass — confirmed (FUN_08940CCC, 0x08940CEC..0x08940D90)
Enable 21 (GU_FRAGMENT_2X), 9 (TEXTURE_2D), 1 (DEPTH_TEST), 4 (BLEND). `sceGuDepthFunc(7 = GEQUAL)`
(0x089A1178, GE 0xDE; the PSP depth buffer is inverted, so this equals GL `LEQUAL`). Disable 7 (fog), 0 (alpha test),
15 (line smooth), 10 (lighting), 3 (stencil), 5 (cull). `sceGuShadeModel(1)`, `sceGuFrontFace(CW)`.
Enable 11 (LIGHT0); disable lights 1–3. `sceGuColorMaterial(GU_AMBIENT)`. `sceGuLightColor(0, GU_DIFFUSE,
0)` (light 0 diffuse black). `sceGuDepthMask(1)` (no depth writes). Matrices: MODEL = I, VIEW = camera+240,
PROJ = camera+176 (camera object at 0x09B6F470). Globals 0x089DA278 (view), 0x089DA27C (cam+304),
0x089DA280 (proj), 0x089DA284 (cam+368, view-projection).

### 3.2 Per MDL draw — confirmed (FUN_089716D4 + FUN_08971850 + FUN_08972194)
```
sceGuTexOffset(0,0)
r = sb(trunc(col.r)); g = sb(trunc(col.g)); b = sb(trunc(col.b))   // byte store: >255 wraps (cannot happen: clamped upstream)
a = trunc(col.a)*2; if (a >= 256) a = 255                          // 128 → 255
C = a<<24 | b<<16 | g<<8 | r
sceGuColorMaterial(GU_AMBIENT)   // material ambient = vertex colour (GE 0x53 = 1)
sceGuEnable(GU_LIGHTING)
sceGuAmbient(C)                  // GE 0x5C/0x5D
sceGuLightColor(0, GU_AMBIENT, C)// GE 0x8F (light 0 ambient)
translucent = (col.a <= 128.0)   // 0x08971724
for groups/prims (1.2, 1.3):
   texture: mat.handle == -1 → disable TEXTURE_2D, else enable + bind:
      sceGuTexFunc(GU_TFX_MODULATE, GU_TCC_RGBA)  (0x08972D5C, GE 0xC9)
      sceGuTexLevelMode(0,0); sceGuTexMode(psm, mips, 0, swizzled)
      sceGuTexFilter(LINEAR_MIPMAP_LINEAR, LINEAR) = table 0x089D0648[5], [1] (single level → linear)
      sceGuTexWrap(REPEAT, REPEAT) = table 0x089D0660[0], [0]  (0x08972E30, GE 0xC7)
   sceGuBlendFunc(mat.render bytes, mat+16, mat+20)
   depthWrite = !(mat.flags & 4) || (!translucent && mat.render == 0x00070600)
   cull = mat.flags & 8 ; frontFace = group.flag ? CCW : CW
   sceGuDrawArray(prim.type, prim.vtype, n, 0, verts)
sceGuColorMaterial(0); sceGuDisable(GU_LIGHTING)         // FUN_089718E0
```
Emissive colour (GE 0x54) is never set by any code in the EBOOT (no 0x54 command constant), so it is 0 (strong).

### 3.3 Colour equation — strong (game state confirmed; GE lighting maths per the hardware/PPSSPP)
With lighting on, CMAT = ambient, no emissive, and light 0 having black diffuse but ambient = C:
```
lit.rgb = clamp01( C.rgb·V.rgb  +  C.rgb·V.rgb·att0 )     // global ambient + light-0 ambient; att0 = 1 for a
                                                         // directional light 0 (light 0's type is inherited, guess)
lit.a   = (A2/255)·V.a                                   // A2 = min(255, 2·alpha)
frag.rgb = clamp01( 2 · T.rgb · lit.rgb )                // MODULATE, then FRAGMENT_2X (RGB only)
frag.a   = T.a · lit.a
untextured prims: frag.rgb = clamp01(2·lit.rgb), frag.a = lit.a
```
Here C = node colour / 255 (128 → 0.502), V = vertex colour, T = texel (CLUT RGBA). Neutral
check: C = 128 and V = 0x8888 give 2·0.502·0.533 ≈ 0.535 and ×2 ≈ 1.07 × texel, so
128 = 1.0 holds across the whole chain. Alpha 128 → A2 = 255 → frag.a = T.a·0.533. With the 2·As blend factor
this gives ≈ T.a.

### 3.4 Blending — confirmed (factors) / strong (clamping)
`out = op( frag.rgb·F_src , dst.rgb·F_dst )`, with F from 1.5. For the three cases in the data:
- additive 0x000A0600, dstfix white: `dst + frag.rgb·2·frag.a`
- subtractive 0x000A0602, dstfix white: `dst − frag.rgb·2·frag.a`
- alpha 0x00070600: `frag.rgb·2a + dst·max(0, 1−2a)`
- 0x000A0600 with dstfix 0: `frag.rgb·2a` (replaces the destination; no alpha test, so transparent texels draw black)

Depth test is always on (GEQUAL), and alpha test is off, so where depth writes are on, transparent texels
also write depth.

## 4. JS-ready pseudo-code (three.js / WebGL)

```js
// mdl: parsed per 1.1-1.5; W: THREE.Matrix4 (node world matrix from agent A, ResRef.scale folded into its 3x3);
// col: [r,g,b,a] node colour in game units (0..255, 128 = 1.0), already multiplied by the instance tint.
function buildMdlMeshes(mdl, texById) {
  const out = [];
  for (const g of mdl.groups) {
    const [sx, sy, sz, tx, ty, tz, su, sv, ou, ov] = g.f;          // 10 floats at group +12
    for (const p of g.prims) {
      const L = geLayout(p.vtype), P = [], UV = [], C = [];
      for (let i = 0; i < p.nverts; i++) {
        const o = p.vtx_off + i * L.size;
        const x = s8(o + L.pos.off), y = s8(o + L.pos.off + 1), z = s8(o + L.pos.off + 2);
        P.push(tx + sx * x / 128, ty + sy * y / 128, tz + sz * z / 128);         // MODEL = T·S
        const u = L.uv ? u8(o + L.uv.off) / 128 : 0, v = L.uv ? u8(o + L.uv.off + 1) / 128 : 0;
        UV.push(u * su + ou, v * sv + ov);                                       // sceGuTexScale/Offset
        const c = u16(o + L.c.off);                                              // RGBA4444, R = low nibble
        C.push((c & 15) * 17 / 255, (c >> 4 & 15) * 17 / 255, (c >> 8 & 15) * 17 / 255, (c >> 12) * 17 / 255);
      }
      const index = p.prim === 4 ? stripIndices(p.nverts) : seqIndices(p.nverts); // 3 = list, 4 = strip
      const mat = p.mat >= 0 ? mdl.materials[p.mat] : null;
      out.push({ P, UV, C, index, mat, tex: mat ? texById(mat.tex_id) : null, frontCCW: g.flag !== 0 });
    }
  }
  return out;                      // geometry is static: build once per MDL
}

const vert = `attribute vec4 color; varying vec2 vUv; varying vec4 vCol;
  void main(){ vUv = uv; vCol = color; gl_Position = projectionMatrix * modelViewMatrix * vec4(position,1.); }`;
const frag = `uniform sampler2D map; uniform bool useMap; uniform vec4 C; uniform float A2;
  varying vec2 vUv; varying vec4 vCol;
  void main(){
    vec3 lit = min(vec3(1.), 2.0 * C.rgb * vCol.rgb);       // ambient + light0 ambient (att0 = 1)
    float la = A2 * vCol.a;
    vec4 T = useMap ? texture2D(map, vUv) : vec4(1.);
    vec3 rgb = min(vec3(1.), (useMap ? 2.0 * T.rgb : 2.0 * vec3(1.)) * lit);  // MODULATE + FRAGMENT_2X
    float a = T.a * la;
    float f = 2.0 * a;                                     // GE src factor 6 = 2*As
    gl_FragColor = vec4(rgb * f, min(1., f));              // premultiply: blend factors below use ONE
  }`;
function materialFor(part, col) {                         // col: game units
  const C = new THREE.Vector4(...col.slice(0, 3).map((x) => (Math.trunc(x) & 255) / 255), 0);
  let a8 = Math.trunc(col[3]) * 2; if (a8 >= 256) a8 = 255;
  const m = new THREE.ShaderMaterial({ vertexShader: vert, fragmentShader: frag, transparent: true,
    uniforms: { map: { value: part.tex }, useMap: { value: !!part.tex }, C: { value: C }, A2: { value: (a8 & 255) / 255 } } });
  const r = part.mat ? part.mat.render : 0x000A0600, op = r & 255, src = r >> 8 & 255, dst = r >> 16 & 255;
  m.blending = THREE.CustomBlending; m.blendSrc = THREE.OneFactor;        // src factor folded into the shader (src must be 6)
  m.blendEquation = op === 2 ? THREE.ReverseSubtractEquation : op === 1 ? THREE.SubtractEquation : THREE.AddEquation;
  if (dst === 10) {                                        // FIX: white → ONE, black → ZERO (other values: CONSTANT_COLOR)
    const fix = part.mat ? part.mat.d & 0xFFFFFF : 0xFFFFFF;
    m.blendDst = fix === 0xFFFFFF ? THREE.OneFactor : fix === 0 ? THREE.ZeroFactor : THREE.ConstantColorFactor;
  } else if (dst === 7) m.blendDst = THREE.OneMinusSrcAlphaFactor;      // shader alpha = min(1, 2a) → max(0, 1-2a)
  const translucent = col[3] <= 128;
  m.depthTest = true; m.depthFunc = THREE.LessEqualDepth;
  m.depthWrite = !(part.mat && part.mat.b & 4) || (!translucent && r === 0x00070600);
  m.side = part.mat && part.mat.b & 8 ? (part.frontCCW ? THREE.FrontSide : THREE.BackSide) : THREE.DoubleSide; // winding: guess, check in game
  m.alphaTest = 0;
  return m;
}
// per frame: mesh.matrix = W (already in the studio's world units); all parts of all groups are drawn,
// in file order; if p.u8[24], apply the depth bias of 2.3 (or polygonOffset as an approximation).
```
Texture sampler: `LinearFilter` for min and mag, `RepeatWrapping` in U and V, no mipmaps, flipY = false
(the GE has v = 0 at the top row of the texture).

## Notes for the other agents
- **A (placement/blending core)**: the node world matrix is FUN_08946AC4. Its mode is the per-node record byte 1
  at instance+72 (0 = parent·local; 1/2 = camera-relative with 0x089DA27C). Parent = `MEM_W(inst+28)+84`, or
  manager+80. ResRef.scale multiplies the rotation part only. VIEW = 0x089DA278 · W (FUN_0896FB5C). The
  instance tint vec4 at `MEM_W(elem+136)+64` multiplies every node colour (assumed 1.0 by default: not
  checked). Effect-pass GE state: 3.1.
- **F (channels)**: the colour channels' update (vtable 0x089DB090, 0x0894E4BC) clamps the vec4 to
  [0, 255] with vmax/vmin at 0x0894E55C.
- **D (emitters)**: channel 20's draw (m3 0x08956200) has the same structure as channel 11. In its 75-byte block,
  +69 = scissor, +72 = depth-bias enable, +73 = bias mode; it draws through FUN_089720B0. The MDL rules
  here apply to its meshes.
- **C/E**: FRAGMENT_2X, depth GEQUAL, no depth write, no alpha test and REPEAT wrap come from the shared pass
  setup. Check whether ANM (0x08955A84) and trails set their own blend the same way (material byte layout).
- FEP_NOTES corrections: MDL prim +12 is the material index, not padding; vertex type 0x98 has no UV
  (it still has colour); the group floats are scale·3, translate·3, uvScale·2, uvOffset·2; material +20 is
  the dst FIX colour.
