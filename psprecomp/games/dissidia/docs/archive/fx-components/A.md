# A: element core, placement, timing, colour and blending

Agent A, 2026-10-24. Source: the static recompilation (`work/output/generated/batch_*.cpp`) and the EBOOT image
(`work/eboot_image.bin`, base 0x08804000), checked against the 14 character FEPs/FEPAs in `work/fx_re/*_40.bin` /
`*_4d.bin` (1430 FEPA records, 1068 effects). Test effects: Warrior of Light `one100` P0010_P_ONE100_SS_HIT_B,
Cloud `sev100` P0071_P_SEV100_FIGA_BALL.
Scratch: `work/fx_re/scratch_A/` (`cf.py FUNC` / `cfr.py ADDR` print a recompiled function compactly with register
names; `fx_*.txt` = every function 0x0894–0x0899, `fa_*.txt` = the FEPA handle code 0x088A4000–0x088AB000;
`trk.py code NAME` dumps an effect's nodes/tracks/ParamSets; `fepa_dump.py`, `fepa_stats.py`, `leaf_stats.py`,
`mat_stats.py` = data surveys).

Labels: **confirmed** = read end to end in code; **strong** = code read, one link rests on GE hardware semantics,
on state set elsewhere or on a field name; **guess** = inferred.

Conventions used below. Matrices are written for column vectors (`v' = M·v`), which is how the game's VFPU code
works: each 16-byte row in memory is one *column* (memory vector 3 = translation). `vmmul(d, E_a, M_b)` computes
`d = a·b`. "Deg" means degrees. A three.js `Matrix4` filled with `fromArray(memory16)` is the same matrix.

## Summary

1. **World unit = metre, effect positions are metres, scales are percent.** The scale channels 7/8/9 output
   `value × 0.01` (FUN_08952A44 / FUN_08952D58 / FUN_08952DB8: `* 0x3C23D70A`). Position channels 1/2/3 are not
   scaled. So the studio's `FX_UNIT = 0.01` on *positions* is wrong; the 0.01 belongs on *scale* only. FEPA offsets
   are metres (root + 0.8–1.3 = chest height). (confirmed)
2. **Effect space is flipped against the game: Y and Z of position and rotation are negated.** Channel 14
   (the "flags" channel) is the element's **transform builder**: it reads the pos/rot/scale tracks it is linked to
   and writes the element matrix `T(px,−py,−pz)·R(rx,−ry,−rz)·S(0.01·s)` (FUN_08950F5C, FUN_08951330). (confirmed)
3. **Rotations are degrees, default order XYZ (three.js 'XYZ': M = Rx·Ry·Rz).** The order byte (ch14 param byte 3,
   instance +472) selects one of 6 orders. (confirmed)
4. **Element world = B · [k·M3 | M.t]**, where B = the effect instance's world matrix (or an emitter's second
   matrix for emitted particles), M = the element matrix, k = the drawing component's basis scale (ANM 0.1, MDL the
   resource's float +0 = 1.0). ch14 byte 1 = **billboard mode** (0 world, 1 camera-facing, 2 axis-locked),
   byte 0 = parent-inheritance mode, byte 2 = 1 → orient along velocity, bytes 4/5 = draw-order layer. (confirmed)
5. **Instance world (B)**. FEPA record with a model + bone: `B3 = Rbone·L3`, `B.t = Bone·(pos,1)`, with
   `L = Rx·Ry·Rz(rot°)·S(scale)`; bone axes normalised unless attach bit 0x8. Attach bits: 0x1 = **spawn once**
   (no follow), else follow every frame (forever, or `+0x78` frames); 0x2 = take rotation from the character root,
   0x4 = take position from the character root, 0x8 = keep bone scale, 0x40 = inherit character alpha/visibility,
   0x100 = end when the character's motion changes (unless 0x80), 0x200 = end on character release. Records without
   a model name are placed by the caller (hit point, projectile): `B = [P3·R·S | pos]`. (confirmed / strong)
6. **Timing.** 1 effect frame = 1 game tick, dt = the character's own clock (hit-stop slows/stops effects). Segments
   apply when `frame ≥ seg.start`; values extrapolate (no clamp) until the next segment. Node flag 0x2 loops the
   element between node h12 and h14; body `a` bit 2 = sustained effect (ends at body `c` or when stopped, fades
   out over 30 / 15 frames for `d` = 0 / 1, kills for `d` = 2). Node `duration` = element life. (confirmed)
7. **Colour and blend.** Effects draw with lighting on (global ambient = light-0 ambient = node colour),
   `ColorMaterial(ambient)`, texture MODULATE+RGBA, **GU_FRAGMENT_2X**, depth test on, depth write off by default,
   no alpha test, fog off. Blend comes from the MDL material / ANM part word `{u8 op, u8 src, u8 dst}` + 2 FIX
   colours: `0x000A0600` additive (2As), `0x00070600` alpha blend (2As / 1−2As), `0x000A0602` reverse subtract.
   Net: 128 = 1.0 for node colour, vertex colour 0x8 and alpha. (confirmed, GE semantics strong)
