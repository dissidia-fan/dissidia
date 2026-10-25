# Guard, dodge and walls: what the defender can do, and what the stage does

Research note, 2026-10-24. Source: the static recompilation (`work/output/generated`, `work/overlays/battle/generated`,
read with `work/magi_re/fn.py`), the EBOOT image (`work/eboot_image.bin`, base 0x08804000), the battle overlay image
(`overlays/battle/data_sections.cpp`, segment 0x089DD4C0), the battle parameter pack `spec` records
(`work/mods/by_crc/93307c1c.bin`, 40 records of 380 bytes from file offset 0x248) and the atkp / timeline / motion-map
sections of the 31 retail fighters (`work/exmode_re/all/*.objx`, unpacked with `charmod_objx.unpack`). Nothing in the
game, mods, tools or studio was changed. Follows on from [../hit-contact/README.md](../hit-contact/README.md) (the
contact test and the clash call), [../hit-reaction/README.md](../hit-reaction/README.md) (reactions, flight states)
and [../attacker-movement/README.md](../attacker-movement/README.md) (homing vs a busy foe).

Labels: **confirmed** = read end to end in code; **strong** = code read, one link inferred; **guess** = inferred from
data or naming. Units: metres, 1 tick = 1/60 s. "Script" = actor+0x180; "atkp class" = atkp +0x04.

## Summary

1. **A guard is an attack.** Block = script 470 (ground) / 471 (air), state 0x15. Its motion (20/21) runs
   `hit_window GUARD` over ticks 0–36: the group holds one kind-6 sphere on the root (Bartz (0, 0.85, 0) r 1.1) and
   the atkp has class 4, reaction 0, angles (+0x0E, +0x0F) = (0, 90) in all 31 fighters. Nothing else marks the
   guard: an incoming kind-4 sphere that touches that kind-6 sphere goes through the **clash rules**
   (`FUN_08809F00`), and the outcome table decides who bounces. (confirmed)
2. **The clash table `T[self class][other class]`** (0x089D4978, 16×16 u32, `FUN_0884A454`) gives each side an
   outcome that `FUN_0884A53C` applies to that side's owner: **0** nothing, **1** recoil backwards (script 901/911,
   slide 1.25–3.1 m), **2** recoil in place, **3** "guarded" (no movement, may cancel the guard at once), **4** take
   the other side's hit through the guard ("guard crush"), **5** both cancelled (1 + 1 becomes 5 + 5), **6** recoil
   sideways (90° from "away from the other"), double speed. Codes 1/2/3/5/6 for the attacker cancel its hit object
   (no hit from it this window). (code confirmed; names of 4/6 strong)
3. **Which hits a guard stops** depends only on the attacker's class (no flag, no power): class 0 (ordinary Brave
   melee), 9 (`FACT_*`) are bounced (attacker 1, guard 3); 7 (magic / shots) is bounced sideways (attacker 6, guard 3);
   8 is bounced sideways and the guard also recoils (6 / 1); **classes 2 and 3 crush the guard** (attacker 0, guard
   4 → the hit is delivered); class 1 is not stopped and pushes the guard back (0 / 1); 5, 6 (dash), 10–15 pass
   (0 / 0) and then hit normally. A **perfect guard** (first 4 ticks, ability 0x40) turns the guard into class 12:
   classes 2 and 3 are then stopped too (class 2: both cancelled; class 3: attacker knocked sideways, guarder
   recoils). (confirmed)
4. **Front only, by swing direction**: the guard's gate passes when the angle between the guarder's forward and the
   reversed blade-motion direction (y = 0) is ≤ 90°. A thrust from behind is not blocked. (strong; gate function
   from the hit-contact note)
5. **Blocked attacker**: recoil script 901 (ground, motion 6: invincible 0–10, `can_act_now` from 80) sliding back
   along its own −forward at S = 0.125–0.3125 m/tick, ×0.9 per tick (≈ 10·S m), or 911 in the air (motion 7,
   launched up-and-back). **The guarder** gets no stun and no push-back on a clean guard; it may leave the guard
   the next tick it has input (`+0x384` bit 0x40000). (confirmed)
6. **Dodge** = scripts 400 (back / neutral, faces the foe), 460 (sideways: turns to the stick and runs that way),
   410 (toward the foe: a hop toward the lock-on point), 440 (air). All: **invincible 0–20** (0–40 with ability
   0x136), `perfect_dodge_window` 0–4, attack cancel 26–56 or 40–56, `can_act_now` from 56. Ground dodges have no
   movement code: the distance is the motion's root motion (`trans_include`); 410's hop is 3–4 m high at 0.1–0.3
   m/tick (spec record). (confirmed)
7. **"Busy" is dodging only**: `FUN_08837458` = script 400–469 or `+0x194` bit 0x10. A guarding foe (470/471) is
   *not* busy, so homing dashes keep tracking it. (confirmed; corrects "dodge / guard" in the attacker-movement note)
