# Open questions for the Move Studio dummy, and two sound questions

Research note, 2026-10-31. Source: the static recompilation (`work/output/generated`, `work/overlays/battle/generated`,
read with `work/magi_re/fn.py`), the EBOOT image (`work/eboot_image.bin`, base 0x08804000), the battle overlay
(`work/OVL_BATTLE_APP.bin`, segment 0x089DD4C0), and `map_sound_param.bin` from the disc (`map/maparc_<map>_2.mpk` →
`map_se_<map>-msnd.mpk`). Nothing in the game, tools or studio was changed.

Labels: **confirmed** = read end to end in code (or data); **strong** = code read, one link inferred; **guess**.
Actor offsets are the fighter object; the motion controller is actor+0x130 (event-ops/A.md), so "+0x195" = controller
+0x65. Mask constants were read from the EBOOT image (static values).

---

## 1. Body push-apart (kind-1 spheres)

### Call chain (confirmed)
- `FUN_0883AF80` (the per-tick position integrator) calls the fighter's vtable +0xA8 with its move for this tick, when
  `FUN_088375D4(self)` holds and self+0x542 is neither 5 nor 3. The slot is `FUN_08833360` (pointer found in the fighter
  vtables at 0x089C6ACC, 0x089C70DC, …; slot → function strong).
- `FUN_08833360(self, move)`: for every other actor (`FUN_08841F64` iterator): skip itself; skip if
  `FUN_08837990(other)` (other+0x388 bit 0x10000); require `FUN_088375D4(other)` = other+0x388 bit 0x1000 **and**
  other+0x192 bit 0x02 clear (`no_body_collision` 0x0013) **and** a non-empty kind-1 set; skip if other+0x542 == 3.
  Then `FUN_0883E904(self+0x468, other+0x468, move)`. +0x468 is the **kind-1** set (`FUN_08831808` builds it with kind 1).
- `FUN_0883E904`: for each sphere Y of the other's set, find the first sphere X of self's set that overlaps
  (`FUN_0883EA64` → `FUN_08812D48`: type 3 vs 3 = **3-D sphere test `|Yc − Xc| < rX + rY`**). For each overlap:
  `d = FUN_08812EF0(X, Y)` (type 3 vs 3 → `FUN_08812414`), `FUN_0883E3BC` splits d, self's spheres move by
  −(self share) (`FUN_0883E2F4`), and `move −= self share` (`FUN_089A38E0` = vsub).

### Push vector `FUN_08812414` (confirmed)
Shape record (28 bytes): +0 type, +4 horizontal radius r, +8 vertical radius h, +0x10 centre. Body records have r = h.
```
rs = rX + rY, hs = hX + hY
n  = ((Yc−Xc).x / rs, (Yc−Xc).y / hs, (Yc−Xc).z / rs)        // ellipsoid-normalised offset
if n.x² + n.z² <= 0:  d = (rs + 0.05, 0, 0)                     // same column: push along +x
else: k = sqrt((1 − n.y²) / (n.x² + n.z²));  d = (n.x, 0, n.z) · (k − 1) · (rs + 0.05)
d.y = 0                                                          // horizontal only
```
0.05 = constant 0x089C6E28. With both centres at the same height: `d = dir · (rs − dist) · (rs + 0.05)/rs`, i.e. the full
overlap plus ~4 % in one tick. Kind-1 spheres are (0, 0.9, 0) r 0.6 on Bartz, so rs = 1.2.

### Split `FUN_0883E3BC(selfSet, otherSet, move, d)` (confirmed, flags' names strong)
Mass `FUN_08838E24` = **1000 if actor+0x196 bit 0x01 (`heavy_body`), else 100**.
"Unpushable" U(x) = x+0x388 bit 0x8000 (mask 0x089C8140) **or** x+0x195 bit 0x80 (`cant_be_pushed`; the lead names the op
0x0049 no_push — op id not re-checked here).