8. Measured channel formulas: all **confirmed**, with two corrections: scale outputs ×0.01, colours are clamped to
   [0, 255] every frame, and a new segment's value **replaces** the running value (plus any offset an override
   added), it does not blend.

---

## 1. Placement: FEPA record, handle, leaf, event 0x0219

### 1.1 The play chain (confirmed)
`FUN_088A9DE8(mgr, fepa_id, char+1224, 0, 0)` → `FUN_088AA5B8` scans FEPA records (+0x24 == id) → `FUN_088AA4E0`
allocates a 0x280-byte **handle** and `FUN_088A68FC` fills it from the record → `FUN_088A4D7C` →
`FUN_08941914` → `FUN_08944694` (name index +0x28 → effect id) → `FUN_089444B0` (group/leaf lookup) →
`FUN_08945BEC(mgr, pkg, body_index, leaf, t0 = 0, f12 = 0)` = new **effect instance** (inst). The leaf record is
the instance's `+464` "spawn description". The handle drives the instance through the setter API at
0x089419C8–0x08941AF0 (each builds a param block for FUN_08944930 with one flag bit):

| API | flag | instance field written by FUN_08944930 |
|---|---|---|
| FUN_089419E8 pos | 0x1 | +144 vec4 position |
| FUN_08941A40 rot | 0x2 (+0x20000) | +160 vec4 rotation, **degrees** (FUN_088A45D8 converts rad → deg) |
| FUN_08941A14 scale | 0x4 | +176 vec4 scale |
| FUN_08941A9C colour | 0x8 | +192 vec4 colour multiplier (1.0 = 1.0) |
| FUN_08941A70 parent | 0x10 | +208 64-byte parent matrix |
| FUN_08941AC8 attach matrix | 0x100 | +272 64-byte world matrix (used when attached) |
| byte +8 | 0x20 | +472 rotation order (0..5) |

### 1.2 FEPA record (260 B) (fields read by the handle code)

| off | type | meaning | code | level |
|---|---|---|---|---|
| 0x00 | f32×3 | position offset, **metres** (added to handle pos +0) | 088A82D8, 088A9024, 088A8810 | confirmed |
| 0x0C | f32×3 | rotation, **degrees** (×π/180, + handle rot +12) | same | confirmed |
| 0x18 | f32×3 | scale (× handle scale +24) | same | confirmed |
| 0x24 | u32 | id (0x0219 arg0) | 088AA5B8 | confirmed |
| 0x28 | u32 | FEP name index (and FXSE row) | 088A68FC | confirmed |
| 0x30 | s32 | **lifetime** in frames → handle+228; counts down by dt, at ≤ 0 the effect is stopped (FUN_088A4A6C → 088A6AA4 / 088A6A78). 0 = no limit. The 0x0219 event's **arg1 overwrites handle+228**, so arg1 = lifetime/kill time in frames | 088A68FC, 088A8B00 @0x088A8F9C | confirmed |
| 0x34 | u32 | trigger class: other play paths (FUN_088AA9EC / 088AABE0 / 088AADAC) pick records by +0x34 and the keys +0x38, +0x3C (bit loop), +0x44, +0x48, +0x50, +0x54, +0x58, +0x5C, +0x64. Not placement. | | strong |
| 0x70 | u32 | target: 0 = character model (bone by name), 1 = sub-object (bone index forced 0), 4 = a global object (0x09B4D440) | 088A68FC, 088A82D8 @0x088A8344 | strong |
| 0x74 | s32 | start delay in frames → handle+224, counted down before the instance is set up | 088A68FC, 088A8810 | confirmed |
| 0x78 | s32 | follow time: when attach bit 0x1 is clear and this is ≠ 0, the bone is followed for this many frames, then frozen | 088A8810 @0x088A89CC, 088A8B00 | confirmed |
| 0x7C | u32 | **attach mode bits**, see 1.3 | | |
| 0x80 | u32 | how the effect ends on a 0x200/0x100 end event: 0 = kill now (088A6C70), 1 = fade (088A6CB8 → 088A4540 with +0x84), 2 = stop emitting and let it finish (088A6AA4) | 088A7890 | strong |
| 0x84 | u32 | fade length for mode 1 | 088A7890 | strong |
| 0x88 | char[32] | model name; **attached iff non-empty** (strlen, FUN_089359E8) or +0x70 == 4 | 088A68FC | confirmed |
| 0xA8 | char[32] | bone name → bone index (char vmethod +40) → handle+244; -1 = not found → not attached | 088A68FC | confirmed |
| 0xC8 | char[32] | motion name → motion id (char vmethod +48) → handle+248 (used by bit 0x100) | 088A68FC, 088A7714 | confirmed |
| 0xE8 | f32×3 | culling-sphere centre offset (local) | 088A6DA0 | strong |
| 0xF4 | f32 | culling-sphere radius × the matrix's largest axis scale | 088A6DA0 | strong |
| 0xF8 | u32 | culling mode: 0 none; out of view → hide (1) or stop (2) | 088A6DA0 @0x088A7080 | strong |
| 0xFC, 0x100 | f32 | camera-distance alpha fade near/far (equal = off; all records have them equal) | 088A8B00 @0x088A8D94 | strong |

