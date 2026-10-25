# C: flipbooks — channel 10 (texanim) and the ANM ("TexAnm") resource

Agent C, 2026-10-24. Source: the static recompilation (`work/output/generated/batch_*.cpp`), the EBOOT
image (`work/eboot_image.bin`, base 0x08804000 — note: reading `EBOOT.dec.elf` at p_offset 4120 gives data
shifted by 64 bytes, use the image), the 14 character FEPs in `work/fx_re/*_40.bin`, the 4 system FEPs,
and `resident.frr` from `general_archive/main/main.bin` (308 ANM blocks in all).
Scratch: `work/fx_re/scratch_C/` (`g.sh ADDR` prints a recompiled function compactly, `callers.sh`,
`anmdump.py FEP idx` dumps an ANM, `comp.py id clip out.png` composites one ANM clip the way the game does,
`harness.js` runs the studio's own fx.js under node + three.js, `survey.py`/`s10.py` data surveys).

Labels: **confirmed** = read end to end in the code; **strong** = code read, but one link rests on PSP GE
hardware semantics or on state set elsewhere; **guess** = inferred.

## Summary

- An ANM is **not** a list of time frames. It has three parallel layers:
  **clips** (12 B: a duration in effect frames) ↔ **sequences** (12 B: a list of part indices) ↔
  **parts** (the 28-byte records the studio calls "frames": one textured quad each, with its own
  texture, tint and GE blend mode). At time *t* the engine finds the clip whose time window holds *t*
  and draws **every part of the sequence with the same index, all at once**. (confirmed)
- Clip *k* always uses sequence *k* (c_clip == c_seq in all 308 ANMs, clip+4 == k always, and the draw
  indexes the sequence table with the clip index). Header +0x1C = total length = Σ clip durations (308/308).
  Header flag 0x80 = loop. Without it, the element **disappears** once *t* passes the end. (confirmed)
- A part is a 4-vertex GE **triangle strip** (`sceGuDrawArray(GU_TRIANGLE_STRIP, 0x81, 4, 0, vtx)`),
  vertex = 5 bytes `{u8 u, u8 v, s8 x, s8 y, s8 z}`. Position = `(x/128·sx + ox, y/128·sy + oy, z/128)`,
  with (sx, sy, ox, oy) = **header floats +64..+76**, applied as GUM MODEL = T(ox,oy,0)·S(sx,sy,1).
  UV = u8/128 (texture scale 1, offset 0). (confirmed / strong for the GE /128)
- Channel 10 builds the element matrix with a constant **0.1 basis scale** (`f12 = 0.1f` into
  FUN_08946AC4; translation not scaled). With header size 30 that is a net ×3 over the raw s8 range.
  Billboarding is **not** a ch10 property: it comes from the node's channel-14 block, byte 1. (confirmed)
- Colour: GE lighting is switched on for the draw; global ambient **and** light-0 ambient = the node's
  channel-17 colour (bytes, 128 = 1.0), material ambient = the part's RGBA tint; MODULATE with the texture;
  GU_FRAGMENT_2X; blend function from the part (`{op, src, dst}` + 2 FIX colours). Net:
  `rgb = T.rgb·c·f`, `factor = T.a·c_a·f_a` (each 1.0 at 128), with clamps. Seen: additive
  `0x000A0600`, alpha `0x00070600`, reverse-subtract `0x000A0602`. All use **2×source alpha**. (strong)
- Channel 10's 31-byte block: depth-bias offset (+0), ANM index (+12), speed (+16), bias scale (+20),
  random start (+24 u16), random-flip mode (+26), bias enable (+29), bias mode (+30). (confirmed)
- **Why one100 SS_HIT_B looks like a sprite sheet in the studio:** node 0 (ch10) is *not* the culprit.
  I ran fx.js itself (node + vendor three.min.js, `harness.js`). For node 0 it emits one 4-vertex quad of
  0.27–0.84 units with UV 0.51–0.74 × 0.01–0.24: a quarter of a soft glow, one cell of the sheet. The sheet
  comes from **nodes 3 and 4 (channel 11, MDL 4)**. MDL 4 uses the same texture 0xF0000005, a 4×4 sheet of
  rings, discs and sparks, with group UV scale 0.234 and offset (0.508, 0.008), so in the game it shows one cell.
  fx.js ignores the group UV transform, so it maps the **whole sheet** onto a disc about 5 units across
  (ch9 scale 213 at t = 3). That gives the big grid of rings and discs (agent B's §1.2 finding). Node 0 is wrong too, in a
  smaller way: it draws one quadrant at a time instead of the 4-part composite, at 1/3 size, with the wrong blend.

## 1. The ANM resource

All offsets are relative to the block start (the engine resolves them against the header pointer:
`anm + anm[off]`, e.g. 0x089427B8, 0x08942800, 0x0894285C). Reader / draw: FUN_089425DC (called via FUN_08942FB8);
time: FUN_08955914 (ch10 update); totals and loop flag read at 0x089559AC / 0x089559D8.

### 1.1 Header (0x50 bytes)
| off | type | meaning | evidence |
|---|---|---|---|
| 0x00 | char[4] | 'ANM\0' | data |
| 0x04 | u32 | resource id (local 0x98…, system 0xF0…) | data |
| 0x08, 0x0C | u32, u32 | 1, 1 (version?) — not read by any path I traced | data (308/308) |
| 0x10 | u32 | flags: **0x80 = loop** (149 × 0, 159 × 0x80) | confirmed 0x089559D8 (`& 0x80`) |
| 0x14, 0x18 | u32 | 0, 0 | data |
| 0x1C | u32 | **total length** in effect frames = Σ clip durations | confirmed 0x089559AC; data 308/308 |
| 0x20 | u32 ×4 | counts: n_tex, n_clip, n_seq, n_part (n_clip == n_seq always) | data |
| 0x30 | u32 ×4 | offsets: tex list, clips, sequences, parts | confirmed: +0x30 at 0x0894283C, +0x34 at 0x08955A20, +0x38 at 0x089427B8, +0x3C at 0x089427F0 |
| 0x40 | f32 | size_x (sx) | confirmed: `sceGumScale({sx, sy, 1})` 0x08942628..0x08942680 |
| 0x44 | f32 | size_y (sy) | same |
| 0x48 | f32 | offset_x (ox) | confirmed: `sceGumTranslate({ox, oy, 0})` before the scale |
| 0x4C | f32 | offset_y (oy) | same |

Typical sizes are 15–45 (one ANM has 120 × 97.6). The offsets are almost always 0, except a few beam-like
sheets (e.g. 81.4 × 1.75 with ox 79.25: a long thin quad anchored at one end).

### 1.2 Texture list (8 B per entry) — confirmed (0x08942834..0x0894284C)
`{u32 tex id, u32 runtime handle}`. The draw uses **+4** (the handle the TEX manager filled in at load).
On disc +4 holds fill bytes (0xCDCDCDCD, 0, or junk). **fx.js and fep.py read ids at a 4-byte stride.**
That is correct only for entry 0 (2 ANMs have 4 textures). Correct: `id = u32(off_tex + 8*i)`.

### 1.3 Clip (12 B) — confirmed (FUN_08955914 0x08955A20..0x08955A74)
`{u32 flags, u32 seq index, u32 duration}`. The engine reads only `duration` (+8). It walks clips
in order, adding up durations, and stops at the first clip whose window holds t. +4 always equals the
clip's own index. Flags: 0x80 is set on the last clip of looping ANMs (data). The code does not use them on this path.

### 1.4 Sequence (12 B) — confirmed (FUN_089425DC 0x089427A8..0x089427E4)
`{u32 flags, u32 n_parts, u32 list_off}`; `list_off` → `u32 part_index[n_parts]`. Flags 0 / 8 / 0x80 / 0x88
are not read by the draw (meaning unknown). n_parts ranges 1–7, most often 1–3.

### 1.5 Part (28 B) — confirmed (FUN_089425DC 0x089427F0..0x08942894)
| off | type | meaning | code |
|---|---|---|---|
| +0 | u32 | texture list index | 0x08942840 (`tex[+0].handle`) |
| +4 | u8[4] | RGBA tint → `sceGuMaterial(GU_AMBIENT, rgba)` (GE 0x55/0x58); 0x80 = 1.0 | 0x08942820 → FUN_089A0C60/089A1DB4 |
| +8 | u32 | vertex count (always 4) | 0x0894286C |
| +12 | u32 | vertex offset (block-relative) | 0x08942870 |
| +16 | u32 | blend word: op = bits 0-3, src = bits 8-11, dst = bits 16-19 → `sceGuBlendFunc(op, src, dst, fix0, fix1)`. Cached: it is only re-sent when it differs from the last (manager+44, which ch10 resets to −1 before each draw) | 0x08942800..0x08942818, FUN_089A12A4 (GE 0xDF/0xE0/0xE1) |
| +20 | u32 | src FIX colour | same |
| +24 | u32 | dst FIX colour | same |

Blend words seen (5,637 parts): `0x000A0600` 4,538 (ADD, src = 2·As, dst = FIX 0xFFFFFF → `Cd + Cs·2As`),
`0x00070600` 968 (ADD, src = 2·As, dst = 1−2·As → alpha blend with doubled alpha), `0x000A0602` 131
(REVERSE_SUBTRACT: `Cd − Cs·2As`). GE factor codes: 2 SRC_ALPHA, 3 1−SRC_ALPHA, 6 2·SRC_ALPHA,
7 1−2·SRC_ALPHA, 10 FIX. Doubled factors clamp to [0,1].

### 1.6 Vertices — confirmed (vtype, prim, count) / strong (GE scaling)
`sceGuDrawArray(4 = GU_TRIANGLE_STRIP, 0x81 = GU_TEXTURE_8BIT|GU_VERTEX_8BIT, 4, NULL, anm+vtx_off)` at
0x08942894 (FUN_089A0520). Layout 5 B: `u8 u, u8 v, s8 x, s8 y, s8 z`. There is no vertex colour, so the colour comes from lighting
(§3). GE 8-bit fixed point in transform mode: x/128, u/128 (texture scale 1: the only sceGuTexScale
calls in the effect code (FUN_08972194, MDL) restore 1.0, and ch10 sets `sceGuTexOffset(0,0)` at 0x08955BF4).
Strip order (0,1,2),(2,1,3). The data uses v = 0 at the top of the cell and +y up (v0 = (u0,v0) ↔ (−x,+y)).
So with three.js `flipY = false` textures the UVs go in as-is. z is usually 127 (≈ 0.99): a small forward
offset in sprite space that is *not* scaled by sx/sy.

Texture state (FUN_08973334 → FUN_08972C14): `sceGuTexFunc(GU_TFX_MODULATE, GU_TCC_RGBA)`,
`sceGuTexFilter(GU_LINEAR_MIPMAP_LINEAR, GU_LINEAR)` (table 0x089D0648[5], [1]), `sceGuTexWrap(REPEAT, REPEAT)`
(table 0x089D0660[0]). Only one level is uploaded, so the filter is effectively linear. (confirmed)

### 1.7 What a "frame" is — example (confirmed with comp.py)
`0xF0000081` (one100 SS_HIT_B node 0; also Cloud sev100, 68 uses): 1 clip (dur 2, loop), 1 sequence = parts
[0,1,2,3]. The four parts share the cell uv (65..95, 1..31) of TEX 0xF0000005 (128×128 sheet), mirrored
into the four quadrants ±x / ±y. Together they draw **one round soft glow** of radius 30 (header size).
`0xF000008A` (Cloud BRAVER): 16 clips × 2 frames, each a 2-part sequence (a blue glow quad + a star quad,
different sizes and tints per clip), 32 frames, loop. Composites: `scratch_C/comp_f0000081.png`, `comp_f000008a_c5.png`.

### 1.8 JS decoder
```js
function decodeAnmC(u8) {                       // u8 = the ANM block
  const dv = new DataView(u8.buffer, u8.byteOffset, u8.byteLength), U = (o) => dv.getUint32(o, true);
  const [nTex, nClip, nSeq, nPart] = [0x20, 0x24, 0x28, 0x2C].map(U);
  const [oTex, oClip, oSeq, oPart] = [0x30, 0x34, 0x38, 0x3C].map(U);
  const F = (o) => dv.getFloat32(o, true);
  const a = { id: U(4), loop: !!(U(0x10) & 0x80), total: U(0x1C),
              size: [F(0x40), F(0x44)], offset: [F(0x48), F(0x4C)], tex: [], clips: [], seqs: [], parts: [] };
  for (let i = 0; i < nTex; i++) a.tex.push(U(oTex + 8 * i));                    // 8-byte stride!
  for (let i = 0; i < nClip; i++) a.clips.push({ flags: U(oClip + 12 * i), dur: U(oClip + 12 * i + 8) });
  for (let i = 0; i < nSeq; i++) { const n = U(oSeq + 12 * i + 4), lo = U(oSeq + 12 * i + 8);
    a.seqs.push(Array.from({ length: n }, (_, k) => U(lo + 4 * k))); }
  for (let i = 0; i < nPart; i++) { const o = oPart + 28 * i, vo = U(o + 12), nv = U(o + 8), P = [], UV = [];
    for (let k = 0; k < nv; k++) { const p = vo + 5 * k;
      UV.push(u8[p] / 128, u8[p + 1] / 128);
      P.push(dv.getInt8(p + 2) / 128 * a.size[0] + a.offset[0], dv.getInt8(p + 3) / 128 * a.size[1] + a.offset[1], dv.getInt8(p + 4) / 128); }
    a.parts.push({ tex: U(o), tint: [u8[o + 4], u8[o + 5], u8[o + 6], u8[o + 7]],   // bytes, 128 = 1.0
                   blend: U(o + 16), fix: [U(o + 20), U(o + 24)], P, UV, I: [0, 1, 2, 2, 1, 3] }); }
  return a;
}
```

## 2. Channel 10 (component type 10, vtable 0x089DBA40)

Descriptor 0x089D9A98 + 8·10 = `0a 03 0200 1f00 0700`: param 31 B, state 7 B. Methods (vtable + 12 + 8k):
init 0x089556EC, (nop 0x0895590C), update 0x08955914, draw 0x08955A84. Method args: a1 = element context
(ctx+136 effect instance, ctx+140 node instance, ctx+144 current track record, ctx+156 resource context),
a2 = parameter block, a3 = state.

### 2.1 Parameter block (31 B) — confirmed (readers listed); data = 1,825 blocks in 18 FEPs
| off | type | meaning | read at | data |
|---|---|---|---|---|
| +0 | f32×3 | depth-bias offset (x, y, z), used as (x, −y, −z) in **view space** (bias mode 3) | FUN_08947B88 0x08947BB4.. (via a2 = param) | x = y = 0; z ∈ {0, ±0.05 … −8}, −0.8 most often |
| +12 | s32 | ANM index (FEP ANM table; −1 = none) | 0x0895592C, 0x08955AD0 | |
| +16 | f32 | playback speed (multiplies time) | 0x089559A0 | 1 (1642), 2 (181), 1.064 (2) |
| +20 | f32 | depth-bias matrix scale (×0.1 as the bias builder's basis scale; no effect on the bias depth) | 0x08955D98 | always 1 |
| +24 | u16 | random start: t0 += rand()·value (frames) | 0x08955714 | always 0 |
| +26 | u8 | random flip mode: 0 none, 1 = {none, H}, 2 = {none, V}, 3 = any of 4 | 0x0895577C..0x089558F4 | always 0 |
| +27, +28 | u8 | unused by ch10 | — | always 0 |
| +29 | u8 | depth-bias enable | 0x08955D8C | 1 (1605) / 0 (220) |
| +30 | u8 | depth-bias space (FUN_08947B88 a3/t0): 0 local, 2 world, 3 view (offset added after the view transform) | 0x08955DC4 | always 3 |

### 2.2 Linked tracks — strong
The track record's u16 list (`Track+8`, n = `Track+4`) is `n` track indices followed by those tracks'
state offsets. For ch10, n = 2: entry 0 = the **colour track** (ch17/15/16), entry 1 = the **flags track
(ch14)**. SS_HIT_B node 0: tracks [ch1, ch4, ch9, ch17, ch14, ch10]; the ch10 list = [3, 4 | 128, 192].
- colour = `nodeState + 96 + list[n+0]` (vec4 floats, 0..255, 128 = 1.0) (0x08955C08..0x08955C40)
- flags record = `node.paramPtr[list[1]]` → bytes {b0, b1 = **orientation mode**, b2} → FUN_08946AC4 a3/t0/t1
  (0x08955B8C..0x08955BD4). SS_HIT_B ch14 = `01 01 00 00 00 00` → mode 1 (camera-facing).

### 2.3 State (7 B) and timing — confirmed (FUN_089556EC, FUN_08955914)
`{f32 t, u16 clip (0xFFFF = finished), u8 flip}`. `dt` = effect instance +352 (the per-tick frame step,
normally 1.0; agent A).
```js
// init (spawn)
st.t = -dt + (p.randStart ? rand() * p.randStart : 0);  st.clip = 0;
st.flip = [0, rand() < .5 ? 0 : 1, rand() < .5 ? 0 : 2, Math.trunc(rand() * 255) >> 6][p.flipMode] || 0;
// update (every effect frame, before draw)
if (st.clip === 0xFFFF) return;
st.clip = 0; st.t += dt;
let T = st.t * p.speed;
if (T >= anm.total) {
  if (!anm.loop) { st.clip = 0xFFFF; return; }               // hidden from now on
  T = Math.trunc(T) % anm.total; st.t = T;                   // quirk: stores the *scaled* time back
}
let k = 0, end = anm.clips[0].dur;
while (T >= end) end += anm.clips[++k].dur;
st.clip = k;
// draw: skip if st.clip === 0xFFFF; else draw anm.seqs[st.clip] (every part)
```
For the studio: with dt = 1 and spawn at node frame 0, at node frame g the time is `T = g·speed`
(before any wrap). The rand() LCG is the global one at 0x089DA120 (`s = s·0x343FD + 0x269EC3; (s>>16)&0x7FFF / 32767`).

### 2.4 Draw (FUN_08955A84) — confirmed sequence
1. Return if clip == 0xFFFF or the ANM handle is −1.
2. `F = FUN_08970B40(flip)`: 0 I, 1 Ry(π) (H-mirror), 2 Rx(π) (V-mirror), 3 Ry(π)·Rx(π).
   `L = F · nodeMatrix(ctx140+80)` (3×3), translation kept (0x08955B50..0x08955B88).
3. `W = FUN_08946AC4(ctx, node, L, b0, mode = b1, b2, s = 0.1)` (0x08955BB8: `f12 = 0x3DCCCCCD`):
   - mode 0: `W = Parent · [0.1·L₃ | L.t]`
   - mode 1: `W₃ = CamRot · diag(effScale) · 0.1·L₃`, `W.t = Parent · L.t` (CamRot = 0x089DA27C matrix,
     effScale = effect instance +48). The sprite faces the camera, and the node's own rotation still spins it in the screen plane.
   - mode 2: axis-locked billboard (agent A).
   Parent = effect root (instance +80) or the parent node's +84.
4. `sceGuSetMatrix(GU_VIEW, View · W)` (FUN_0896FB5C, View = 0x089DA278). Enable TEXTURE_2D,
   `TexOffset(0,0)`, `DepthMask(1)` (**no depth write**), disable CULL_FACE.
5. colour C = clamp₀..₂₅₅(nodeColour ⊙ effectTint(instance +64)); per component clamp (0x08955C74..0x08955D88).
   effectTint is presumably (1,1,1,1) unless the game fades the effect (guess).
6. If p.biasEnable: `Bm = FUN_08947B88(..., mode b1, p.biasMode, off = p.vec, scale 0.1·p.f20)`.
   `f0 = FUN_0896FD08(W, Bm)` = z/w(bias point) − z/w(W origin). `FUN_0896FD88(f0)` loads a projection with
   P[2][2] −= f0, which shifts every fragment's NDC depth by +f0. Draw, then `FUN_0896FE50` restores the projection.
7. `FUN_08942FB8(resctx, handle, clip, &C)` → FUN_089425DC (§3).

## 3. The flipbook draw path (FUN_089425DC) — confirmed calls, strong GE maths

```
sceGumMatrixMode(GU_MODEL); sceGumPushMatrix(); sceGumLoadIdentity();
sceGumTranslate({ox, oy, 0}); sceGumScale({sx, sy, 1}); sceGumUpdateMatrix();          // 0x08942614..0x0894265C
amb = (A = min(255, trunc(2·C.a))) << 24 | C.b << 16 | C.g << 8 | C.r                     // 0x08942660..0x08942774
sceGuEnable(GU_LIGHTING); sceGuColorMaterial(0); sceGuAmbient(amb); sceGuLightColor(0, GU_AMBIENT, amb);
for part in seqs[clip]:
   if part.blend != last: sceGuBlendFunc(op, src, dst, fix0, fix1); last = part.blend
   sceGuMaterial(GU_AMBIENT, part.tint)                       // GE 0x55 + 0x58
   bind texture tex[part.tex].handle (MODULATE/RGBA, linear, repeat)
   sceGuDrawArray(GU_TRIANGLE_STRIP, 0x81, 4, 0, vtx)
sceGuDisable(GU_LIGHTING); sceGumPopMatrix(); sceGumUpdateMatrix();
```
Global effect pass (FUN_08940CCC): GU_FRAGMENT_2X on, DEPTH_TEST on (GEQUAL, PSP inverted Z), BLEND on,
alpha test off, fog off, cull off, light 0 enabled (diffuse 0), lights 1–3 off. (confirmed calls; same as B)

GE lighting with no vertex colour and ColorMaterial(0): lit = matAmbient·globalAmbient + matAmbient·light0Ambient.
Lit alpha = matAmbient.a·globalAmbient.a. Light 0 adds nothing else (diffuse colour 0, no specular). (strong: GE semantics)
In bytes (C = channel colour after clamp, Fp = part tint, T = texel):
```
L.rgb = min(255, 2·C.rgb·Fp.rgb/255)                  // two equal ambient terms
L.a   = min(255, 2·C.a) · Fp.a / 255
S.rgb = min(255, 2 · T.rgb·L.rgb/255)                // MODULATE then FRAGMENT_2X (RGB only)
S.a   = T.a·L.a/255
k     = min(1, 2·S.a/255)                            // GE src factor 6 (2·As)
0x000A0600: D = min(1, D + S·k)        0x00070600: D = S·k + D·max(0, 1 − 2·S.a/255)
0x000A0602: D = max(0, D − S·k)
```
In normalized units (c = C/128, f = Fp/128, so 1.0 at 128): `S.rgb ≈ min(1, T.rgb·c·f)`, `k ≈ min(1, T.a·min(1,c_a)·f_a)`.
The doublings (2× ambient, FRAGMENT_2X, 2·As) cancel the 128 = 1.0 convention exactly.

## 4. JS pseudo-code (three.js / WebGL)

```js
// inputs: anm = decodeAnmC(blob); texFor(id) -> THREE.Texture (flipY=false, RepeatWrapping, LinearFilter)
// node: W = element world matrix as in §2.4 step 3 (with the 0.1 basis scale and the ch14 mode; agent A),
//       C = [r,g,b,a] channel-17 colour bytes (×effect tint, clamped 0..255), st = state after update (§2.3)
function drawCh10(scene, anm, st, W, C, p, camera) {
  if (st.clip === 0xFFFF) return;
  const flip = [I, rotY(Math.PI), rotX(Math.PI), rotY(Math.PI).multiply(rotX(Math.PI))][st.flip];  // apply in sprite space
  const Ab = Math.min(255, Math.trunc(2 * C[3]));
  for (const pi of anm.seqs[st.clip]) {
    const part = anm.parts[pi];
    const L = [0, 1, 2].map((i) => Math.min(255, 2 * C[i] * part.tint[i] / 255) / 255);   // lit rgb 0..1
    const La = Ab * part.tint[3] / 255 / 255;                                             // lit alpha 0..1
    const mat = new THREE.ShaderMaterial({
      uniforms: { map: { value: texFor(anm.tex[part.tex]) }, lit: { value: new THREE.Vector4(...L, La) },
                  bias: { value: p.biasEnable ? depthBiasNdc(W, p.vec, camera) : 0 } },
      vertexShader: `uniform float bias; varying vec2 vUv; void main(){ vUv = uv;
        gl_Position = projectionMatrix * modelViewMatrix * vec4(position,1.); gl_Position.z += bias * gl_Position.w; }`,
      fragmentShader: `uniform sampler2D map; uniform vec4 lit; varying vec2 vUv; void main(){
        vec4 T = texture2D(map, vUv);
        vec3 rgb = min(vec3(1.), 2. * T.rgb * lit.rgb);        // MODULATE + FRAGMENT_2X
        float k  = min(1., 2. * T.a * lit.a);                  // 2·As
        gl_FragColor = vec4(rgb * k, k); }`,                   // premultiplied by the source factor
      transparent: true, depthTest: true, depthWrite: false, side: THREE.DoubleSide,
      blending: THREE.CustomBlending, blendSrc: THREE.OneFactor,
      blendDst: (part.blend >> 16 & 15) === 7 ? THREE.OneMinusSrcAlphaFactor : THREE.OneFactor,   // 7: 1−2As (k in alpha)
      blendEquation: (part.blend & 15) === 2 ? THREE.ReverseSubtractEquation : THREE.AddEquation });
    const g = new THREE.BufferGeometry();
    g.setAttribute('position', new THREE.Float32BufferAttribute(part.P, 3));   // already ×size + offset
    g.setAttribute('uv', new THREE.Float32BufferAttribute(part.UV, 2));
    g.setIndex(part.I);
    const m = new THREE.Mesh(g, mat); m.matrixAutoUpdate = false;
    m.matrix.copy(W).multiply(flip);           // W already holds the 0.1 basis scale and the billboard
    scene.add(m);                              // draw parts in sequence order
  }
}
// depth bias (mode 3): NDC depth of (view-space origin + (x, −y, −z)) minus NDC depth of the origin
function depthBiasNdc(W, v, cam) {
  const o = new THREE.Vector3().setFromMatrixPosition(W).applyMatrix4(cam.matrixWorldInverse);
  const b = o.clone().add(new THREE.Vector3(v[0], -v[1], -v[2]));
  const z = (q) => q.clone().applyMatrix4(cam.projectionMatrix).z;   // applyMatrix4 divides by w
  return z(b) - z(o);
}
```
Units: the 0.8 bias is in view-space (world) units, the same units as the effect root matrix.
How effect units map to studio metres is agent A's work (FX_UNIT).

## 5. Other ANM / TEX users (channels 53–57, 80, 63, 105)

Usage in the 18 FEPs: ch10 936 nodes, ch21 1,138, ch80 45, ch105 89; **ch53–57 and ch63: 0**.
- 53, 55, 56, 57 (ANM @16) and 54 (ANM @160) draw through the same FUN_08942FB8 → FUN_089425DC, so §1 and §3
  apply. Draw methods 0x08961980 / 0x08962218 / 0x08962B64 / 0x089636FC / 0x08963E70. They are unused in shipped data, so I did not decode them further.
- **80** (72 B, ANM @44; methods init 0x089660D0, update 0x08966584, draw 0x089667E8): a multi-sprite
  ANM (strong). The count comes from a linked track. Init gives each sprite a random scale factor `1 − rand·p[60]`
  (if p[69]) and a random angle `rand·180` (if p[70]), plus a random start frame `trunc(rand·(total−1))` (if p[68]).
  Update runs the same clip search with speed p[48]. Non-looping holds clip 0 instead of hiding (0x08966648).
  p[32..43] = depth-bias vec (z −0.8), p[52] 20, p[56] 8, p[64] 1.0. Two FUN_08942FB8 calls per sprite.
  To finish: agent F/D.
- **21** (particle emitter, agent D) also draws its particles with FUN_08942FB8 (×2 in 0x08958DB4), so it uses §1/§3.
- **63** (TEX @0): unused. Its draw 0x0897FF44 does not use the ANM path.
- **105** (86 B, TEX @44; draw 0x089656D8): its own strip geometry (FUN_08964EAC, sceGuDrawArray via 0x089A07F4),
  two texture binds, three blend functions. This is a trail/streak ("DASH_WIND", "TRAIL" effects), so it belongs to agent E.

## 6. Where fx.js goes wrong (channel 10 / ANM)
1. It treats the 28-byte part records as time frames and picks one by node progress. Correct: draw the whole
   sequence of the current clip (§2.3). For 0xF0000081 that is 4 quads at once, not one quadrant per quarter of the node's life.
2. Time is in effect frames × speed against clip durations, with loop flag 0x80; non-looping ANMs vanish after `total`.
3. Geometry: s8/127 → /128, missing header size (×30 here) and offset, missing the 0.1 basis scale (net ×3 bigger in game).
4. Texture list stride is 8, and each part picks its texture with part+0.
5. Colour/blend: it should be per part. Tint × channel colour via the formula in §3, alpha matters (2·As), three blend
   modes, no depth write, depth bias toward the camera. The studio always uses AdditiveBlending, ignores the alpha path and uses a constant tint.
6. Orientation: it always faces the camera. The game only does that for ch14 byte 1 = 1 (mode 0 is world-oriented, mode 2 axis-locked),
   and in mode 1 the node rotation still spins the sprite in the screen plane.

## Notes for the other agents
- **A**: ch10 passes a constant `f12 = 0.1` to FUN_08946AC4 (0x08955BB8). Ch11 passes ResRef.scale (B).
  Mode byte = ch14 byte 1 via the track's linked-track list (§2.2). Effect instance +48 = scale vector used by the
  billboard, +64 = colour tint vec4, +80 = root matrix, +352 = frame step. Descriptor table check: at 0x089D9A98 in
  `eboot_image.bin` the table starts at channel 0 (EBOOT.dec.elf with p_offset 4120 reads 64 bytes off).
- **B**: confirmed from the other side that MDL 4 (one100 SS_HIT_B nodes 3–4) uses TEX 0xF0000005 with group UV
  scale 0.234 / offset (0.508, 0.008). Ignoring it is what draws the sprite-sheet grid the user reported.
- **D**: particle (ch21) quads use the same ANM renderer (§1, §3). Pass the particle's clip index and colour.
- **E**: ch105 is a strip/trail component with its own TEX and blend (§5).