Mode (self = the fighter that is moving this tick, other = the one it walks into):
| Condition (first match after the override below) | Self moves | Other moves |
|---|---|---|
| self+0x542 == 5 | 0 | 0 |
| other+0x542 == 5 | all of d (backwards) | 0 |
| U(self) and U(other), or neither, or U(self) only | `d·mO/(mS+mO)` | `d·mS/(mS+mO)` (other+0x2D4 position and its spheres) |
| U(other) only | all of d | 0 |
Override (checked after): self+0x38C bit 0x800 (0x089C813C) → self takes all; else self+0x38C bit 0x1000 (0x089C8134) →
nobody moves. Note: U(self) alone does **not** stop self being pushed back (modes 1 and 2 run the same code; mode 2 only
calls the mass function twice more). So `cant_be_pushed` protects a fighter only from others walking into it.

Per tick: one pass per moving fighter, in its own integrator tick, resolving the whole overlap at once (no rate limit).
Each fighter runs the pass on its own tick, so two fighters both moving each resolve once.

---

## 2. Reactions 3, 14, 15, 17, 18, 20 (`FUN_0883A6FC`, jump table 0x089C80D8)

Field names as in hit-reaction/README.md (`kb_yaw` +0x18, `kb_forward` +0x1C, `kb_up` +0x20, `stun` +0x24, `follow`
+0x28, `kb_bone` +0x2C). "Low" = defender height above floor (+0x3C0) **≤ 2.0 m** (const 0x089C7EC8).

| R | Movement | Conf. |
|---|---|---|
| 3 | Low: `FUN_08832560` → `FUN_0883241C(…,0)`: snap to the floor, state 0, **script 601**, ground slide along push at +0x24 m/tick (×0.9/tick, `FUN_08832150`), launch `FUN_08A58A5C` with height **+0x28** and no horizontal speed (vector (0, +0x28, 0, +0x2C): x,z = 0, so +0x2C is a speed with no direction = unused); landing 102. High: `FUN_0883265C(…,0)` = **reaction 7**: straight flight along −(attacker up axis), +0x18 m at +0x20 m/tick, script 700. | confirmed |
| 14 | `FUN_08832D64` → `FUN_08A51750(def, bone, 1)`: drag state 0x17, **script 610**, target as 11–13 (attacker or bone +0x2C, rotY(+0x18)·fwd·+0x1C), **up not in target, arc +0x20·sin(π/2·u)** (flag 0x01000000 clear), **eased u = sin(π/2·t/T)** (0x02000000 clear), **T = 64 ticks fixed** (+0x24 ignored; const 0x08A51994), follow +0x28, keep-velocity end (0x08000000). Phase 1: no 700→701 change; if up > 0 a drop phase with **0.1** (not 0.7) of the drag vector (const 0x08A52A98), dur = up/0.09; the drop also ends as soon as defender+0x188 bit 0x01 is set (on the floor, strong). End: `FUN_0883497C(−0.05)` carry last step, drag 0.95, free fall, landing 102 (not 702). | confirmed (bit 0x188 meaning strong) |
| 15 | High (> 2 m): exactly **reaction 12** (`FUN_08832BB0(…,1)` with all six fields). Low: snap to floor, state 0, then `FUN_08A51750(…, 0)`: **script 611**, as 14 but t starts at **−10** (const 0x08A51990) and T = 64 − 10 = **54**: the defender is **held still 10 ticks** (phase 0 skips while t < 0), then dragged 54 ticks; defender+0x384 |= 0x1000. | confirmed |
| 17 | Low: as reaction 3 low (601, slide +0x24, launch +0x28). High: **reaction 1** (if +0x1C == 0 and not airborne: ground slide `FUN_08831F04` with +0x24, flinch 500/501; else launch +0x1C high at +0x20 along push, 550/551). | confirmed |
| 18 | No movement, no script. defender+0x31C = **60.0** (const 0x089C7EA4). `FUN_0883C2E8` counts it down by dt each tick; while > 0 `FUN_0883AF80` **zeroes the defender's whole move for the tick** (`FUN_089A3890` on the accumulated move at 0x0883B30C): a **60-tick freeze in place** (root motion, velocity, gravity steps all dropped). Any later hit with another reaction clears it to 0 (`FUN_0883A150`). | confirmed (freeze), strong (that it covers vertical too: the zeroed vector is the full move) |
| 20 | `FUN_08832744(4)` → `FUN_08A6158C`: straight flight (state 3) along **normalize(hit record +0x10)** in 3-D, +0x18 m at +0x20 m/tick; 700 front / 750 back; state byte +0x20 = 1 (r7). Record +0x10 comes from the hit object's vtable +0x20 (for a melee object its +0x00); the 5 retail uses are stage vents. | confirmed (direction source for a fighter hit object strong) |

