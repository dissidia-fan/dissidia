# Agent E: trails (locus), model emitters, VTL, and how 0x042A / 0x022E drive weapon trails

Scope: channels 26, 30, 150, 105, 106, 79, 108, 78, 20; the VTL resource; event 0x042A (record 9999)
and 0x022E. Source: the static recompilation (`work/output/generated/batch_*.cpp`) and the decrypted
EBOOT (`work/EBOOT.dec.elf`: **segment 0 is loaded at 0x08804040, file offset 0x1018**; the older
notes' "base 0x08804000" is 0x40 off for data reads). Scratch scripts are in `work/fx_re/scratch_E/`
(`fn.py` prints a function in short form, `at.py` prints from any mid-function address,
`vt.py <chan…>` prints a channel's component vtable, `pdump.py` / `tdump.py` dump node params and track
dependencies, `p105.py` gives statistics over all locus params, `vtl.py` dumps VTL blocks).

Evidence marks: **confirmed** = read end to end in code; **strong** = code read, one link inferred;
**guess** = plausible, not proven.

## Summary

* **The weapon trail is the FEP "LOCUS" effect, drawn by channels 106 + 105, not by 26/30/150.**
  Event 0x042A with record 9999 spawns, for example, `P0071_P_SEV100_SWORD_LOCUS` or
  `P0010_PONE100_LOCUS` on the weapon bone. In that node:
  * **ch106** keeps a world-space history of **two points per frame**. They are param `A` (@0) and `B`
    (@16), given in the element's local space (the weapon bone's frame after FEPA placement), with y
    and z negated. There are `n` samples (@12: 8 for Cloud, 10 for WoL), and a new sample is taken
    once per frame.
  * **ch105** turns that history into a **GE cubic B-spline surface**: `sceGuDrawSpline`, 4 × n control
    points, open/open, with v subdivision @84. It is drawn in world space.
  * Shape and colour: the ribbon **tapers linearly to zero width at the oldest sample**. Its colour is
    a gradient from param colour0 (newest) to colour1 (oldest). Colour values run 0..255, not 128 = 1.
    Optionally, alpha ramps in at the head (@56).
  * Texture: a TEX with a texel rect and scrolling, U along the trail and V across it (selectable).
  * Blending is selectable (@81: 0 = alpha, 1 = additive, 2 = subtractive). Culling is off. The
    trail is drawn with **GU_FRAGMENT_2X (colour doubling)**, which is the main reason the studio's
    version looks dark.
* **ch79** is inert: it zeroes a 16-byte state and has no parameters or behaviour. The node's own
  colour channel (ch17) is **not used** by the trail draw, except in a rare @85 mode.