### 1.3 Attach mode +0x7C (bits)

| bit | effect | code | level |
|---|---|---|---|
| 0x01 | **spawn once**: matrix computed at start only (handle+252 = 0), else follows | 088A8810 @0x088A89A4, 088A8B00 @0x088A8B80 | confirmed |
| 0x02 | rotation from the character **root** matrix (unnormalised) instead of the bone | 088A9024 @0x088A9338 | confirmed |
| 0x04 | position from the root matrix instead of the bone | 088A9024 @0x088A93D4 | confirmed |
| 0x08 | keep the bone's scale (skip normalising the bone axes) | 088A9024 @0x088A9298 | confirmed |
| 0x40 | instance alpha = character alpha (char vmethod +24), visibility = character visibility (vmethod +64), every frame | 088A7664, 088A8B00 @0x088A8CE8 | confirmed |
| 0x80 | suppresses 0x100 | 088A7714 | confirmed |
| 0x100 | end the effect (per +0x80) when the character's motion ≠ the record's motion (+0xC8) | 088A7714 | strong (caller is a callback) |
| 0x200 | end the effect (per +0x80) on the character "release" callback | 088A7770 | strong |

Seen values: 0 (653), 0x102 (206), 0x100 (157), 0x80 (123), 0x103 (61), 0x180, 0x101, 0x1, 0x182, 0x3, 0x2, 0x82.
So **follows the bone every frame** unless bit 0x1 is set (0x101, 0x103, 0x1, 0x3 spawn once) or +0x78 limits it.

### 1.4 The handle (0x280 B) fields that matter (confirmed: constructor FUN_088A6690, start FUN_088A8810, per-frame FUN_088A8B00)
`+0` pos (0), `+12` rot rad (0), `+24` scale (1), `+48` parent matrix, `+112` attach matrix out, `+176` colour (1,1,1,1),
`+192` FEPA record, `+204` fx object, `+208` character, `+220` flags (bit0 = has character; 4/8/0x10/0x20/0x40 =
pos/rot/scale/parent/colour changed → re-sent next frame), `+224` delay, `+228` lifetime, `+232` distance alpha,
`+236` fixed dt (−1 = use the clock), `+240` rec+0x70, `+244` bone, `+248` motion, `+252` follow frames (−1 = forever).

### 1.5 Instance world matrix B (inst+80), computed every frame by FUN_0894622C (confirmed)
- **Attached** (inst+380 ≠ 0, set by FUN_088A4924 = skeleton + bone): `B = inst+272`, the matrix the handle sent
  (FUN_088A9024, recomputed each frame while following):
  ```
  L    = Rx·Ry·Rz(rec.rot° + h.rot) · S(rec.scale ⊙ h.scale)          // no translation
  Rsrc = (mode & 2) ? Root3 : (mode & 8 ? Bone3 : normalize_columns(Bone3))
  B3   = (Rsrc · H48)3 · L3                                            // H48 = handle+48, identity by default
  B.t  = ((mode & 4) ? Root : BoneN) · (rec.pos + h.pos, 1)            // BoneN = bone with normalised axes unless mode&8
  ```
  Root = character matrix (skeleton vmethod +8), Bone = bone world matrix (skeleton vmethod +16, bone index).
- **Not attached** (no model/bone, e.g. SS_HIT_B and FIGA_BALL): pos/rot/scale/parent come from the setters:
  `B3 = P3 · Rorder(rot° + leaf.rot) · S(scale ⊙ leaf.scale)`, `B.t = pos + leaf.pos` (P = inst+208, its
  translation is **not** applied and pos is not rotated by P; FUN_0894622C @0x08946274–0x08946370, rotation via
  FUN_08945930 with order inst+472). For these effects the studio must supply the caller's point (hit position,
  projectile) as `pos` and its facing as P.
- The handle also runs culling (+0xE8..+0xF8) and the delay (+0x74) before the instance appears.

### 1.6 Leaf record (156 B) (instance +464)

| off | type | meaning | code | level |
|---|---|---|---|---|
| 0 | u32 | effect id | 089444B0 | confirmed |
| 4 | u8 | 1 (−) ; +6 u8 orientation mode (only in the alternative update FUN_0894ABA4, 0 in all data) | 0894ABA4 @0x0894AF68 | strong |
| 7 | u8 | **fog**: ≠ 0 → GU_FOG enabled for this instance's draws (inst+480) | 08945BEC, 08949E2C @0x08949F40 | confirmed |
| 8 | u8 | spawn-attach type 1/2 with ptr +12 (always 0) | 08945BEC | confirmed |
| 10 | u8 | culling volume mode (0 none; 2/3 use +64/+80) | 0894ABA4 | strong |
| 11 | u8 | distance-fade mode (0 none; 4 = fade between +148 and +152) | 0894ABA4 | strong |
| 16 / 32 / 48 | f32×4 | local pos / rot° / scale added to (multiplied with) the instance's (0 / 0 / 1 in all data) | 0894622C, 08970D9C | confirmed |
| 64 | f32×3 | culling box/sphere size (100,100,100) | 0894ABA4 @0x0894AD84 | strong |
| 80 | f32×3 | culling centre | same | strong |
| 142 | u8 | → inst+482 | 08945BEC | − |
| 144 | u16 | start delay: instance time starts at −h144 | 08945BEC @0x08945F40 | confirmed |
| 148 / 152 | f32 | distance fade near / far (300 / 500), only with +11 == 4 | 0894ABA4 @0x0894AF00 | strong |

