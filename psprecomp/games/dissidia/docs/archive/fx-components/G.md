# FX components, part G: 8888 weapon glows, the ch111 screen blur, and the line / point emitters (100, 102, 103, 95, 82, 58)

Agent G, 24 Oct 2026. These are the three things Move Studio (`studio/fx.js`) did not draw yet. Sources: the static
recompilation (`work/output/generated`, `work/overlays/battle/generated`, printed with `work/magi_re/fn.py`), the
decrypted EBOOT (`work/EBOOT.dec.elf`: segment 0 is loaded at 0x08804040 from file offset 0x1018) and the battle overlay
(`work/OVL_BATTLE_APP.bin`, loaded at 0x089FB46C). The retail data comes from the FEPs and FEPAs in `work/fx_re/*_40.bin` /
`*_4d.bin` and `scratch_F/*_40.bin` (31 fighters and 9 library FEPs), plus the timelines of the 31 retail `p_*.objx`
(`work/exmode_re/all`, unpacked with `tools/charmod_objx.py`). VFPU semantics were checked against the PPSSPP interpreter in
`runtime/src/vfpu_ppsspp` (ReadMatrix / Int_Vmmul / Int_Vtfm / Int_Vrot).

Evidence marks, as in A–F: **confirmed** = read end to end in code; **strong** = code read, but one link (the meaning of a
field, or a callee) is inferred; **guess** = plausible, not traced.

Notation: `P+n` = byte n of the channel's current parameter block, `dep[i]` = the state of the i-th linked track, `dt` = the
instance time step (instance+352, 1 per tick). `rand()` is the FEP LCG (0x089DA120) in [0, 1]; `rand(±r)` = `2r·rand() − r`.

## Summary

1. **8888 weapon glows go through the same event as the 9999 trails (confirmed).** Event **0x042A** (and its 5-argument
   twin 0x055F) `{slot, id, bit, scale}` is generic. Neither the handler (0x0882A8B0) nor the per-frame owner
   FUN_0884E660 nor the filter 0x0884E888 compares the id with 9999; record id 8888 is just a second family of FEPA
   records. Every retail 8888 record is a *sustained* body (`a = 2`, FEPA lifetime 0) attached to a weapon or hand bone
   (attach 0x80 / 0x180). So it plays for exactly as long as the 0x042A range on its slot is active. When the range ends,
   or when the character's script id changes, the game stops it with FUN_088A6AA4. The body's `d` byte then sets how it
   ends: 3 = the loops play out (most records), 1 = a 15-frame fade, 2 = it vanishes at once. One extra rule is in the
   battle overlay: an actor state entered for EX Burst scripts 8000 / 8102 (vtable 0x08A6DC40, entry FUN_08A4F168) kills
   all of the actor's 8888 effects at once, with no fade (FUN_088AA114(set, 8888) → FUN_088A6A78). The studio only needs
   to drop its `a[1] === 9999` test.
2. **Channel 111 is not a rotation of the frame. It is a directional "ghost" (strong; the GE calls are confirmed).**
   The screen copy is drawn again over the frame, unscaled, and moved by a screen-space vector `V`. `V` is the parameter
   direction (`P+0`, `P+4`), turned by the z angle of the linked rotation channel, normalised, and multiplied by the
   ch109 value in **pixels**. The draw is the full-screen 4-column strip of ch110 (positions in 480×272 pixels through an
   orthographic projection), with model matrix = translate(V). The blend comes from `P+12`, the colour from the colour
   channel × the instance colour, and `P+14` sets the number of passes. The vrot that gave the channel its name only turns
   the direction. In both retail uses the rotation is 0: Golbez CHOU_METEO_UP gives a 4 px shift down, and Gabranth's EX
   attack gives about 5 px to the left.
3. **Line and point emitters (confirmed for 100, 102, 103, 95; strong for 82; guess for most of 58).**
   * **100** spawns sparks in a cone around the element's +Y axis. Each spark moves at `v0 = d·(P+32 ± P+36)` per tick,
     with acceleration `d·(P+40 ± P+44) − (0, P+48, 0)`, applied as the discrete sum `t(t+1)/2`. It keeps a history of its
     last `P+66` head positions and is drawn as a `GU_LINE_STRIP` coloured from `P+0` (head) to `P+16` (tail). `P+67` is a
     pause after each burst.
   * **102** lines start on a sphere shell of radius [`P+52`, `P+56`] and fly toward the element origin. The tail trails
     the head at most `P+32` behind. When a line reaches the centre it shortens and dies.
   * **103** lines start in a box `P+64..72` and move with `vel (P+0..8 ± P+16..24)` and `acc (P+32..40 ± P+48..56)`, both
     with y and z negated. The tail trails at most `P+12` behind. Colours are `P+80` (head) and `P+96` (tail).
   * **95** points bounce inside a box `P+0..8` and are drawn as one-pixel `GU_POINTS`.
   * **82** is a camera-facing textured ribbon along the ch70 path. It has four colour vectors, a texel rectangle and UV
     scrolling.
   * **58** is a set of up to `P+132` flipbook sprites whose fade depends on the camera distance. It is drawn with its own
     view-space matrices.
   * In all of them the vertex positions are element-local. They go through the element matrix (FUN_08946AC4 with scale
     1.0, the ch14 billboard mode) **like MDL vertices: no (x, −y, −z) flip**. Only 103 negates y/z, and only for its
     parameter velocities. The current `fxSparks` gets the 100 and 102 layouts partly wrong and treats 103 as 102 (§3.6).

---

## 1. FEPA id 8888: weapon glows and blade auras

### 1.1 Code path (confirmed)