* **Channels 26 / 30 / 150 are not trails.** They are **"emit child nodes at mesh vertices"**
  components. Positions come from a VTL vertex list (scaled by the MDL ref's scale float):
  * 26 emits at a rate;
  * 30 does the same and also orients each child along the emission direction;
  * 150 fires a one-shot burst at each segment start.
* **ch108** places the element on a VTL vertex (by index or at random). **ch78** moves the element
  along a VTL vertex path at a given speed. Neither appears in the 18 FEPs checked (ch30 doesn't either).
* **VTL** ("vertex list") holds named index lists into a table of float xyz points. It is decoded below.
* **ch20 is not an emitter of copies.** It draws **one MDL** with an animated UV offset
  (u, v + velocity + acceleration, optional random start). The model matrix comes from the element
  (with billboard modes) and the colour is the element colour times the instance colour.
* **0x022E does not touch the FEP trail.** It sets the reach of the **actor's weapon-sweep hit test**
  (actor+0x76C, built from the bone matrix every tick). When that sweep hits, the game spawns a
  textured slash mark that fades over 120 frames. That system is gameplay plus a hit spark, not the
  swing ribbon. This corrects K.md's "weapon-trail" reading (strong).

---

## 0. Component plumbing (needed to read any channel; for all agents)

* `0x089D95D0[ch]` is a pointer to a 4-byte static C++ object. The registry code at
  **0x0894DD74–0x0894E374** (inside FUN_0894DCC8) writes each object's vtable pointer, in pairs of
  `lui/addiu` and `sw`. A vtable is 8 header bytes, then 9 methods × `{s16 this_adj, s16, u32 fn}`.
  The method at vtable+8+8k is called with `a0 = object, a1 = runner, a2 = current param block,
  a3 = this track's state, f12 = local time`. In the runner, +136 = effect instance, +140 = element,
  +144 = track. (Seen in FUN_0894A104 and FUN_0894BA60.) **confirmed**
* Method meaning, from the callers:
  * m0 = bind / init. Called from FUN_0894A104 when the element is created.
  * m1 = new segment starts. FUN_0894A3EC calls it with t0 = the old param block.
  * m2 = update, phase 2. FUN_0894A5B0 / A674 → FUN_0894A7AC → BA60, using list node+40.
  * m3 = **draw**. FUN_08949E2C / FUN_0894A960 call vtable+32 using list node+44.
  * m4 = release.
  * m5 = time integration (phase 5, list node+52).
  * m6 = phase 7, m7 = phase 8, m8 = reset.

  **confirmed** for m0, m1, m2, m3 and m5; m4 is **strong**.
* **Track data** (`track+8`, `n = track+4`) holds `n` dependency track indices, followed by `n` state
  offsets. A component reads a dependency's state at `element + 96 + off`; the element state base is
  element+96. Example: locus ch105 has deps `[ch17, ch14, ch106]` with offsets `[96,160,176]`, and
  ch106 has deps `[ch14]`. **confirmed** (FUN_08964A5C reads `data[n+2]`).
* Instance fields:
  * inst+64 = instance colour vec4. FUN_08944930 at instantiation sets it to (1,1,1,1).
  * inst+80 = instance world matrix.
  * inst+352 = this tick's frame step dt. FUN_0894B48C writes it, and it is normally 1.0 per frame
    (guess).
  * inst+456 = resource tables: +48 TEX, +52 MDL, +56 ANM, +60 VTL. Each entry is a 20-byte ref;
    ref+0 is the float "scale" (1.0) and ref+16 is the runtime handle.

  **confirmed** except the dt value.
* An element's world matrix = `element+80 (local) × parent`. The parent is the parent element's +84,
  or else inst+80 (FUN_08967B64 → FUN_08967868, a row-vector 4×4 multiply). **confirmed**
* For A/B: FUN_08946AC4 builds an element model matrix. Byte 1 of the ch14 params selects the mode:
  0 = world, 1 = camera billboard, 2 = axis billboard. It also scales the matrix by f12 (the MDL ref
  scale). **strong**
* Game-side `sceGuEnable` IDs follow pspsdk (jump table 0x089D18F4): 2 scissor, 5 cull, 9 texture,
  21 = FRAGMENT_2X. Enabling 21 rewrites TFUNC (0xC9) with bit 16 set, i.e. RGB doubling
  (0x089A14BC). **confirmed**
* Other GE wrappers:
  * `FUN_089A12A4(op,src,dst,fixA,fixB)` = sceGuBlendFunc (cmd 0xDF/E0/E1);
  * `FUN_089A07F4(vtype,uc,vc,ue,ve,idx,vtx)` = **sceGuDrawSpline** (cmd 0x06);
  * `FUN_0899FF9C(u,v)` = sceGuPatchDivide (cmd 0x36);
  * `FUN_089A0520` = sceGuDrawArray;
  * `FUN_089A0F84` = sceGuTexOffset (0x4A/0x4B);
  * `FUN_0896FB5C` = load an identity world matrix.

  **confirmed**
* The FEP RNG is the global at 0x089DA120: `x = x*0x343FD + 0x269EC3; r = ((x>>16)&0x7FFF)/32767`.
  Every "random" below uses it. **confirmed**

## 1. Per-channel layouts

Descriptor sizes are {param, state}, from 0x089D9A98. Component vtables (from `vt.py`):

| ch | vtable | m0 | m1 | m2 | m3 | m4 | m5 |
|---|---|---|---|---|---|---|---|
| 20 | 0x089DBB80 | 08956164 | 089564FC | – | 08956200 (draw) | – | 08956820 |
| 26 | 0x089DB310 | 0894FF60 | 0894FF74 | 0894FF88 (emit) | – | – | 0894FF80 |
| 30 | 0x089DB360 | 08950628 | 0895063C | 08950650 (emit) | – | – | 08950648 |
| 150 | 0x089DB2C0 | 0894F91C | 0894F92C | 0894F938 (burst) | – | – | 0894F984 |
| 79 | 0x089DB8B0 | 08954240 | – | 08954280 (ret) | – | 08954288 (ret) | – |
| 105 | 0x089DC248 | 08964A5C | 08964B18 | 08964B40 (UV scroll) | **089656D8 (draw)** | 08964CE4 | 08964CEC |
| 106 | 0x089DC428 | 0896722C | 089672BC (ret) | 089672C4 (sample) | – | 08967338 | 08967358 |
| 108 | 0x089DB9A0 | 089545B8 | 0895467C | 08954684 | – | 08954704 | – |
| 78 | 0x089DB9F0 | 08954DD0 | 08954F3C | 08954F44 | – | 08954FD0 | – |

A dash means the default no-op (0x0894E38x–E3B8). ch106 m8 = 0x08967360: if t0 & 2, it calls
FUN_0896785C, which sets the "first" flag so the history snaps (resets) on the next update. No caller
of m8 was found in the EBOOT (guess: a battle-overlay or loop-restart path).

### 1a. ch106 "locus history" (76 B params, 4 B state = pointer to a 64 B object)
Param layout (FUN_08967650, FUN_0896808C, FUN_08967C98):

| off | type | meaning | evidence |
|---|---|---|---|
| 0 | vec3 f32 | point **A** in element-local space; used as (x, −y, −z) | confirmed |
| 12 | s16 | **n** = number of history samples (data: 4, 5, 8, 10, 12, 15) | confirmed |
| 14 | u8 | jitter period in frames (new random target every k samples) | confirmed |
| 16 | vec3 f32 | point **B**, same convention | confirmed |
| 32 | vec3 f32 | drift velocity added to each new sample, applied as (x, −y, −z) in world space; **0 in all data** | confirmed |
| 48 | vec3 f32 | jitter smoothing factor per axis (`cur += (target−cur)*k`) | confirmed |
| 64 | vec3 f32 | jitter amplitude (target = rand·2a − a); **0 in all data** | confirmed |
| 28/44/60/76 | – | 0xCD padding | data |

Object (64 B, created in m0 by FUN_08967FCC/FUN_08967518/FUN_08967650):

| off | meaning |
|---|---|
| +0 | bufA: n × {vec4 pos, vec4 vel} |
| +4 | bufB: same layout |
| +8 | "first" flag (1 = re-initialise) |
| +12 / +28 | jitter current / target |
| +44 | jitter countdown |
| +48 | n |
| +52 | u16[4n] scratch, filled by ch105 m0 with (2i, 2i, 2i+1, 2i+1); not used by the draw |
| +56 | accumulated time |
| +60 | time of the last shift |

Update FUN_0896808C (m2; skipped while runner flag bit 0 is set or inst+12 ≠ 0, i.e. paused):
1. `t += dt`. If `t ≥ last + 1`, then `last = t` and this tick **shifts** (one new sample per frame).
2. On the first update, FUN_08967C98 fills every sample k with `P = world(A)`, `pos_k = P + k·drift`,
   `vel = drift` (B likewise), then returns.
3. On a shift, for i = n−1 … 1: `pos[i] = pos[i−1] + vel[i−1]` and `vel[i] = vel[i−1]`, for A and B.
4. Every tick: `pos[0] = world(A)` and `pos[0] = world(B)`, where
   `world(p) = (p.x, −p.y, −p.z, 1) × (elementLocal × parentWorld)` (FUN_08967B64).
5. On a shift: run the jitter step, then
   `vel[0] = rot(jitter) + (d.x, −d.y, −d.z)` for both buffers.

**confirmed.** With drift and jitter at 0 (all retail data), **sample i is the world position of A
and B i frames ago**, clamped to the spawn frame.

### 1b. ch105 "locus renderer" (86 B params, 32 B state)

| off | type | meaning | evidence |
|---|---|---|---|
| 0 | vec4 f32 | colour at the newest sample, RGBA **0..255** | confirmed |
| 16 | vec4 f32 | colour at the oldest sample | confirmed |
| 32 | vec3 f32 | used only in the @85 path (FUN_08947B88); (0,0,−1) when @85 = 1 | strong |
| 44 | s32 | TEX index (table inst+456+48) | confirmed |
| 48, 52 | f32 | UV scroll per frame in texels: `u -= @48`, `v -= @52`, wrapped into [0,W) / [0,H) (m2 0x08964B40) | confirmed |
| 56 | f32 | head fade-in length in % of the trail (0, 5, 10, 25, 50 in data) | confirmed |
| 60 | f32 | 1.0 always; not read by these methods | – |
| 64, 66 | u16 | texel rect origin u0, v0 | confirmed |
| 68, 70 | u16 | texel rect size uw, vh | confirmed |
| 72 | u8 | 1 = textured (vertex type 0x19F) / 0 = untextured (0x19C, 16 B vertices) | confirmed |
| 73, 74 | u8 | randomise u0 within [u0, u0+uw) / v0 within [v0, v0+vh) at bind (FUN_08964CF4) | confirmed |
| 75 | u8 | 1 = min LINEAR_MIPMAP_NEAREST, mag LINEAR, wrap REPEAT; 0 = NEAREST, REPEAT (FUN_08973334 tables 0x089D0648/0660) | confirmed |
| 76 | u8 | 1 = U runs along the trail and V across; 0 = the reverse | confirmed |
| 77 | u8 | direction of the along-coordinate (see formulas) | confirmed |
| 78 | u8 | direction of the across-coordinate | confirmed |
| 79 | u8 | 0 = disable the scissor test while drawing | confirmed |
| 80, 82, 83 | u8 | not read by the 105 methods | – |
| 81 | u8 | blend: 0 = (ADD, SRC_ALPHA, 1−SRC_ALPHA); 1 = (ADD, SRC_ALPHA, FIX 0xFFFFFF), additive; 2 = (REVERSE_SUBTRACT, SRC_ALPHA, FIX 0xFFFFFF), dst − src·α | confirmed |
| 84 | u8 | spline v subdivisions per span (2..10) | confirmed |
| 85 | u8 | 1 = draw under an extra element/billboard matrix (FUN_08946AC4 + FUN_08947B88); 2 of 89 uses | strong |

State: +16 = u offset (texels), +20 = v offset, +24 = uw, +28 = vh. They are set at bind and reset
again at every segment start (m1 calls FUN_08964CF4 without the random step).

Draw FUN_089656D8 → vertex build FUN_08964EAC → FUN_08964990, **confirmed**:
* L = the ch106 object (dependency #2). `n = L.n`, `W, H` = TEX width/height (tex+28 / +32),
  `inst` = inst+64.
* `col_i = clamp0_255((c0 + (c1 − c0)·i/(n−1)) ⊙ inst)`, for i = 0 (newest) … n−1 (oldest).
* Fade-in: `f = @56·0.01·(n−1)`. If f == 0, then `fade_i = 1`. Otherwise `fade_0 = 0`,
  `fade_{i+1} = fade_i + 1/f`, clamped to ≤ 1. Then `alpha_i *= fade_i`.
* Taper: `t_i = 1 − i/(n−1)`, `m = (A_i + B_i)/2`, `P_i = m + (A_i − m)·t_i`, `Q_i = m + (B_i − m)·t_i`.
* UVs (`su, sv` = state offsets):
  * **@76 = 1**:
    * along: `u_i = (su + i·uw/n)/W` when @77 = 1, or `(su + uw − i·uw/n)/W` when @77 = 0;
    * across: `v_P = sv/H`, `v_Q = (sv+vh)/H` when @78 = 1, swapped when @78 = 0.
  * **@76 = 0**:
    * across: `u_P = su/W`, `u_Q = (su+uw)/W` when @77 = 0, swapped when @77 = 1;
    * along: `v_i = (sv + i·vh/n)/H` when @78 = 1, or `(sv + vh − i·vh/n)/H` when @78 = 0.

  The along-step is `size/n`, not `size/(n−1)`.
* Each row writes 4 control points **(P, P, Q, Q)** as `{f32 u, f32 v, u32 ABGR, f32 x, y, z}`.
* Draw state:
  * enable or disable TEXTURE_2D (@72); bind the texture (@75);
  * scissor (@79); disable CULL_FACE; disable state 2 or leave it per @79;
  * **enable FRAGMENT_2X**; blend per @81;
  * `PatchDivide(1, @84)`; world = identity;
  * `DrawSpline(vtype, ucount 4, vcount n, uedge 3, vedge 3, no indices)`.

  Afterwards the scissor state is restored.

GE spline semantics: edge 3 = open/open. This is a clamped (open-uniform) cubic B-spline; colours and
UVs are blended with the same basis. Because the u control points are P,P,Q,Q, each tessellated row is
a straight segment from P(v) to Q(v). The curve passes exactly through the newest and oldest rows and
smooths the rows in between. This is **strong** (it is GE behaviour, not game code).

### 1c. ch79 (0 B params, 16 B state)
m0 copies the zero vec4 at 0x089D9FE0 into its state; m2 and m4 return. Nothing reads it as a
dependency (59 nodes have it). **confirmed: inert.**

### 1d. ch26 "emit at VTL vertices" (26 B params, 6 B state {f32 acc, u16 seq})
Params: @0 f32 rate, @4 f32 and @8 f32 (direction scales), @12 s16 child node, @14 s16 MDL,
@16 s16 VTL, @18 s16 VTL list, @20 u16 cooldown, @22 s16 (must be ≥ 0), @24 u8 probability %,
@25 u8 vertex order.

Update FUN_0894FF88, **confirmed**:
1. `acc += @0 · dt · 0.5`. If `acc < 1`, stop.
2. Otherwise `k = trunc(acc)`, `acc −= k`, then `acc −= @0·@20·0.5`, so @20 is a pause measured in
   frames.
3. For each of the k emissions:
   1. If `@24 < 100`, emit only when `rand·100 < @24`.
   2. Pick the vertex: sequential (`seq++`, wrap) when @25 == 1, otherwise random in
      [0, list count).
   3. `pos = VTL.v[list[idx]] · MDLref.scale`.
   4. FUN_089488A8 transforms the point into world space and returns two direction vectors, which are
      scaled by @8 and @4.
   5. FUN_0894C244 spawns child node @12 there. If @12 is a group, FUN_089469E0 / FUN_08946A28 loop
      over its members.

Data: Bartz `EX_EISYO2` = `{rate 2, child 8, MDL 95, VTL 0, list 0, @20 30, 100 %, sequential}`.

### 1e. ch30 (28 B)
Same as 26, with these shifts: @22 s16, @24 s16 = the child's rotation track, @26 probability,
@27 order. After the spawn it writes `atan2`-based angles (degrees, 180/π) into the child's rotation
state (`child + 96 + track[@24].state_off`), so each child faces its emission direction
(0x08950B60–0x08950C04). **confirmed.** Not used in the 18 FEPs.

### 1f. ch150 (23 B, state {u16, u8 fire})
m0 and m5 set `fire = 1`. m2 calls FUN_0894F990 only when `fire` is set, then clears it: **one burst
per segment start or loop**. The fields match ch26 (@0 count/rate, @4, @8, @12 child, @14 MDL, @16 VTL,
@18 list, @20, @22 mode). **strong.** Data: Gilgamesh `RETRVIEWTER_KISEKI` uses eight of them
(child nodes 0..7, MDL 6).

### 1g. ch108 (25 B, 21 B state): place the element on a VTL vertex
@16 MDL, @18 VTL, @20 list, @22 mode (0 = vertex index in state+16, 1 = random via FUN_08954918),
@24 = reset to the origin. `state.pos = VTL.v[list[idx]] · MDLscale` (FUN_0895477C); m2 advances or
applies it (FUN_08954A74). **strong.** Unused in the 18 FEPs.

### 1h. ch78 (13 B, 20 B state + 24 B object): move along a VTL path
@0 f32 speed (units per frame), @4 MDL, @6 VTL, @10 u8 (0 = start at vertex 0 via FUN_089554A4,
1 = random start via FUN_08955434/0895563C), @12 u8 (start at the origin). Each frame FUN_08955034
moves the position toward the next vertex by `speed`, then advances the index when it gets there.
**strong.** Unused in the 18 FEPs.

### 1i. ch20 "MDL with scrolling UVs" (75 B params, 32 B state)

| off | meaning |
|---|---|
| 0..11 | not read by the ch20 methods (pattern 0, 0, −0.5/−0.8/−1.0, the same header as ch10/11) |
| 12 | s32 MDL index |
| 16, 20 | u, v offset |
| 24, 28 | du, dv per frame |
| 32, 36 | ddu, ddv per frame² |
| 40..60 | random ranges for u, v, du, dv, ddu, ddv (`x += rand·2r − r` when @74; m1 0x08956640+) |
| 64 | f32 used in the @72 path |
| 69 | u8 scissor flag |
| 72 | u8 extra-matrix path (FUN_08947B88; @73 mode) |
| 74 | u8 randomise |

* State: +8 = (u, v) at the segment start, +16 = (du, dv), +24 = (ddu, ddv). m0 copies them from
  @16/@24/@32; m1 re-bases them at segment changes so the motion stays continuous; m5 integrates and
  wraps them into [0, 4096).
* Draw FUN_08956200:
  * `u = u0 + du·t + ½·ddu·t²`, and v likewise (t = frames since the segment start);
  * model matrix = FUN_08946AC4 (element world, billboard mode from ch14, scale = MDL ref float);
  * colour = (dependency #0 state = the ch17 colour, 0..255) ⊙ inst colour;
  * FUN_089720B0(mdl, u, v, colour) → FUN_08972194 draws the MDL with that texture offset;
  * then sceGuTexOffset(0, 0).

  **confirmed.**
* UV units are **probably texels** (guess). The data has a random u range of 128 and dv = 5.26 per
  frame on 128-px textures; B should confirm in FUN_08972194.
* There is **no emission of copies**: it is one mesh per element. Many copies come from several nodes
  or a parent emitter (ch21/26).

## 2. VTL resource (vertex lists)
Layout, from FUN_08973E20 / FUN_08973E58 / FUN_08973E78 and checked on the 20 VTL blocks in the dumps
(**confirmed**):
```
+0x00 'VTL\0'   +0x04 u32 id   +0x08 u32 1   +0x0C u32 1   +0x10 u32 size   +0x14 u32 0
+0x18 u32 n_lists   +0x1C u32 n_verts   +0x20 u32 lists_off (=0x28)   +0x24 u32 verts_off
lists_off: n_lists × {u32 count, u32 off}      (off from the VTL start → u32 vertex indices[count])
verts_off: n_verts × {f32 x, y, z}             (12 B each)
```
`list(k) = indices at off_k`, and `point(j) = verts[j]`. Example (Bartz 0x9800CD80): 2 lists
`[3,5]` and `[4]`, 9 vertices at ±50. Components always pair the VTL index with an MDL index. The
MDL's ref must be loaded (handle ≠ −1), and its scale float multiplies the points; the mesh itself is
not drawn. Users: ch21 (@246), 26, 30, 150, 108, 78.

## 3. Event 0x042A (record 9999), and what 0x022E really changes

### 0x042A → LOCUS effect (confirmed; mechanism from D.md, completed here)
`{slot, 9999, bit, scale}`. FUN_0884E660 → FUN_088AA6F8 with filter 0x0884E888 picks the **first**
FEPA record that meets all of these:
* +0x24 == 9999;
* +0x34 == 0;
* bit `arg2` is set in +0x44;
* bone +0xA8 is a visible draw part.

FEPA +0x28 gives the name index, and through it the LOCUS effect. That effect is placed with FEPA
pos/rot/scale (attach 0x80 = follow the bone), and arg3 scales the instance (FUN_088A72E8), which
scales A, B and therefore the trail. The body is looping (a = 2), so the instance keeps sampling
while the 0x042A range is active.

Real data:

| char | bit | record | effect | bone | FEPA pos / rot / scale | ch106 A / B (file) | n | ch105 |
|---|---|---|---|---|---|---|---|---|
| sev100 | 0 | #7 | SWORD_LOCUS (#7) | r_wep | (0,−0.02,0) / 0 / 1 | (1.65,−0.15,0) / (0.05,−0.15,0) | 8 | c0 (255,128,64,128), c1 (255,64,12,0), TEX#5 = 0xF0000004 (system), rect 0,0,128,64, no fade, @76/77/78 = 1, additive, div 8 |
| sev100 | 1 / 2 / 3 / 4 | #37 / 39 / 60 / 61 | ULTIMA / FAST / ULTIMA_EX / FAST_EX_LOCUS | r_wep | (0.22,−0.16,0), (0.2,−0.12,0) … | | | |
| one100 | 0 | #7 | PONE100_LOCUS (#1) | r_wep | 0 / 0 / 1 | (1.4,0,0) / (0.1,0,0) | 10 | c0 (200,112,128,128), c1 (128,92,64,0), TEX#4, fade 5 % (head row α = 0), div 4, additive |
| one100 | 1 | #57 | the same PONE100_LOCUS | **l_wep** (shield) | (0,−0.5,0) / (0,0,90) / (0.65,1,1) | | | |
| one100 | 2 | #54 | MSWORD_LOCUS (#56) | r_wep | | | | |

Across all 89 ch106 blocks, drift and jitter amplitude are zero. Tip and base lie along the bone's
+X axis, which matches the weapon bones.

Not traced: whether stopping the effect (`h+0xDC |= 0x100`, FUN_088A6AA4) cuts the trail at once or
lets the 12- or 6-frame node run out. The ch17 alpha keys in the node do **not** affect the ribbon.

### 0x022E: the weapon-sweep hit test, not the visual trail (strong)
actor+0x76C holds 0xE0-byte emitters. Per tick, FUN_0884D658:
1. Takes the bone matrix M (FUN_088369A4, bone at +0x14).
2. Computes the base `M·(−e_base,0,0,1)` (+0xC) and the tip `M·(e_tip,0,0,1)` (+4). 0x022E
   overrides +4 for the range; it is reset from +8 every tick.
3. Keeps the previous base and tip (+176 / +192).
4. Spawns a pool object (FUN_088978C4, type 6) with callback **FUN_0884CF64**. That callback
   intersects the swept quad (previous → current) with hit volumes (FUN_08860230, FUN_0884CA90),
   keeps up to 10 already-hit objects (+24.., count +64), and on contact calls FUN_0884B458 →
   FUN_0884C5E8. That builds a camera-facing **slash-mark quad strip** (4 or 12 vertices, type 0x183)
   in the actor's 0x140-byte ring (+48 vertices, +300 count, +304 texture id).
5. FUN_0884B908 draws the ring: triangle strip, `sceGuColor(α<<24 | rgb)`, blend from the texture
   entry's byte +4 (1 = additive, 2 = subtractive, else alpha), and FRAGMENT_2X (FUN_0884BE74).
6. Each record fades through FUN_0884A9B0: `life −= dt`, and
   `α = min(base, 255·life/120)` once life drops below 120 (the constant at 0x089C8AA8).

So 0x022E **lengthens the blade's hit reach**. For example, Cloud 044 sets bone 50 to 1.65 for frames
12–18, which matches the locus tip at 1.65. The FEP ribbon keeps the length of its own ch106 points.

## 4. Pseudo-code for the studio

```js
// ---------- common ----------
const flipYZ = p => [p[0], -p[1], -p[2]];                  // FEP param points / drift
const xform = (p, M) => [                                  // row vector × 4×4 (M[r*4+c])
  p[0]*M[0] + p[1]*M[4] + p[2]*M[8]  + M[12],
  p[0]*M[1] + p[1]*M[5] + p[2]*M[9]  + M[13],
  p[0]*M[2] + p[1]*M[6] + p[2]*M[10] + M[14]];
// elementWorld(f) = elementLocal(f) × instanceWorld(f); instanceWorld = FEPA placement (pos/rot/scale,
// times the 0x042A arg3 scale) × bone matrix at frame f (Agent A's placement). Identity element here.

// ---------- ch106 + ch105 weapon trail ----------
function locusSamples(p106, spawnFrame, t, elementWorld) {
  const n = p106.n, A = flipYZ(p106.A), B = flipYZ(p106.B), d = flipYZ(p106.drift);
  const F = Math.floor(t), S = [];
  for (let i = 0; i < n; i++) {
    const f = (i === 0) ? t : Math.max(spawnFrame, F - i);  // sample 0 follows the current pose
    const M = elementWorld(f);
    const a = xform(A, M), b = xform(B, M);
    S.push({ a: add(a, scale(d, i)), b: add(b, scale(d, i)) });  // drift is 0 in retail data
  }
  return S;                                                      // S[0] = newest
}

function locusControlRows(S, p, scroll /* {su, sv} texels */, tex /* {W, H} */, instColor = [1,1,1,1]) {
  const n = S.length, rows = [];
  const fadeLen = p.fadePct * 0.01 * (n - 1);
  let fade = fadeLen === 0 ? 1 : 0;
  const step = fadeLen === 0 ? 0 : 1 / fadeLen;
  for (let i = 0; i < n; i++) {
    const k = i / (n - 1), tw = 1 - k;
    const m = lerp(S[i].a, S[i].b, 0.5);
    const P = lerp(m, S[i].a, tw), Q = lerp(m, S[i].b, tw);
    const c = [0,1,2,3].map(j => clamp((p.c0[j] + (p.c1[j]-p.c0[j])*k) * instColor[j], 0, 255) | 0);
    c[3] = (c[3] * Math.min(fade, 1)) | 0;  fade = Math.min(fade, 1) + step;
    const along = i / n;                                         // step size/n (sic)
    let uP, uQ, vP, vQ;
    if (p.alongU) {                     // @76 = 1
      const u = p.dirAlong ? (scroll.su + along*p.uw) : (scroll.su + p.uw - along*p.uw);
      uP = uQ = u / tex.W;
      [vP, vQ] = p.dirAcross ? [scroll.sv/tex.H, (scroll.sv+p.vh)/tex.H] : [(scroll.sv+p.vh)/tex.H, scroll.sv/tex.H];
    } else {                            // @76 = 0
      const v = p.dirAcross ? (scroll.sv + along*p.vh) : (scroll.sv + p.vh - along*p.vh);
      vP = vQ = v / tex.H;
      [uP, uQ] = p.dirAlong ? [(scroll.su+p.uw)/tex.W, scroll.su/tex.W] : [scroll.su/tex.W, (scroll.su+p.uw)/tex.W];
    }
    rows.push({ P, Q, uvP: [uP, vP], uvQ: [uQ, vQ], col: c });
  }
  return rows;
}

// Clamped (open/open) cubic B-spline along the rows, `div` segments per span (GE edge 3,3).
function bsplineRows(rows, div) {
  const n = rows.length, spans = n - 3, knots = [0,0,0];
  for (let i = 0; i <= spans; i++) knots.push(i);
  knots.push(spans, spans, spans);
  const out = [];
  for (let s = 0; s <= spans * div; s++) {
    const x = Math.min(s / div, spans - 1e-6);
    const w = rows.map((_, j) => coxDeBoor(j, 3, x, knots));      // basis weights
    const mix = get => rows.reduce((acc, r, j) => addScaled(acc, get(r), w[j]), null);
    out.push({ P: mix(r => r.P), Q: mix(r => r.Q), uvP: mix(r => r.uvP), uvQ: mix(r => r.uvQ),
               col: mix(r => r.col) });
  }
  return out;  // draw as a triangle strip: (P0, Q0, P1, Q1, …)
}

// Material: TEX @44 (palette applied), filter LINEAR when @75, wrap REPEAT;
// fragment.rgb = min(1, 2 · tex.rgb · col.rgb/255)   // FRAGMENT_2X; alpha is not doubled
// fragment.a   = tex.a · col.a/255
// blend @81: 0 → srcα / 1−srcα; 1 → dst + src·srcα (additive); 2 → dst − src·srcα
// no culling; depth state = the global FEP state (Agent A). Per frame: su −= @48, sv −= @52 (wrap).

// ---------- ch20 model with scrolling UVs ----------
function ch20Draw(p, t /* frames since segment start */, elementModel, elementColour, instColour) {
  const u = p.u + p.du*t + 0.5*p.ddu*t*t, v = p.v + p.dv*t + 0.5*p.ddv*t*t;   // + random start if @74
  // elementModel = element world (billboard per ch14 byte 1) × MDL ref scale
  // colour = ch17 state (0..255) ⊙ instColour; draw MDL p.mdl with texture offset (u, v) (units: texels? see 1i)
}

// ---------- ch26 / 150 children at VTL vertices ----------
// per frame: acc += rate*0.5*dt; while acc >= 1 emit (k = floor(acc); acc -= k + rate*cooldown*0.5);
// position = VTL.verts[list[idx]] * MDLref.scale in the element frame; spawn child node @12 there.
```

## 5. Notes for the other agents
* **A (placement/blend):**
  * The FEP param points are used as (x, −y, −z) before the element matrix (ch106 at FUN_08968238).
    Check whether the ch1 position does the same.
  * Instance colour inst+64 defaults to 1.0.
  * Component methods and track dependencies are described in §0.
  * The ch14 param byte 1 is the billboard mode (FUN_08946AC4).
  * The FRAGMENT_2X / TFUNC handling, the pspsdk state IDs and the blend wrapper are described in §0.
* **B (meshes):** ch20's colour is the ch17 state multiplied by the instance colour, plus a UV offset
  passed to FUN_08972194. Please confirm the offset units.
* **D (particle emitters):** ch21's VTL @246 uses the same VTL layout (§2). ch26/30/150 share the
  emission and spawn helpers FUN_089488A8 and FUN_0894C244.
* **Corrections:**
  * FEP_NOTES' "channels 26/30/150 = trails/strips" is wrong (they are vertex emitters).
  * The real trail channels are 106 and 105.
  * K.md's 0x022E "visual trail" is the hit-sweep reach.