For the studio dummy (`studio/dummy.js`, `dmApplyHit` and `dmTickFoe`):
- 3/17: `low = f.pos.y <= 2`; low → `f.pos.y = 0; play(601); move = slide(push, k.stun); vel = 0; dmStartAir(f, k.follow)`;
  high → 3: `fly(v3(0,−1,0), k.kb_yaw, k.kb_up, 700)`; 17: the `case 1` code.
- 14: drag with `arc: true, linear: false, dur: 64, t: 1, R: 14`, play 610; in phase 1 use 0.1 instead of 0.7, don't add 1 to the
  motion, end the drop when y ≤ 0; end like 11/12 (carry, 0.95).
- 15: high → `case 12`; low → `f.pos.y = 0`, as 14 with play 611, `t: −9, dur: 54` and no movement while t < 0.
- 18: `f.freeze = 60`; in `dmTickFoe` skip all movement (drag/line/slide/air) while `freeze > 0`, decrement it; clear on the next hit.
- 20: `fly(dir, k.kb_yaw, k.kb_up, front ? 700 : 750)` with dir = the hit object's +0x10 vector (no source in the studio:
  fall back to push and say so).
- Stun windows: 601 / 610 / 611 are not 500–561 motions; their own timelines decide `can_act` (not read here).

---

## 3. Guard recoil speed and the wall re-arm

### Recoil speed `FUN_0884A4C4(owner)` (confirmed)
`t` = actor+0x1A8 = controller +0x78, the **motion time**. `a` = actor+0x2A0, `b` = actor+0x2A4 = controller +0x170/+0x174.
Written only by `FUN_0882FD08(ctl, start, end)`, called from the `hit_window` 0x0200 handler (0x08829AD8), the 0x0743 /
K.md op at 0x08829AE4 and 0x08829B64: `a = max(a, start)`, `b = max(b, end)` (end 0xFFFF → the motion's end,
`FUN_0882FC20`); both reset to −1 when a script starts (`FUN_0882EF10`, 0x0882F0AC). So **a / b = start / end frame of the
attacker's latest hit window so far in the current script**.
```
k = t <= a ? 1 : t >= b ? 0 : 1 − (t − a)/(b − a)
S = (2 + 3k) / 16        // 0.3125 m/tick up to the window start, falling to 0.125 at its end (consts 0x089C8A84..8C)
```
Outcome 1: slide speed S along −owner forward; outcome 2: S = 0; outcome 6 (`FUN_0884A53C` at 0x0884A5F0): **2·S**
(const 0x089C8A90 = 2.0) along rotY(normalize(owner − other), 90°). Ground slide ×0.9/tick (total ≈ 10·S).

### Wall re-arm (confirmed; vtable slots strong)
The once-per-flight gate is actor+0x384 bit 0x10000, set by the wall rush `FUN_0883C7BC` (0x0883CCC4) and tested at its
start (0x0883C8C0). It is cleared only by **state exits**: state 3 (straight flight) exit `FUN_08A61278` (vtable 0x08A61710
+0x1C) and state 0x17 (drags) exit `FUN_08A519FC` (vtables 0x08A52A30 / 0x08A51930 +0x1C). The wall rush itself switches
to state 0x12, so leaving the flight re-arms it; a floor contact that does not change state keeps it set until the flight
ends. Studio (`dmWallCheck` / `dmApplyHit`): clear `f.wallRushed` when the dummy leaves a line/drag move (end of flight,
landing, wall), not on every hit; a new flight starts armed either way.