8. **Wall rush** (`FUN_0883C7BC`) happens only for **straight flights (reactions 5–8, 20) and reaction 12 drags**
   (state 0x17 with flag 0x01000000, script 700/701/750/751, t > 2), when the tick's movement sweep hit stage
   geometry, the velocity is non-zero and points into the surface within **60° of head-on** (angle(v, n) ≥ 120°).
   No minimum speed. Wall → snap to the contact, script **704** (754 for a back blow 750), state 0x12, then 101 fall;
   motion 32/36 is invincible throughout, `can_act_now` from 70. Floor → ordinary down (702/752). Once per flight
   (`+0x384` bit 0x10000). Shake 10 ticks. **No extra atkp or Brave damage** is applied there. (confirmed; HP
   follow-up open)

---

## 1. Guard

### 1.1 Starting and holding it
| Item | Finding | Where | Conf. |
|---|---|---|---|
| Gate | vtable +0x158 `FUN_08837B9C`: `+0x384 & 0x40000` and not in an action, or `can_act` (+0x190 bit 1) / +0x198 bit 2 and not already blocking | event-ops A | confirmed |
| Start | `FUN_08A5B2B4`: airborne (`FUN_088374B0`) → script 471, queued 101; else 470, queued 1; state **0x15** | | confirmed |
| State 0x15 | vtable 0x08A5B368: enter `FUN_08A5B00C` sets `+0x384` bit 0x20000 (= "is blocking", `FUN_08837310`), 471 also bit 0x1000; exit (0x08A5B060) clears 0x20000, 0x40000, 0x1000 | | confirmed |
| Per tick | `FUN_08A5B0B4`: if bit 0x40000 (a guard succeeded) and the fighter has input (`FUN_08B33418`) → leave at once (470 → script 1, state 0; 471 → 101, state 0x12). If bit 0x02000000 → leave the same way. Else when the motion has handed over to its queued script (1 / 101) → state 0 / fall | | confirmed |
| Guard motions (retail, Bartz layout in 20/31 fighters) | 470 = motion 20: `hit_window GUARD` 0–36, `perfect_guard_window` 0–4, `face_target {360,0,1,0}` 0–2, `attack_over_cpu` 38, `can_act_now` 50. 471 = 21: same + `air_motion` 0–42 | timelines | confirmed |
| GUARD atkp | 31/31: class 4, reaction 0, flags 0, power 0, rehit 0, angles (0, 90) | hits.json | confirmed |
| Volume | kind 6 on root (Bartz (0, 0.85, 0) r 1.1), group 4; the hit object exists ticks 0–36 (needed by `FUN_08808A74`) | | confirmed |

So a whiffed guard leaves the guarder unable to act from tick 37 to 49 (motion over, no guard). (confirmed from data)

### 1.2 Resolving an incoming sphere (`FUN_08809F00(defender, entry)`)
Called from the contact pass after "may hit" (invincible, already hit, same owner) and before the hurt test.

| Step | Code | Conf. |
|---|---|---|
| Skip if defender status mask 0x089C6C98 (collision-off class) | head | confirmed |
| The entry's element (attacker kind-4 sphere, swept copies included) must touch a sphere of the defender's kind-6 set (`FUN_0883EA64`), and the defender must own a live hit object for that record's group (`FUN_08808A74`). X = attacker object, Y = defender object | | confirmed |
| Gates: `g1 = FUN_088092E4(X, Y)` with **Y's** angles, `g2 = FUN_088092E4(Y, X)` with X's angles. Clash if `g1 && g2`, or `g1` alone when Y's class ∈ {4, 5, 6, 9, 12, 13, 14, 15} | 0x08809FD8–0x0880A06C | confirmed |
| Gate test: a = −X.swing (y = 0; owner forward if zero), b = Y.swing (owner forward if zero; a guard has only kind-6 records, so b = **guarder forward**); pass if `Y.atkp[+0x0E] ≤ |angle(b, a)| ≤ +0x0E + +0x0F` (degrees). Projectile objects always pass | `FUN_088092E4` (hit-contact note) | strong |
| Write the defender into entry object +0x1C and Y's atkp id into +0x30 | | confirmed |
| Classes cA = X.atkp+4, cD = Y.atkp+4 (summons → master). Ability 0x137 turns class 9 into 4; status bit 0x40000000 (`+0x380`) turns 0/1 into 2; the EX-burst owner (`FUN_08A50140/54`) turns 0/1 into 2 and 7/8 into 3 | | confirmed |
| **Perfect guard**: a class-4 side whose owner is blocking (470/471), has ability 0x40, `+0x195` bit 0x20 (`perfect_guard_window`) and not `+0x198` bit 0x08 → if its plain outcome T[4][other] is not 0 or 3 (it would have been crushed or pushed), its own outcome is later forced to 1; its class becomes **12** (so the other side reads T[other][12]); its object gets flag 0x200; `FUN_08B34F9C` runs (bonus / HUD) | 0x0880A20C–0x0880A384 | confirmed (ability names strong) |
| oA = T[cA][cD], oD = T[cD][cA] (forced to 1 by the perfect-guard step); 1 + 1 → 5 + 5 | | confirmed |
| `FUN_0884A53C(X, Y, oA)`, `FUN_0884A53C(Y, X, oD)` apply the outcomes (§2) | | confirmed |
| oA ∈ {1, 2, 3, 5, 6} → attacker object flag 0x08 (cancelled: no more hits this window) | 0x0880A43C | confirmed |
| A spark at the midpoint of the two sphere centres + `FUN_08ABB714` when oA ∈ {1, 2, 5} or oD ∈ {1, 5} | 0x0880A490 | strong (visual) |
| **oA or oD = 4** → the side with 4 takes the entry's hit: `FUN_08808EE4(entry, …)`, event 3 (`FUN_088593B4`, guard crush), `+0x380` bit 0x08000000 and `+0x5B5` bit 0x20 | 0x0880A684 | confirmed (event meaning strong) |
| **oA or oD = 3** → event 2 on that side (guard success) | 0x0880A91C | confirmed |
| Return (oA ≠ 0): the hurt test is skipped for this entry | | confirmed |

