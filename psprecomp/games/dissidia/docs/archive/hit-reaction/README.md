# Hit reactions: how a hit moves the defender

Research note, 2026-10-24. Source: the static recompilation (`work/output/generated`, `work/overlays/battle/generated`,
read with `work/magi_re/fn.py`), the EBOOT / battle-overlay images, the battle parameter pack `battle.bin`
(`work/mods/by_crc/93307c1c.bin`, ARC at file offset 0x1A0, section `spec`; mods do not touch it), and the atkp,
timeline and hit-volume sections of the 31 fighter objx files in `work/exmode_re/all/` (2,068 atkp records).
Nothing in the game, mods, tools or studio was changed.

Labels: **confirmed** = read end to end in code; **strong** = code read, one link inferred (a field name, VFPU
semantics, state set elsewhere); **guess** = inferred from data or naming.

Units: world unit = metre (see fx-components/A.md), 1 tick = 1/60 s, `dt` = 1 at normal speed
(`FUN_08838300` = actor clock × global speed 0x089D5EAC; hit-stop/slow-motion would go through it).

## Summary

1. **The reaction byte (+0x05) picks one of 21 handlers** (jump table 0x089C80D8 in `FUN_0883A6FC`). The
   meaning of the float block +0x18..+0x2C depends on the reaction (table in section 2). It runs on the tick the
   volume touches; nothing is queued. (confirmed)
2. **Three movement models**, all position based except the launch:
   - **Held / dragged (reactions 11, 12, 13, 14, 15)**: the defender is moved from where it stood to a target
     point = attacker position (or attacker bone +0x2C) + rotY(+0x18 rad)·attacker_forward·(+0x1C) + (0,+0x20,0),
     over +0x24 ticks with an ease-out `u = sin(π/2·t/T)`; for the first +0x28 ticks the target is recomputed every
     tick from the attacker's current position and facing (`follow`). State 0x17. (confirmed)
   - **Straight flight (reactions 3, 5, 6, 7, 8, 20)**: constant velocity along a direction for +0x18 metres at
     +0x20 m/tick, then free fall with the velocity carried over. State 3. (confirmed)
   - **Launch / knock (reactions 1, 2, 4, 19)**: rise to a height with a double-sine ease, then a gravity fall;
     horizontal speed decays ×0.975 per tick. States 4 / 2 / 1. (confirmed)
3. **Gravity during any hit reaction is the same for every fighter**: `spec` record 0, `g = 2·3.75/42² =
   0.0042517 m/tick²` (15.3 m/s²), terminal fall 0.6 m/tick, rise time `T = 42·sqrt(h/3.75)` ticks. (confirmed)
4. **Hit-stun is set by the reaction motion, not by a field**: every fighter's 500/501/550/551/560/561 motions end
   at frame 50 (`end_of_move`, keyed 0x0002) and only then hand over to stand (script 1) or fall (101, which has
   `can_act` from 0). A new hit restarts the reaction from scratch; there is no juggle decay or counter in this
   path. Blow-aways (700/750) have no `can_act`; landing from them plays 702/752 (down, invincible 0–100, can act
   at 70). (confirmed for the code and all 31 timelines; "nothing else limits juggles" is strong)
