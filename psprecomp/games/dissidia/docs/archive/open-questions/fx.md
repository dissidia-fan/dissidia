# Effect renderer: the open points of fx-components F/G, settled

31 Oct 2026. Read from the recompiled EBOOT (`work/output/generated`, printed with `work/magi_re/fn.py`), the decrypted
EBOOT for jump tables, and the 40 FEPs in `work/fx_re/scratch_F/*_40.bin` (census scripts were run from a scratch folder,
not committed). **Studio status (1 Nov 2026, branch studio-fx-traced):** §1, §2, the §3 clean-ups and the §7 cadence (ch61, ch95/100/102/103)
are applied in `studio/fx.js`; still open there: the ch101 depth bias (P+84, the screen pass draws over everything), the
lens mode (P+78) is drawn from the strong reading, and §4 / §5 are not drawn. Marks as in A–G: **confirmed** = read end to end in code; **strong** = code read, one link inferred;
**guess** = plausible, not traced. `P+n` = parameter byte n, `E` = element matrix (+80), `B` = base matrix (+84).

## 1. ch101 shockwave (type 58): A and B are not radii

Functions: create 0x08959E44 → alloc FUN_0895E058, init FUN_0895C890, geometry FUN_0895DA4C; key m1 FUN_0895CC24;
update m2 FUN_0895CB28 → FUN_0895D37C; draw m3 FUN_0895D0BC → blend FUN_0895DE24, fan FUN_0895DEC4, strip FUN_0895E528.

Object (0x44 B): `+0 N = P+83`, `+4 A (vec2)`, `+12 A base`, `+20 A vel`, `+28 A acc` (copies), `+36 B`, `+40 B base`,
`+44 B vel`, `+48 B acc`, `+52 uvInner[N+1]`, `+56 uvOuter[N+1]`, `+60 posInner[N+1]`, `+64 posOuter[N+1]`.

**Geometry (confirmed, FUN_0895DA4C, built once).** Index 0 = the centre (0,0,0). Points i = 1..N:
`posOuter[i] = Rz(θi)·(0, −0.5, 0)`, `θi = (i−1)·360°/N`; `posInner[i] = posOuter[i]·P+68`. Element-local, in the z = 0
plane, drawn through `FUN_08946AC4(element, ch14 byte 1, scale 1.0)` (the usual element matrix + billboard mode).
So the ring's size is **0.5 × the element's scale** (channels 7–9, ×0.01) and the inner edge is `P+68` of that.

**UVs (confirmed).** Recomputed every update by one of three modes, picked in FUN_0895D37C in this order:
* `P+78 ≠ 0` → FUN_0895D40C, *lens*: for each outer point `d = normalize(p.x, −p.y, −1)`, `n = normalize(M·d)` with M the
  global at `*0x089DA278` (view, per D), `uvOuter = ((n.x+1)/2, (n.y+1)/2)`; `uvInner = uvOuter[0] + P+68·(uvOuter − uvOuter[0])`.
  Retail: only `sys_0 F0200_ASSIST_APP` (2 blocks). M = view is strong.
* else `P+77 ≠ 0` → FUN_0895E108, *refraction scale* (every other retail block): with `MVP = proj(*0x089DA284) · E`,
  `uvOuter[i] = screen(MVP · (B·p_i))`, `uvInner[i] = screen(MVP · (B·P+68·p_i))`, centre = screen of the origin.
  `screen(q) = clamp((q.x/q.w + 1)/2), clamp((1 − q.y/q.w)/2)` (PSP v points down).
* else `P+76 ≠ 0` → FUN_0895D668 + FUN_0895D8A0, *shift*: the UVs are the projected points themselves plus a screen
  offset: `a = (A.x, −A.y, 0)` turned by the rotation of the matrix at element+80, length kept, divided by the copy's
  width/height (screen-copy +20/+24) and by w, added in NDC before the `(x+1)/2` mapping. Never drawn by retail data
  (P+77 is set in all 69 blocks, and it wins).

So **B is the refraction scale**: the vertex at radius r shows the frame at radius `B·r` (B < 1 magnifies, B > 1 shrinks,
B < 0 mirrors through the centre: fiv200 EX_MU_803A_BG uses −1.2; B = 0 smears the centre pixel: gst130 RUINGA_HIT).
**A is a screen-space shift (pixels at w = 1)**, unused by retail.

**Animation (confirmed, FUN_0895CB28 / FUN_0895CC24).** Exactly one of A or B is animated:
`if P+76: A = A0 + P+16·τ + P+32·τ²/2  else if P+77: B = B0 + P+28·τ + P+44·τ²/2` (τ = frames since the segment start;
the key handler rebases A0/B0 for continuity like the other curves). 22 retail blocks set P+76 and P+77 together:
there **B stays at P+12** (its vel/acc are ignored).