When oA = 0 (e.g. class 1, 2, 3 vs guard) the contact pass goes on to the normal hurt test against the guarder's
kind-2/3 spheres; with oD = 4 the hit was already delivered and the already-hit bit stops a second one. (confirmed)

## 2. Outcome codes (`FUN_0884A53C(obj, other, code)`)

Owner = obj+0x18 (kind 3 → master +0x5D4). Each owner is resolved once per tick: if `+0x384` bit 0x20 is already
set the call returns 0, otherwise it sets it. (confirmed)

| Code | Effect on the owner | Conf. |
|---|---|---|
| 0, ≥ 7 | nothing | confirmed |
| 1 | if `FUN_08808190` (per-object first contact) passes: dir = **−owner forward**, speed S = `FUN_0884A4C4`, call vtable +0xE0 = `FUN_08834274(owner, other.atkp.reaction, 1, dir, S)` → recoil; turn to −dir; if the other object's script id (+0x2E) is the other owner's current script → other owner `+0x384` bit 0x08000000 | confirmed |
| 2 | as 1 with S = 0 (recoil motion, no slide) | confirmed |
| 3 | `+0x384` |= 0x40000 (may cancel the guard / may block again) and 0x10000000 | confirmed (0x10000000 meaning open) |
| 4, 5 | nothing here (4 = see §1.2, the hit is delivered; 5 = only the cancel) | confirmed |
| 6 | dir = rotY(normalize(owner.pos − other.pos), 90°), speed 2·S, vtable +0xE0 → recoil; turn | confirmed (sign of the 90° strong) |

**Recoil `FUN_08834274`** (fighters only):
- In the air (`FUN_088374B0`) or when the reaction arg = 2: v = normalize(dir.x, 3, dir.z) · 0.4·S → `FUN_08A58A5C`
  (launch), mv+0x24 = −0.01, script **911** (motion 7: `enter_state` 0, invincible 0–10, `landing_allowed` and
  `can_act_now` from 80), landing 102. (confirmed; launch semantics strong)
- On the ground: script **901** (motion 6: invincible 0–10, `ground_follow`, `can_act_now` from 80), blend 16, ground
  slide `FUN_08832150(dir, S)` (×0.9 per tick, stops < 0.01 → total ≈ 10·S), actor+0x3E0 = 16, state 0. (confirmed)
- `S = (2 + 3k) / 16` m/tick with `k = 1` if t ≤ a, `1 − (t − a)/(b − a)` between, 0 if t ≥ b, where t = owner+0x1A8,
  a = +0x2A0, b = +0x2A4 (`FUN_0884A4C4`, constants 2, 5, 16 at 0x089C8A84..8C). t is the motion clock (the same
  field gates a flight's wall check at > 8 ticks); a/b were not traced (the current attack's window?). (formula
  confirmed; inputs guess)