Because every retail leaf has pos 0, rot 0, scale 1 and modes 0, the leaf can be ignored by the studio except +7 (fog)
and +144 (delay, always 0 in the 14 characters).

---

## 2. Node / element transform

### 2.1 The element object (FUN_0894CF98, confirmed)
`+0` type 0x75, `+2` runtime flags (0x2 looped, 0x4 first-frame done, 0x8 skip, 0x10 wrap pending), `+4` node
flags, `+8` list (0/1), `+24` parent element, `+28` base element (child particles; B = base+84), `+32` child count,
`+34` node index, `+48` f32 time, `+52` life (node +4), `+54` current segment, `+60` 24-byte draw-order record
(only if node list 1 is non-empty), `+64` node, `+68` state base (= element + 0x60), `+72`/`+76` per-track current
param block / ParamSet entry, `+80` matrix 1, `+84` matrix 2, `+88` kind.
Node `kind` (b11) sets the matrices: 255 → none, 2 and 6–9 → two (+80, +84), all others → one (+80).
Node `b9` bit 0 picks the update list: set → list 0 (updated **before** the instance matrix, FUN_08949DA4),
clear → list 1 (updated **after** it, FUN_0894A824/0894A674). All retail nodes have b9 = 2 → list 1.
Node `b10`, `b16`, `life` (+17): no reader found (life == duration in data). (guess: tool data)

### 2.2 Track links (confirmed, agrees with F)
`Track {u16 ch, u16 state_off, u32 n, ptr u16 data}`: `data[0..n−1]` = indices of tracks this component reads,
`data[n..2n−1]` = their state offsets; state of a track = `element + 0x60 + state_off`. ch14's links are
`[pos, scale, rot]` (in that order), ch10/ch11's are `[colour, ch14]`.

### 2.3 Channel 14 = transform builder (6-byte param)

| byte | meaning | code | level |
|---|---|---|---|
| 0 | parent mode (only when el+24 ≠ 0): 0 = position through parent (`t = P·(t,1)`), 3 = full `P·M`, 4 = parent rotation normalised + position through parent, 1/2/5–9 = none | FUN_08946D88, table 0x089D03E8 | confirmed |
| 1 | **billboard mode** at draw: 0 world, 1 camera-facing, 2 axis-locked camera-facing | FUN_08946AC4 | confirmed (mode 2 detail strong) |
| 2 | 1 = rotate the element to face its velocity (pos channel 2: velocity, channel 3: vel + acc·t) via FUN_089511A8 | FUN_08951330 @0x089514AC | confirmed (FUN_089511A8 not read in detail) |
| 3 | rotation order (0..5, as 2.5) | FUN_08950C58 | confirmed |
| 4, 5 | draw-order layer / sub-layer (FUN_08970568: 0 = by instance bucket inst+400, 3 = by element position, else table 0x089DA2E4/0x089DA2FC) | FUN_08951330 @0x08951790 | strong |

Init (vt+8, FUN_08950F5C) and update (vt+24, FUN_08951330) both do:
```
p = state(pos).out ; r = state(rot).out ; s = state(scale).out      // s already ×0.01
p.y = −p.y; p.z = −p.z; r.y = −r.y; r.z = −r.z                       // effect space → game space (= F·M·F, F = diag(1,−1,−1))
M = T(p) · R_order(r°) · S(s)            (FUN_08950C58: 3×3 R·S, column 3 = (p,1))
if (byte2 == 1 and velocity ≠ 0) M3 = M3 · Rvel(velocity)            (FUN_089511A8, strong)
M = parentMode(byte0, M)                 (FUN_08946D88, only for elements with a parent)
el.matrix(+80) = M
```

### 2.4 Rotation (confirmed)
`R(r)`: VFPU `vrot` with the angle × 1/90 (quarter turns) → **degrees**. Standard right-handed axis rotations
(Rz columns (c,s,0),(−s,c,0); Ry (c,0,−s),(s,0,c); Rx (0,c,s),(0,−s,c)). Orders (FUN_08945930 = FUN_08950C58):

| order | matrix (column vectors) | three.js Euler order |
|---|---|---|
| 0 (all data) | Rx·Ry·Rz | 'XYZ' |
| 1 | Ry·Rx·Rz | 'YXZ' |
| 2 | Rz·Rx·Ry | 'ZXY' |
| 3 | Rx·Rz·Ry | 'XZY' |
| 4 | Ry·Rz·Rx | 'YZX' |
| 5 | Rz·Ry·Rx | 'ZYX' |
| ≥ 6 | no rotation | |