| step | address | what it does |
|---|---|---|
| event | 0x0882A8B0 (op 0x042A), 0x0882A918 (op 0x055F) | Runs only for fighters (FUN_0883739C). Calls FUN_08B33CD8 → FUN_08B2E1A8: it sets bit `slot` in actor+0x6D8 and writes `{id, bit, scale, altActor}` to actor+0x5D8+16·slot. The mask is cleared every tick (FUN_08829848), so a bit is set exactly while a range on that slot runs. `scale` 0 means 1.0. |
| per frame | FUN_0884E660 (from FUN_088413D0) | For each actor and each of the 16 slots, with state at 0x09B166F0 + 0x48·actor: {active mask, script id at spawn, 16 handles}. |
| spawn | FUN_088A9E24 → FUN_088A9E60 → FUN_088AA6F8 with filter 0x0884E888, **max count 1** | Walks the actor's FEPA records (0x104 B each) and instances the **first** that passes the filter (FUN_088AA4E0). The instance is then scaled by `scale` (FUN_088A72E8), and the slot stores the current script id (actor+0x180). |
| filter | 0x0884E888 | `rec+0x24 == id`, `rec+0x34 == 0`, `rec+0x44 & (1 << bit)`, and the bone named at rec+0xA8 is a visible draw part (actor+0x4C8 vmethod +0x28, then FUN_0886CC38). With 0x055F the bone and effect set come from the alternate actor. |
| stop | 0x0884E700–E748, 0x0884E7F8–E830 | When the bit is gone, or the script id differs from the stored one: `h+0xDC |= 0x100` (from now on the effect runs on the global clock, so hit-stop no longer freezes it, A §3), then FUN_088A6AA4 (the normal stop: the body's `d` mode applies), and the slot is freed. |
| EX Burst kill | battle overlay FUN_08A4F168, the first method of the actor-state vtable 0x08A6DC40 (its FUN_08A4F23C returns script 8000 or 8102) | FUN_088AA114(effect set, **0x22B8 = 8888**) → FUN_088AAF08: every live instance whose FEPA id is 8888 gets FUN_088A6A78 (state 2, gone this frame). This is the only place in the code where the constant 8888 appears. |

So an 8888 record is started, placed and ended exactly like a 9999 trail (E §3). The differences are in the data: what
the effect body draws (meshes, flipbooks, particles, ch20 scrolling meshes, sometimes ch68/80 ribbons, never ch105/106),
and the EX Burst kill.

### 1.2 Placement and lifetime (confirmed)

* Every retail 8888 record has a model name (`p_xxx100_1p`) and a weapon or hand bone. It is therefore attached, and
  follows the bone every frame (A §1.3). The attach bits are 0x80, or 0x180 (0x80 cancels the 0x100 "end on motion
  change").
* FEPA pos, rot and scale place it on the bone. Examples: Cloud `SWORD_LIGHT`: pos (0.1, 0.05, 0), rot z −90°, scale 1.2.
  Kuja `WEP_GLOW` (thr100): scale (0.5, 0.82, 0.5). Then × `arg3` (always 0 in retail, i.e. 1.0).
* FEPA lifetime (+0x30) is 0 on every 8888 record. The body is `a = 2` (sustained) with `c = 0`, except gst130's
  0x02000000, which is effectively never. **The glow lasts exactly as long as the range** (union of the ranges on the
  slot, §1.4), and then ends by the body's `d`: 3 on 39 of the 44 records, 1 on eht100 / nin100 FINISH_GLOW (15-frame
  fade), 2 on nin100 FINISH_GLOW_DB (vanishes).
* Ranges ending at 65535 (one third of the uses) end with the motion: the script id changes, and the game stops the
  effect.
* Only the first matching record is used. For example, Terra (six100) records #84 (r_hand) and #85 (l_hand) both carry
  bit 3; only #84 can spawn, unless r_hand is hidden.

### 1.3 Retail data

44 records with id 8888 in the 14 FEPAs dumped; uses in the 31 timelines: 585 ranges, 30 fighters (`record: 8888` in
`bone_effect`).

| char | rec | bit | bone | effect | body (a/dur/d, channels) |
|---|---|---|---|---|---|
| sev100 Cloud | 8 | 0 | r_wep | SWORD_LIGHT | 2/16/3: ch10 flipbooks |
| sev100 | 38 / 40 | 1 / 2 | r_wep | ULTIMA_AURA / FAST_GLOW | 2/50/3: ch10, 20, 18 |
| one100 WoL | 8 | 0 | r_wep | RADIATION_SWORD | 2/60/3: ch11 meshes |
| one100 | 68 | 1 | l_wep | SHIELD_GLOW | 2/130/3: ch11 |
| thr100 Kuja | 96 / 97 / 98 | 0 / 1 / 2 | r_wep / l_wep / r_wep1 | WEP_GLOW | 2/60/3: ch10, 20, 61+62 |
| six100 Terra | 6 / 7 | 0 / 1 | r_hand / l_hand | FINISH_GLOW_SIX100 | 2/36/3: ch21, 68+79+80 ribbon |
| nin100 Zidane | 11 / 12 / 13 | 0 / 1 / 2 | r_wep / l_wep / r_wep | FINISH_GLOW(_DB) | 2/30/1 and 2/30/2 |
| two100 Firion | 76…152 | 5…13 | r_wep3, l_wep3, r_wep, r_wep2, l_wep, r_wep4, l_wep2 | KNIFE / SWORD / SPEAR_FINISH | 2/120/3: ch11, 20 |

Timeline patterns:
* Motion 042 (scripts 813 / 821, the Chase strike) runs `{1, 8888, 0|1, 0}` over 18–54 for almost every fighter, with
  the pad mode picking the normal or the EX Mode glow (Cloud: bit 0 normal, bit 1 EX).
* The EX Burst and finish motions (8000–8110, 10090…) hold bits for 0–65535.
* Cloud 066 (slots 2/3, bit 5, ticks 140–142 and 160–168) shows short glows on extra slots that run in parallel.

### 1.4 Studio: placing an 8888 glow

Replace the `a[1] === 9999` branch in `fxInstances`. It covers both ids, plus the slot rule.
```js
} else if (c === 0x042A) {
  // bone_effect {slot, id, bit, scale}: id 9999 = LOCUS trails, 8888 = weapon glows; the code is the same (G §1.1)
  const id = a[1], bit = a[2] || 0;
  const cand = (fx.byId.get(id) || []).filter((r) => r.trig === 0 && ((r.keys >>> bit) & 1));
  // FUN_0884E888: the FIRST record whose bone is a visible draw part; none -> nothing spawns
  const pick = (ctx && ctx.hasBone ? cand.filter((r) => ctx.hasBone(r.bone)) : cand).slice(0, 1);
  if (pick.length) out.push({ type: 'fx', tick: e.start, end, recs: pick, kind: 'ranged', idx: i, slot: (a[0] || 0) & 15,
    scale: typeof a[3] === 'number' && a[3] > 0 ? a[3] : 1, trail: true /* place on rec.bone (E §3) */ });
}
```
At the end of `fxInstances`, merge the ranges per slot. The game keeps **one** instance per slot while any range on it
runs: overlapping or touching ranges (Noctis 052/053) continue one effect, with the record of the range that started it.
```js
const slots = new Map();
for (const x of out.filter((x) => x.slot != null).sort((p, q) => p.tick - q.tick || p.idx - q.idx)) {
  const prev = slots.get(x.slot);
  if (prev && (prev.end == null || x.tick <= prev.end)) {           // still active -> extend, keep prev's record
    prev.end = prev.end == null || x.end == null ? null : Math.max(prev.end, x.end); x.merged = true;
  } else slots.set(x.slot, x);
}
return out.filter((x) => !x.merged);
```
Nothing else changes. `fxRunInstance` already ends a sustained body at `inst.end` through its `d` mode, and `fxPlacer`
attaches to `rec.bone` (rec.model is set on every 8888 record). If the preview ever runs across an EX Burst start
(scripts 8000 / 8102), kill all live 8888 instances there with no fade.

---

## 2. Channel 111: the screen copy redrawn shifted

### 2.1 Methods (type 65, vtable 0x089DBE50)

| m | address | does |
|---|---|---|
| m0 | 0x0895A6C8 | Allocates a 16-byte object (FUN_0895C2BC, ctor FUN_0895C170 → FUN_0895B1C4: `{strip0, strip1, index, vtbl}`). Then calls **FUN_0895B244** (shared with ch110), which builds two 10-vertex strips `{f32 u, v, x, y, z}` covering the screen in 4 columns: x = 0…S step S/4, y = 0 or the screen height, z = 128. |
| m1 | 0x0895A758 | no-op |
| m2 | 0x0895A760 → FUN_0895C200 | `screenCopy+64 = (P+13 == 1)` (feedback). Rewrites the UVs of `strip[index]`: u = 0, ¼, …, 1 (`W/W`·¼ steps, i.e. the whole copy), v = 0 / 1. The ch110 strips are not rescaled. |
| m3 | 0x0895A7B0 → **FUN_0895C37C** | Draw (below). |
| m4 | 0x0895A800 | Releases the object. |
| m5 | 0x0895A854 | Returns 1. |

Track links (e.g. for200 node 4): `[1, 3, 4, 5]` → **dep[0] = rotation (ch4), dep[1] = colour (ch17), dep[2] = flags
(ch14), dep[3] = scalar (ch109)**.

### 2.2 Draw FUN_0895C37C (confirmed, except the buffer meaning)

1. Saves the depth-test state. Disables DEPTH_TEST (1), SCISSOR (2) and CULL (5).
2. Blend from **P+12**, with the shared switch (F summary):
   0 = (ADD, DOUBLE_SRC_ALPHA, 1−DOUBLE_SRC_ALPHA), 1 = (ADD, 2·As, FIX 0xFFFFFF), 2 = (REVERSE_SUBTRACT, 2·As, FIX 0xFFFFFF).
3. Enables TEXTURE_2D. **P+15** ≠ 0 selects a bilinear filter.
4. `sceGuColor(clamp(dep[1].colour × instance colour, 0, 255))`, packed ABGR.
5. FUN_0896FBF4: view = identity, projection = ortho mapping x 0…480 → −1…1 and y 0…272 → +1…−1 (pixels, y down).
6. Builds the offset:
   * `a = dep[0].rot.z · π/180` (state +8, degrees).
   * `R = Rz(a)` from `vrot` (columns (cos, sin), (−sin, cos)) × the constant identity at 0x089DA020.
   * `v = R · (P+0, P+4, 0, 1)` = `(cos a·P0 − sin a·P4, sin a·P0 + cos a·P4)`.
   * `V = normalize(v) · value(109)` (FUN_0895B774 reads the 109 object's value).
7. Model matrix = identity with translation `(V.x, V.y, 0)`.
8. Draws `passes = P+14` times `sceGuDrawArray(TRIANGLE_STRIP, 0x183, 10, strip[index])`, textured with the screen copy
   (FUN_0897371C). If **P+13** = 1, the texture comes from buffer +8 and is re-bound before every pass; otherwise it comes
   from +12 and is bound once. These are the same two buffers as ch110: +12 = the copy of the scene, +8 = the live frame
   (strong, F §110). FUN_08973714 is an empty function.
9. `index ^= 1`. Restores the matrices (FUN_0896FBC0) and the depth test.

So pixel `p` of the result shows the frame at `p − V`, blended over the frame. There is no rotation or zoom of the image.

### 2.3 Parameters (18 B)

| off | type | meaning | evidence |
|---|---|---|---|
| 0 | f32 | direction x (screen pixels; only the direction counts) | confirmed |
| 4 | f32 | direction y (screen y points down) | confirmed |
| 8 | f32 | 128 in data; not read by any 111 method (the strip z is the constant 128 from FUN_0895B244) | confirmed (unread) |
| 12 | u8 | blend 0/1/2 | confirmed |
| 13 | u8 | feedback: re-read the live frame before each pass | confirmed (flag), strong (meaning) |
| 14 | u8 | passes | confirmed |
| 15 | u8 | bilinear filter | confirmed |
| 16..17 | – | 0 | – |

Strength = the ch109 value (`value + speed·t`, F §109) in **pixels**. Sign matters: a negative value flips the direction.

Retail (4 nodes; 2 distinct blocks):

| effect | dir | 109 value / speed | rotation | colour (ch17 alpha) | result |
|---|---|---|---|---|---|
| for200 Golbez `GOL_CHOU_METEO_UP` node 4 | (0, −1) | −4 / 0 | 0 | 30 → 64 over 300 frames | the frame shifted 4 px **down**, alpha-blended at ≈ 0.24–0.5 |
| gst200 Gabranth `EX_ATTACK03_ATTACK_LAST` node 5 | (−0.8, −0.1) | 5 / 0 | 0 | 2.5 → ≈ 60 within 10 frames | shifted by (−4.96, −0.62) px |

Blend 0, passes 1, feedback 0, bilinear 1 in both.

### 2.4 Studio: `fxRotBlur` and its post pass

In `fxDrawElement`: `else if (ch === 111) fxRotBlur(ctx, el, tr, sg);`
```js
function fxRotBlur(ctx, el, tr, sg) {                      // ch111: the frame redrawn shifted by V pixels (G §2)
  const { fep, node } = el, o = sg.o, dep = (c0, c1) => tr.deps.find((d) => { const x = node.tracks[d]; return x && x.chan >= c0 && x.chan <= c1; });
  const d109 = dep(109, 109); if (d109 == null) return;
  const s9 = fxSeg(node.tracks[d109], Math.floor(el.g)); if (!s9) return;
  const s = fxF(fep, s9.o) + fxF(fep, s9.o + 4) * (el.g - s9.es);          // ch109 value = pixels
  const dr = dep(4, 6), rz = dr != null ? ((fxVal(el, dr) || { v: [0, 0, 0] }).v[2] || 0) * FX_D2R : 0;
  const C = fxElColour(el, fxColDep(el, tr)).map((c, i) => Math.min(255, Math.max(0, c * el.I.colour[i]))); if (C[3] <= 0) return;
  const x = fxF(fep, o), y = fxF(fep, o + 4), vx = Math.cos(rz) * x - Math.sin(rz) * y, vy = Math.sin(rz) * x + Math.cos(rz) * y;
  const L = Math.hypot(vx, vy); if (!(L > 0) || !s) return;
  FXV.post.push({ kind: 'shift', d: [vx / L * s, vy / L * s], rgba: C, blend: fxB(fep, o + 12),
    passes: Math.max(1, Math.min(4, fxB(fep, o + 14))), feedback: fxB(fep, o + 13) === 1 });
}
```
In `fxRender`, create the material next to `zoomMat`. It uses the same colour convention as the zoom pass:
```js
FXV.shiftMat = fxPostMat(`uniform sampler2D src; uniform vec2 off; uniform vec4 col; uniform float aMul; varying vec2 vUv;
  void main() { vec2 uv = vUv - off;                        // the quad is moved by +V, so pixel p samples p - V
    if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0) discard;
    vec3 rgb = texture2D(src, uv).rgb * min(vec3(1.0), 2.0 * col.rgb); float f = min(1.0, aMul * col.a); gl_FragColor = vec4(rgb * f, f); }`,
  { src: { value: null }, off: { value: new THREE.Vector2() }, col: { value: new THREE.Vector4() }, aMul: { value: 2 } });
```
Then add a case to the `FXV.post` loop:
```js
} else if (p.kind === 'shift') {
  const base = src;                                          // feedback 0: every pass samples the same copy
  for (let i = 0; i < p.passes; i++) {
    const dst = src === FXV.rt ? FXV.rt2 : FXV.rt;
    R.setRenderTarget(dst); FXV.quad.material = FXV.copyMat; FXV.copyMat.uniforms.src.value = src.texture; R.render(FXV.postScene, FXV.postCam);
    const m = FXV.shiftMat; m.uniforms.src.value = (p.feedback ? src : base).texture;
    m.uniforms.off.value.set(p.d[0] / 480, -p.d[1] / 272);   // PSP pixels -> UV; GL v is up, PSP y is down
    m.uniforms.col.value.set(p.rgba[0] / 255, p.rgba[1] / 255, p.rgba[2] / 255, p.rgba[3] / 255); blendOf(m, p.blend);
    FXV.quad.material = m; R.render(FXV.postScene, FXV.postCam); src = dst;
  }
}
```
Then remove 111 from `FX_NOT_DRAWN`.

---

## 3. Line and point emitters

### 3.0 Common to 95, 100, 102, 103 (confirmed)

* **Update gate.** m2 is skipped while `ctx+0 & 1` or `instance+12 ≠ 0` (pause). So emission and motion advance once per
  tick.
* **Emission count** (all four, same code):
  ```
  acc += rate·dt
  if acc < 1:  n = 0
  else:        n = trunc(acc);  acc -= n;  acc -= pause·rate
  ```
  `pause` is a u8 parameter, so a burst is followed by `pause` empty frames (100 N_HIT: rate 7 and pause 60 = one burst
  of 7). New elements take the first free slot of a fixed pool. The pool size comes from the init block, and nothing
  spawns when it is full.
* **Life and fade.** `life = L + trunc(rand(±Lr))`. Each update does `life −= 1`; at 0 the element is dead. The element
  is therefore drawn for life − 1 ticks.
  * 95, 102, 103: while `life < floor(L/2)`, `alpha −= 2/L` per tick, clamped at 0.
  * 100: while `life < floor(L/2)`, `alpha −= dt/floor(L/2)`.
  * In both cases the fade covers the last ≈ L/2 ticks, based on the parameter `L`, not the randomised life.
* **Draw.**
  * Model matrix = FUN_08946AC4(element, ch14 bytes, scale 1.0), the element world matrix with the ch14 billboard mode.
    The view becomes view × model (FUN_0896FB5C).
  * TEXTURE_2D off.
  * Blend: shared 0/1/2 switch (FUN_08983DB4 → FUN_08983CD8).
  * Vertex type 0x19C (u32 ABGR + f32 xyz). **Positions are written unchanged** (FUN_089A34C0 is a plain copy).
* **Colour.** `K = colour(dep[0]: ch15/17) × instance colour`.
  * 100 and 95: K is clamped to 0..255 and divided by 255.
  * 102 and 103: `K ⊙ colX/128` is clamped to 0..255 and divided by 255.
  * Then `alpha ×= element alpha`, and the vertex is `saturate(… ⊙ pointColour) · 255`.
  * There is no FRAGMENT_2X, but the blend factors use 2·alpha.
  * In the studio's shader: `C = (0.5, 0.5, 0.5)`, `lit2 = 1`, `A2 = 1` passes the vertex colour through, and `aMul`
    (set by `fxBlend`) doubles the alpha.

### 3.1 Channel 100: spark streaks (type 91, vtable 0x089DCEF8; class vtable 0x089DD230)

m0 0x0897DC04 builds the object (ctor FUN_08984DB8): `maxSparks = P+60` sparks of 80 B, each with `n = P+66` points of
32 B (`{vec4 colour, vec4 pos}`). m0 fails when `P+62 = 0`. Every m2 / m3 call repacks the parameters with FUN_0898514C,
then:
* m2 → FUN_08984508: `time += dt`; emit (FUN_08984574); update (FUN_08984B40, which calls the per-spark FUN_089850A4).
* m3 → class vt+16 = FUN_08984DEC: draw.

| off | type | meaning | evidence |
|---|---|---|---|
| 0 | vec4 | point colour at the head (128 = 1.0) | confirmed |
| 16 | vec4 | point colour toward the tail; point k gets `P0 + (P16 − P0)·k/n` | confirmed |
| 32 | f32 | speed v0 (m/tick) along the direction | confirmed |
| 36 | f32 | ± speed | confirmed |
| 40 | f32 | acceleration along the direction (m/tick²) | confirmed |
| 44 | f32 | ± acceleration | confirmed |
| 48 | f32 | gravity: subtracted from the local y acceleration | confirmed |
| 52 | f32 | rate (sparks per tick) | confirmed |
| 56 | f32 | cone angle in degrees; yaw and pitch each uniform in ±angle/2, wrapped into ±180° | confirmed |
| 60 | u16 | max sparks (pool, from the init block) | confirmed |
| 62 | u16 | life L | confirmed |
| 64 | u16 | ± life | confirmed |
| 66 | u8 | points per spark n (history length, from the init block) | confirmed |
| 67 | u8 | pause after a burst (frames) | confirmed |
| 68 | u8 | blend 0/1/2 | confirmed |

**Emission (FUN_08984574).** For each new spark:
* `a1 = rand(±h)`, `a2 = rand(±h)`, with `h = P56·π/360`.
* Direction `d = Rx(a2)·Rz(a1)·(0, 1, 0) = (−sin a1, cos a1·cos a2, cos a1·sin a2)`.
* `v0 = d·(P32 ± P36)` (spark+0) and `a = d·(P40 ± P44)` (spark+16).
* Start `s = v0·rand()` (spark+32: up to one tick of travel along the ray).
* `life` at spark+48/50, alpha 1 at spark+52, alive at spark+56, `born = time` at spark+60.
* All n points are set to position `s`, with colours `P0/128 + k·(P16 − P0)/(128·n)`.

**Update (FUN_08984B40).** If dt ≠ 0, for each live spark: `life −= 1` (death at 0), then the history shifts,
`pt[k] = pt[k−1]` for k = n−1…1. Then for each live spark, with `t = time − born` and `T = 1 + 2 + … + ⌊t⌋ = t(t+1)/2`:
`pt[0] = s + v0·t + (a − (0, P48, 0))·T`, then the alpha fade of §3.0.

**Draw (FUN_08984DEC).** `sceGuDrawArrayN(GU_LINE_STRIP, 0x19C, n, liveSparks, verts)`. Vertex k:
`saturate(K/255 ⊙ ptColour_k)`, alpha × spark alpha.

Retail (357 nodes, 30 fighters):

| effect | head → tail colour | v0 ± | accel | grav | rate / pause | cone | max / life ± / n | blend |
|---|---|---|---|---|---|---|---|---|
| one100 `N_HIT` n2 | (154,157,124,128) → (153,72,1,0) | 0.25 | 0 | 0.0025 | 7 / 60 | 260 | 7 / 12 / 3 | add |
| one100 `F_HIT` n3 | (176,172,128,128) → (127,128,70,51) | 0.3 | 0 | 0.003 | 20 / 60 | 260 | 20 / 15 / 3 | add |
| one100 `THUNDER_HIT` n7 | white | 0.2 ± 0.15 | 0 | 0 | 8 / 1 | 360 | 20 / 16 / 2 | add |
| one200 `NM_HIT_ONE200_1` n5 | (255,150,100,128) → (100,60,0,64) | 0.02 ± 0.01 | 0 | 0 | 10 / 2 | 60 | 10 / 30 / 2 | add |

### 3.2 Channel 102: converging line sparks (type 92, vtable 0x089DCA98)

The state is `{ptr {lines, count}, f32 acc}`, with lines of 96 B (`A` +0, `B` +16, `vel` +32, `acc` +48, `life` +64,
`half` +66, `alpha` +68, `fade` +72, `D` +76, `alive` +80). m0 0x08974AC0 fails when `P+66 = 0`. m2 = 0x08974DCC (emit,
then update). m3 0x08974C88 → **FUN_089C2E10**(obj, blend P+71, colA P+0, colB P+16, model, dep 0).

| off | type | meaning | evidence |
|---|---|---|---|
| 0 | vec4 | colour at the head A (128 = 1.0) | confirmed |
| 16 | vec4 | colour at the tail B | confirmed |
| 32 | f32 | D: maximum line length (m) | confirmed |
| 36 | f32 | speed toward the centre (m/tick; negative = outward) | confirmed |
| 40 | f32 | ± speed | confirmed |
| 44 | f32 | acceleration toward the centre | confirmed |
| 48 | f32 | ± acceleration | confirmed |
| 52 | f32 | shell radius min | confirmed |
| 56 | f32 | shell radius max | confirmed |
| 60 | f32 | rate | confirmed |
| 64 | u16 | max lines (pool) | confirmed |
| 66 | u16 | life L | confirmed |
| 68 | u16 | ± life | confirmed |
| 70 | u8 | pause | confirmed |
| 71 | u8 | blend | confirmed |

**Emission.**
* `u = normalize(rand(±1), rand(±1), rand(±1))`.
* `A = B = u·(r0 + rand()·(r1 − r0))`.
* `in = normalize(0 − A)`, `vel = in·(P36 ± P40)`, `acc = in·(P44 ± P48)`, `D = P32`.

**Update, each tick.** First `life −= 1` and the fade. Then:
```
if |A| <= |vel|:   D -= |vel| - |A|;  A = 0;  if D <= 0: dead      // the head arrived: the streak shrinks into the centre
else:              A += vel
if D < |A - B|:    B = A + normalize(B - A)·D                       // the tail follows at most D behind
vel += acc
```

**Draw.** `sceGuDrawArray(GU_LINES, 0x19C, 2·live)`: (A, colour A) and (B, colour B), each `saturate(K ⊙ col/128 / 255)`,
alpha × line alpha.

Retail (16 nodes, charge auras):

| effect | head → tail | D | speed / acc | shell | rate | max / life | blend |
|---|---|---|---|---|---|---|---|
| one200 `FIRE_EISYO1` n6 | (200,180,128,128) → (128,128,0,0) | 0.5 | 0.1 / 0.01 | 1.5–2.4 | 2 (1 in set 3, 0 in set 4) | 12 / 10 | add |
| one200 `EX_TAME` n0 | (255,80,100,200) → (255,0,0,0) | 1.0 | 0.2 / 0 | 1.5–5 | 1 (0 later) | 20 / 20 | add |
| sev110 `EX_AURA_GLOW` n2 | (100,200,250,128) → (128,128,128,0) | 1.0 | 0.4 ± 0.2 / 0 | 5–12 | 2 | 30 / 15 | add |

### 3.3 Channel 103: box streaks (type 93, vtable 0x089DCB38)

These are the same 96-byte records as 102. m0 0x08975DDC fails when `P+46 = 0`. m2 = 0x089760E8. m3 0x08975FA4 →
**FUN_089C30F4**(obj, blend P+63, colHead P+80, colTail P+96, model, dep 0). It is the same draw as 102: GU_LINES with
head A and tail B.

| off | type | meaning | evidence |
|---|---|---|---|
| 0 | vec3 | velocity (applied as x, −y, −z) | confirmed |
| 12 | f32 | D: maximum length | confirmed |
| 16 | vec3 | ± velocity per axis | confirmed |
| 28 | f32 | rate | confirmed |
| 32 | vec3 | acceleration (x, −y, −z) | confirmed |
| 44 | u16 | max lines (pool) | confirmed |
| 46 | u16 | life L | confirmed |
| 48 | vec3 | ± acceleration | confirmed |
| 60 | u16 | ± life | confirmed |
| 62 | u8 | pause | confirmed |
| 63 | u8 | blend | confirmed |
| 64 | vec3 | spawn box size (full; spawn in ±size/2, not flipped) | confirmed |
| 76 | – | 0xCD pad | data |
| 80 | vec4 | head colour | confirmed |
| 96 | vec4 | tail colour | confirmed |

**Emission.**
* `A = B = (rand(±P64/2), rand(±P68/2), rand(±P72/2))`.
* `vel = (P0 ± P16, −P4 ± P20, −P8 ± P24)`.
* `acc = (P32 ± P48, −P36 ± P52, −P40 ± P56)`.
* `D = P12`.

**Update.** Life and fade, then `A += vel; vel += acc;`, then the tail clamp of 102. There is no centre rule.

Retail (13 nodes):

| effect | box | vel ± | D | rate | max / life ± | head → tail | blend |
|---|---|---|---|---|---|---|---|
| one100 `LIGHT_WAVE_A` n5 | 0.2 × 30 × 0.2 | 0 ± (0, 0.2, 0) | 15 | 7 (0 in set 2) | 50 / 15 ± 5 | (128,128,255,128) → (0,0,153,37) | add |
| for110 `EX_SKY_01` n3 | 4 × 15 × 4 | (0, 1 → −y, 0) ± 0.5 | 10 | 4 | 200 / 10 ± 10 | (128,128,255,50) → (0,0,153,0) | add |
| for200 `TOUKA_L_BODY_BOTTOM` n11 | 0.05 × 8 × 0.05 | ± 0.1 y | 15 | 7 | 20 / 15 ± 5 | (200,150,98,128) → (150,64,0,37) | add |

### 3.4 Channel 95: bouncing points (type 86, vtable 0x089DD038)

Records are 64 B: pos +0, dir +16, speed +32, dSpeed +36, alpha +40, fade +44, alive +48, life +50, half +52. m0 0x0897EF5C
(pool `P+32`) fails when `P+34 = 0`. m2 = 0x0897F3E4. m3 = 0x0897F11C.

| off | type | meaning | evidence |
|---|---|---|---|
| 0 | vec3 | box size (full). Spawn at rand(±size/2); walls at ±(size/2 + 0.0001) | confirmed |
| 12 | f32 | speed (m/tick) | confirmed |
| 16 | f32 | ± speed | confirmed |
| 20 | f32 | speed change per tick | confirmed |
| 24 | f32 | ± speed change | confirmed |
| 28 | f32 | rate | confirmed |
| 32 | u16 | max points (pool) | confirmed |
| 34 | u16 | life L | confirmed |
| 36 | u16 | ± life | confirmed |
| 38 | u8 | pause | confirmed |
| 39 | u8 | blend | confirmed |

**Emission.** `pos = rand(±size/2)` per axis, `dir = normalize(rand(±1)³)`, `speed = P12 ± P16`, `dSpeed = P20 ± P24`.

**Update.** Life and fade. Then the point moves `speed` along `dir`. When the step crosses a wall, it stops there, `dir`
on that axis is mirrored, and the rest of the distance continues; this repeats while distance is left. Then
`speed += dSpeed`.

**Draw.** `sceGuDrawArray(GU_POINTS, 0x19C, live)`: 1-pixel points, colour `saturate(K/255)`, alpha × point alpha.

Retail: 1 node, one100 `SHIELD_HIT` n4. Box 1 × 1 × 1, speed 0.02 ± 0.01, no speed change, rate 10 (0 in set 2), 40 points,
life 40 ± 3, additive.

### 3.5 Channels 82 and 58 (one node each)

**82: ribbon along a path** (type 80, vtable 0x089DCE58; strong for the layout, guess for the geometry variants). Only
use: Zidane (nin100) `EX_FINISH_KISEKI` n0, links (17, 14, 70).
* m0 0x0897CEA8 builds a 0x100-byte ribbon object (FUN_08985B58). FUN_08986F0C copies the parameters, and
  FUN_089871E4 binds the path (dep 2 = ch70).
* The texel origin is randomised when `P+140` is set.
* m1 0x0897D0B4 re-rolls the scroll jitter at every segment.
* m2 0x0897D1A0 scrolls the UV by `(P+96 + jit_u, P+100 + jit_v)` per tick (FUN_089860EC).
* m3 → FUN_0898620C:
  * texture `P+76` from the instance TEX table, bound by FUN_08973334;
  * texture offset 0, CULL off, blend `P+145`, TEXTURE on;
  * three vertex builders chosen by `P+144` and `P+112`: FUN_0898658C (`P+144` = 0), FUN_089866C8/08986A58
    (`P+144` ≠ 0 and `P+112` = 0, the retail case), FUN_08986834/08986B00;
  * drawn as an indexed TRIANGLE_STRIP (0x119C / 0x119F).
* The default builder FUN_08987848 makes the strip camera-facing (`vcrsp` of the path tangent and the direction to the
  camera, × half-width `P+108`). The colour builder FUN_08987E60 gives corner `(side s, end e)` the colour
  `clamp(P[16·s] ⊙ P[32 + 16·e] / 128 / 128 ⊙ K, 0, 255)`, with sides P+0 / P+16 and ends P+32 (head) / P+48 (tail).

| off | type | meaning | evidence |
|---|---|---|---|
| 0, 16 | vec4 | colour of side A / side B (128 = 1) | strong |
| 32, 48 | vec4 | multiplier at the head / tail end (128 = 1) | strong |
| 64 | vec3 | → obj+144 (0 in data) | unread |
| 76 | s32 | TEX index | confirmed |
| 80, 84 | f32 | random u / v start range (texels), used when P+140 | confirmed |
| 88, 92 | f32 | ± scroll jitter u / v (per segment) | confirmed |
| 96, 100 | f32 | scroll u / v per tick (texels) | confirmed |
| 104 | f32 | 50 (→ obj+224, default 100: a percentage) | unread |
| 108 | f32 | half-width (4 in data) | strong |
| 112..120 | f32 ×3 | ×0.01 → obj+160..168 (0, 15, 5) | strong (scaling) |
| 124, 128 | f32 | 30, 1.0 | unread |
| 132..138 | u16 ×4 | texel rect u0, v0, w, h (0, 0, 80, 64) | confirmed |
| 140 | u8 | randomise the texel start | confirmed |
| 143 / 144 / 145 / 146 / 147 | u8 | obj+176 / builder mode / **blend** / obj+243 / obj+179 | strong for 145 |

**58: camera-faded flipbook sprites** (type 47, vtable 0x089DBDB0; strong for the layout, guess for the motion). Only use:
Lightning (gst130) `RAIKOU_FINISH` n0, links (16, 14).
* m0 0x0895A488 → FUN_0895FD0C creates `count = P+132` sub-objects of 108 B (FUN_0898D604). Each gets a random flip from
  `P+145`: 0 none, 1 U 50 %, 2 V 50 %, 3 any.
* FUN_089600A0 places the sprites.
* m2 0x0895A520 → FUN_0895FE04: every `P+136` ticks while `P+134` ≠ 0, the sprites are re-placed (FUN_089600A0). Each
  tick they move (FUN_0898D824):
  * `pos(+32) += normalize(P+0..8)·speed` (y and z negated), and `speed(+48) += P+120`;
  * size `(+52,+56) += (+60,+64)`, and `(+60,+64) += (P+64, P+68)`;
  * colour `(+0) += (+16)`, and `(+16) += P+32..44`;
  * the alpha fades with the distance to the camera (the camera matrix translation): it ramps in over
    [P+88, P+88 + P+92] and out over [P+96, P+96 + P+100].
* m3 0x0895A548:
  * TEXTURE on, texture offset 0, DEPTH_TEST off, CULL off;
  * **view = identity**; each sprite's matrix is built by FUN_0898DD14 (vrot billboard, ±0.1 jitter);
  * the ANM flipbook `P+80` (instance ANM table);
  * then model = identity and the depth test back on.
* Data: count 10, re-placed every tick, ANM 1, fades near 400 / 300 and far 1500 / 300, `P+48` = `P+52` = 1.0.

### 3.6 Corrections to `fxSparks` (current fx.js)

* **100.**
  * The tail colour is `P+16`, not the head with alpha 0.
  * `P+40/44` is the acceleration (missing), and `P+67` is the pause (missing).
  * The cone is around **+Y** (`d = (−sin a1, cos a1·cos a2, cos a1·sin a2)`), not +Z.
  * Gravity is discrete, `t(t+1)/2`, subtracted from local y.
  * Points k ≥ 1 are the head positions k ticks ago, clamped to the start point.
  * The colour step is `k/n`.
  * Positions are not flipped.
* **102.**
  * `P+36` is the speed (not a length) and `P+44` the acceleration.
  * `P+32` is the maximum length, and `P+52/56` the spawn shell.
  * Lines converge on the centre and die there.
  * The blend is `P+71`, not fixed at 1.
* **103.** It has its own layout (§3.3); it is not 102's.

### 3.7 Studio: draw functions

Helpers, placed next to `fxSparks`:
```js
/* line / point components: element-local vertices through the element matrix (08946AC4, scale 1, ch14 billboard), like MDL */
function fxPrimWorld(ctx, el, tr) {
  const t14 = fxCh14(el, tr), b14 = fxCh14Bytes(el, t14);
  return fxDrawMat(ctx, fxElBase(el, ctx.tick), fxElMat(el, t14), 1, b14[1], el.I.iscale);
}
const fxPrimK = (el, tr) => fxElColour(el, fxColDep(el, tr)).map((c, i) => c * el.I.colour[i]);   // node colour × instance (0..255)
const fxSat = (x) => Math.min(1, Math.max(0, x));
const fxV3 = { add: (a, b) => [a[0] + b[0], a[1] + b[1], a[2] + b[2]], sub: (a, b) => [a[0] - b[0], a[1] - b[1], a[2] - b[2]],
  mul: (a, k) => [a[0] * k, a[1] * k, a[2] * k], len: (a) => Math.hypot(a[0], a[1], a[2]),
  norm: (a) => { const l = Math.hypot(a[0], a[1], a[2]); return l > 0 ? [a[0] / l, a[1] / l, a[2] / l] : [0, 0, 0]; } };
/* acc += rate·dt; at >= 1: n = trunc(acc), acc -= n + pause·rate (95/100/102/103) */
function fxEmitN(st, rate, pause) { st.acc += rate; if (st.acc < 1) return 0; const n = Math.trunc(st.acc); st.acc -= n + pause * rate; return n; }
function fxSlot(pool, max) { const s = pool.find((x) => !x.alive); if (s) return s; if (pool.length >= max) return null; pool.push({}); return pool[pool.length - 1]; }
/* the parameter block of track tr at element age f (the emitters read the current segment every tick) */
const fxSegAt = (el, tr, f) => fxSeg(tr, Math.floor(fxElTime(el, f)));
function fxPrimDraw(ctx, key, W, P, Ca, kind, blend) {
  if (!P.length) return;
  const old = FXV.pool.get(key); if (old && old.geometry) old.geometry.dispose();
  const m = fxMesh(key, fxGeo(P, Array(P.length / 3 * 2).fill(0), Ca, null), kind), u = m.material.uniforms;
  u.map.value = null; u.useMap.value = 0; u.C.value.set(0.5, 0.5, 0.5); u.lit2.value = 1; u.A2.value = 1;   // vertex colour as is
  fxBlend(m.material, fxBlendMode(blend), 0xFFFFFF);                // aMul = 2: the GE's 2·alpha
  m.matrix.copy(W);
}
```
`fxMesh` needs a `'points'` kind: `kind === 'points' ? new THREE.Points(geo, fxNewMat()) : …`. `FX_VS` needs a
`uniform float psize;` and `gl_PointSize = psize;`, set to `max(1, canvasHeight / 272)` so a point stays one PSP pixel.

Dispatch, in `fxDrawElement`:
`else if (ch === 100) fxStreaks100(ctx, el, tr, k); else if (ch === 102 || ch === 103) fxLines(ctx, el, tr, k); else if (ch === 95) fxPoints95(ctx, el, tr, k);`.
`fxDrawElement` skips expired elements, so the emitters stop at the end of the node like the other channels.
```js
function fxStreaks100(ctx, el, tr, key) {                  // G §3.1
  const { fep } = el, rnd = fxRng(el.seed ^ 100), st = { acc: 0, time: 0 }, sp = [];
  const i0 = fxSeg(tr, 0); if (!i0) return;
  const max = Math.min(256, fxH(fep, i0.o + 60)), n = Math.max(1, fxB(fep, i0.o + 66)); if (!max || !fxH(fep, i0.o + 62)) return;
  for (let f = 0; f <= Math.floor(el.age); f++) {
    const sg = fxSegAt(el, tr, f); if (!sg) break; const o = sg.o;
    st.time += 1;
    for (let k = fxEmitN(st, fxF(fep, o + 52), fxB(fep, o + 67)); k > 0; k--) {
      const s = fxSlot(sp, max); if (!s) break;
      let h = fxF(fep, o + 56) * Math.PI / 360; while (h < -Math.PI) h += 2 * Math.PI; while (h > Math.PI) h -= 2 * Math.PI;
      const a1 = fxR(rnd, h), a2 = fxR(rnd, h), d = [-Math.sin(a1), Math.cos(a1) * Math.cos(a2), Math.cos(a1) * Math.sin(a2)];
      const acc = fxF(fep, o + 40) + fxR(rnd, fxF(fep, o + 44)), v0 = fxF(fep, o + 32) + fxR(rnd, fxF(fep, o + 36));
      const start = fxV3.mul(d, v0 * rnd()), L = fxH(fep, o + 62);
      Object.assign(s, { alive: true, d, v0, acc, start, born: st.time, life: L + Math.trunc(fxR(rnd, fxH(fep, o + 64))), half: L >> 1, a: 1,
        pts: Array.from({ length: n }, () => start.slice()),
        c0: [0, 1, 2, 3].map((j) => fxF(fep, o + 4 * j) / 128), c1: [0, 1, 2, 3].map((j) => fxF(fep, o + 16 + 4 * j) / 128) });
    }
    const G = fxF(fep, o + 48);
    for (const s of sp) {
      if (!s.alive) continue;
      if (--s.life <= 0) { s.alive = false; continue; }
      for (let j = n - 1; j > 0; j--) s.pts[j] = s.pts[j - 1];
      const t = st.time - s.born, T = t * (t + 1) / 2;
      s.pts[0] = [0, 1, 2].map((j) => s.start[j] + s.d[j] * s.v0 * t + (s.d[j] * s.acc - (j === 1 ? G : 0)) * T);
      if (s.life < s.half && s.half > 0) s.a = Math.max(0, s.a - 1 / s.half);
    }
  }
  const K = fxPrimK(el, tr).map((c) => Math.min(255, Math.max(0, c)) / 255), P = [], Ca = [];
  for (const s of sp) if (s.alive) for (let j = 0; j + 1 < n; j++) for (const q of [j, j + 1]) {   // strip -> segments
    P.push(...s.pts[q]);
    Ca.push(...[0, 1, 2, 3].map((c) => fxSat(K[c] * (c === 3 ? s.a : 1) * (s.c0[c] + (s.c1[c] - s.c0[c]) * q / n))));
  }
  const sg = fxSeg(tr, Math.floor(el.g)); fxPrimDraw(ctx, key, fxPrimWorld(ctx, el, tr), P, Ca, 'lines', sg ? fxB(fep, sg.o + 68) : 1);
}

function fxLines(ctx, el, tr, key) {                       // ch102 (G §3.2) and ch103 (G §3.3)
  const { fep } = el, is103 = tr.chan === 103, rnd = fxRng(el.seed ^ tr.chan), st = { acc: 0 }, Ls = [];
  const i0 = fxSeg(tr, 0); if (!i0) return;
  const max = Math.min(256, fxH(fep, i0.o + (is103 ? 44 : 64))); if (!max || !fxH(fep, i0.o + (is103 ? 46 : 66))) return;
  for (let f = 0; f <= Math.floor(el.age); f++) {
    const sg = fxSegAt(el, tr, f); if (!sg) break; const o = sg.o;
    const rate = fxF(fep, o + (is103 ? 28 : 60)), pause = fxB(fep, o + (is103 ? 62 : 70));
    for (let k = fxEmitN(st, rate, pause); k > 0; k--) {
      const ln = fxSlot(Ls, max); if (!ln) break;
      let A, vel, acc, D, L, Lr;
      if (is103) {
        A = [0, 1, 2].map((j) => fxR(rnd, fxF(fep, o + 64 + 4 * j) / 2));
        const sgn = [1, -1, -1];
        vel = [0, 1, 2].map((j) => sgn[j] * fxF(fep, o + 4 * j) + fxR(rnd, fxF(fep, o + 16 + 4 * j)));
        acc = [0, 1, 2].map((j) => sgn[j] * fxF(fep, o + 32 + 4 * j) + fxR(rnd, fxF(fep, o + 48 + 4 * j)));
        D = fxF(fep, o + 12); L = fxH(fep, o + 46); Lr = fxH(fep, o + 60);
      } else {
        const u = fxV3.norm([fxR(rnd, 1), fxR(rnd, 1), fxR(rnd, 1)]), r0 = fxF(fep, o + 52), r1 = fxF(fep, o + 56);
        A = fxV3.mul(u, r0 + rnd() * (r1 - r0)); const inw = fxV3.norm(fxV3.mul(A, -1));
        vel = fxV3.mul(inw, fxF(fep, o + 36) + fxR(rnd, fxF(fep, o + 40)));
        acc = fxV3.mul(inw, fxF(fep, o + 44) + fxR(rnd, fxF(fep, o + 48)));
        D = fxF(fep, o + 32); L = fxH(fep, o + 66); Lr = fxH(fep, o + 68);
      }
      Object.assign(ln, { alive: true, A, B: A.slice(), vel, acc, D, life: L + Math.trunc(fxR(rnd, Lr)), half: L >> 1, fade: 2 / L, a: 1 });
    }
    for (const ln of Ls) {
      if (!ln.alive) continue;
      if (--ln.life <= 0) { ln.alive = false; continue; }
      if (ln.life < ln.half) ln.a = Math.max(0, ln.a - ln.fade);
      if (!is103 && fxV3.len(ln.A) <= fxV3.len(ln.vel)) {    // 102: the head reached the centre
        ln.D -= fxV3.len(ln.vel) - fxV3.len(ln.A); ln.A = [0, 0, 0];
        if (ln.D <= 0) { ln.alive = false; continue; }
      } else ln.A = fxV3.add(ln.A, ln.vel);
      const BA = fxV3.sub(ln.B, ln.A), l = fxV3.len(BA);
      if (ln.D < l) ln.B = fxV3.add(ln.A, fxV3.mul(BA, ln.D / l));
      ln.vel = fxV3.add(ln.vel, ln.acc);
    }
  }
  const sg = fxSeg(tr, Math.floor(el.g)); if (!sg) return; const o = sg.o, K = fxPrimK(el, tr);
  const cols = [is103 ? 80 : 0, is103 ? 96 : 16].map((b) => [0, 1, 2, 3].map((j) => Math.min(255, Math.max(0, K[j] * fxF(fep, o + b + 4 * j) / 128)) / 255));
  const P = [], Ca = [];
  for (const ln of Ls) if (ln.alive) { P.push(...ln.A, ...ln.B); for (const c of cols) Ca.push(c[0], c[1], c[2], fxSat(c[3] * ln.a)); }
  fxPrimDraw(ctx, key, fxPrimWorld(ctx, el, tr), P, Ca, 'lines', fxB(fep, o + (is103 ? 63 : 71)));
}

function fxPoints95(ctx, el, tr, key) {                    // G §3.4
  const { fep } = el, rnd = fxRng(el.seed ^ 95), st = { acc: 0 }, ps = [];
  const i0 = fxSeg(tr, 0); if (!i0) return;
  const max = Math.min(256, fxH(fep, i0.o + 32)); if (!max || !fxH(fep, i0.o + 34)) return;
  for (let f = 0; f <= Math.floor(el.age); f++) {
    const sg = fxSegAt(el, tr, f); if (!sg) break; const o = sg.o;
    const hs = [0, 1, 2].map((j) => fxF(fep, o + 4 * j) / 2), lo = hs.map((h) => -h - 1e-4), hi = hs.map((h) => h + 1e-4);
    for (let k = fxEmitN(st, fxF(fep, o + 28), fxB(fep, o + 38)); k > 0; k--) {
      const p = fxSlot(ps, max); if (!p) break; const L = fxH(fep, o + 34);
      Object.assign(p, { alive: true, pos: hs.map((h) => fxR(rnd, h)), dir: fxV3.norm([fxR(rnd, 1), fxR(rnd, 1), fxR(rnd, 1)]),
        v: fxF(fep, o + 12) + fxR(rnd, fxF(fep, o + 16)), dv: fxF(fep, o + 20) + fxR(rnd, fxF(fep, o + 24)),
        life: L + Math.trunc(fxR(rnd, fxH(fep, o + 36))), half: L >> 1, fade: 2 / L, a: 1 });
    }
    for (const p of ps) {
      if (!p.alive) continue;
      if (--p.life <= 0) { p.alive = false; continue; }
      if (p.life < p.half) p.a = Math.max(0, p.a - p.fade);
      let rem = p.v, at = p.pos;
      for (let g = 0; g < 8 && rem > 0; g++) {               // bounce off the box walls (several per tick if needed)
        const q = fxV3.add(at, fxV3.mul(p.dir, rem)); let hit = -1, wall = 0;
        for (const [side, w] of [[0, lo], [1, hi]]) for (let j = 0; j < 3; j++) if (side ? q[j] > w[j] : q[j] < w[j]) { hit = j; wall = w[j]; }
        if (hit < 0) { at = q; break; }
        const r = Math.abs(wall - at[hit]) / Math.max(1e-9, Math.abs(q[hit] - at[hit]));
        const h = fxV3.add(at, fxV3.mul(p.dir, rem * r)); h[hit] = wall;
        rem -= fxV3.len(fxV3.sub(h, at)); at = h; p.dir = p.dir.slice(); p.dir[hit] = -p.dir[hit];
      }
      p.pos = at; p.v += p.dv;
    }
  }
  const K = fxPrimK(el, tr).map((c) => Math.min(255, Math.max(0, c)) / 255), P = [], Ca = [];
  for (const p of ps) if (p.alive) { P.push(...p.pos); Ca.push(K[0], K[1], K[2], fxSat(K[3] * p.a)); }
  const sg = fxSeg(tr, Math.floor(el.g)); fxPrimDraw(ctx, key, fxPrimWorld(ctx, el, tr), P, Ca, 'points', sg ? fxB(fep, sg.o + 39) : 1);
}
```
82 and 58 are approximations (guess, one node each). They are only worth adding for Zidane's and Lightning's finishers.
```js
// ch82: camera-facing textured ribbon along the element's own position history (the ch70 path is not ported; use ch68's
// sampled history as a stand-in), half-width P+108, colours side A/B (P+0/P+16) x end head/tail (P+32/P+48), /128/128
function fxRibbon82(ctx, el, tr, key, path /* world points, newest first */) {
  const { fep } = el, o = fxSeg(tr, Math.floor(el.g)).o, K = fxPrimK(el, tr), w = fxF(fep, o + 108) * el.I.iscale[0];   // units of P+108 unread (4 in data)
  const col = (s, e) => [0, 1, 2, 3].map((j) => Math.min(255, Math.max(0, fxF(fep, o + 16 * s + 4 * j) * fxF(fep, o + 32 + 16 * e + 4 * j) / 16384 * K[j])) / 255);
  // per path point i (k = i/(n-1)): side = normalize(cross(tangent, toCamera)) * w; vertices p ± side; colour lerp(col(s,0), col(s,1), k);
  // u along the path from the texel rect P+132..138 (+ scroll P+96/P+100 per tick), v across; blend P+145; TEX P+76
}
// ch58: P+132 flipbook sprites (ANM P+80), billboarded; alpha ramps in over [P+88, P+88+P+92] and out over
// [P+96, P+96+P+100] of the camera distance; re-placed every P+136 ticks when P+134 != 0; random flips per P+145
```

## Open points

* **111:** buffers +8 and +12 of the screen-copy object (F §110: live frame vs copy) and the copy width `W` (m2 writes UVs
  0…1 of the whole copy; if the copy is wider than 480 the game also squeezes it slightly).
* **82:** the ch70 path generator (FUN_0896BD58…) and the two retail-mode vertex builders (FUN_089866C8 / FUN_08986A58)
  are not read. **58:** the placement (FUN_0898E120 → FUN_0898E564) and the matrix builder FUN_0898DD14 are not read.
* **8888:** which actor state vtable 0x08A6DC40 is (the state entered for EX Burst scripts 8000/8102: strong). Whether
  `h+0xDC |= 0x100` changes anything besides the clock (A §3 says it selects the global time step).