### Clash table (rows = own class, columns = other class, 0x089D4978)
```
     0  1  2  3  4  5  6  7  8  9 10 11 12 13 14 15
 0:  1  2  2  6  1  1  0  0  6  0  0  0  1  1  1  0
 1:  0  1  2  6  0  1  0  0  0  0  0  0  1  1  1  0
 2:  0  0  1  0  0  0  0  0  0  0  0  0  1  1  0  0
 3:  0  0  6  0  0  0  0  0  0  0  0  0  6  6  0  0
 4:  3  1  4  4  0  0  0  3  1  3  0  0  0  0  0  0
 5:  3  3  1  1  0  0  0  3  3  3  0  0  0  0  0  0
 6:  0  0  0  0  0  0  0  3  0  0  0  0  0  0  0  0
 7:  6  6  6  6  6  6  6  0  6  6  0  0  6  6  0  6
 8:  0  6  6  6  6  6  0  0  0  0  0  0  6  6  0  6
 9:  0  0  0  0  1  1  0  6  0  1  6  0  1  1  1  0
10:  6  6  6  6  0  0  0  0  6  6  0  0  0  0  0  0
11:  all 0
12:  3  3  3  3  0  0  0  3  3  3  0  0  0  0  0  0
13:  3  3  1  3  0  0  0  3  3  3  0  0  0  0  0  0
14:  3  3  1  0  0  0  0  0  0  3  0  0  0  0  0  0
15:  0  0  0  1  0  0  0  3  3  0  0  0  0  0  0  0
```
Examples: Brave vs Brave (0/0) → 5/5, both cancelled, spark. Class 0 vs class 2 → class 0 recoils in place (2),
class 2 goes on (0). Anything vs class 7 (shots) mostly gives the shooter 6. Class 11 never clashes.

### Guard (class 4) against each attacker class
| Attacker class (retail n) | Typical names | Attacker | Guard | Result | Conf. |
|---|---|---|---|---|---|
| 0 (630) | NEAR, AIR_MIDDLE, chase hits | 1 | 3 | **blocked**, attacker bounces back (901/911) | confirmed |
| 1 (115) | `_ABILITY_FINISH`, some EX hits | 0 | 1 | guard pushed back (901), then the hurt test runs (hits unless the guarder is invincible) | code confirmed, outcome guess |
| 2 (542) | LIMIT_ATK, finishers, `*_ABI` | 0 | 4 | **guard crushed**: hit delivered, event 3 | confirmed |
| 3 (207) | LONG, finishers, some HP | 0 | 4 | guard crushed | confirmed |
| 4 / 5 / 6 / 10–15 | guards, DASH, EX_START | 0 | 0 | no clash; then the normal hurt test (classes 4, 5, 9, 12–15 never hurt) | confirmed |
| 7 (205) | magic / shots (THUNDARA, BLIZAGA), `GUARD_REF` | 6 | 3 | blocked, attacker object knocked sideways | confirmed (projectile-owner effect guess) |
| 8 (80) | some finishers | 6 | 1 | both recoil | confirmed |
| 9 (93) | `FACT_*` | 1 | 3 | blocked | confirmed |
| perfect guard (class 12) vs 0 / 1 / 2 / 3 | | 1 / 5 / 5 / 6 | 3 / 5 / 5 / 1 | all four stopped (attack cancelled); vs 1–3 the guard itself is not "ok" (no early cancel), vs 3 it recoils | confirmed |

HP vs Brave (flags 0x02) plays no part in the code, but in the data the 280 HP-flagged atkp are class 2 (121), 3 (95), 1 (33), 11 (24), 8 (6), 4 (1): none is class 0, so a plain guard stops an HP attack only when it is class 8.

## 3. Dodge

| Item | Finding | Where | Conf. |
|---|---|---|---|
| Gate | `FUN_08B3A5BC` (`can_act` bit, `dodge_cancel` 0x0028, 0x041E, `+0x384` & 0x40/0x80) | event-ops A | confirmed |
| Choice | `FUN_08B2D84C`: airborne → `FUN_08A54CD0` (state 8, **440**). Else angle between stick (+0x550) and the direction to the target (target +0x80, or +0x2FC / +0x300 / own forward): **< 45° → 410** (`FUN_08A61AC8`), **> 135° or no stick → 400** (`FUN_08A56A5C`, state 5), **45–135° → 460** (`FUN_08A588FC`, state 7). Thresholds written at 0x08B3AD90 / 94 = 45°, 135° (0x08B3A968) | | confirmed |
| 400 (state 5, vtable 0x08A56AE8) | turn toward the target first (`FUN_08835F60`), play 400 → queued 1; the state only waits for script 1. No movement code: root motion | `FUN_08A5679C`, `FUN_08A56828` | confirmed |
| 460 (state 7, vtable 0x08A58918) | turn to the stick direction (`FUN_08835BA4(+0x550)`), play 460 → queued 1; root motion carries it along the stick | `FUN_08A5860C` | confirmed |
| 410 (state 6 via `enter_state` at frame 4, vtable 0x08A61B38) | arm mode 1, store the target point (+0x590). On entering: face the target; d = flat distance to it clamped to [spec+108, +112] = [1, 5] m, u = (d − 1)/4 (+ ability 0x3B × 0.001); height = lerp(+116, +120, u) = 3 → 4 m, speed = lerp(+124, +128, u) = 0.1 → 0.3 m/tick; `FUN_08A5BF84` launch, landing 101/102 | `FUN_08A617BC` | code confirmed; height/speed roles strong |
| 440 (state 8, vtable 0x08A54D10) | air dodge, shared air update/landing (`FUN_08A5B534`, `FUN_08A5B690`); root motion + gravity | | strong |
| Timelines (motions 16–19, 21+ fighters identical) | `invincible` 0–20, `invincible_with_ability` 0–40 (ability 0x136), `perfect_dodge_window` 0–4, `dodge_attack_cancel` 26–56 (400, 440) / 40–56 (410, 460), `landing_allowed` 50 (410) / 40 (440), `can_act_now` 56 | timelines | confirmed |
| spec record values | 27/40 records (1, 5, 3, 4, 0.1, 0.3); others (1, 5, 3.5, 4, …), (1, 5, 4, 5, …), (2, 6, 4.5, 5.5, 0.25, 0.45) | battle.bin | confirmed |