5. **Flags bit 0x02 = HP attack** (damage = attacker's current Brave, power ignored). HP and Brave hits use the
   same reaction table. (confirmed)
6. **Hit-stop: not found.** +0x0D is not read on the fighter melee path. (open)
7. **Front / back**: a hit counts as "front" when the defender's facing is more than 90° from the push direction
   (i.e. it faces the attacker). Front picks 500 / 560 / 700, back 501 / 561 / 750, and the defender is turned to
   face the attacker (front) or away from it (back). (confirmed)

---

## 1. The hit path

`FUN_08809CEC` (contact pass, per actor) → for each contact of an active hit object with the defender's kind-3
volume set (actor+0x470) or kind-2 set (actor+0x46C) → `FUN_08808EE4(contact, defender, set, kind3flag)`:

- skipped if `FUN_088377D0` (invulnerable) unless atkp flags & 0x20; skipped if defender vtable +0x138 / +0x148
  refuse. (confirmed)
- hit object vtable +0x30 (`FUN_0880B9C4`) → `FUN_088080C4` → `FUN_08807D4C` (may this object hit) →
  `FUN_0880AD28` (per-group "already hit" bitset, rehit timers) → `FUN_08807E6C` builds the 64-byte hit record
  (`FUN_08813120` / `FUN_088131BC`) and plays hit sound / shake. (confirmed)
- defender vtable +0x70 = `FUN_08B2DE00` (fighters) → **`FUN_0883A150` (take hit)** → `FUN_0883A6FC` (reaction
  dispatch). (confirmed)

### Hit record (64 bytes, pool at 0x089D47B0)
| Off | Meaning | Where | Conf. |
|---|---|---|---|
| +0x00 | push direction: (defender.pos − attacker.pos) with y = 0, normalised; −defender forward if zero or no attacker (melee hit object vtable +0x18 = `FUN_08807AB8`) | `FUN_08813010` | confirmed |
| +0x10 | second vector from hit object vtable +0x20 (melee: hit object +0x00; used by reaction 20 and for non-melee front/back) | `FUN_08813010` | strong |
| +0x20 | attacker; +0x24 defender; +0x2C hit object; +0x30 atkp record | `FUN_088131BC` | confirmed |
| +0x28 | flags: bit 0 set by every handler and pre-set when the hit is not "fresh" (see 3.4); bit 1 = contact was with the kind-3 set | `FUN_088080C4`, `FUN_08807E6C` | confirmed (meaning of bit 0 open) |
| +0x34/+0x35 | reaction byte (atkp +0x05) | `FUN_088131BC` | confirmed |
| +0x38 | damage, computed only for HP hits | `FUN_088131BC` | confirmed |

### Before the dispatch (`FUN_0883A150`, `FUN_0883A6FC`)
- defender+0x304 = attacker (the "last attacker", used for `follow`). (confirmed)
- HP hit (flags & 2): `FUN_08831EA8` sets the HP-damage status bits, defender+0x53E = damage. (confirmed)
- Front flag = `FUN_0883764C`: angle between defender forward (+0x90) and the push direction > 90° → front;
  stored as a status bit (mask 0x089C7F28). (confirmed)
- No reaction at all when defender +0x197 bit 0x80 or +0x188 bit 0x20 or status mask 0x089D8098 (armour-type
  states), and an ability check (0x2D / 0x2E / 0x100 counters) can absorb Brave hits. (confirmed for the checks,
  names guess)
- Status mask 0x089C80A0 (actor+0x388 bit 0x20) turns any non-zero reaction into 1 with no launch and a 0.5 slide.
  (confirmed; which state sets the bit is open)
- Defender is turned to −push (front) or +push (back): `FUN_08835BA4`. (strong: function name inferred)

## 2. atkp fields and the 21 reactions

### Fixed fields
| Off | Meaning | Where | Conf. |
|---|---|---|---|
| +0x00 | id (binary search) | `FUN_08837F14` | confirmed |
| +0x04 | class: not read on the reaction path | — | open |
| +0x05 | reaction 0–20 | `FUN_0883A6FC` | confirmed |
| +0x08 | flags: 0x02 HP attack; 0x20 ignore invulnerability; 0x100 lets `FUN_08B33938` run its Brave/HUD part even when `FUN_08B34488` fails; 0x10 only seen together with 0x02 | `FUN_088132BC`, `FUN_08813510`, `FUN_08808EE4`, `FUN_08B33938` | confirmed / 0x100, 0x10 guess |
| +0x0D | not read for fighter attackers; for other attackers passed to `FUN_08ABB690` / `FUN_08ABB60C` (`FUN_08808C98`). Values 0–20. **Not shown to be hit-stop.** | | open |
| +0x10 | power (s16). Ignored for HP hits: damage = attacker Brave (`*(brave obj +0x14)`) | `FUN_08813510` | confirmed |
| +0x12 | rehit ticks: when > 0 the group's "already hit this defender" bit is cleared again after this many ticks | `FUN_0880AAE0`, `FUN_0880A9E8` | confirmed |
| +0x14 | if > 0 on a fresh hit by a fighter: `FUN_08ACEEF4(mgr, attacker, float)` | `FUN_08807E6C` | confirmed call; guess: EX force |
| +0x15 / +0x16 / +0x17 | screen shake: duration, amp ×0.01, amp ×0.01 (`FUN_088749A0`) | `FUN_08807E6C` | confirmed |
| +0x30 | hit sound (−1 none) | `FUN_088A24D4` | confirmed |

### Reaction table (`FUN_0883A6FC`, accessors `FUN_0881334C` / `FUN_0881341C` / `FUN_088133B4` / `FUN_08813484` / `FUN_088132D0`)
`push` = record +0x00 (horizontal, away from attacker). `A` = attacker. `fr` = front flag. Retail counts are of
the 2,068 records.

| R | n | Handler | Uses | Movement | Script | Conf. |
|---|---|---|---|---|---|---|
| 0 | 276 | — | — | none (damage / brave only) | — | confirmed |
| 1 | 305 | `FUN_08831F04` / `FUN_08832034` | +1C, +20, +24 | if +1C = 0 and on the ground: ground slide along push at +0x24 m/tick, ×0.9 per tick (`FUN_08832150`, `FUN_08832244`). Else launch: height +1C, horizontal speed +20 along push | 500/501 (alternate, else fr) / 550/551 | confirmed |
| 2 | 84 | `FUN_0883241C`(…,1) | +24, +28, +2C | slide +24 along push; launch height +28 (no horizontal) | 600 | confirmed |
| 3 | 5 | `FUN_08832560` | +18, +20, +24, +28, +2C | if height above floor (+0x3C0) ≤ 2: snap to floor, slide +24, launch height +28, script 601. Else straight flight **down** (−A up axis) +18 m at +20 m/tick | 601 / 700 | confirmed |
| 4 | 354 | `FUN_088325D0`(0) → `FUN_08A56F98` | +1C, +20 | launch: height +1C, horizontal speed +20 along push | 700 | confirmed |
| 5 | 150 | `FUN_08832744`(0) | +18, +20 | straight flight along (defender − attacker) in **3D**, +18 m at +20 m/tick | 700 fr / 750 | confirmed |
| 6 | 49 | `FUN_0883265C`(1) | +18, +20 | straight flight along A's up axis (A matrix row +0x10), +18 m at +20 | 700 | strong (up axis from `FUN_08836648`) |
| 7 | 76 | `FUN_0883265C`(0) | +18, +20 | as 6 but downward | 700 | strong |
| 8 | 42 | `FUN_08832744`(1) | +18, +20 | straight flight along push, +18 m at +20 | 700 fr / 750 | confirmed |
| 9 / 10 | 0 | `FUN_08832744`(2/3) | none | A forward rotated −90° / +90°, distance 0 (accessor returns 0) | 700/750 | confirmed, unused |
| 11 | 93 | `FUN_08832BB0`(0) → `FUN_08A51570` | +18…+2C | dragged to target (no up in target), arc `+20·sin(π/2·u)`, **linear u**; then a drop phase (below) | 700 fr / 750 → 701/751 | confirmed |
| 12 | 86 | `FUN_08832BB0`(1) | +18…+2C | dragged to target **including** up, linear u; then free fall with carried velocity, air drag 0.90 | 700/750 → 701/751 | confirmed |
| 13 | 511 | `FUN_08832C20` → `FUN_08A527CC` | +18…+2C | dragged to target including up, **eased u**, then free fall straight down (horizontal velocity zeroed) | 560 fr / 561 (alternate) | confirmed |
| 14 | 21 | `FUN_08832D64` → `FUN_08A51750` | +18, +1C, +20, +28, +2C | variant of 11/12 (not read in detail) | | guess |
| 15 | 2 | `FUN_08832D9C`, else as 12 | | | | guess |
| 16 | 0 | — | | none | | confirmed |
| 17 | 4 | `FUN_08832560`, else as 1 | | like 3 on the floor, otherwise like 1 | | confirmed |
| 18 | 2 | — | | defender+0x31C = 60.0 (a 60-tick timer); no movement | | confirmed (timer meaning open) |
| 19 | 3 | `FUN_088325D0`(1) | +1C, +20 | like 4 but hangs in the air for a few ticks first (state 2 timer, consts 0x08A5712C…) | 700 | strong |
| 20 | 5 | `FUN_08832744`(4) | +18, +20 | straight flight along record +0x10 (stage steam vents) | 700/750 | confirmed |

So the HIT_FIELDS names fit only some reactions: for 11–15 `kb_yaw` = yaw in **radians** about +Y applied to the
attacker's forward, `kb_forward` = distance in front (m, negative = behind), `kb_up` = height above the attacker's
feet (or bone), `stun` = ticks to reach the target, `follow` = ticks the target keeps tracking the attacker,
`kb_bone` = attacker bone for the origin (−1 = root). For 5–8/20 `kb_yaw` is a **distance** and `kb_up` a **speed**.
For 1/4/19 `kb_forward` is a **height** and `kb_up` a **speed**.

## 3. Motion after the hit

### 3.1 Dragged states (11–15): state 0x17, block at mv+0x80 (mv = actor+0x314)
Setup (`FUN_08A527CC`, `FUN_08A51570`): start = defender position, yaw/fwd/up/dur/follow/bone copied, `t = dt`.
Target (`FUN_08A51AA0`): `dir = normalize(A.forward with y=0)`, rotated about +Y by +0x18 radians
(`x' = x cos a + z sin a`, `z' = −x sin a + z cos a`), × +0x1C; origin = A bone +0x2C if > 0 else A position;
`target = origin + dir (+ (0,+0x20,0) when flag 0x01000000)`. (confirmed; rotation sign strong — VFPU vrot)

Per tick, `FUN_08A51C80` (before) and `FUN_08A52384` (displacement):
| Phase | What happens | Conf. |
|---|---|---|
| 0 | if `t ≤ follow`: recompute target (follow is forced to 0 when A+0x380 bit 0x01000000). `u = t/dur` (linear when flag 0x02000000, else `sin(π/2·t/dur)`). point = lerp(start, target, u); if flag 0x01000000 clear: point.y += +0x20·sin(π/2·u). Move = point − previous point; if the defender is off the curve by more than 1.5× that step, it moves that step length straight toward the point instead. Ends when `t > dur` (or `t > 5` and defender+0x380 bit 0x01000000). | confirmed |
| 1 (one tick) | 700→701 / 750→751 (blend 16). 510/511 leave the state at once. If flag 0x01000000 or up ≤ 0: **end** (below). Else set up the drop: new start = old target, new target = old target + 0.7·(old target − old start) (0.1 for scripts 610/611), `dur = up / (0.09·P60)` = up/0.09 ticks. | confirmed |
| 2 | drop: u = t/dur (linear), point = lerp + up·cos(π/2·u) | confirmed |
| 3 / end | `FUN_08834934` sets landing scripts (−1 / 101, 102); with flag 0x08000000 (11, 12): `FUN_0883497C(−0.05)` = keep last tick's horizontal velocity, air drag 0.95−0.05, keep falling at least as fast as now, free fall. Else (13): zero horizontal velocity, free fall (`FUN_08934054` → `FUN_08A5BE70(h=0)` → state 1). | confirmed |

After phase 0 with t > 2 and flag 0x01000000, a collision along the velocity calls `FUN_0883C7BC`
(wall / floor rush: turn to the wall, state 0x12, script 704 / 754, then 101). (confirmed call, details strong)

### 3.2 Straight flight: state 3 (`FUN_08A6158C`, update `FUN_08A6146C`, end `FUN_08A61298`)
`v = dir·dist` with speed `w`; `T = |v|/w`; each tick move `normalize(v)·w·dt` (no gravity). At `t > T`: 700→701 /
750→751 (blend 20), carry the velocity into a free fall (`FUN_0883497C(0)`, air drag 0.95). Wall check each tick as
above. (confirmed)

### 3.3 Launch and free fall: states 1 / 2 / 4 (`FUN_08A5BF84`, `FUN_08A5BE70`, `FUN_08A5B59C` → `FUN_08A5BC58` → `FUN_08A5B724`)
Vertical (block mv+0x10: t, T, y0, h, …; y0 = height when the state is entered, `FUN_08A5B40C`):
```
g    = P36 / (0.5 * P40²)          // spec record 0: P36 = 3.75 m, P40 = 42 ticks → 0.0042517 m/tick²
h    = min(height, 100);  T = sqrt(2h/g)                 // = 42·sqrt(h/3.75)
t < T : y = y0 + h·P56·sin(π/2 · sin(π/2 · t/T))          // P56 = 1: double-sine ease-out, no overshoot
t ≥ T : y = y0 + h·P56 − 0.5·g·m·(t−T)²·P60·gravMul       // m eases toward its target (both 1 here), gravMul = actor+0x3DC (op 0x013B)
dy clamped to [P96·dt, block+0x10·dt]                     // P96 = −0.6 m/tick
```
Horizontal (`FUN_088344E8(a=0.9, b=0.95+drag)`, per-tick factors rescaled by dt: `1−(1−k)·dt/2`):
the launch speed is stored in the push velocity actor+0xC0; with no steering input it is multiplied by
`0.975` per tick (`0.95` after `FUN_0883497C(−0.05)`), and the move is that velocity × dt. (confirmed; "no input
during hit-stun" strong)

The hit reaction always uses `spec` record 0 (`FUN_08A5C24C`: script 500–798 → record 0, else the fighter's own,
e.g. record 1: 3.5 m / 48 ticks). Values from `battle.bin` spec record 0: +12 1.8 (body height?), +36 3.75,
+40 42, +56 1, +60 1, +68 3.75, +72 42 (alternate set when actor+0x384 bit 0x80), +88 1, +92 1, +96 −0.6. (confirmed)

Ground slide (reaction 1 on the ground, 2, 3): `FUN_08832244`: move `dir·S·dt`, `S ×= 0.9^dt`, stop below 0.01;
total ≈ 10·S metres. (confirmed)

### 3.4 Landing, down, getting up
- Air states land through state vtable +0x40 = `FUN_08A5B690`: if the script is 700/750 (`L_08838120`) or 701/751
  (`L_088380F8`) → fighter vtable +0xC8 = `FUN_08833E2C`: play **702** (from 700/701) or **752**, state 0. Else
  vtable +0xC0 → `FUN_08833C98`: play the queued landing script (102 for hit reactions: `can_act_now` from 14), or
  stand at once when already on the floor with actor+0x384 bit 0x04000000. (confirmed)
- 702/752 (all 31 fighters): invincible 0–100, `can_act` / `can_act_now` from 70, wake-up invincibility 8 ticks
  (op 0x017C). So a blow-away ends ~70 ticks after touching down. (confirmed from data)
- `landing_allowed` (0x002D) is absent from 500–561, so a held defender cannot land before frame 50; 101 / 701 /
  751 have it for the whole motion. (confirmed from data)

## 4. Hit-stun and juggling

| Rule | Evidence | Conf. |
|---|---|---|
| Flinch 500/501, stagger 560/561, launch 550/551 last **50 ticks** (keyed `end_of_move` @50 in all 31 fighters; it sets motion-player +0x54 bit 4, which `FUN_08830444` treats as "motion ended"). Then 500/510 → stand (script 1, blend 16, queued by `FUN_08831F04`), 560/561 → fall 101 (`can_act` from 0, queued by `FUN_08A527CC`). | timelines, code | confirmed (550 → next script: strong) |
| The movement part (`stun` +0x24 for 11–15) is shorter than the motion: after it the defender just free-falls from where it was left, still unable to act until frame 50. | §3.1 | confirmed |
| Every hit re-runs the handler: new start = current position, t = 0, new motion from frame 0. So later hits refresh the full 50 ticks and re-anchor the defender. No decay, counter or juggle cap was found in `FUN_0883A150` / `FUN_0883A6FC` or the states. | code | confirmed for these functions; strong overall |
| Airborne defenders can be hit (no invincible ranges in 500–751); 702/752 are invincible (no OTG). | timelines | confirmed |
| Height in a juggle: a 13 hit puts the defender at attacker feet + `kb_up` (e.g. Bartz AIR_MIDDLE1_1 up 0.1 → level with the attacker), then it falls with g from h = 0. | §3.1, §3.3 | confirmed |
| `follow`: the target is re-taken from the attacker's current position and facing for `follow` ticks, so the defender moves with a dashing attacker. | `FUN_08A51C80` | confirmed |
| A group can hit a defender once per hit window; `rehit` re-arms it every N ticks. A separate 30-tick "fresh hit" timer (+0x44 of the defender's hit tracker at actor+0x3AC+0x28, `FUN_0880ACBC`, const 0x089C6CA0 = 30) gates the EX-force (+0x14) award and pre-sets record bit 0. | code | confirmed mechanism; effect of bit 0 open |
| Hit-stop | not found | open |

## 5. Hit volumes (objx section type 5)

Header `u32 1, u32 0, u32 count, u32 0`, then 32-byte records `{u32 group, u8 kind, u8 3, u16 bone, f32 off[3],
f32 0, f32 r, f32 r}` (all 2,886 retail records: byte 5 = 3, +0x1C = 0, the two radii equal).

| Item | Finding | Conf. |
|---|---|---|
| Placement | `FUN_08836648`: bone 0x4000 = the actor's root matrix (actor+0x00, a 4×4: row +0x10 is the up axis); other bones = bone matrix × model matrix (actor+0x330). The offset is in that local frame: root volumes use (0, height, forward), weapon volumes put spheres along the bone's local X (Bartz r_wep: x = 0.50, 0.90, 1.40). | strong |
| Sets | `FUN_08831808` builds one collision set per kind with `FUN_0883E6E8` (count of records of that kind over all groups, 28-byte elements): actor+0x46C kind 2, +0x470 kind 3, +0x474 kind 6, +0x478 kind 5, +0x47C 0xD, +0x480 0xE, +0x484 0, +0x488 8, +0x48C 4. | confirmed |
| Hurt volumes | the contact pass checks the defender's **kind 3** set, then **kind 2**. Every fighter has exactly two kind-2 records (hip + neck, r 0.3 on Bartz) and two kind-3 records (both shins or thighs, r 0.3). Probably capsules between the two points. | sets confirmed; capsule guess |
| Other group-0 kinds | 0: root, (0, 1.1, 0) r 0.9 (body / push?); 1: (0, 0.9, 0) r 0.6; 11: (0, 1.25, 0) r 0.6; 15 and 10: small spheres on the chest bones (lock-on / camera points?). | guess |
| Attack volumes | groups ≥ 1, kinds 4 (1,159), 6 (871), 5 (468), 7 (87); `hit_window {atkp, group}` arms all records of that group. How kinds 4–7 differ was not settled. | partial |
| Test | the sphere/capsule overlap itself runs in the collision system before `FUN_08809CEC` and was not read. | open |

## 6. Checks against retail data (Bartz fiv100)

- Ground combo NEAR / NEAR_COMBO / NEAR_COMBO3 (13: 0, 1.3, 0.01, 16, 16): the foe is pulled to 1.3 m in front of
  Bartz over 16 ticks, tracking him the whole time; each hit refreshes 50 ticks of stun. NEAR_COMBO5 pushes it to
  2.5 m over 22 ticks.
- Launcher NEAR_FLOAT (11: 0, 1.4, 4, 60): 1.4 m in front, arc 4 m high over 60 ticks (700/750), then a drop of
  4/0.09 ≈ 44 ticks drifting 0.7× further → lands → 702/752 down unless hit again. Air follow-ups AIR_MIDDLE1_1
  (13: 0, 2, 0.1, 6, 6, rehit 2) hold the foe 2 m ahead at Bartz's own height; between hits it falls with g
  (≈ 1.9 m in 30 ticks).
- Finisher NEAR_COMBO2 (12: 0, 0.01, 15, 50): blown straight up 15 m over 50 ticks, then free fall (≈ 84 ticks from
  15 m), 701/751 → 702/752. MIDDLE_2COMBO3 (12: 0, 15, 4, 20): 15 m away and 4 m up in 20 ticks. BR_A_IMPACT2_1
  (12: 0, 8, −17, 20): smashed 17 m down.
- HP attacks (flags 0x12/0x92, power 0): HP_A_BLADE1_2 (5: 20 m at 1 m/tick, 700/750), LAND_FINISH2 (6: 20 m up at
  0.7), AIRLONG2_2 (4: 7 m high, no horizontal speed).
- Chase hits (1121–1123): SE_ATTACK_UPPER 6 (15 m up at 0.5), SE_ATTACK_LOWER 7 (15 m down), CHASE_ATTACK 5
  (15 m away at 0.5); these are the 700/701/750/751 cases the `chase_window` op checks.

## 7. Not settled
- Hit-stop (no reader of +0x0D on the fighter path; maybe none exists, or it lives in the global speed 0x089D5EAC).
- The effect of hit-record bit 0 (pre-set on non-fresh hits) and kind-3 contacts (record bit 1).
- Reactions 14 and 15 in detail; the meaning of class +0x04; flag bits 0x10, 0x100.
- The geometric test (sphere vs capsule) and the roles of attack kinds 4–7 and body kinds 0/1/10/11/15.
- Which states set the "light flinch" bit (actor+0x388 bit 0x20) and actor+0x384 bit 0x04000000.

## 8. JS for the studio

Pseudo-code for a dummy foe at 60 ticks/s. `hit = {atkp, attackerPos, attackerFwd (unit, y=0), attackerUp,
attackerBonePos(bone)}`. Y is up. Positions in metres.

```js
const G = 2 * 3.75 / (42 * 42);           // 0.0042517 m/tick², spec record 0
const MAX_FALL = 0.6, H_MAX = 100;
const STUN_MOTION = 50;                   // 500–561 end_of_move
const DOWN_CAN_ACT = 70;                  // 702/752

function rotY(v, a) { const c = Math.cos(a), s = Math.sin(a);
  return { x: v.x * c + v.z * s, y: v.y, z: -v.x * s + v.z * c }; }
const add = (a, b) => ({ x: a.x + b.x, y: a.y + b.y, z: a.z + b.z });
const sub = (a, b) => ({ x: a.x - b.x, y: a.y - b.y, z: a.z - b.z });
const mul = (a, k) => ({ x: a.x * k, y: a.y * k, z: a.z * k });
const len = a => Math.hypot(a.x, a.y, a.z);
const norm = a => { const l = len(a); return l > 0 ? mul(a, 1 / l) : { x: 0, y: 0, z: 0 }; };
const lerp = (a, b, u) => add(a, mul(sub(b, a), u));
const fwdOf = f => ({ x: Math.sin(f.facing), y: 0, z: Math.cos(f.facing) });
const angleBetween = (a, b) => Math.acos(Math.max(-1, Math.min(1, a.x * b.x + a.z * b.z)));

function makeFoe(pos, facing) {
  return { pos, facing, state: 'idle', motion: null, motionT: 0, canActIn: 0,
           move: null, vel: { x: 0, y: 0, z: 0 }, drag: 0.975, air: null, lastAttacker: null };
}

// air block: rise with double sine to y0+h, then gravity
function startAir(f, h, maxDy = 0) { h = Math.min(Math.max(h, 0), H_MAX);
  f.air = { t: 0, T: Math.sqrt(2 * h / G), y0: f.pos.y, h, maxDy }; }
function airDy(f) { const a = f.air; a.t += 1;
  let y = a.t < a.T ? a.y0 + a.h * Math.sin(Math.PI / 2 * Math.sin(Math.PI / 2 * a.t / a.T))
                    : a.y0 + a.h - 0.5 * G * (a.t - a.T) ** 2;
  let dy = y - f.pos.y;
  if (a.t >= a.T) dy = Math.min(Math.max(dy, -MAX_FALL), a.maxDy);   // falling: clamp to [-0.6, maxDy]
  return dy; }

function applyHit(f, hit) {
  const k = hit.atkp, R = k.reaction; if (R === 0 || R === 16) return;
  f.lastAttacker = hit;
  let push = sub(f.pos, hit.attackerPos); push.y = 0; push = len(push) ? norm(push) : mul(fwdOf(f), -1);
  const front = angleBetween(fwdOf(f), push) > Math.PI / 2;
  f.facing = Math.atan2(front ? -push.x : push.x, front ? -push.z : push.z);
  const airborne = f.pos.y > 0.001 || f.state === 'air' || f.state === 'launched';
  const play = (m) => { f.motion = m; f.motionT = 0; };
  const fly = (dir, dist, speed, m) => { f.state = 'launched'; play(m);
    f.move = { kind: 'line', dir: norm(dir), speed, T: speed ? dist / speed : 0, t: 0 }; };
  const launch = (h, speed, m) => { f.state = 'air'; play(m); f.vel = mul(push, speed); f.drag = 0.975; startAir(f, h); };

  switch (R) {
    case 1: if (k.kb_forward === 0 && !airborne) { f.state = 'stun'; play(front ? 500 : 501);
              f.move = { kind: 'slide', dir: push, S: k.stun }; }
            else launch(k.kb_forward, k.kb_up, front ? 550 : 551); break;
    case 4: case 19: launch(k.kb_forward, k.kb_up, 700); break;
    case 2: f.state = 'air'; play(600); f.move = { kind: 'slide', dir: push, S: k.stun };
            f.vel = { x: 0, y: 0, z: 0 }; startAir(f, k.follow /* +0x28 = height here */); break;
    case 5: fly(sub(f.pos, hit.attackerPos), k.kb_yaw, k.kb_up, front ? 700 : 750); break;
    case 6: fly(hit.attackerUp, k.kb_yaw, k.kb_up, 700); break;
    case 7: fly(mul(hit.attackerUp, -1), k.kb_yaw, k.kb_up, 700); break;
    case 8: fly(push, k.kb_yaw, k.kb_up, front ? 700 : 750); break;
    case 11: case 12: case 13: {
      const arc = R === 11, linear = R !== 13;
      f.state = R === 13 ? 'stun' : 'launched';
      play(R === 13 ? (front ? 560 : 561) : (front ? 700 : 750));
      f.move = { kind: 'drag', start: { ...f.pos }, prev: { ...f.pos }, t: 1, phase: 0, arc, linear, R,
                 yaw: k.kb_yaw, fwd: k.kb_forward, up: k.kb_up,
                 dur: k.stun > 0 ? k.stun : STUN_MOTION, follow: k.follow, bone: k.kb_bone };
      f.move.target = dragTarget(f.move, hit); break; }
    default: f.state = 'stun'; play(front ? 500 : 501);      // 3, 14, 15, 17, 18, 20: approximations
  }
  f.canActIn = (f.motion >= 500 && f.motion <= 561) ? STUN_MOTION : Infinity;
}

function dragTarget(m, hit) {
  const origin = m.bone > 0 ? hit.attackerBonePos(m.bone) : hit.attackerPos;
  let t = add(origin, mul(rotY(hit.attackerFwd, m.yaw), m.fwd));
  if (!m.arc) t.y += m.up;                 // 12, 13: up is part of the target
  return t;
}

function tick(f, attackerNow /* same shape as hit, for follow */) {
  f.motionT += 1; if (f.canActIn > 0) f.canActIn -= 1;
  const m = f.move;
  if (m && m.kind === 'drag') {
    if (m.phase === 0) {
      if (m.t <= m.follow && attackerNow) m.target = dragTarget(m, attackerNow);
      let u = Math.min(m.t / m.dur, 1); if (!m.linear) u = Math.sin(Math.PI / 2 * u);
      const p = lerp(m.start, m.target, u); if (m.arc) p.y += m.up * Math.sin(Math.PI / 2 * u);
      stepToward(f, m, p);
      if (m.t > m.dur) {
        if (f.motion === 700 || f.motion === 750) f.motion += 1;   // 701 / 751
        if (m.arc && m.up > 0) {                                   // reaction 11: drop phase
          const d = mul(sub(m.target, m.start), 0.7);
          m.start = { ...m.target }; m.target = add(m.target, d);
          m.dur = m.up / 0.09; m.t = 0; m.phase = 2;
        } else endDrag(f, m);
      }
    } else if (m.phase === 2) {
      const u = Math.min(m.t / m.dur, 1);
      const p = lerp(m.start, m.target, u); p.y += m.up * Math.cos(Math.PI / 2 * u);
      stepToward(f, m, p); if (u >= 1) endDrag(f, m);
    }
    m.t += 1;
  } else if (m && m.kind === 'line') {
    m.t += 1; f.pos = add(f.pos, mul(m.dir, m.speed));
    if (m.t > m.T) { f.motion = f.motion === 700 ? 701 : f.motion === 750 ? 751 : f.motion;
      f.vel = { x: m.dir.x * m.speed, y: 0, z: m.dir.z * m.speed }; f.drag = 0.975;
      f.move = null; f.state = 'air'; startAir(f, 0, Math.min(0, m.dir.y * m.speed)); }
  } else if (m && m.kind === 'slide') {
    if (m.S >= 0.01) { f.pos = add(f.pos, mul(m.dir, m.S)); m.S *= 0.9; } else f.move = null;
  }
  if (f.air) {                               // states 1/2/4 and the fall after drags/lines
    f.vel = mul(f.vel, f.drag);
    f.pos = add(f.pos, { x: f.vel.x, y: airDy(f), z: f.vel.z });
  }
  if (f.pos.y <= 0 && (f.air || f.state === 'launched')) land(f);
  if (f.state === 'stun' && f.motionT >= STUN_MOTION) { f.state = f.pos.y > 0 ? 'falling' : 'idle'; f.canActIn = 0; }
}

function stepToward(f, m, p) {               // FUN_08A52384
  const step = sub(p, m.prev); m.prev = p; m.lastStep = step;
  let d = sub(p, f.pos); const s = len(step);
  if (len(d) > 1.5 * s) d = mul(norm(d), s);
  f.pos = add(f.pos, d);
}
function endDrag(f, m) {
  f.move = null;
  if (m.R === 13) { f.vel = { x: 0, y: 0, z: 0 }; startAir(f, 0); }        // falls straight down
  else { const s = m.lastStep || { x: 0, y: 0, z: 0 };                    // FUN_0883497C(-0.05)
    f.vel = { x: s.x, y: 0, z: s.z }; f.drag = 0.95; f.state = 'air'; startAir(f, 0, Math.min(0, s.y)); }
}
function land(f) {
  f.pos.y = 0;
  const held = f.motion >= 500 && f.motion <= 561 && f.motionT < STUN_MOTION;  // no landing_allowed yet
  if (held) return;                                       // stays on the floor until frame 50
  f.air = null; f.vel = { x: 0, y: 0, z: 0 };
  if ([700, 701, 750, 751].includes(f.motion)) { f.motion = f.motion < 750 ? 702 : 752; f.motionT = 0;
    f.state = 'down'; f.canActIn = DOWN_CAN_ACT; f.invincibleFor = 100; }
  else { f.motion = 102; f.motionT = 0;
    f.state = 'recovering'; f.canActIn = 14; }
}
```
Notes for the studio: while `state === 'down'` and `invincibleFor > 0` hits should not connect. The connect test is not
settled: until it is, test the active group's spheres (bone matrix × offset, radius r) against a capsule from the
defender's hip to neck (r 0.3) and one between the shins (r 0.3).