---

## 4. plya +0x24..+0x30 (strong)
`FUN_08B39A90(actor, …)` (overlay) takes the action record from `FUN_08B2E638` and refuses the action unless
- horizontal distance to the target (`FUN_08B2E734`, y zeroed) is in **[+0x24, +0x28]**, and
- target height (`FUN_08B2E7A4`: target.y − self.y, or `FUN_08B2E804`: target.y − the floor under self) is in
  **[+0x2C, +0x30]**;
- plus: target airborne and record +0x18 bit 0x04000000 → refused; a bit 0x01 in the other record skips all windows.
Its only callers are `FUN_08B2E1EC` / `FUN_08B2E37C`, reached from `FUN_08B39024` on a timer (actor+0x12C0 accumulates to
6.0, then tries the command at +0x12BC/+0x12B8), not from button input. So: **yes, distance and height windows for automatic
action selection** (CPU/auto); whether the ordinary CPU brain uses other readers was not traced (guess: same windows).
The Studio's labels in `editors.js` (`Foe distance` 0x24/0x28, `Foe height` 0x2C/0x30) are right.

---

## 5. Sounds

### snd_rt.scd (bank 70000, cues 7000000+) (strong / guess)
Path pointers: 0x089D7E80 (static, never written) and the field/world-map file list at 0x089D80C0 (next to worldmap.bin,
R_SYM*.objx, menu files). `FUN_088E0AD8` (called from `FUN_088E8DF8` next to the `map_se_%s_rt.mpk` loader `FUN_088E897C`,
and from `FUN_0891406C`) loads it with `FUN_088FE708` and registers the buffer as resource **"talkevent"**
(`FUN_088A0DBC(*(0x089D6C80), "talkevent", buf, size, 1)`, handle at +0x1708). So it is the sound bank of **real-time /
talk event scenes** (story cutscenes on the stage, "rt"), not of fighters. No retail fighter timeline cue is in 7000000+
(shared-sounds README). The Move Studio does not need it. (load and name: confirmed; "event scenes" meaning: guess)

### Footstep slot order (confirmed, code + data)
`FUN_0883DA88` → `FUN_08896B40(stageSnd, material, kind, footwear)` reads
`id = *(records + material·0x90 + kind·0x10 + footwear·4 + 0x20)` and plays `id` as a cue (`FUN_088A24D4(1, id, 1, 0)`).
`map_sound_param.bin` ("MAPS", 5784 bytes, **identical on one00, two00, ele00**): 0x90-byte records
`{char name[0x20] ("snd_stone"…), u32 cue[7 kinds][4 footwear]}`. The ids are full cue numbers, **not** kind·4+footwear:

| kind | footwear 0 | 1 | 2 | 3 |
|---|---|---|---|---|
| 0 step (and 6) | 5000011 | 5000003 | 5000007 | 10016 (snd_common) |
| 1 landing | 5000017 | 5000015 | 5000016 | 10019 (snd_common) |
| 2 impact | 5000027 | 5000025 | 5000026 | 5000025 |
| 3 second impact | 5000022 | 5000020 | 5000021 | 5000020 |
| 4 | 100027 (snd_battle) | same | same | same |
| 5 | 1100054 | same | same | same |
(snd_stone; every material uses its own bank 50000–52900 with the same slots. sand, grass, wire_net, cloth, orgamic,
black_cloud and water give footwear 3 the footwear-1 slot (3 / 15) of their own bank instead of snd_common 10016 / 10019.)

Studio (`studio/playsnd.js`): replace `PSND_FLOOR + k*4` with this table (stone, footwear 0 → slots 11, 17, 27, 22, cue
100027, cue 1100054, 11 for kinds 0–6), or read map_sound_param.bin from the stage mpk.

---

Applied to the Move Studio 2026-11-01 (§1–§3 in `studio/dummy.js` / `actor.js`, §5 footsteps in `studio/playsnd.js`, atkp
+0x2C in `studio/editors.js`); `studio/dummy.js`'s header lists what is still approximate.