How the rest of the game treats a dodging foe:
- **Hits**: while `invincible` the contact pass skips the defender before the clash test (no clash, no hit). From
  tick 21 the dodger is an ordinary target (no guard sphere). (confirmed)
- **Homing** (0x0D10 / 0x0925 / action homing): busy = script 400–469 → the destination freezes (unless
  `skip_checks`), `stop_if_busy` ends the dash. `face_target` / `turn_to_opponent` with arg2 = 0 stop turning. Guard
  470/471 is **not** busy. (confirmed)
- `FUN_08B2DC38`: an attack that would connect on an invulnerable fighter posts event 4 (dodged). (strong)

Root-motion distance is per fighter (animation data); the studio already reads `trans_include` (dummy.js `rootAt`),
so sample motions 16–19 the same way rather than using a constant.

## 4. Walls and floor

### 4.1 Where the check runs
| Item | Finding | Conf. |
|---|---|---|
| Sweep | the actor's movement step `FUN_0883B470` sweeps against the stage (`FUN_0889CF04`, mask 0x36) and sets `+0x380` bit **0x2000** when the move hit geometry, bit **0x4000** for a second surface class (`FUN_0881C68C`); both are cleared at the start of each step | confirmed |
| Callers of `FUN_0883C7BC(actor, v)` | (a) drag state 0x17 per tick `FUN_08A51C80`: phase-0 time > 2, drag flag 0x01000000, script 700/750 (`L_08838120`) or 701/751 (`L_088380F8`), v ≠ 0; (b) straight flight state 3 `FUN_08A61298`: v ≠ 0, and when the flight's flag byte (+0x20, a `FUN_08A6158C` argument) is set only once actor+0x1A8 > 8 | confirmed |
| So | reactions **5, 6, 7, 8, 20** (state 3) and **12** (drag with "up in target", 700/750) can wall-rush. 1, 2, 4, 19 (launch states), 11 (arc drag) and 13 (stagger 560/561) never do — they just slide along walls (the sweep) | strong (11/12/13 flag split from the hit-reaction note) |

### 4.2 `FUN_0883C7BC`
1. Needs `+0x380` bit 0x2000 or 0x4000 this tick, and not `+0x384` bit 0x10000 (already rushed). (confirmed)
2. Ray from the body centre (`FUN_08837030`) along normalize(v), length **max(|v|, 2.0)** (`FUN_0889BEB0`, mask 0x36).
   No hit → nothing. (confirmed)
3. **Angle**: angle(normalize(v), surface normal) must be **≥ 120°** (else nothing). (confirmed)
4. vtable +0xD0 (fighters: returns 0), stage callback `FUN_08B53748`, surface attribute bit 0x01000000 and
   `FUN_0881C68C` can veto (special surfaces). (confirmed calls; surface meanings guess)
5. Surface type 1 (**floor**): vtable +0xC8 = `FUN_08833E2C` (700/701 → **702**, 750/751 → **752**, down), y snapped
   to the floor (pos.y − actor+0x3C0). No rush flags. (confirmed; "type 1 = floor" strong)
6. Otherwise (**wall**): `+0x384` |= 0x10000; position = hit point + normal · body radius (`FUN_08837014`,
   `FUN_08834F30`); turn along the normal (`FUN_08835BA4`); script **704**, or **754** when the script was 750 (normal
   flipped); state **0x12**; queued 101 (blend 8); wall effect `FUN_08ACFA14` (manager 0x08AD0158); then `+0x380` |=
   0x8000 | 0x10000, `FUN_08833C18(0.5)`, screen shake `FUN_08834160` (10 ticks, amp 0.5 / 0.5). (confirmed)
7. `FUN_08833C18`: sets status 0x089C7F38 and, if the actor has status 0x089C7F78 (bit 0x20 of +0x388: an HP hit is
   pending), re-raises the HP-damage status with the second flag (`FUN_08831EA8(…, 1)`). It computes
   `int(+0x53E × 0.5)` (half the HP damage) but `FUN_08831EA8` never reads that argument. So there is **no wall-rush
   atkp and no Brave bonus** in this path; whether the HP manager does something with the second status flag was not
   followed. (code confirmed; gameplay effect open)

### 4.3 After the rush
- Motions 32 (704; also 703) and 36 (754/753), 20+ fighters: `air_motion` and `invincible` for the whole motion,
  `can_act_now` from 70; no hit windows. The fighter stays at the wall (state 0x12, no carried velocity) and then
  plays 101 (fall, `can_act` from 0) and lands with 102. (data confirmed; "no carried velocity" strong)