The FEPA rotation (FUN_088A9024, FUN_088A82D8: rotZ, rotY, rotX pre-multiplied) is also Rx·Ry·Rz = 'XYZ'.

### 2.5 Draw matrix (FUN_08946AC4, confirmed)
```
B = el.base ? el.base.matrix2(+84) : inst.matrix(+80)
k = basis scale given by the drawing component (ch10 ANM: 0.1; ch11 MDL: ResRef f32 +0, 1.0 in data)
mode 0: W = B · [k·M3 | M.t]
mode 1: W3 = Cam3 · diag(inst+48 scale) · k·M3 ;  W.t = B · (M.t, 1)      // Cam = *0x089DA27C, copied from camera +304 in FUN_08940CCC; by use the camera's world rotation (strong)
mode 2: up = normalize(B3 · k·M3 · Ŷ); right ⟂ up facing the camera; W = basis(up, camera) · ... (strong)
```
`inst+48` = instance scale ⊙ leaf scale. So billboards keep the instance (FEPA) scale but drop its rotation.
The component then multiplies its own model matrix (MDL group `T·S`, ANM header `T·S`, see B and C) and the GE view
= `V(*0x089DA278) · W` (FUN_0896FB5C).

### 2.6 World unit (confirmed)
FEPA offsets are metres (game world); element positions are in the instance's space (so × FEPA scale);
element scales are percent (×0.01). Check, SS_HIT_B node 0 (ANM, ch9 = 27.3 %): `0.273 × 0.1 × 31 (header size) ×
0.7 (FEPA) ≈ 0.59 m` half-width at s8 = 128. FIGA_BALL node 0 (MDL, ch9 4 % → 15 %).

---

## 3. Timing

### 3.1 Clock (confirmed / strong)
`dt` per tick = global step `*(0x089D69E0)+24` (FUN_08864DD8) × handle +300 (1.0) (FUN_088A6028). Per handle
FUN_088A7D0C: handle `+236 ≥ 0` → that; handle flag `+220 & 0x100` → global step; handle has a character (`+220 & 1`) → **the character's own time scale** (char
vmethod +56; hit-stop freezes the effect too). Effect frames = game ticks (1/60 s, strong).
Instance: `inst.t(+348) += dt` (FUN_0894B48C), starting at −leaf.h144. Each element: `el.time += dt`, skipped while < 0.

### 3.2 Spawn schedule (confirmed, FUN_0894B48C @0x0894B87C)
While `0 ≤ t < body.duration`: every spawn entry with `frame ≤ int(t)` (next index inst+468) creates its nodes
with `el.time = t − frame` (FUN_0894CF98), then applies all segments up to `int(el.time)` (FUN_0894A3EC).
Parent = none, so B = instance matrix.

### 3.3 Body fields (confirmed)
- `a` (copied to inst+4): bit 0x1 = after `duration`, wait until all elements are gone, then end; bit 0x2 =
  **sustained** (does not end at `duration`; ends at frame `c` if `c ≠ 0`, or when stopped). Neither bit: the instance is
  destroyed as soon as `t ≥ duration` (elements still alive are cut).
- `c` (+8): auto-stop frame for sustained effects (0 = never).
- `d` (+12): what stopping does: 0 = fade out over **30** frames, 1 = over **15**, 2 = kill at once, 3 = no fade
  (at `c`, looping nodes stop looping and play out). Fade: `inst+364 = 1 − (t − t_stop)/F` multiplies the instance
  alpha; at 0 the instance is destroyed.
SS_HIT_B: a=0, dur=128, d=0. FIGA_BALL: a=2 (sustained), dur=50, c=0, d=3.

### 3.4 Element life and loops (confirmed)
- Non-looping element: removed when `int(el.time) ≥ node.duration` (+4). Node flag 0x1 = keep alive until its
  children (emitted particles, el+32) are gone.
- Node flag 0x2 = loop: once `el.time ≥ h14` (and the instance is sustained and not stopping), FUN_0894A2AC applies
  segments to `h14−1`, re-seeks the node's list-3 tracks to the segment at `h12` (phase 5, vt+48), and sets
  `el.time = h12 + ((int(t) − h14) mod (h14 − h12)) + frac(t)`. So **h12 = loop start, h14 = loop end**.
  FIGA_BALL node 0: loops 18..20, node 1: 30..32.

### 3.5 Segments (confirmed: FUN_0894A3EC, FUN_0894C0DC, FUN_0894BA60)
ParamSet entry = `{u16 track, u16 off, u16 start, u16 len}`. A segment's entries are applied when
`frame ≥ seg.frame0` (u16 at ParamSet +0). On apply, the component's vt+16 gets `T = old_entry.len`:
`base = new.value + (state_at(T) − old_param_at(T))`, likewise rate/vel/acc. With no overrides that is just
`new.value` (a jump to the new key). Every tick, `out = f(params, t − entry.start)` with no clamp:
after `len` the curve **extrapolates** until the next segment starts. The init ParamSet counts as start 0.

