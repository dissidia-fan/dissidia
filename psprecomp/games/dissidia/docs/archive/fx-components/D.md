# D: particle emitters (FEP channel 21; 22/23/24; 18)

Agent D, 2026-10-24. Source: static recompilation `work/output/generated/batch_*.cpp` (EBOOT base 0x08804000),
data from the 14 character FEPs + 3 system FEPs in `work/fx_re/*_40.bin`, `sys_*_fep.bin`.
Scratch scripts: `work/fx_re/scratch_D/` (`simp.py` turns a recompiled function into readable pseudo-C,
`scan21.py` collects every ch18/21/22/23/24 parameter block into `chan21.json`, `dec21.py` names the
277-byte fields, `sim.py` a Python emission model).

Confidence: **confirmed** = read end to end in code; **strong** = code read, one link (caller, unit,
GE behaviour) inferred; **guess** = plausible only.

## Summary

* Channel 21 is a real CPU particle system, not a sprite burst. Component vtable 0x089DBBD0:
  init 0x089569C0, segment-start 0x08956AD0, **update/emit 0x08957344** (phase 2), **draw 0x08958DB4**,
  destroy 0x08956C64, segment-accept 0x08956CB8 (returns 1). **confirmed**
* Each emitter allocates a ring buffer of `maxP` (u16 @240) particles of 160 B. Emission is a
  fractional accumulator: `rate` (f32 @44) particles per frame, emitted in bursts every
  `interval+1` frames (u8 @252), with a probability per particle (u8 @253, %), until `rate/-rateAcc`
  frames after the segment start (f32 @60 < 0) or forever. Emission stops when the buffer is full.
  Segments replace the whole 277-byte block (e.g. Bartz's sword appear sets rate 2 in seg0, 0 in seg1). **confirmed**
* Particle motion is analytic, evaluated at draw: `pos = p0 + v·t + a·t²/2`, `scale% = s + s'·t + s''·t²/2`,
  `roll° = r + r'·t + r''·t²/2`, `colour = c + c'·t` (linear), where t = particle age in frames.
  Only the age and the ANM clock are stepped per frame. **confirmed**
* Spawn shapes (u8 @254): box, sphere/cone, ring, VTL vertex list, each with a "directional" (axis +
  random cone) or "radial" velocity; plus a second "force" vector (dir @32, speed @196, accel @200),
  which is how gravity / rising is done. **confirmed**
* Lifetime: u16 @242 frames; a particle also dies when a non-looping ANM flipbook ends. Per-particle
  fade-in / fade-out (u8 @272/273/274) multiplies alpha. **confirmed**
* Drawn as one camera-facing billboard per particle: the particle's ANM frame (flipbook quad(s)) with
  world matrix `[B·Rz(−roll)·Flip·diag(scale·0.001·instScale) | pos]`, B = camera rotation; depth write
  off, culling off; blend comes from the ANM frame's render word; particle colour goes in as GE
  ambient colour. **strong** (GE exposure constant: guess)
* Space: "local" particles (default) are stored in the emitter node's frame and re-transformed every
  frame (they move with the node). With byte @251 = 7 on node kinds 0/3/4 (and always for node kinds
  2/6..9) they are converted to world at spawn and left behind. **confirmed** (node-kind meaning: A)
* RNG: one global MS-LCG `seed = seed·0x343FD + 0x269EC3`, `r = ((seed>>16)&0x7FFF)/32767`, at
  0x089DA120 (initial value 1), shared with every other effect component → the game itself is not
  repeatable per effect; a preview should seed its own LCG per emitter. **confirmed**
* Channels 22/23/24 are not emitter ranges for particles: they are one-shot **random offsets applied
  at segment start to another track's state** (position / rotation / scale), i.e. they jitter the
  node itself. Channel 18 is a **node spawner** (spawns copies of another node at a rate). **confirmed**

---

## 1. Component types and methods

### 1.1 Channel → vtable (confirmed)

`0x089D95D0[chan]` is a pointer to a slot; the slots are filled at boot by the code at 0x0894DD74
(`lui/addiu/sw` pairs). Slot → vtable (GCC style: 8-byte entries `{s16 this_delta, pad, u32 fn}`
starting at vtable+8):

| chan | slot | vtable | +8 init | +16 seg-start | +24 update (ph 2) | +32 draw | +40 destroy | +48 seg-accept | +56/+64 |
|---|---|---|---|---|---|---|---|---|---|
| 18 | 0x089D98A0 | 0x089DB220 | 0x0894ED48 | 0x0894ED58 | **0x0894ED6C** | default | default | 0x0894ED64 | default |
| 20 | 0x089D98A8 | 0x089DBB80 | 0x08956164 | 0x089564FC | default | 0x08956200 | default | 0x08956820 | default |
| 21 | 0x089D98AC | 0x089DBBD0 | **0x089569C0** | **0x08956AD0** | **0x08957344** | **0x08958DB4** | **0x08956C64** | 0x08956CB8 | default |
| 22 | 0x089D98B0 | 0x089DC6F8 | 0x0896DA28 (ret 1) | **0x0896DA30** | default | default | default | default | default |
| 23 | 0x089D98B4 | 0x089DC7E8 | 0x0896E234 (ret 1) | **0x0896E23C** | default | … | | | |
| 24 | 0x089D98B8 | 0x089DC8D8 | 0x0896E9A8 (ret 1) | **0x0896E9B0** | default | … | | | |

Defaults are stubs 0x0894E388..0x0894E3B8. Descriptors (0x089D9A98): ch21 `{64,4,3,0, param 277,
state 14}`, ch22/23 `{9,0,1,0, 44, 0}`, ch24 `{10,0,1,0, 44, 0}`, ch18 `{9,1,1,0, 24, 4}`.

### 1.2 Who calls which slot (confirmed)

* +8 init: FUN_0894A104 (bind tracks). Args `a0=this, a1=ctx, a2=param, a3=state`. +40 (destroy) is
  called from FUN_0894A104 and FUN_0894C938.
* +16 segment start: FUN_0894A3EC — when node frame reaches segment k's start it sets, per entry,
  `element[72][track] = new param block`, `element[76][track] = ParamSet entry` and calls +16 with
  `(ctx, newParam, state, oldParam)`. Every ch21 node in the data has init + seg0 at frame 0, so +16
  always runs at frame 0.
* +24 update: FUN_0894BA60 phase 2 over node list 0 (node+40). `f12` = node time − segment start.
* +32 draw: FUN_08949E2C / FUN_0894A960 over node list 1 (node+44), filtered by the per-channel flag
  table 0x089D99E0 bit 0 vs. the pass argument (two passes).
* +48 is called on segment change (ch21 returns 1 = accept).

`ctx` (a1 of every method) is the per-instance runner: `ctx+136` = effect instance (instance+352 =
**dt**, +456 = package resource tables {+48 TEX, +52 MDL, +56 ANM, +60 VTL}, +460 = body, +48 = instance
scale vec3, +64 = instance colour vec4, +80 = instance matrix), `ctx+140` = element, `ctx+144` = track,
`ctx+156` = ANM runtime table, `ctx+168` = VTL runtime table. Element: +24 heap, +28 parent element,
+48 f32 node time, +64 node, +68 state base, +72/+76 per-track current param / ParamSet entry,
+80 matrix (node-local → parent), +84 second matrix (kinds 2, 6..9), **+88 u8 = node byte 11 ("kind")**.

dt (instance+352) is 1.0 per effect frame in normal play: **strong** (Bartz `P0051_WEP_APPEAR_SWORD`
emits rate 2 for the 11-frame seg0 and has maxP 22).

### 1.3 Method behaviour (ch21)

**init 0x089569C0** (confirmed): `S` = 14-byte track state: `{u32 buf, f32 acc, f32 amount, u16 seqIdx}`;
`S.buf = alloc(maxP·160 + 48)` (FUN_08971134, heap element+24), buffer header `B`:
`h28 = maxP, h30 = live = 0, h32 = head = 0, b34 = worldVel` where
`worldVel = (b251 == 7 && node.kind ∈ {0,3,4})`. Particles at `B+48 + i·160`.

**segment start 0x08956AD0** (confirmed; skipped if element.h2 & 2):

```
B.endT  (f4)  = rate <= 0 ? 0 : (rateAcc < 0 ? rate / -rateAcc : -1)   // 0 = never emit, -1 = no end
B.age   (f24) = 0                                                      // emitter clock, per segment
B.life  (f8)  = float(u16 @242)
S.amount       = rate
fade (u8 @272): off → fIn=1, fOutStart=0, fOut=0
  on, n=life, a=b273 (fade-in frames), b=b274 (fade-out frames):
    if a+b < n : fOutStart=n-b; fOut = b>0 ? 1/b : 0; fIn = a>0 ? 1/a : 1
    elif b < n : fOutStart=n-b; fIn = 1/(n-b);       fOut = b>0 ? 1/b : 0
    else       : fOutStart=0;   fIn = 0;             fOut = 1/b
(B.f12 = fOutStart, B.f16 = fIn, B.f20 = fOut)
```

**destroy 0x08956C64**: frees `S.buf`. **seg-accept 0x08956CB8**: returns 1.

---

## 2. Parameter block (277 B, channel 21)

Offsets in the block; "n" = number of the 3661 blocks (1138 emitter nodes) where the value differs
from the default. All formulas below are **confirmed** in 0x08957344 / 0x08958DB4 unless marked.
`R(x)` = `(2r−1)·x` (one RNG draw, uniform in [−x, x]).

| off | type | name | meaning (code) | typical data |
|---|---|---|---|---|
| 0 | vec3 f32 | `box` | shape 0/2: spawn offset `R(box.x), R(box.y), R(box.z)`; shape 1/3/6/7: `box.x` = spawn distance | 0.1 .. 10 |
| 12 | s32 | `anm` | ANM table index (−1 = none ⇒ **update returns before ageing**, nothing lives) | 0..30 |
| 16 | vec3 | `axis` | cone / ring axis (authoring space, see §2.4) | (0,−1,0) 82 % |
| 28 | f32 | `anmSpeed` | ANM clock multiplier | 1, 2 |
| 32 | vec3 | `forceDir` | 2nd velocity direction (normalised after randomising) | (0,−1,0) |
| 44 | f32 | `rate` | particles per frame (amount = rate·dt + rateAcc·dt²/2) | 1, 2, 4 |
| 48 | vec3 | `forceDirRand` | `forceDir += R(forceDirRand)` per axis if b262 | 0 |
| 60 | f32 | `rateAcc` | only sets the emission end time `rate/−rateAcc` (and the dt² term) | 0 |
| 64 | vec4 | `col` | initial colour RGBA, 128 = 1.0 (if b268, else 128,128,128,128) | 128 |
| 80 | vec4 | `colRate` | colour change per frame (linear) | 0, −2..−10 alpha |
| 96 | vec4 | `colRand` | `col += R(colRand)` (if b269) | |
| 112 | vec4 | `colRateRand` | `colRate += R(colRateRand)` (if b269) | |
| 128 | vec2 | `scale` | initial scale % x,y (if b263, else 100) | 10..60 |
| 136 | vec2 | `scaleVel` | scale % per frame | −0.5..2 |
| 144 | vec2 | `scaleAcc` | scale % per frame² | ±0.02 |
| 152 | vec2 | `scaleRand` | randomisation of scale (b264, mode b265) | 5, 10, 20 |
| 160 | vec2 | `scaleVelRand` | " of scaleVel | |
| 168 | vec2 | `scaleAccRand` | " of scaleAcc | |
| 176 | f32 | `spread` | random angle (degrees, ±) about each axis (cone/ring position and directional velocity) | 180, 360 |
| 180 | f32 | `speed` | speed along the emission direction (if b259) | 0.01..0.3 |
| 184 | f32 | `accel` | accel along the emission direction (if b259) | −0.005 |
| 188 | f32 | `speedRand` | `speed += R(speedRand)` (if b260) | 0.01..0.5 |
| 192 | f32 | `accelRand` | `accel += R(accelRand)` (if b260) | |
| 196 | f32 | `fSpeed` | speed along forceDir (if b261) | 0.01..0.05 |
| 200 | f32 | `fAccel` | accel along forceDir (if b261) — gravity / buoyancy | 0.0005..0.01 |
| 204 | f32 | `fSpeedRand` | (if b262) | |
| 208 | f32 | `fAccelRand` | (if b262) | |
| 212 | f32×3 | `rot` | roll angle°, roll speed °/f, roll accel °/f² (if b266) | 30, −90; 5 |
| 224 | f32×3 | `rotRand` | ± randomisation of the three (if b267) | 180, 360; 5 |
| 236 | f32 | `zbias` | depth-only bias, view units (if b270 and ≠ 0); negative = towards camera | −0.1..−2 |
| 240 | u16 | `maxP` | ring buffer size | 3..40 |
| 242 | u16 | `life` | particle lifetime, frames | 16..60 |
| 244 | s16 | `mdl` | MDL index; only checked (≠ −1, handle valid) as a gate for shapes 8/9 | −1 |
| 246 | s16 | `vtl` | VTL index (vertex cloud) for shapes 8/9 | |
| 248 | s16 | `vtlSet` | vertex set inside the VTL (0xCCCC fill when unused) | |
| 250 | u8 | `flip` | random mirror: 0 none, 1 = 50 % U-mirror, 2 = 50 % V-mirror, 3 = random of {none,U,V,UV} | 0, 3 |
| 251 | u8 | `space` | 0xFF default; 7 = world-space particles (see §2.4); draw skips the node transform when ∈ {2,6,7} | 255, 7 |
| 252 | u8 | `interval` | after an emission, `acc −= amount·k + rateAcc/2·k²` ⇒ one burst every k+1 frames | 0..12 |
| 253 | u8 | `prob` | % chance each particle of a burst is really emitted | 100 |
| 254 | u8 | `shape` | spawn shape / velocity mode, §2.2 | 3, 0, 2, 7 |
| 255 | u8 | `vtxOrder` | shapes 8/9: 2 = sequential vertex (S.seqIdx, wraps), else random | 0 |
| 256 | u8 | `useDist` | cone shapes: 0 → distance `r·box.x`; 1 → b257 | |
| 257 | u8 | `distMode` | 0: `box.x·(1 − pct·r)`; 1: `box.x·pct·r` (pct = b258/100) | |
| 258 | u8 | `distPct` | % | 25 |
| 259..262 | u8 | flags | useSpeed, rndSpeed, useForce, rndForce | |
| 263..270 | u8 | flags | useScale (always 1), rndScale, scaleRandMode, useRot, rndRot, useCol, rndCol, useZbias | |
| 271 | u8 | — | always 3, not read by ch21 code | |
| 272 | u8 | `fade` | enable per-particle alpha fade | |
| 273 | u8 | `fadeIn` | frames | 0..10 |
| 274 | u8 | `fadeOut` | frames | 0..24 |
| 275, 276 | u8 | — | always 0 | |

### 2.1 Particle record (160 B, runtime, confirmed)

`+0 vec4 p0, +16 vec4 v, +32 vec4 a, +48 vec3 scale%, +64 vec3 scaleVel, +80 vec3 scaleAcc,
+96 vec4 col, +112 vec4 colRate, +132 f32 age, +136 f32 rot, +140 rotVel, +144 rotAcc, +148 f32 anmTime,
+152 u16 anmStep, +154 u8 flipBits`. (scale z is 100 / 0 / 0 and unused by the billboard.)

### 2.2 Shapes (u8 @254; two jump tables, confirmed)

Position table 0x089D0538, velocity table 0x089D0560:

| shape | position | velocity direction | uses (data, of 1138 nodes) |
|---|---|---|---|
| 0 | box | directional (axis + spread cone) | 185 |
| 1 | sphere/cone: random angles (x,y,z) | directional | 61 |
| 2 | box | radial (from the spawn point) | 110 |
| 3 | sphere/cone | radial | 643 |
| 4, 5 | none (**p0 is not written**: stale slot data) | none | 0 |
| 6 | ring: random angle about Y only | directional | 15 |
| 7 | ring | radial | 95 |
| 8 | VTL vertex | directional | 9 |
| 9 | VTL vertex | radial | 20 |

* **Cone matrix** (0x08957938..0x08957B4C, also 0x08958060 for velocity): angles
  `ax = R(spread), ay = R(spread), az = R(spread)` (ring: `ax = az = 0`, `ay = R(spread)`, one draw),
  degrees → VFPU quarter turns (×1/90). `Rrand = Rx(ax)·Ry(ay)·Rz(az)` (standard right-handed
  matrices, column vectors). `L = lookAt(axis)` (FUN_08956CC0): `Z = normalize(axis)`,
  `up = (axis.x==0 && axis.z==0) ? (0,0,1) : (0,1,0)`, `X = normalize(axis × up)`, `Y = Z × X`,
  columns X, Y, Z. If `axis.x == axis.y == 0` the code first sets `axis.x = 1e-6`.
  `M = L·Rrand`. Position = `M·(0,0,dist)`, velocity dir = `M·(1e-5, 0, 1)`.
* **dist** (cone/ring): `useDist == 0 ? r·box.x : distMode == 0 ? box.x·(1 − pct·r) : box.x·pct·r`.
  (distMode ≥ 2 would use `spread` as distance: unused.)
* **Box**: `p = (R(box.x), R(box.y), R(box.z))`.
* **VTL** (0x08957D44): VTL block `{'VTL\0', id, 1, 1, size, 0, u32 nSets @24, u32 nVerts @28,
  u32 setsOff @32, u32 vertsOff @36}`; set = `{u32 count, u32 off → u32 vertex indices}`; vertices
  are float3 (12 B) at vertsOff. Pick index `order==2 ? seq++ % count : trunc(r·(count−1))`.
  Positions are already in engine space (no y/z flip, §2.4). Gate: MDL @244 must resolve.
* **Radial** direction = the spawn position transformed by FUN_089571F4: local mode
  `normalize(p)`; world mode `normalize(W·p − W.t)` (× W axis lengths when the b251==7 override is on).
* **Directional** in world-override mode (b34): `M` is pre-multiplied by the 3×3 of the node matrix
  (element+80) and of the parent matrix, i.e. the direction is taken to world. In local mode it stays
  in node space (converted at draw).

### 2.3 Velocity, force, scale, roll, colour (confirmed)

```
spd = useSpeed ? speed : 0;   acc = useSpeed ? accel : 0
if rndSpeed: spd += R(speedRand); acc += R(accelRand)         // two consecutive draws
v = dir·spd;  a = dir·acc                                     // dir from §2.2 (none for 4/5)
f = forceDir; if space==7: f.y = -f.y; f.z = -f.z
fs = useForce ? fSpeed : 0;  fa = useForce ? fAccel : 0
if rndForce: f += (R(rx),R(ry),R(rz)) of forceDirRand; fs += R(fSpeedRand); fa += R(fAccelRand)
fn = normalize(f) (0 if |f| = 0);  v += fn·fs;  a += fn·fa
scale = useScale ? scale@128 : 100 ; scaleVel/Acc = @136/@144 (else 0)
if rndScale:                                                  // FUN_08956E48
  mode 0 (uniform): d0=R(scaleRand.x), d1=R(scaleVelRand.x), d2=R(scaleAccRand.x); add d0/d1/d2 to x and y
  mode 1, for i in x,y:  s[i] *= 1+R(sr[i])/100; v[i] *= 1+R(svr[i])/100; a[i] *= 1+R(sar[i])/100;
                          s[i] += R(sr[i]);       v[i] += R(svr[i]);       a[i] += R(sar[i])   // both, as coded
rot = useRot ? rot@212[0..2] : 0;  if rndRot: rot[k] += R(rotRand[k])
col = useCol ? col@64 : 128;  colRate = useCol ? @80 : 0
if rndCol: for k in 0..3: col[k] += R(colRand[k]); colRate[k] += R(colRateRand[k])   // interleaved draws
```

### 2.4 Space (confirmed in code; meaning of node kinds is agent A's)

FUN_089571F4 → FUN_089484A4 (shared "space mode" transform). Mode = node kind (element+88), except
`space == 7 && kind ∈ {0,3,4}` → mode 7 with axis-scale flag.

* **Local** (kinds 0,1,3,4,5 with space ≠ 7; 79 % of nodes): p0, v, a stay in node authoring space. At draw:
  `world = Parent · Element · F · pos`, `F = diag(1,−1,−1)` (y and z negated) unless shape 8/9.
  Element = element+80, Parent = `(element+28 ? element[28]+84 : instance+80)`. Particles follow the
  node and its parent every frame.
* **World** (`space == 7` on kinds 0/3/4: 16 %; any kind 2/6..9): p0 = `(Parent·Element)·p` at spawn
  (axis and forceDir have y,z negated at spawn instead of F; VTL points not flipped), draw uses pos as is
  (draw skips the transform when space ∈ {2,6,7}). Particles stay where they were emitted.
* Odd combinations exist in data: kind 6 with space 255 (9 nodes: spawned through mode 6 *and*
  transformed again at draw), kind 1 with space 7 (3 nodes: spawned local, drawn untransformed).
  Reproduce literally.

### 2.5 RNG (confirmed)

```
seed: u32 at 0x089DA120 (data value 1), global, shared by ch18/21/22/23/24 and other components
next(): seed = (seed*0x343FD + 0x269EC3) >>> 0; return ((seed >>> 16) & 0x7FFF) / 32767.0
```
Draw order per particle: prob (if prob<100) → flip (if flip≠0) → position (box: 3; cone: 3 + dist 1 if
useDist==0 or distMode∈{0,1}; ring: 1 + dist; VTL: 1 unless sequential) → speed (2 if rndSpeed) →
cone velocity (3, shapes 0,1,6,8) → force (3 + 2 if rndForce) → scale (mode 0: 3; mode 1: 12) →
roll (3 if rndRot) → colour (8 if rndCol). A prob-rejected particle consumes only its prob draw.

---

## 3. Emission and update (0x08957344, confirmed)

```
update(dt = instance.f352):
  if dt == 0: return
  B.age += dt
  emitting = !(B.endT == 0 || (B.endT > 0 && B.endT < B.age))
  if emitting:
    amount = rate*dt + rateAcc*dt*dt/2 ;  S.amount = amount
    if amount > 0:
      S.acc += amount
      if S.acc >= 1:
        n = trunc(S.acc); step = dt / S.acc            // S.acc before trunc
        S.acc -= n;  S.acc -= amount*interval + rateAcc/2*interval*interval
        for i = n .. 1:
          if prob < 100 && trunc(r()*100) >= prob: continue
          if B.live >= maxP: break                       // buffer full: rest of burst lost
          slot = (B.head + B.live) % maxP;  B.live++
          p.age = p.anmTime = i*step - 1;  p.anmStep = 0
          p.flip = …; spawn position/velocity/… (§2)
  // age + kill (ANM required: anm == -1 → return here, nothing ages)
  dead = 0
  for k in 0 .. B.live-1 (oldest first):
    p = slot (head+k) % maxP
    if B.life <= p.age: dead++; continue
    t = p.anmTime * anmSpeed;  T = ANM.total (u32 @28)
    if t >= T:
      if !(ANM.flags @16 & 0x80): dead++; continue         // one-shot flipbook ended
      t = t - trunc(t)/trunc(T) * T (integer quotient);  p.anmTime = t      // stores the *scaled* time
    step = 0; cum = clip[0].dur
    while !(t < cum): step++; cum += clip[step].dur      // clips at ANM+offs[1], 12 B, dur u32 @+8
    p.anmStep = min(step, nClips-1)
    p.age += dt;  p.anmTime += dt
  B.head = (B.head + dead) % maxP;  B.live = max(0, B.live - dead)   // FIFO: removes the oldest `dead`
```
Data checks: Bartz `P0051_WEP_APPEAR_SWORD_LEAF_0` (fiv100 effect 46 node 3): box 0.3×1.3×0.3,
rate 2 for frames 0..11 (seg1 sets rate 0), maxP 22, life 32, space 7 (world), force (0,−1,0)·(0.03 ±
0.025) ⇒ rises in engine space, scale 10 ± 5 % shrinking 0.2 %/f, fade 5/5. Cloud sev100: 74 emitters,
all local (kind 1, space 255), mostly shape 3/7, interval 2. Across all 1138 emitters 344 overflow
maxP in the model (hit bursts: rate 16, maxP 16, interval 2 → only the first burst fits): by design.

---

## 4. Drawing (0x08958DB4, strong)

Per draw pass, once: `sceGuEnable(TEXTURE_2D)`, `sceGuTexOffset(0,0)`, `sceGuDepthMask(1)` (no
z-write), `sceGuDisable(CULL_FACE)` (0x089A0384 / 0x089A0F84 / 0x089A119C / 0x089A03DC). Then for each
live particle, oldest first (no sorting), skipping `age >= life`:

```
t   = p.age
pos = p0 + v*t + a*t*t/2
s   = (scale + scaleVel*t + scaleAcc*t*t/2) * 0.001 * instance.scale(+48)        // 100 % → 0.1
ang = -(rot + rotVel*t + rotAcc*t*t/2) degrees
col = col + colRate*t                                                            // vec4
M3  = Rz(ang) · Flip(p.flip) · diag(s.x, s.y, s.z)      Flip: 1 = RotY(π), 2 = RotX(π), 3 = both (FUN_08970B40)
if space ∉ {2,6,7}: pos = Parent·Element·(shape∈{8,9} ? pos : F·pos)
M3  = B · M3,  B = 3×3 of the matrix at *0x089DA27C (camera billboard: inverse view rotation, strong)
GU_VIEW = (*0x089DA278 view) · [M3 | pos]                                           (FUN_0896FB5C)
fade: f = t < fOutStart ? fIn*t : 1 - fOut*(t - fOutStart);  col.a = f<0 ? 0 : f<1 ? col.a*f : col.a
col = clamp(col * instance.colour(+64), 0, 255)
if useZbias && zbias != 0: projection[2][2] adjusted so depth is that of the point moved by -zbias
   along view z (screen position unchanged) (FUN_0896FD08/FD88; restored by FUN_0896FE50)
draw ANM step p.anmStep with colour col                                            (FUN_08942FB8 → FUN_089425DC)
```

ANM draw (C's area, recorded here for the colour path): `sequence = ANM+offs[2] + step*12
{flags, n, off→frame indices}`; each frame record (28 B at offs[3]) is drawn as `sceGuDrawArray(
TRIANGLE_STRIP, vtype 0x81 = u8 uv + s8 xyz, nverts)` with texture `tex[frame.tex]`, blend
`sceGuBlendFunc(op = rs&15, src = rs>>8&15, dst = rs>>16&15, fixA = frame+20, fixB = frame+24)`.
Seen render words: 0xA0600 (4367 frames: add, src·2α + dst·FIX(white) = additive), 0x70600 (968:
src·2α + dst·(1−2α)), 0xA0602 (126: reverse subtract, darkens). Quads are ±127 in x,y and +127 in z
(the z pushes the sprite towards the camera after billboarding). Lighting is enabled with no vertex
colour: global ambient (0x5C/0x5D) and light-0 ambient = particle colour (alpha byte = min(255,
trunc(2·a))), material ambient + alpha (0x55/0x58) = the frame's u8 RGBA. So the lit colour is
particle × frame; with double-alpha blending 128 × 128 ≈ fully opaque.
**guess**: exposure 128 = 1.0 for RGB as well (exact GE light-sum / texture-doubling factor is A/C's).
The emitter node's own colour channel (15) state is **not read** by this draw; only instance+64.

---

## 5. JS-ready pseudo-code (deterministic preview)

```js
// Inputs: P(seg) -> decoded 277-byte block active at node frame g (segments = {start,len,block});
// nodeWorld(g) -> 4x4 (Parent·Element, column-major), camRot -> 3x3 inverse view rotation,
// anm -> {total, loop, clipDur[], nClips, draw(step, colour)}, vtl -> {sets[[idx]], verts[[x,y,z]]},
// kind = node byte 11, inst = {scale:[1,1,1], colour:[1,1,1,1]}.
const F = v => [v[0], -v[1], -v[2]];
function lcg(seed){ let s = seed>>>0; return () => { s = (Math.imul(s,0x343FD) + 0x269EC3)>>>0; return ((s>>>16)&0x7FFF)/32767; }; }
const R = (rnd, x) => (2*rnd()-1)*x;

function simulateEmitter(node, g, seed = 1){
  const rnd = lcg(seed), parts = [];   // FIFO, oldest first
  let st = null, acc = 0, seq = 0;
  for (let f = 0; f <= g; f++){
    const seg = node.segments.findLast(s => s.start <= f);
    if (seg && seg.start === f) st = segStart(seg.block);          // 0x08956AD0
    if (!st) continue;
    const P = st.P, dt = 1;
    st.age += dt;
    const emitting = !(st.endT === 0 || (st.endT > 0 && st.endT < st.age));
    if (emitting){
      const amount = P.rate*dt + P.rateAcc*dt*dt/2;
      if (amount > 0){
        acc += amount;
        if (acc >= 1){
          const n = Math.trunc(acc), step = dt/acc;
          acc -= n; acc -= amount*P.interval + P.rateAcc/2*P.interval*P.interval;
          for (let i = n; i >= 1; i--){
            if (P.prob < 100 && Math.trunc(rnd()*100) >= P.prob) continue;
            if (parts.length >= P.maxP) break;
            parts.push(spawn(P, i*step - 1, rnd, node, f, () => seq++));
          }
        }
      }
    }
    if (P.anm < 0) continue;
    const anm = node.anm(P.anm);
    let dead = 0;
    for (const p of parts){
      if (st.life <= p.age){ dead++; continue; }
      let t = p.anmTime * P.anmSpeed;
      if (t >= anm.total){
        if (!anm.loop){ dead++; continue; }
        t -= Math.trunc(Math.trunc(t)/Math.trunc(anm.total))*anm.total; p.anmTime = t;
      }
      let s = 0, cum = anm.clipDur[0];
      while (!(t < cum) && s < anm.nClips-1) { s++; cum += anm.clipDur[s]; }
      p.anmStep = s; p.age += dt; p.anmTime += dt;
    }
    parts.splice(0, dead);                                           // game removes the oldest `dead`
  }
  return { parts, st };
}

function segStart(P){
  const st = { P, age: 0, life: P.life,
    endT: P.rate <= 0 ? 0 : (P.rateAcc < 0 ? P.rate/-P.rateAcc : -1), fIn: 1, fOutStart: 0, fOut: 0 };
  if (P.fade){ const n = P.life, a = P.fadeIn, b = P.fadeOut;
    if (a+b < n){ st.fOutStart = n-b; st.fOut = b>0 ? 1/b : 0; st.fIn = a>0 ? 1/a : 1; }
    else if (b < n){ st.fOutStart = n-b; st.fIn = 1/(n-b); st.fOut = b>0 ? 1/b : 0; }
    else { st.fOutStart = 0; st.fIn = 0; st.fOut = 1/b; } }
  return st;
}

function spawn(P, age0, rnd, node, f, nextSeq){
  const world = (P.space === 7 && [0,3,4].includes(node.kind)) || [2,6,7,8,9].includes(node.kind);
  const p = { age: age0, anmTime: age0, anmStep: 0, flip: 0 };
  if (P.flip === 1) p.flip = rnd()*256 < 128 ? 0 : 1;
  else if (P.flip === 2) p.flip = rnd()*256 < 128 ? 0 : 2;
  else if (P.flip === 3) p.flip = Math.trunc(rnd()*255) >> 6;
  const axis = P.space === 7 ? F(P.axis) : P.axis.slice();
  if (axis[0] === 0 && axis[1] === 0) axis[0] = 1e-6;
  const coneM = (ay_only) => { const ax = ay_only ? 0 : R(rnd,P.spread), ay = R(rnd,P.spread), az = ay_only ? 0 : R(rnd,P.spread);
      // ring: only one draw (ay); cone: ax, ay, az in that order
      return mul3(lookAt(axis), mul3(rotX(ax), mul3(rotY(ay), rotZ(az)))); };
  const dist = () => !P.useDist ? rnd()*P.box[0]
      : P.distMode === 0 ? P.box[0] - P.box[0]*(P.distPct/100)*rnd()
      : P.distMode === 1 ? P.box[0]*(P.distPct/100)*rnd() : P.spread;
  let pos = [0,0,0], dir;
  switch (P.shape){
    case 0: case 2: pos = [R(rnd,P.box[0]), R(rnd,P.box[1]), R(rnd,P.box[2])]; break;
    case 1: case 3: { const M = coneM(false); pos = mulV(M, [0,0,dist()]); break; }
    case 6: case 7: { const M = coneM(true);  pos = mulV(M, [0,0,dist()]); break; }
    case 8: case 9: { const set = node.vtl.sets[P.vtlSet];
      const k = P.vtxOrder === 2 ? set[nextSeq() % set.length] : set[Math.trunc(rnd()*(set.length-1))];
      pos = node.vtl.verts[k].slice(); break; }
    default: pos = null;                                            // 4/5: game leaves stale data
  }
  let radial;
  if (world && pos){ const W = node.world(f); const wp = xform(W, pos); radial = normalize(sub(wp, W.t)); pos = wp; }
  // (space-7 override on kinds 0/3/4 also multiplies `radial` by W's column lengths: FUN_089484A4 flag 2)
  else if (pos) radial = normalize(pos);
  let spd = P.useSpeed ? P.speed : 0, acc = P.useSpeed ? P.accel : 0;
  if (P.rndSpeed){ spd += R(rnd,P.speedRand); acc += R(rnd,P.accelRand); }
  if ([0,1,6,8].includes(P.shape)){
    let M = coneM(false);                                           // 3 fresh draws
    if (P.space === 7 && [0,3,4].includes(node.kind)) M = mul3(rot3(node.world(f)), M);
    dir = mulV(M, [1e-5, 0, 1]);
  } else if ([2,3,7,9].includes(P.shape)) dir = radial; else dir = [0,0,0];
  p.p0 = pos; p.v = scale(dir, spd); p.a = scale(dir, acc);
  let fd = P.space === 7 ? F(P.forceDir) : P.forceDir.slice();
  let fs = P.useForce ? P.fSpeed : 0, fa = P.useForce ? P.fAccel : 0;
  if (P.rndForce){ fd = fd.map((c,i) => c + R(rnd,P.forceDirRand[i])); fs += R(rnd,P.fSpeedRand); fa += R(rnd,P.fAccelRand); }
  const fn = normalize(fd);
  p.v = add(p.v, scale(fn, fs)); p.a = add(p.a, scale(fn, fa));
  p.s = P.useScale ? P.scale.slice() : [100,100]; p.sv = P.useScale ? P.scaleVel.slice() : [0,0]; p.sa = P.useScale ? P.scaleAcc.slice() : [0,0];
  if (P.rndScale){
    if (P.scaleRandMode === 0){ const d0 = R(rnd,P.scaleRand[0]), d1 = R(rnd,P.scaleVelRand[0]), d2 = R(rnd,P.scaleAccRand[0]);
      for (const i of [0,1]){ p.s[i] += d0; p.sv[i] += d1; p.sa[i] += d2; } }
    else for (const i of [0,1]){
      p.s[i]  += p.s[i]*R(rnd,P.scaleRand[i])*0.01;  p.sv[i] += p.sv[i]*R(rnd,P.scaleVelRand[i])*0.01;  p.sa[i] += p.sa[i]*R(rnd,P.scaleAccRand[i])*0.01;
      p.s[i]  += R(rnd,P.scaleRand[i]);              p.sv[i] += R(rnd,P.scaleVelRand[i]);              p.sa[i] += R(rnd,P.scaleAccRand[i]); }
  }
  p.r = P.useRot ? P.rot.slice() : [0,0,0];
  if (P.rndRot) for (const k of [0,1,2]) p.r[k] += R(rnd,P.rotRand[k]);
  p.c = P.useCol ? P.col.slice() : [128,128,128,128]; p.cr = P.useCol ? P.colRate.slice() : [0,0,0,0];
  if (P.rndCol) for (const k of [0,1,2,3]){ p.c[k] += R(rnd,P.colRand[k]); p.cr[k] += R(rnd,P.colRateRand[k]); }
  p.world = world;
  return p;
}

function drawEmitter(node, g, cam, inst, sprite){            // sprite(step, M4, rgba, blendFromAnm)
  const { parts, st } = simulateEmitter(node, g);
  if (!st) return; const P = st.P;
  for (const p of parts){
    const t = p.age; if (t >= st.life) continue;
    let pos = add(add(p.p0, scale(p.v, t)), scale(p.a, t*t/2));
    if (!(P.space === 2 || P.space === 6 || P.space === 7))
      pos = xform(node.world(g), (P.shape === 8 || P.shape === 9) ? pos : F(pos));
    const sx = (p.s[0] + p.sv[0]*t + p.sa[0]*t*t/2) * 0.001 * inst.scale[0];
    const sy = (p.s[1] + p.sv[1]*t + p.sa[1]*t*t/2) * 0.001 * inst.scale[1];
    const sz = 0.1 * inst.scale[2];                                 // 100 % in z
    const ang = -(p.r[0] + p.r[1]*t + p.r[2]*t*t/2) * Math.PI/180;
    const M3 = mul3(cam.billboard, mul3(rotZrad(ang), mul3(flip3(p.flip), diag3(sx, sy, sz))));
    const c = p.c.map((v,k) => v + p.cr[k]*t);
    let fa = t < st.fOutStart ? st.fIn*t : 1 - st.fOut*(t - st.fOutStart);
    c[3] = fa < 0 ? 0 : fa < 1 ? c[3]*fa : c[3];
    const rgba = c.map((v,k) => Math.min(255, Math.max(0, v*inst.colour[k])));
    sprite(p.anmStep, compose(M3, pos), rgba, P.useZbias ? P.zbias : 0);
    // sprite(): draw ANM sequence `step` (s8 verts /128 in GE 3D mode, guess), vertex colour
    // = rgb/128·frame.rgb/128 (guess on exposure), alpha = min(255,2a)/255·frame.a/255 with the
    // frame's blend (double-alpha); depthWrite off, cull off, depth bias -zbias in view z.
  }
}
// rotX/rotY/rotZ(deg): standard right-handed, column vectors. lookAt(axis): Z = n(axis),
// up = (axis.x==0&&axis.z==0) ? [0,0,1] : [0,1,0], X = n(cross(axis, up)), Y = cross(Z, X), columns X,Y,Z.
// flip3: 0 = I, 1 = diag(-1,1,-1), 2 = diag(1,-1,-1), 3 = diag(-1,-1,1).
```
Note: replaying from frame 0 each preview frame is required (emission is stateful); this is cheap
(maxP ≤ 40, ≤ a few hundred frames).

---

## 6. Channels 22 / 23 / 24: random start offsets (confirmed)

Methods +16 at 0x0896DA30 (22), 0x0896E23C (23), 0x0896E9B0 (24); everything else is a stub; no
state. On every segment start (= frame 0 in data; all 22/24 blocks have only init + seg0) they add
random values to **another track's state**: the track's u16 data list ends with the target's
state offset, `target = element + 96 + data[n]` (data[0] = target track index). Data: ch22 → ch1/3
(position), ch23 → ch4/5/6 (rotation), ch24 → ch7/9 (scale).

| off | 22 / 23 | 24 |
|---|---|---|
| 0 | vec3 range for the base value | same |
| 12 | u8 enable[0] | u8 uniform: **0 = one random for x,y,z (x's range)**, ≠0 = independent |
| 13 | u8 mode[0] | u8 enable[0] |
| 14 | u8 enable[1] | u8 mode[0] |
| 15 | u8 mode[1] | u8 enable[1] |
| 16 | vec3 range for velocity | same |
| 28 | u8 enable[2] | u8 mode[1] |
| 29 | u8 mode[2] | u8 enable[2] |
| 30 | — | u8 mode[2] |
| 32 | vec3 range for acceleration | same |

Mode: 0 (or ≥3) `R(x)` = [−x, x]; 1 = [0, x]; 2 = [−x, 0]. Byte tables 0x089DA0E0 / 0x089DA0F8 /
0x089DA110 give the offsets. Targets: k=0 → `+16` (base) and `+0` (current) = base + d
(ch24: `+0 = (+16 + d)·0.01`, the base is in %); k=1 → `+32` (rate / velocity); k=2 → `+48` (acceleration).
For the studio: at node start add `d` to the node's initial position / rotation (degrees) / scale %
and its rate terms. RNG: same global LCG, x,y,z draws per enabled entry.

## 7. Channel 18: node spawner (strong; likely E's or F's to finish)

0x0894ED6C (24-byte block, 4-byte state = accumulator): `acc += P.f0 · dt · 0.5` (as coded); if
`acc >= 1`: `n = trunc(acc)`, `acc = frac(acc) − P.f0 · P.h18` (same burst-interval idea as ch21 @252); probability u8 @23 %; `P.h16` = node index to spawn
(<0: nothing), `P.h20` <0: nothing. Each spawn: offset `R(P.f4)` per axis around the target track's
current position (element+96+data[n], y,z negated), transformed by the spawned node's kind
(FUN_089488A8), velocity scaled by `P.f12` and `P.f8`, then FUN_0894C244 / FUN_08946A5C start the child
node (FUN_089469E0 / FUN_08946A28 enumerate children). 423 blocks in the data.

## 8. Notes for other agents

* A: node kind (byte 11) selects matrix storage in FUN_0894CF98 (kind 2 and 6..9: two matrices +80/+84;
  255: none; others: one at +80) and the space transform FUN_089484A4 (modes 0,3,4,5 copy; 1 = E;
  2,6..9 = Parent·E or Parent). F = diag(1,−1,−1) between authoring and engine space appears at every
  particle draw and in ch18. ParamSet `{u16 start, u16 length}` (second u16 is a length: Bartz seg1
  `11, 32`). The globals `*0x089DA278` (view), `*0x089DA27C` (billboard), `*0x089DA280`, `*0x089DA284`
  (projection-related, used by the z-bias) are worth confirming.
* B/C: ANM clip `{flags, index(=i), dur}` drives the step; header +28 = Σ dur; flags +16 bit 0x80 = loop
  (146 of 292 local ANMs). Per-frame render word = blend `op | src<<8 | dst<<16`, fixA/fixB at +20/+24.
* E: ch20 (75 B model emitter) shares the slot table; its draw is 0x08956200, update stub.
* F: ch14 has update 0x08951330 and seg-accept 0x08951164; ch15 colour state is clamped 0..255 in its
  segment-start (0x0894E4BC).