- `FUN_0883809C` treats 702–704 / 752–754 as not "reeling" (500–798 minus downs and wall hits). 703/753 have no
  writer in the code read (unused?). (confirmed / open)
- Stage walls are the stage collision mesh hit by the sweep and the ray; nothing else is needed for a flat studio
  wall: a plane `{point, normal}` gives the hit point, the normal and type "wall" (|n.y| small) or "floor" (n.y ≈ 1).

## 5. Retail checks

- GUARD atkp identical in all 31 fighters (class 4, reaction 0, flags 0, angles 0/90); Bartz GUARD volume kind 6
  root (0, 0.85, 0) r 1.1, group 4.
- Motions 6/7 (901/911) in 21+ fighters: invincible 0–10, `can_act_now` 80 (911: `landing_allowed` 80). A blocked
  Brave attack therefore costs the attacker 80 ticks; the guarder can act on its next input.
- Dodge motions 16–19 and guard 20/21 timelines match the table in 21+ fighters (the others have a different motion
  map, e.g. one fighter's guard is at 19/20).
- Class counts (2,068 atkp): 0 630, 2 542, 3 207, 7 205, 1 115, 9 93, 8 80, 11 72, 4 41, 12 34, 6 22, 10 10, 5 4,
  13–15 1–2. Class 2/3 hold most finishers and LIMIT hits: those are the guard crushers.
- spec records 0–39: forward-dodge hop parameters as in §3.

## 6. Not settled
- Inputs a/b of the recoil speed (`+0x2A0`, `+0x2A4`) and the meaning of `+0x384` bits 0x10000000 / 0x08000000.
- What clears `+0x384` bit 0x10000 (once-per-flight wall gate) — probably a new hit or landing.
- What the HP manager does with the second HP-damage status raised by a wall rush.
- Projectile hit objects in the clash (code 6 on a shot owner), and `FUN_08B34F9C` (perfect-guard reward).
- Motion lengths of 704/754 (read them from the GMO; only `can_act_now` 70 is in the timeline).

## 7. JS for the studio

Builds on the hit-contact and hit-reaction JS (`touches`, `placeVolume`, `applyHit`, the dummy foe `f`). `hit` = one
contact-list sphere of the attacker's hit object `obj` (with `obj.atkp`, `obj.swing` = −normalize(Σ(prev − cur)) or
the attacker forward), `defender` = the dummy with `guard` / `dodge` state from the functions below.