**Draw (confirmed, FUN_0895D0BC).** Screen copy bound as the texture (`copy+64 = P+80 > 0`, as ch110's feedback select);
`C = clamp(colour(dep 0) × instance colour)`. If `P+84`: a depth-only bias like ch10/11 (FUN_08947B88 with the vec3 at
`P+48` and `P+72` (1.0 in data) → FUN_0896FD08/FD88, undone by FUN_0896FE50). Then
1. TRIANGLE_FAN (vtype 0x183, no vertex colour, `sceGuColor(C)` full alpha): centre + inner circle (posInner/uvInner),
   closed by repeating point 1. This is the refracted disc.
2. TRIANGLE_STRIP (vtype 0x19F): pairs (posOuter, uvOuter, alpha `C.a·P+64/128`), (posInner, uvInner, alpha `C.a·P+60/128`),
   closed. The ring fades from P+60 at the inner edge to P+64 at the outer edge.
Blend from `P+79` (shared 0/1/2 switch). `P+81`, `P+82` are not read by any type-58 function (confirmed).

| off | meaning | level |
|---|---|---|
| 0 vec2 / 16 / 32 | A shift (pixels), vel, acc | confirmed |
| 12 / 28 / 44 | B refraction scale, vel, acc | confirmed |
| 48 vec3, 72 f32, 84 u8 | depth bias vector, bias scale, bias on | strong |
| 60 / 64 | alpha ×1/128 at the inner / outer ring edge | confirmed |
| 68 | inner radius ratio (and inner UV ratio) | confirmed |
| 76 / 77 / 78 | animate+shift mode / refraction mode / lens mode | confirmed |
| 79 / 80 / 83 | blend / screen-copy buffer select / segments N | confirmed |

**Studio (`fxShockwave` + the `'ring'` post pass):** radius `r = 0.5 · |E scale| · iscale` (not B); inner = P+68;
ring in the element's XY plane through the element world matrix (billboarded by ch14), not always camera-right;
`B = P+12 + (P+76 ? 0 : P+28·τ + P+44·τ²/2)`; per vertex sample the scene copy at the projection of `B·vertex` (do it
on the CPU: project `B·p` with the same matrix and pass it as the uv attribute) instead of `c + (vUv−c)·0.9`; draw the inner
disc too (fan, alpha `C.a`, no fade), then the annulus with alphas `P+60/128` inner and `P+64/128` outer; seg = P+83 (not ×2).

## 2. ch18 / ch151 child placement — confirmed for the offsets, strong for the parent link

Spawn code: ch151 FUN_0894F3E8 (m2 0x0894F390, triggered by m1/m5 = element start and every key), ch18 m2 0x0894ED6C
(same sequence). Per child:
1. `J = (rand(±P+4), rand(±P+4), rand(±P+4))` (engine space, the FEP LCG).
2. `T` = identity with translation = the linked track's current value (dep 0: ch1/2/3), y and z negated.
3. FUN_089488A8(child kind, J, T, parentMat, outPos, outDir, normalize=1), jump table 0x089D0438:
   kinds 0, 2–8 → `pos = J, dir = J`; **kind 1** → `pos = T·J` (= parent position + J), `dir = J`;
   kind 9 → `pos = (parentMat·T)·J, dir = pos − translation` (no retail child is kind 9). `dir` is normalised (0 stays 0).
4. `vel = dir·P+8`, `acc = dir·P+12`; all three get y, z negated back to effect space.
5. FUN_0894C244 → FUN_0894CF98 creates the child with **element+24 = the spawner element**; the child's position
   channel create (ch3 FUN_08951A98 reads `ctx+152`) **adds** pos to its value, vel to its rate, acc to its acceleration
   (flags 1/2/4 = 7).
So F's "scaleB/scaleA" are radial **speed** (P+8) and **acceleration** (P+12). Example: six200 TRYN_HIT 4 children,
jitter 1, speed 0.1, acc −0.002 (an outward burst that slows).

After creation the child is placed like any element with a parent (element+24):
* ch14 byte 0 (= node kind in all 40 FEPs) is the parent mode, FUN_08946D88 / table 0x089D03E8, applied every frame with
  `P = spawner.E` (skipped when the spawner has no matrix, i.e. kind 255): 0 = child position through P,
  3 = `P·M`, 4 = P's rotation normalised + position through P, others none (A §2.3, confirmed).
* World kinds 2, 6–8 (FUN_0894CBA8 @0x0894CBFC): `B = instance(+80) · spawner.E` at spawn (or the spawner's base B ·
  spawner.E when the spawner has one); kind 2 then clears B's 3×3 (translation only). Strong (gated by `ctx+148 ≥ 6`, not traced).
Retail child kinds: 1 (≈ 900 uses, mostly from kind-255 spawners), 3, 4, 6, 0.

**Studio (`fxChildren`, `fxElMat`, `fxElement`):**
```js
const J = [fxR(rnd, j), fxR(rnd, j), fxR(rnd, j)], L = Math.hypot(...J), d = L > 0 ? J.map((x) => x / L) : [0, 0, 0];
const p0 = cn.kind === 1 ? at() : [0, 0, 0];                     // only kind 1 adds the spawner's position track
spawns.push({ r, ni, off: J.map((x, k) => p0[k] + x), vel: d.map((x) => x * fxF(fep, o + 8)), acc: d.map((x) => x * fxF(fep, o + 12)) });
// fxElMat: p += off + vel·g + acc·g²/2  (the child's own curve gains them)
// non-world kinds 0/3/4 with a spawner that has a matrix: compose with the spawner's live matrix per its kind (0, 3, 4)
// world kinds 2/6/7/8: frozen = place(t0+born) · spawnerE(at the spawn age); kind 2 keeps only the translation
```

## 3. ch100 / ch102 / ch103 line layouts — confirmed (G §3)

G read all three end to end (100: FUN_08984574 emit, FUN_08984B40 update, FUN_08984DEC draw; 102: 0x08974DCC,
FUN_089C2E10; 103: 0x089760E8, FUN_089C30F4) and `fxStreaks100` / `fxLines` / `fxPoints95` in fx.js already follow it.
Nothing open. Two clean-ups: `fxSparks` (fx.js ~line 1150) has no caller and still holds the old guessed layouts; the
file header still calls the spark layouts "partly guessed". One timing point, see §7: motion, history and life step
**once per update** (not ×dt), only emission is ×dt (FUN_08974DCC reads instance+352 once, for `acc += rate·dt`).

## 4. ch66 / ch70 path generators — confirmed for 66, strong for 70

Both are point lists for the ribbons 80/82 (not drawn by the Studio yet); 68 is the sampled history (F).

**ch66 (type 50): a rope that unrolls behind the element** (1 node, gst100 RETRVIEWTER_KISEKI). m0 0x08965DD4
(alloc FUN_0896A620, ctor FUN_08969F74, init FUN_0896A120), m5 / reset FUN_0896A374, m2 0x08965E94 → FUN_0896A6E0.
Object: `+0 n = P+32 + 1`, `+4 pts[n]`, `+8 len[n]`, `+12 edgeA[n]`, `+16 edgeB[n]`, `+20 links taut`, `+28 slack ratio`.
* Reset (start and every key): `head = E · (dep1 state, 1)` (dep 1 = ch79, all zero → the element origin); every point =
  head; `len[i] = P+34 ? rand in [P+28, P+30] (u16) : P+12`.
* Update (skipped while paused): `pts[0] = head`; for i = 0..n−2: once link i is past the taut count, if
  `|pts[i] − pts[i+1]| < len[i]` → stop (slack = that distance / len[i]); else taut count +1. Taut links get
  `pts[i+1] = pts[i] + normalize(pts[i+1] − pts[i])·len[i]` (follow-the-leader at a fixed spacing).
* `P+35` set: `edgeA[i] = pts[i] + R·F(P+0..8)`, `edgeB[i] = pts[i] + R·F(P+16..24)`, R = E's 3×3, F = (x, −y, −z).
Params (36 B): `0 vec3 edgeA`, `12 f32 spacing`, `16 vec3 edgeB`, `28/30 u16 spacing min/max`, `32 s16 points−1`,
`34 u8 random spacing`, `35 u8 two-edge mode`. Retail: 31 points, spacing 0.2, one edge.

**ch70 (type 54): a drifting smoke path** (3 nodes: one200 WATER_KISEKI, nin100 DFLAME_BULLET, nin100
EX_FINISH_KISEKI → ch82). m0 0x08967388 (alloc FUN_0896C8F0, ctor FUN_0896BD58, init FUN_0896BE68),
m2 0x08967438 → FUN_0896C9B0, first frame FUN_0896C5BC, edges FUN_0896C708, m8 FUN_0896C160 (re-arms the first frame).
Object: `+0 n = P+12 + 1`, `+4 pts[n]`, `+36 vel[n]`, `+12/+16 edge arrays`, `+40 wander (vec3)`, `+56 wander target`,
`+72 countdown`, `+76 first-frame flag`.
* `head(v) = (B·E)·(v, 1)` (FUN_0896C488; B = the base matrix or instance+80).
* First update: all points = head(0), velocities 0 (strong).
* Each update (not ×dt): for i = n−1..1: `pts[i] = pts[i−1] + vel[i−1]`, `vel[i] = vel[i−1]`; `pts[0] = head(0)`.
  Every `P+15` updates the wander target is re-rolled `rand(±P+64/68/72)` per axis; `wander += (target − wander)·P+48/52/56`;
  `vel[0] = head(wander) − head(0) + (P+32, −P+36, −P+40)`.
* `P+14` set: edges as ch66 with `P+0..8` and `P+16..24`. `P+28` → +77, not read by the update (strong).
Params (76 B): `0 vec3 edgeA`, `12 s16 points−1`, `14 u8 two-edge`, `15 u8 wander interval`, `16 vec3 edgeB`,
`28 u8`, `32 vec3 drift (y, z negated)`, `48 vec3 wander smoothing`, `64 vec3 wander range`.
Retail one200: 5 points, drift y −0.1 (rises 0.1/update in engine space), smoothing 0.5, range ±0.3, every 4.

**Studio:** add `fxPath66(el, tr)` / `fxPath70(el, tr)` returning the point list (newest first) for when ch80/82 are drawn;
no change to what is drawn today.

## 5. ch58 (type 47): a camera-centred sprite field — strong

Not a strip. Placement FUN_089600A0 → FUN_0898E120 → FUN_0898E564 (run for every free sprite; sprite +80 = `P+142` (u16)
life, +89 = life < 1, i.e. 0 = no limit, as in retail: strong): `dir = normalize(rand(±1)³)`, `pos = dir · rand()·(P+96 + P+100) − camT`
(camT = translation of `*0x089DA27C`), so the sprites fill a ball around the camera. Draw (G §3.5): view = identity,
billboards, ANM `P+80`, alpha in over camera distance `[P+88, P+88+P+92]` and out over `[P+96, P+96+P+100]`.
Retail (gst130 RAIKOU_FINISH): 10 sprites, near 400/300, far 1500/300, size 1.0, static. Studio: keep in
`FX_NOT_DRAWN`, or draw 10 billboards of ANM P+80 scattered 400–1800 units from the camera.

## 6. ch110 P+8, P+10, P+13..16 — confirmed unread

Every type-64 method was listed: m0 0x08959688 (→ FUN_0895B244), m1 0x08959718 (`return`), m2 0x08959720
(→ FUN_0895B98C), m3 0x08959770 (→ FUN_0895BDD4), m4 0x089597C0, m5 0x08959814 (`return 1`). Byte reads of the block:
FUN_0895B98C reads `P+0/2` (s16 centre), `P+12`, `P+18`; FUN_0895BDD4 reads `P+17`, `P+18`, `P+19`, `P+20`. Nothing reads
8, 10 or 13–16, and track links carry state, not parameters. Studio: nothing to change.

## 7. dt per displayed frame — confirmed: 2.0 in the retail game, 1.0 in the port's 60 fps mode (fixed in the native build)

`dt` (instance+352) = the global step (`0x089D5EAC`, FUN_08864DD8) × the handle's scale (FUN_0894B48C writes it). The
global step is the frame timer's dt, which FUN_08883F9C stores as vblanks (clamped to the interval 2): **2.0 per displayed
frame at 30 Hz** (docs/frame-rate.md). The recomp's 60 fps mode sets interval 1 → dt 1.0.

Consequence: components that step **per update** instead of ×dt run at the 30 Hz cadence in retail:
* ch61 (0x08953A50): `mult += rate` per update, `timer += 0.5·dt`. At dt 2 a ramp lasts exactly `ramp` updates. At dt 1 a
  ramp lasts 2·ramp updates with the same per-update rate, so it overshoots by 2× and the envelope diverges (clamped by
  62 into a strobe). Also: entering stage 0 or 2 **always** zeroes the rate (not only on a skipped stage).
* ch95/100/102/103: life −1, motion, history shift per update; emission ×dt.
* ch70 per update; ch18 `acc += rate·dt·0.5`, element time, curves, ch109 and ch21 by dt.

**Studio (tick-based, 1 tick = 1/60 s):** to show what the PSP shows, run these per-update components every other tick with
dt = 2: in `fxEnvelope` use `s.timer += 0.5` and `s.mult[i] += s.rate[i] * 0.5` per tick (the same thing for a smooth
ramp), and zero `s.rate` whenever `st` becomes 0 or 2; in `fxStreaks100` / `fxLines` / `fxPoints95`, step motion and life on
even ticks only while emission uses `rate·2` on those ticks (or keep 1/tick and note that it matches the 60 fps port).
For the port itself, ch61 and the line emitters are not dt-correct at 60 fps (worth a `runtime/framerate.cpp` entry).

**Fixed in the native build (1 Nov):** `runtime/framerate.cpp` steps ch61, 95, 100, 102, 103, the ch68 / ch70 trail
paths and the ch105 UV scroll on every other 60 Hz update with dt x 2, and the ch106 locus history (`t += dt` reaching
`last + 1` shifts on every update at dt 1 as well, so weapon trails were half as long) shifts on every other update
while its head follows the bone on each. Details and the list of channels checked: docs/frame-rate.md, "Effect
components". The Studio note above still applies to the Studio.