### 3.6 Per-frame order (confirmed)
FUN_08949C94 (advance + spawn) → FUN_08949DA4 (list-0 elements: phase 7 once, segment advance/loop, phase 2) →
FUN_0894A824 (per instance: FUN_0894622C instance matrix, then list-1 elements the same way) → draw walk
FUN_08970A4C (sorted buckets; FUN_08949E2C draws node list 1 with vt+32, pass a3=0 for all channels except 50,
then pass a3=1 for channel 50) → FUN_0894A090 (phase 8 post-draw).

---

## 4. Channel 14, render state, colour and blending

### 4.1 Effect pass state (FUN_08940CCC, confirmed)
Enable: 21 **GU_FRAGMENT_2X**, 9 TEXTURE_2D, 1 DEPTH_TEST, 4 BLEND, 11 LIGHT0. Disable: 7 FOG, 0 ALPHA_TEST,
15 LINE_SMOOTH, 10 LIGHTING (re-enabled per draw), 3 STENCIL, 5 CULL_FACE, 12–14 LIGHT1–3. DepthFunc 7 (GEQUAL on
the PSP's inverted Z = normal "less-or-equal" in GL). **DepthMask(1) = no depth writes.** ColorMaterial(ambient).
Texture bind (FUN_08972C14): TexFunc(MODULATE, RGBA), filter min LINEAR_MIPMAP_LINEAR / mag LINEAR, wrap REPEAT/REPEAT.

### 4.2 Blend word (MDL material +12, ANM part +16) (confirmed: FUN_08972194 @0x08972300, FUN_089425DC @0x08942800)
`op = byte0, src = byte1, dst = byte2` → `sceGuBlendFunc(op, src, dst, fixA = +16, fixB = +20)` (ANM: +20/+24),
GE command 0xDF. Factors (PSP GE): src 2 SRCALPHA, 3 INVSRCALPHA, 6 **2·As**, 7 **1−2·As** (both clamped to [0,1]),
10 FIXA; dst the same with 10 FIXB; op 0 add, 1 subtract, 2 **reverse subtract**.

| word | count (local MDL materials / ANM parts, 14 FEPs) | meaning | WebGL |
|---|---|---|---|
| 0x000A0600, fixB 0xFFFFFF | 945 / 4367 | `Cs·2As + Cd` additive | alpha' = min(1,2a); blend(SRC_ALPHA, ONE), ADD |
| 0x000A0600, fixB 0 | 23 / 0 | `Cs·2As` (replace) | blend(SRC_ALPHA, ZERO) |
| 0x00070600 | 160 / 968 | `Cs·2As + Cd·(1−2As)` alpha blend | alpha' = min(1,2a); blend(SRC_ALPHA, ONE_MINUS_SRC_ALPHA) |
| 0x000A0602 | 90 / 126 | `Cd − Cs·2As` darken | alpha' = min(1,2a); REVERSE_SUBTRACT(SRC_ALPHA, ONE) |

### 4.3 Depth write and culling, MDL (confirmed: FUN_08972194 @0x08972344)
`translucentOK = (node alpha ≤ 128) && word == 0x00070600` (FUN_089716D4 passes `a ≤ 128`).
Depth write **on** if `!(mat.flags & 4) || translucentOK`, else off. `mat.flags & 8` → back-face culling on (front
face = group +8), else off. ANM parts: depth write off, culling off (FUN_08955A84 @0x08955BF8). Flags seen:
7 (most), 15, 4, 39, 11, 43, 47, 3, 12.

### 4.4 Colour pipeline (confirmed for code, GE lighting/doubling = strong)
```
chan  = clamp(colour channel 15/16/17 output, 0, 255)                 // FUN_0894E3C0/E7D0/EA58 clamp every frame
inst  = instance colour (+64 = +192 copy; rgb ×, a = colour.a × fade(+364) × distance/LOD fade)
C     = chan ⊙ inst                                                   // 0..255 floats (FUN_08955E94 @0x08956010)
MDL (FUN_08971850):  rgb8 = trunc(C.rgb) (byte), a8 = min(255, 2·trunc(C.a))
ANM (FUN_089425DC):  same packing (a doubled, clamped)
lit.rgb = clamp01( 2 · rgb8/255 · M.rgb )      // global ambient = light0 ambient = C, ColorMaterial(ambient)
lit.a   = a8/255 · M.a                          // M = vertex colour (MDL, RGBA4444) or part colour (ANM, u8 0x80 = half)
frag.rgb = clamp01( 2 · tex.rgb · lit.rgb )    // MODULATE + FRAGMENT_2X (RGB only)
frag.a   = tex.a · lit.a
blend factor = clamp01(2 · frag.a)             // 6/7 factors
```
Node colour 128, vertex 0x8 (8/15) or part 0x80, texture white → output ≈ 1.0: the doublings cancel the 128 = 1.0
convention. (agrees with B and C)

### 4.5 Fog and screen-space
Leaf +7 ≠ 0 → fog on for that instance (state 7). Instance +1 == 1 marks a 2D/screen instance: `sceGuOffset(1808 − x,
1912 + y)` from its position (FUN_08949E2C @0x08949F14, FUN_0894622C @0x089465F0); 0 for all character effects.

---

## 5. Channel formulas (confirmed)
Param layout (descriptor param size): value +0, rate/vel +16, acc +32. State: out +0, base +16, rate/vel +32, acc +48.

| ch | type | init (vt+8) | update (vt+24), τ = el.time − entry.start | code |
|---|---|---|---|---|
| 1 / 4 / 7 | const | out = base = v (7: out = 0.01·v) | – | 08951860, 08952274, 08952A44 |
| 2 / 5 / 8 | linear | base = v, rate = r | out = base + rate·τ (8: ×0.01) | 08951E98, 089520D0; 0895270C; 08952ED8 |
| 3 / 6 / 9 | quadratic | base, vel, acc | out = base + vel·τ + acc·τ²/2 (9: ×0.01) | 08951A98, 08951D00; 08952604; 08952DB8 |
| 15 / 16 / 17 | colour vec4 | as 1/2/3 | same, then clamp each to [0, 255] | 0894E3C0, 0894EA58, 0894E7D0 |
Position/rotation w = 0, a 4-wide VFPU add. The spawn override (call_ctx+152, bit 1/2/0x40) can replace position /
velocity / scale at init (emitters, agent D).

---

## JS for the studio

```js
// ---- per-channel value (agrees with section 5) ----
function chanValue(ch, p, tau) {                 // p = {v:[..], r:[..], a:[..]} floats from the param block
  const q = [2,5,8,16].includes(ch) ? 1 : [3,6,9,17].includes(ch) ? 2 : 0;
  let out = p.v.map((v, i) => v + (q >= 1 ? p.r[i] * tau : 0) + (q === 2 ? p.a[i] * tau * tau / 2 : 0));
  if (ch >= 7 && ch <= 9) out = out.map((x) => x * 0.01);                  // percent
  if (ch >= 15 && ch <= 17) out = out.map((x) => Math.min(255, Math.max(0, x)));
  return out;
}
// segment lookup: last segment with frame0 <= floor(el.time) that has an entry for this track; else init set.
// tau = el.time - entry.start ; no clamp at entry.start + entry.len.

const ORDERS = ['XYZ', 'YXZ', 'ZXY', 'XZY', 'YZX', 'ZYX'];
const D2R = Math.PI / 180;

// ---- element matrix (channel 14) ----
function elementMatrix(pos, rotDeg, scale /* already x0.01 */, ch14) {
  const order = ORDERS[ch14[3]];
  const m = new THREE.Matrix4();
  if (order) m.makeRotationFromEuler(new THREE.Euler(rotDeg[0] * D2R, -rotDeg[1] * D2R, -rotDeg[2] * D2R, order));
  m.scale(new THREE.Vector3(scale[0], scale[1], scale[2]));
  m.setPosition(pos[0], -pos[1], -pos[2]);
  return m;                                              // ch14[0] parent modes only matter for emitted children
}

// ---- instance matrix B ----
function instanceMatrix(rec, h /* {pos,rotDeg,scale,parent,bone,root} */) {
  const L = new THREE.Matrix4()
    .makeRotationFromEuler(new THREE.Euler((rec.rot[0] + h.rotDeg[0]) * D2R, (rec.rot[1] + h.rotDeg[1]) * D2R,
                                           (rec.rot[2] + h.rotDeg[2]) * D2R, 'XYZ'))
    .scale(new THREE.Vector3(rec.scale[0] * h.scale[0], rec.scale[1] * h.scale[1], rec.scale[2] * h.scale[2]));
  const p = new THREE.Vector3(rec.pos[0] + h.pos[0], rec.pos[1] + h.pos[1], rec.pos[2] + h.pos[2]);
  if (!rec.model || !h.bone) {                           // placed by the caller: B = [P3·L3 | pos]
    const B = new THREE.Matrix4().extractRotation(h.parent || new THREE.Matrix4()).multiply(L);
    return B.setPosition(p);
  }
  const mode = rec.attach;                               // +0x7C
  const unit = (M) => { const e = M.clone(); const s = new THREE.Vector3(); const q = new THREE.Quaternion();
                        e.decompose(new THREE.Vector3(), q, s); return new THREE.Matrix4().makeRotationFromQuaternion(q)
                          .setPosition(new THREE.Vector3().setFromMatrixPosition(M)); };
  const boneN = (mode & 8) ? h.bone : unit(h.bone);
  const R = ((mode & 2) ? h.root : boneN).clone().setPosition(0, 0, 0);
  const B = R.multiply(L);
  B.setPosition(p.applyMatrix4((mode & 4) ? h.root : boneN));
  return B;                     // recompute every frame unless (mode & 1), or only for rec.followFrames (+0x78) frames
}

// ---- draw matrix ----
function drawMatrix(B, M, k /* ANM 0.1, MDL res.scale */, billboard /* ch14[1] */, cam /* camera world matrix */, instScale) {
  const M3 = M.clone().setPosition(0, 0, 0).multiplyScalar(k); M3.elements[15] = 1;
  const t = new THREE.Vector3().setFromMatrixPosition(M);
  if (billboard === 1) {                                 // camera-facing; keeps instance scale, drops its rotation
    const W = new THREE.Matrix4().extractRotation(cam)
      .multiply(new THREE.Matrix4().makeScale(instScale[0], instScale[1], instScale[2])).multiply(M3);
    return W.setPosition(t.applyMatrix4(B));
  }
  return B.clone().multiply(M3.setPosition(t));          // mode 0 (mode 2: lock to B's Y axis, face the camera)
}

// ---- colour, blend, depth ----
function shade(chan, inst, word, fixB, matFlags) {
  const C = chan.map((c, i) => c * inst[i]);
  const rgb = C.slice(0, 3).map((c) => (Math.trunc(c) & 255) / 255);
  const a = Math.min(255, 2 * Math.trunc(C[3])) / 255;
  // lit = clamp01(2*rgb*vertexColour); frag = clamp01(2*tex*lit); alpha = tex.a*a*vertexAlpha; factor = min(1, 2*alpha)
  const op = word & 0xff, src = (word >> 8) & 0xff, dst = (word >> 16) & 0xff;
  const blend = { equation: op === 2 ? THREE.ReverseSubtractEquation : op === 1 ? THREE.SubtractEquation : THREE.AddEquation,
                  src: THREE.SrcAlphaFactor,                       // with alpha doubled+clamped in the shader (src 6)
                  dst: dst === 7 ? THREE.OneMinusSrcAlphaFactor : dst === 10 ? ((fixB & 0xffffff) ? THREE.OneFactor : THREE.ZeroFactor) : THREE.OneFactor };
  const depthWrite = !(matFlags & 4) || (C[3] <= 128 && word === 0x00070600);   // MDL only; ANM: false
  return { rgb, a, blend, depthWrite, cull: !!(matFlags & 8), depthTest: true, alphaTest: 0, fragment2x: true };
}
```

---

## Checked against data
- **SS_HIT_B** (one100): FEPA #66 id 68, scale 0.7, attach 0, no model → caller-placed. Body a=0, dur 128, d=0;
  spawns 0: nodes 0–4,6,7; 6: node 5. Node 0 = ANM 0 (system), ch9 27.3→17.5→… %, ch17 colour 145/167/196/100,
  ch14 `01 01 00 00 00 00` → billboard 1 (camera-facing), rot order 0. Node 2 = MDL 86 (render 0x000A0600 flags 7 fixB −1
  = additive, no depth write), ch4 rot (0,0,80°) → game −80° about Z, ch7 scale (50,50,0) % → (0.5,0.5,0).
- **FIGA_BALL** (sev100): FEPA #20 id 20, rot z 0.01°, scale 1.2, attach 0, no model → placed by the projectile.
  Body a=2 (sustained), dur 50, c=0, d=3 → lives until the projectile stops it. Nodes 0–5 flag 0x2, loops 18–20 /
  30–32 (h12/h14). Nodes 6/8 kind 4 = ch21 emitters (agent D), node 7 kind 255 = ch18 (no matrix).

## For the other agents
- **B (meshes)**: depth-write rule 4.3 (`translucentOK` term) and culling flag 0x8 come from FUN_08972194; the MDL
  draw passes `k = ResRef f32 +0` to FUN_08946AC4; 0x0000 vertex colours (black, alpha 0) make those vertices
  invisible under additive blend (lighting uses the vertex colour as material ambient).
- **C (flipbooks)**: ANM basis k = 0.1 and FUN_08970B40(param byte +6) adds an extra 3×3 rotation before
  FUN_08946AC4 (FUN_08955A84 @0x08955B44).
- **D (particles)**: children get `el+24` parent and `el+28` base (B = base +84); spawn override block at
  call_ctx+152 (bit 1 pos vec4 ptr +4, bit 2 vel ptr +8, bit 0x40 scale ptr +28) used by channel inits.
- **E (trails)**: attach bit 0x8 (keep bone scale) and the root/bone choice (0x2/0x4) matter for weapon effects.
- **F**: channel 50 is the only channel with bit 0 in table 0x089D99E0 and is drawn in a second pass after all
  others (FUN_08970A4C).

## Open
- Billboard mode 2 exact basis (FUN_08946AC4 @0x08946BDC) and velocity-orient FUN_089511A8: read, not fully
  transcribed. (strong)
- Which skeleton matrices the character vmethods +8/+16 return (root = character world matrix; bone = bone world
  matrix incl. root): strong by use, not traced into the character code.
- 1 tick = 1/60 s: the global step `*(0x089D69E0)+24` is 1.0 per tick in normal play (strong); slow motion lowers it.
- Node b10/b16/life and FEPA +0x34..+0x64 trigger keys: not decoded (not placement).