```js
const CLASH = [
  [1,2,2,6,1,1,0,0,6,0,0,0,1,1,1,0],[0,1,2,6,0,1,0,0,0,0,0,0,1,1,1,0],[0,0,1,0,0,0,0,0,0,0,0,0,1,1,0,0],
  [0,0,6,0,0,0,0,0,0,0,0,0,6,6,0,0],[3,1,4,4,0,0,0,3,1,3,0,0,0,0,0,0],[3,3,1,1,0,0,0,3,3,3,0,0,0,0,0,0],
  [0,0,0,0,0,0,0,3,0,0,0,0,0,0,0,0],[6,6,6,6,6,6,6,0,6,6,0,0,6,6,0,6],[0,6,6,6,6,6,0,0,0,0,0,0,6,6,0,6],
  [0,0,0,0,1,1,0,6,0,1,6,0,1,1,1,0],[6,6,6,6,0,0,0,0,6,6,0,0,0,0,0,0],[0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0],
  [3,3,3,3,0,0,0,3,3,3,0,0,0,0,0,0],[3,3,1,3,0,0,0,3,3,3,0,0,0,0,0,0],[3,3,1,0,0,0,0,0,0,3,0,0,0,0,0,0],
  [0,0,0,1,0,0,0,3,3,0,0,0,0,0,0,0]];
const GUARDISH = new Set([4, 5, 6, 9, 12, 13, 14, 15]);
const flat = v => { const l = Math.hypot(v.x, v.z); return l > 0 ? { x: v.x / l, y: 0, z: v.z / l } : null; };
const angDeg = (a, b) => Math.acos(Math.max(-1, Math.min(1, a.x * b.x + a.z * b.z))) * 180 / Math.PI;
const rotY = (v, a) => ({ x: v.x * Math.cos(a) + v.z * Math.sin(a), y: v.y, z: -v.x * Math.sin(a) + v.z * Math.cos(a) });

// Dummy guard: call guardStart(f, airborne) to block; GUARD atkp = {class 4, unk_0e 0, unk_0f 90}, sphere kind 6
// on the root (0, 0.85, 0) r 1.1 (read the fighter's own group-4 record), live while 0 <= f.guard.t <= 36.
function guardStart(f, airborne) {
  f.guard = { t: 0, script: airborne ? 471 : 470, ok: false, perfect: false };
  f.state = 'guard'; f.motion = airborne ? 21 : 20; f.motionT = 0;
}
function guardTick(f) {                       // FUN_08A5B0B4; the dummy "has input" as soon as a guard succeeded
  const g = f.guard; if (!g) return;
  g.t += 1;
  if (g.ok || g.t >= 50) { f.guard = null; f.state = f.pos.y > 0.001 ? 'falling' : 'idle'; f.canActIn = 0; }
}
function gate(selfSwing, otherSwing, angles) {   // FUN_088092E4: angles = [unk_0e, unk_0f] of the OTHER object
  const a = flat({ x: -selfSwing.x, y: 0, z: -selfSwing.z }), b = flat(otherSwing);
  if (!a || !b) return true;
  const d = angDeg(a, b);
  return angles[0] <= d && d <= angles[0] + angles[1];
}

// Returns {attacker: code, guard: code, blocked, crushed} or null when the sphere does not touch the guard.
// blocked = the attack's object is cancelled (skip the hurt test); crushed = deliver the hit now (applyHit).
function guardResolve(hit, obj, defender, opts = {}) {
  const g = defender.guard;
  if (!g || g.t > 36 || defender.inv > 0) return null;
  const gs = defender.guardSphere;                              // placed kind-6 sphere {shape:3, r1, r2, c}
  if (!touches(hit, gs)) return null;
  const k = obj.atkp, gFwd = { x: Math.sin(defender.facing), y: 0, z: Math.cos(defender.facing) };
  const g1 = gate(obj.swing, gFwd, [0, 90]);                    // the guard's angles
  const g2 = gate(gFwd, obj.swing, [k.unk_0e, k.unk_0f]);       // the attack's angles
  if (!(g1 && (g2 || GUARDISH.has(4 /* the guard's class */)))) return null;
  const cA = k.class;                                           // (ability 0x137 / EX-burst class changes left out)
  const perfect = g.t <= 4 && opts.perfectGuardAbility;         // ability 0x40 + perfect_guard_window
  let oA, oD;
  if (perfect) {                                                // guard becomes class 12
    const plain = CLASH[4][cA];
    oA = CLASH[cA][12]; oD = (plain !== 0 && plain !== 3) ? 1 : CLASH[12][cA];
  } else { oA = CLASH[cA][4]; oD = CLASH[4][cA]; }
  if (oA === 1 && oD === 1) oA = oD = 5;
  const out = { attacker: oA, guard: oD, blocked: [1, 2, 3, 5, 6].includes(oA), crushed: oA === 4 || oD === 4 };
  if (oD === 3) g.ok = true;                                    // +0x384 bit 0x40000: may leave the guard now
  return out;
}

// Recoil for codes 1 / 2 / 6 (FUN_08834274). t, a, b: the recoiling side's motion clock and window (guess: the
// attack's hit_window start/end); returns the motion to play and the movement for the attacker simulation.
function recoil(code, self, otherPos, t, a, b) {
  const k = t <= a ? 1 : t >= b ? 0 : 1 - (t - a) / (b - a);
  let S = (2 + 3 * k) / 16, dir;
  if (code === 1 || code === 2) dir = { x: -Math.sin(self.facing), y: 0, z: -Math.cos(self.facing) };
  else if (code === 6) { dir = rotY(flat({ x: self.pos.x - otherPos.x, y: 0, z: self.pos.z - otherPos.z }), Math.PI / 2); S *= 2; }
  else return null;
  if (code === 2) S = 0;
  if (self.airborne) {
    const v = { x: dir.x, y: 3, z: dir.z }, l = Math.hypot(v.x, v.y, v.z), s = 0.4 * S;
    return { script: 911, motion: 7, launch: { x: v.x / l * s, y: v.y / l * s, z: v.z / l * s }, invincible: 10, canAct: 80 };
  }
  return { script: 901, motion: 6, slide: { dir, S, decay: 0.9 }, invincible: 10, canAct: 80 };
}

// Dummy dodge. kind: 'back' (400), 'side' (460, dir = unit flat vector), 'forward' (410), 'air' (440).
// root(t): the dodge motion's trans_include position at tick t in the dummy's own frame (+z forward), as dummy.js
// rootAt; spec: the fighter's spec record (+108 … +128), default record 0.
const DODGE_MOTION = { back: 16, forward: 17, air: 18, side: 19 };
function dodgeStart(f, kind, targetPos, sideDir, spec = { dmin: 1, dmax: 5, h0: 3, h1: 4, s0: 0.1, s1: 0.3 }) {
  const toT = flat({ x: targetPos.x - f.pos.x, y: 0, z: targetPos.z - f.pos.z });
  if (kind === 'back' && toT) f.facing = Math.atan2(toT.x, toT.z);
  if (kind === 'side' && sideDir) f.facing = Math.atan2(sideDir.x, sideDir.z);
  f.dodge = { kind, t: 0, origin: { ...f.pos }, facing: f.facing };
  f.state = 'dodge'; f.motion = DODGE_MOTION[kind]; f.motionT = 0;
  if (kind === 'forward' && toT) {                               // hop from frame 4 (enter_state), FUN_08A617BC
    f.facing = Math.atan2(toT.x, toT.z);
    const d = Math.min(Math.max(Math.hypot(targetPos.x - f.pos.x, targetPos.z - f.pos.z), spec.dmin), spec.dmax);
    const u = (d - spec.dmin) / (spec.dmax - spec.dmin);
    f.dodge.hop = { at: 4, h: spec.h0 + (spec.h1 - spec.h0) * u, speed: spec.s0 + (spec.s1 - spec.s0) * u, dir: toT };
  }
}
function dodgeStep(f, root) {                // call once per tick; sets f.inv for the contact test
  const D = f.dodge; if (!D) return;
  D.t += 1;
  f.inv = D.t <= 20 ? 1 : 0;                 // invincible 0–20 (0–40 with ability 0x136)
  if (D.hop) {
    if (D.t === D.hop.at) { f.vel = { x: D.hop.dir.x * D.hop.speed, y: 0, z: D.hop.dir.z * D.hop.speed };
      f.drag = 0.975; dmStartAir(f, D.hop.h); }
  } else if (root) {                          // ground / air dodge: root motion only
    const p = rotY(root(D.t), D.facing);       // model +z forward turned by the facing
    f.pos = { x: D.origin.x + p.x, y: f.pos.y, z: D.origin.z + p.z };
  }
  if (D.t >= 56) { f.dodge = null; f.inv = 0; f.state = f.pos.y > 0.001 ? 'falling' : 'idle'; f.canActIn = 0; }
}
const isBusy = f => !!f.dodge;               // FUN_08837458: 400–469 only; a guarding dummy is not busy

// Wall / floor. wall = {point, normal (unit)}; the floor is y = 0. Call after the dummy moved this tick with the
// movement it made (v = pos − prevPos). Returns null, {type:'floor'} or {type:'wall', script, pos}.
function wallCheck(f, wall, v, prevPos) {
  const flying = f.move && (f.move.kind === 'line' ||
                 (f.move.kind === 'drag' && f.move.R === 12 && f.move.phase === 0 && f.move.t > 2));
  if (!flying || f.wallRushed) return null;
  if (f.move.kind === 'drag' && ![700, 701, 750, 751].includes(f.motion)) return null;
  const L = Math.hypot(v.x, v.y, v.z); if (L === 0) return null;
  const n = wall.normal, side = p => (p.x - wall.point.x) * n.x + (p.y - wall.point.y) * n.y + (p.z - wall.point.z) * n.z;
  const R = 0.6;                                               // body radius (kind-1 sphere); centre = pos + 0.9 up
  const c = { x: f.pos.x, y: f.pos.y + 0.9, z: f.pos.z };
  if (side(c) > R) return null;                                // the sweep did not touch the wall this tick
  const u = { x: v.x / L, y: v.y / L, z: v.z / L };
  if (Math.acos(u.x * n.x + u.y * n.y + u.z * n.z) * 180 / Math.PI < 120) return null;   // must hit within 60°
  const ray = Math.max(L, 2), dist = side(c) / -(u.x * n.x + u.y * n.y + u.z * n.z);
  if (!(dist <= ray)) return null;
  const hitP = { x: c.x + u.x * dist, y: c.y + u.y * dist, z: c.z + u.z * dist };
  if (n.y > 0.7) return { type: 'floor', motion: f.motion < 750 ? 702 : 752 };          // ordinary down
  f.wallRushed = true;                                          // once per flight (+0x384 bit 0x10000)
  const back = f.motion === 750;
  f.pos = { x: hitP.x + n.x * R, y: hitP.y + n.y * R - 0.9, z: hitP.z + n.z * R };
  const face = back ? { x: -n.x, z: -n.z } : { x: n.x, z: n.z };
  f.facing = Math.atan2(face.x, face.z);
  f.move = null; f.air = null; f.vel = { x: 0, y: 0, z: 0 };
  f.motion = back ? 754 : 704; f.motionT = 0; f.state = 'wall'; f.inv = Infinity; f.canActIn = 70;
  return { type: 'wall', script: f.motion, pos: f.pos, shake: { ticks: 10, amp: 0.5 } };
}
// After the 704/754 motion: f.motion = 101, f.inv = 0, startAir(f, 0) (fall), land → 102.
```

Studio tick order: attacker update → hit objects (`attackSpheres`) → for each contact sphere: `guardResolve` first
(if it returns `blocked` skip the hurt test; if `crushed` call `applyHit` now; apply `recoil` to the attacker and,
for guard code 1, to the dummy) → `contact` / `applyHit` → dummy movement (`dmTickFoe`, `dodgeStep`, `guardTick`)
→ `wallCheck` with this tick's movement. Clear `f.wallRushed` when a new hit starts a new reaction.
